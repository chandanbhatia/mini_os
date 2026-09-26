#include "os.h"

#define OS_IDLE_STACK_SZ 128 /* words */

#define TASK_READY_ARRAY_SIZE (OS_PRIORITY_MAX + 1U)

typedef struct
{
    OS_TCB *head;
    OS_TCB *tail;
} os_ready_list_t;

static OS_TCB os_tasks[OS_MAX_TASKS];

static os_ready_list_t g_ready_lists[TASK_READY_ARRAY_SIZE] = {NULL};

static OSTickCb os_tick_cb = NULL;

static OS_TCB *g_current_tcb   = NULL;
static uint32_t g_ready_bitmap = 0;

static uint32_t g_os_ms     = 0;
static uint8_t g_os_n_tasks = 0;
static uint8_t g_os_cur     = 0;

/* Bootstrap flag: on first PendSV call we must NOT save
 * the startup context into any real TCB.                */
static volatile uint8_t bootstrapping = 0;

static volatile bool if_os_started = false;

/* Idle task stack owned here; no user allocation needed */
static uint32_t os_idle_stack[OS_IDLE_STACK_SZ] __attribute__((aligned(8)));

static void os_idle_func(void);
static void os_remove_task_ready(OS_TCB *tcb);

void os_block_current_until(uint32_t until_ms);
uint32_t os_pendsv_schedule(uint32_t cur_sp);

/**
 * os_task_trampoline
 * First PC of every new task.  Re-enables IRQs (disabled during
 * context switch) then enters the user function.
 */
static void os_task_trampoline(void)
{
    os_tasks[g_os_cur].func();

    /* Task function returned should not happen in a well-written task */
    os_tasks[g_os_cur].state = OS_TASK_TERMINATED;

    while (1)
    {
        os_yield();
    }
}

/**
 * @brief os_init_stack
 *
 * Builds the 17-word initial frame so that the first
 * POP {r4-r11, PC} in PendSV_Handler launches the task:
 *
 *   POP reads EXC_RETURN (0xFFFFFFF9) into PC
 *   exception return
 *   CPU pops hardware exception frame
 *   PC = os_task_trampoline
 *
 * Frame built high-to-low with *--sp so SP ends at [r4] slot.
 *
 * Regarding xPSR bit 9 (STKALIGN):
 *   We 8-byte-align stack_top.  After 17 *--sp:
 *     initial_SP  = stack_top - 68 (mod 8 = 4)
 *     hw_frame_SP = initial_SP + 36 (mod 8 = 0) ? 8-byte aligned ?
 *   So bit 9 = 0 is correct (no dummy padding word).
 */
static uint32_t os_init_stack(uint32_t *stack, uint32_t words)
{
    stack[0] = OS_STACK_SENTINEL; /* guard at lowest address */

    uint32_t top = (uint32_t)(stack + words) & ~7U; /* 8-byte align */
    uint32_t *sp = (uint32_t *)top;

    /* Hardware exception frame (popped by CPU on exception return) */
    *--sp = 0x01000000UL;                 /* xPSR : Thumb, no flags   */
    *--sp = (uint32_t)os_task_trampoline; /* PC   : task entry        */
    *--sp = 0xFFFFFFF9UL;                 /* LR   : EXC_RETURN sentinel */
    *--sp = 0UL;                          /* r12                      */
    *--sp = 0UL;                          /* r3                       */
    *--sp = 0UL;                          /* r2                       */
    *--sp = 0UL;                          /* r1                       */
    *--sp = 0UL;                          /* r0                       */

    /* Software frame-must match PUSH {r4-r11, LR} layout exactly.
     * PUSH stores lowest-numbered reg at lowest address:
     *   [SP+0]  = r4   [SP+4]  = r5  ...  [SP+28] = r11  [SP+32] = LR
     * Build high-to-low so final sp points to r4 slot.             */
    *--sp = 0xFFFFFFF9UL; /* [SP+32]: EXC_RETURN ? POPped as PC   */
    *--sp = 0UL;          /* [SP+28]: r11                         */
    *--sp = 0UL;          /* [SP+24]: r10                         */
    *--sp = 0UL;          /* [SP+20]: r9                          */
    *--sp = 0UL;          /* [SP+16]: r8                          */
    *--sp = 0UL;          /* [SP+12]: r7                          */
    *--sp = 0UL;          /* [SP+8 ]: r6                          */
    *--sp = 0UL;          /* [SP+4 ]: r5                          */
    *--sp = 0UL;          /* [SP+0 ]: r4  ? initial SP            */

    return (uint32_t)sp;
}

