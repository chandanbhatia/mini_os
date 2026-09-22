#include "os.h"

#define OS_IDLE_STACK_SZ 128 /* words */

static OS_TCB os_tasks[OS_MAX_TASKS];

static uint8_t os_n_tasks  = 0;
static uint8_t os_cur      = 0;
static uint32_t os_ms      = 0;
static OSTickCb os_tick_cb = NULL;

/* Idle task stack owned here; no user allocation needed */
static uint32_t os_idle_stack[OS_IDLE_STACK_SZ];

static void os_idle_func(void);

void os_block_current_until(uint32_t until_ms);

/* =============================================================
 * os_task_trampoline
 * First PC of every new task.  Re-enables IRQs (disabled during
 * context switch) then enters the user function.
 * ============================================================= */
static void os_task_trampoline(void)
{
    /* Explicitly enable IRQs. The context switch disabled them; a new task
     * has no saved PRIMASK to restore so we enable unconditionally here.
     * This is the only legitimate use of a hardcoded 0 in this codebase. */
    __asm volatile("cpsie i" ::: "memory");
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
 * Pre-loads a fake context frame so the first POP {r4-r11, PC}
 * in os_do_switch drops into os_task_trampoline.
 *
 * Also writes the stack sentinel at the lowest address (stack[0])
 * for overflow detection.
 *
 * Frame layout (SP points to sp[0], grows upward to sp[8]):
 *   sp[0..7] = r4..r11 (initialised to debug patterns)
 *   sp[8]    = PC      = &os_task_trampoline
 *
 * If FPU is enabled, 16 additional words for s16-s31 are placed
 * below the integer frame (lower address = pushed last by VPUSH).
 * ============================================================= */
static uint32_t os_init_stack(uint32_t *stack, uint32_t words)
{
    DEV_ASSERT(stack != NULL);

    stack[0] = OS_STACK_SENTINEL;

    /* Align top of stack to 8 bytes */
    volatile uint32_t top = (uint32_t)(stack + words);
    top &= ~7U;
    uint32_t *sp = (uint32_t *)top;

#if defined(__FPU_USED) && (__FPU_USED == 1)
    /* Reserve 16 words for s16-s31 FPU frame */
    sp -= 16;
    for (int i = 0; i < 16; i++)
    {
        sp[i] = 0;
    }
#endif

    /* Integer frame: r4-r11 + PC */
    sp -= 9;
    sp[0] = 0x00000004UL;                 /* r4  debug sentinel */
    sp[1] = 0x00000005UL;                 /* r5  */
    sp[2] = 0x00000006UL;                 /* r6  */
    sp[3] = 0x00000007UL;                 /* r7  */
    sp[4] = 0x00000008UL;                 /* r8  */
    sp[5] = 0x00000009UL;                 /* r9  */
    sp[6] = 0x0000000AUL;                 /* r10 */
    sp[7] = 0x0000000BUL;                 /* r11 */
    sp[8] = (uint32_t)os_task_trampoline; /* PC */

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

/**
 * @brief Saves exactly the registers that a cooperative switch needs to save.
 * os_do_switch() is entered as a regular C function call via BL. The calling convention (AAPCS) 
 * guarantees that r0-r3 and r12 are either saved by the compiler before the call or are no longer live.
 * LR holds the return address back into the caller, so saving it and later popping it as PC resumes the task exactly where it yielded
 * 'To do': If FPU is enabled (__FPU_USED == 1), VPUSH {s16-s31} / VPOP {s16-s31} 
 */
__attribute__((naked)) static void os_do_switch(uint32_t *save_sp, uint32_t load_sp)
{
    __asm volatile(
        "PUSH {r4-r11, LR}    \n"
        "STR  SP, [r0]        \n"
        "MOV  SP,  r1         \n"
        "POP  {r4-r11, PC}    \n" ::: "memory");
}

/**
 * @brief Voluntarily yields CPU execution to allow other ready tasks to run.
 */
void os_yield(void)
{
    uint32_t primask = os_enter_critical();

    uint8_t cur = os_cur;

    if (os_tasks[cur].state == OS_TASK_RUNNING)
    {
        os_tasks[cur].state = OS_TASK_READY;
    }

    /* Round-robin search for next READY task */
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

    /* No ready task found ? fall back to idle (always slot 0),always READY its loop calls os_yield() endlessly) */
    if (checked == os_n_tasks)
    {
        next = 0;
    }

    if (next == cur)
    {
        os_tasks[cur].state = OS_TASK_RUNNING;
        os_exit_critical(primask);
        return;
    }

    os_cur               = next;
    os_tasks[next].state = OS_TASK_RUNNING;

    /* IRQs remain disabled across the switch.
     * Re-enabled by the trampoline (first run) or here (resumed run). */
    os_do_switch((uint32_t *)&os_tasks[cur].sp,
                 os_tasks[next].sp);

    /* -- Resumed here when this task is rescheduled -- */
    os_exit_critical(primask);
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
 * @brief Starts the cooperative scheduler and yields control to the highest-priority/first task.
 *        Does not return under normal operating conditions.
 */
void os_start(void)
{
    /* Prefer slot 1 (first user task) if it exists */
    os_cur                 = (os_n_tasks > 1) ? 1U : 0U;
    os_tasks[os_cur].state = OS_TASK_RUNNING;

    static uint32_t _startup_sp; /* throwaway main() never resumes */
    os_do_switch(&_startup_sp, os_tasks[os_cur].sp);

    /* never reached */
    while (1)
        ;
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
