#include "os.h"

#define OS_IDLE_STACK_SZ 128 /* words */

static OS_TCB os_tasks[OS_MAX_TASKS];

static OSTickCb os_tick_cb = NULL;

static uint32_t os_ms     = 0;
static uint8_t os_n_tasks = 0;
static uint8_t os_cur     = 0;

/* Bootstrap flag: on first PendSV call we must NOT save
 * the startup context into any real TCB.                */
static volatile uint8_t bootstrapping = 0;

static volatile bool if_os_started = false;

/* Idle task stack owned here; no user allocation needed */
static uint32_t os_idle_stack[OS_IDLE_STACK_SZ] __attribute__((aligned(8)));

static void os_idle_func(void);

void os_block_current_until(uint32_t until_ms);
uint32_t os_pendsv_schedule(uint32_t cur_sp);

/* =============================================================
 * os_task_trampoline
 * First PC of every new task.  Re-enables IRQs (disabled during
 * context switch) then enters the user function.
 * ============================================================= */
static void os_task_trampoline(void)
{
    os_tasks[os_cur].func();

    /* Task function returned should not happen in a well-written task */
    os_tasks[os_cur].state = OS_TASK_TERMINATED;

    while (1)
    {
        os_yield();
    }
}

/* =============================================================
 * os_init_stack
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
 * ============================================================= */
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
 * @brief Initializes the core RTOS kernel structures and task control blocks.
 *        Must be called before creating tasks or starting the scheduler.
 */
void os_kernel_init(void)
{
    memset((void *)os_tasks, 0, sizeof(os_tasks));
    os_n_tasks = 0;
    os_cur     = 0;
    os_ms      = 0;
    os_tick_cb = NULL;

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
    os_n_tasks       = 1;
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
                   uint32_t stack_words)
{
    if (os_n_tasks >= OS_MAX_TASKS || !func || !stack || stack_words < 64)
    {
        return -1;
    }

    uint8_t id = os_n_tasks++;
    OS_TCB *t  = &os_tasks[id];

    for (uint32_t i = 0; i < stack_words; i++)
    {
        stack[i] = OS_STACK_SENTINEL;
    }

    t->func       = func;
    t->name       = name ? name : "task";
    t->stack_mem  = stack;
    t->stack_size = stack_words;
    t->state      = OS_TASK_READY;
    t->id         = id;
    t->sp         = os_init_stack(stack, stack_words);

    return (int)id;
}

/* =============================================================
 * PendSV_Handler - naked context switch
 *
 * r0 = cur_sp (argument to os_pendsv_schedule)
 * r0 = next_sp (return value from os_pendsv_schedule)
 *
 * Safe from SysTick preemption because both exceptions are
 * configured to the same priority (see os_start).
 * ============================================================= */
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

/* =============================================================
 * os_pendsv_schedule - C scheduler called from PendSV_Handler
 *
 * cur_sp : SP value after PUSH {r4-r11, LR} in the handler,
 *          i.e. the bottom of the current task's software frame.
 *
 * Returns: SP of the next task to run.
 * ============================================================= */
uint32_t os_pendsv_schedule(uint32_t cur_sp)
{
    /* Bootstrap: first invocation has no real current task to save */
    if (bootstrapping)
    {
        bootstrapping = 0;
        /* os_cur and os_tasks[os_cur].state already set by os_start() */
        return os_tasks[os_cur].sp;
    }

    /* Normal operation: save current task's SP */
    os_tasks[os_cur].sp = cur_sp;

    if (os_tasks[os_cur].state == OS_TASK_RUNNING)
    {
        os_tasks[os_cur].state = OS_TASK_READY;
    }

    /* Round-robin: find next READY task */
    uint8_t cur     = os_cur;
    uint8_t next    = (uint8_t)((cur + 1) % os_n_tasks);
    uint8_t checked = 0;

    while (checked < os_n_tasks)
    {
        if (os_tasks[next].state == OS_TASK_READY)
        {
            break;
        }
        next = (uint8_t)((next + 1) % os_n_tasks);
        checked++;
    }
    /* All tasks sleeping/blocked ? run idle (always READY at slot 0) */
    if (checked == os_n_tasks)
    {
        next = 0;
    }

    os_cur               = next;
    os_tasks[next].state = OS_TASK_RUNNING;

    return os_tasks[next].sp;
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

    os_ms++;

    for (uint8_t i = 0; i < os_n_tasks; i++)
    {
        /* Wake sleeping tasks whose deadline has arrived */
        if (os_tasks[i].state == OS_TASK_SLEEPING &&
            (int32_t)(os_ms - os_tasks[i].sleep_until_ms) >= 0)
        {
            os_tasks[i].state = OS_TASK_READY;
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
        os_tick_cb(os_ms);
    }
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
    os_cur                 = (os_n_tasks > 1) ? 1U : 0U;
    os_tasks[os_cur].state = OS_TASK_RUNNING;

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
    uint32_t primask                = os_enter_critical();
    os_tasks[os_cur].sleep_until_ms = os_ms + ms;
    os_tasks[os_cur].state          = OS_TASK_SLEEPING;
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
    os_tasks[os_cur].sleep_until_ms = until_ms;
    os_tasks[os_cur].state          = OS_TASK_SLEEPING;
}

/**
 * @brief Unblocks a specific task by ID and sets its state back to ready.
 *
 * @param id Task ID to unblock.
 */
void os_unblock_task(uint8_t id)
{
    if (id < os_n_tasks &&
        (os_tasks[id].state == OS_TASK_BLOCKED ||
         os_tasks[id].state == OS_TASK_SLEEPING))
    {
        os_tasks[id].state = OS_TASK_READY;
    }
}

/**
 * @brief Retrieves the Task ID of the currently executing task.
 *
 * @return uint8_t ID of the running task.
 */
uint8_t os_current_id(void)
{
    return os_cur;
}

/**
 * @brief Gets the current system uptime in milliseconds.
 *
 * @return uint32_t Ticks elapsed since system boot.
 */
uint32_t os_now_ms(void)
{
    return os_ms;
}

/**
 * @brief Blocks the current task indefinitely until explicitly unblocked by another task or ISR.
 */
void os_block_current(void)
{
    os_tasks[os_cur].state = OS_TASK_BLOCKED;
}