/**
 * @brief Scheduler Ready Queue Management (O(1) Bitmap + __CLZ)
 */
static void os_add_task_ready(OS_TCB *tcb)
{
    if (tcb == NULL)
    {
        return;
    }

    uint32_t primask = os_enter_critical();

    tcb->state   = OS_TASK_READY;
    uint8_t prio = tcb->priority;

    /* Append to the end of the priority linked list */
    if (g_ready_lists[prio].head == NULL)
    {
        g_ready_lists[prio].head = tcb;
        g_ready_lists[prio].tail = tcb;
        tcb->next                = NULL;
    }
    else
    {
        g_ready_lists[prio].tail->next = tcb;
        g_ready_lists[prio].tail       = tcb;
        tcb->next                      = NULL;
    }

    /* Set active bit in ready bitmap */
    g_ready_bitmap |= (1U << prio);

    /* Trigger PendSV immediately if new task outranks currently running task */
    if (g_current_tcb != NULL && prio > g_current_tcb->priority)
    {
        SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    }

    os_exit_critical(primask);
}

/**
 * @brief Scheduler Remove Queue Management
 */
static void os_remove_task_ready(OS_TCB *tcb)
{
    if (tcb == NULL)
    {
        return;
    }

    uint32_t primask = os_enter_critical();
    uint8_t prio     = tcb->priority;

    /* Assert if ready list empty for priority Index, Major issue in scheduler or stack corruption */
    dev_assert(g_ready_lists[prio].head != NULL);
    dev_assert(g_ready_lists[prio].tail != NULL);

    OS_TCB *curr = g_ready_lists[prio].head;
    OS_TCB *prev = NULL;

    while (curr != tcb)
    {
        dev_assert(curr != NULL);
        prev = curr;
        curr = curr->next;
    }

    /* Update Head or Middle Link */
    if (prev == NULL)
    {
        g_ready_lists[prio].head = curr->next;
    }
    else
    {
        prev->next = curr->next;
    }

    /* Update Tail Pointer if tail node was removed */
    if (g_ready_lists[prio].tail == curr)
    {
        g_ready_lists[prio].tail = prev;
    }

    /* Disconnect removed node */
    tcb->next = NULL;

    /* Clear bitmap if this priority list is now empty */
    if (g_ready_lists[prio].head == NULL)
    {
        g_ready_bitmap &= ~(1U << prio);
    }

    os_exit_critical(primask);
}

/**
 * @brief Initializes the core RTOS kernel structures and task control blocks.
 *        Must be called before creating tasks or starting the scheduler.
 */
void os_kernel_init(void)
{
    memset((void *)os_tasks, 0, sizeof(os_tasks));
    g_os_n_tasks = 0;
    g_os_cur     = 0;
    g_os_ms      = 0;
    os_tick_cb   = NULL;

    g_ready_bitmap = 0;
    for (uint32_t i = 0; i < TASK_READY_ARRAY_SIZE; i++)
    {
        g_ready_lists[i].head = NULL;
        g_ready_lists[i].tail = NULL;
    }

    /* Create idle task at slot 0 right here so
     * os_task_create() fills slots 1..N and os_start()
     * needs no runtime array shift.                     */
    OS_TCB *idle     = &os_tasks[0];
    idle->func       = os_idle_func;
    idle->name       = "idle";
    idle->stack_mem  = os_idle_stack;
    idle->stack_size = OS_IDLE_STACK_SZ;
    idle->state      = OS_TASK_READY;
    idle->id         = 0;
    idle->sp         = os_init_stack(os_idle_stack, OS_IDLE_STACK_SZ);
    idle->priority   = OS_PRIORITY_IDLE;
    idle->next       = NULL;
    g_os_n_tasks     = 1;

    os_add_task_ready(idle);
}

/**
 * @brief Creates a new cooperative task and initializes its stack frame.
 *
 * @param func        Pointer to the task entry function.
 * @param name        Human-readable task name string (for debugging).
 * @param stack       Pointer to the user-allocated stack memory buffer.
 * @param stack_words Size of the stack memory buffer in 32-bit words.
 * @return int        Assigned Task ID (>= 0) on success, or negative value on error.
 */
int os_task_create(osThreadFunc_t func,
                   const char *name,
                   uint32_t *stack,
                   uint32_t stack_words,
                   uint8_t priority)
{
    if (g_os_n_tasks >= OS_MAX_TASKS || !func || !stack || stack_words < 64)
    {
        return -1;
    }

    uint8_t id  = g_os_n_tasks++;
    OS_TCB *tcb = &os_tasks[id];

    for (uint32_t i = 0; i < stack_words; i++)
    {
        stack[i] = OS_STACK_SENTINEL;
    }

    tcb->func       = func;
    tcb->name       = name ? name : "tcbask";
    tcb->stack_mem  = stack;
    tcb->stack_size = stack_words;
    tcb->state      = OS_TASK_READY;
    tcb->id         = id;
    tcb->sp         = os_init_stack(stack, stack_words);
    tcb->priority   = priority > OS_PRIORITY_MAX ? OS_PRIORITY_MAX : priority;
    tcb->next       = NULL;

    os_add_task_ready(tcb);

    return (int)id;
}

/**
 * @brief PendSV_Handler - naked context switch
 *
 * r0 = cur_sp (argument to os_pendsv_schedule)
 * r0 = next_sp (return value from os_pendsv_schedule)
 *
 * Safe from SysTick preemption because both exceptions are
 * configured to the same priority (see os_start).
 */
__attribute__((naked)) void PendSV_Handler(void)
{
    __asm volatile(
        /* 1. Save current task's software frame + EXC_RETURN */
        "PUSH {r4-r11, LR}          \n"

        /* 2. Pass current SP to C scheduler, get next SP back */
        "MOV  r0, SP                \n" /* r0 = cur_sp (argument) */
        "BL   os_pendsv_schedule    \n" /* r0 = next_sp (return)  */

        /* 3. Load next task's SP and restore its software frame */
        "MOV  SP, r0                \n" /* SP = next_sp           */
        "POP  {r4-r11, PC}          \n" /* PC = EXC_RETURN        */
                                        /* ? exception return     */
                                        /* ? CPU pops hw frame    */
        ::: "memory");
}

/**
 * @brief os_pendsv_schedule - C scheduler called from PendSV_Handler
 *
 * cur_sp : SP value after PUSH {r4-r11, LR} in the handler,
 *          i.e. the bottom of the current task's software frame.
 *
 * Returns: SP of the next task to run.
 */
uint32_t os_pendsv_schedule(uint32_t cur_sp)
{
    /* Bootstrap: first invocation has no real current task to save */
    if (bootstrapping)
    {
        bootstrapping = 0;
        /* g_os_cur and os_tasks[g_os_cur].state already set by os_start() */
        return os_tasks[g_os_cur].sp;
    }

    /* Normal operation: save current task's SP */
    os_tasks[g_os_cur].sp = cur_sp;

    if (os_tasks[g_os_cur].state == OS_TASK_RUNNING)
    {
        os_tasks[g_os_cur].state = OS_TASK_READY;
    }

    /* Rotate head to tail for equal-priority round-robin (O(1)) */
    uint32_t primask = os_enter_critical();
    uint32_t prio    = 31U - (uint32_t)__CLZ(g_ready_bitmap);

    OS_TCB *head = g_ready_lists[prio].head;
    g_os_cur     = head->id;

    /* Only rotate if at least 2 tasks exist at this priority level */
    if (head != NULL && head->next != NULL)
    {
        g_ready_lists[prio].head       = head->next; /* New head */
        head->next                     = NULL;       /* Old head becomes tail */
        g_ready_lists[prio].tail->next = head;
        g_ready_lists[prio].tail       = head;
    }

    os_tasks[g_os_cur].state = OS_TASK_RUNNING;

    os_exit_critical(primask);

    return os_tasks[g_os_cur].sp;
}

/**
 * @brief Voluntarily yields CPU execution to allow other ready tasks to run.
 */
void os_yield(void)
{
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    __DSB();
    __ISB();
}

/**
 * @brief Weak Stack Overflow callback.
 */
__attribute__((weak)) void os_stack_overflow_hook(uint8_t task_id)
{
    (void)task_id;
    /* Default: spin here or call a core dump function to log the issue;
     * Override this function to add logging/reset logic. */
    __asm volatile("cpsid i"); /* disable all IRQs */
    while (1)
        ;

    /* Also can call dev assert here */
}

/**
 * @brief Advances the system tick counter by 1 ms and processes sleeping/blocked tasks.
 *        Must be called periodically from the hardware SysTick or timer ISR.
 */
void os_tick(void)
{
    if (!if_os_started)
    {
        return;
    }

    g_os_ms++;

    for (uint8_t i = 0; i < g_os_n_tasks; i++)
    {
        /* Wake sleeping tasks whose deadline has arrived */
        if (os_tasks[i].state == OS_TASK_SLEEPING &&
            (int32_t)(g_os_ms - os_tasks[i].sleep_until_ms) >= 0)
        {
            os_tasks[i].state = OS_TASK_READY;
            os_add_task_ready(&os_tasks[i]);
        }

        /* Stack overflow detection */
        if (os_tasks[i].stack_mem &&
            os_tasks[i].stack_mem[0] != OS_STACK_SENTINEL)
        {
            os_stack_overflow_hook(i);
        }
    }

    if (os_tick_cb)
    {
        os_tick_cb(g_os_ms);
    }

    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    __DSB();
    __ISB();
}

/**
 * @brief Starts the cooperative scheduler and triggers PendSV to launch the first user task.
 *        Does not return under normal operating conditions.
 */
void os_start(void)
{
    uint32_t lowest = (1UL << __NVIC_PRIO_BITS) - 1UL;
    NVIC_SetPriority(PendSV_IRQn, lowest);  /* context switch      */
    NVIC_SetPriority(SysTick_IRQn, lowest); /* SAME-critical!    */

    __disable_irq();

    g_os_cur                 = (g_os_n_tasks > 1) ? 1U : 0U;
    os_tasks[g_os_cur].state = OS_TASK_RUNNING;

    bootstrapping = 1;
    SCB->ICSR     = SCB_ICSR_PENDSVSET_Msk;

    if_os_started = true;
    __DSB();

    __enable_irq(); /* PendSV fires, first task starts */
    while (1)
        ; /* never reached */
}

/**
 * @brief Idle Func
 *
 */
static void os_idle_func(void)
{
    while (1)
    {
        os_yield();
    }
}

/**
 * @brief Blocks the current task for a specified duration in milliseconds.
 *
 * @param ms Duration to sleep in milliseconds.
 */
void os_sleep_ms(uint32_t ms)
{
    uint32_t primask                  = os_enter_critical();
    os_tasks[g_os_cur].sleep_until_ms = g_os_ms + ms;
    os_tasks[g_os_cur].state          = OS_TASK_SLEEPING;
    os_exit_critical(primask);
    os_yield();
}

/**
 * @brief Blocks the current task for a specified duration in milliseconds.
 *
 * @param ms Duration to sleep in milliseconds.
 */
void os_block_current_until(uint32_t until_ms)
{
    os_tasks[g_os_cur].sleep_until_ms = until_ms;
    os_tasks[g_os_cur].state          = OS_TASK_SLEEPING;
    os_remove_task_ready(&os_tasks[g_os_cur]);
}

/**
 * @brief Unblocks a specific task by ID and sets its state back to ready.
 *
 * @param id Task ID to unblock.
 */
void os_unblock_task(uint8_t id)
{
    if (id < g_os_n_tasks &&
        (os_tasks[id].state == OS_TASK_BLOCKED ||
         os_tasks[id].state == OS_TASK_SLEEPING))
    {
        os_tasks[id].state = OS_TASK_READY;
        os_add_task_ready(&os_tasks[id]);
    }
}

/**
 * @brief Retrieves the Task ID of the currently executing task.
 *
 * @return uint8_t ID of the running task.
 */
uint8_t os_current_id(void)
{
    return g_os_cur;
}

/**
 * @brief Gets the current system uptime in milliseconds.
 *
 * @return uint32_t Ticks elapsed since system boot.
 */
uint32_t os_now_ms(void)
{
    return g_os_ms;
}

/**
 * @brief Blocks the current task indefinitely until explicitly unblocked by another task or ISR.
 */
void os_block_current(void)
{
    os_tasks[g_os_cur].state = OS_TASK_BLOCKED;
    os_remove_task_ready(&os_tasks[g_os_cur]);
}
