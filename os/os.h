#ifndef OS_H
#define OS_H

#include <stdint.h>
#include "fw_common.h"

#define OS_MAX_TASKS 8

#define OS_STACK_SENTINEL 0xDEADBEEFU
#define OS_WAIT_FOREVER   0xFFFFFFFFU

typedef void (*osThreadFunc_t)(void);

typedef enum
{
    OS_TASK_READY      = 0,
    OS_TASK_RUNNING    = 1,
    OS_TASK_SLEEPING   = 2,
    OS_TASK_BLOCKED    = 3,
    OS_TASK_TERMINATED = 4,
} OSThreadState;

typedef struct
{
    const char *name;
    uint32_t sp;         /* saved stack pointer value    */
    uint32_t *stack_mem; /* memory for stack            */
    uint32_t stack_size; /* stack buffer size in words   */
    OSThreadState state;
    uint32_t sleep_until_ms;
    osThreadFunc_t func;
    uint8_t id;
} OS_TCB;

typedef void (*OSTickCb)(uint32_t tick_ms);

/**
 * @brief Advances the system tick counter by 1 ms and processes sleeping/blocked tasks.
 *        Must be called periodically from the hardware SysTick or timer ISR.
 */
void os_tick(void);

/**
 * @brief Initializes the core RTOS kernel structures and task control blocks.
 *        Must be called before creating tasks or starting the scheduler.
 */
void os_kernel_init(void);

/**
 * @brief Creates a new cooperative task and initializes its stack frame.
 *
 * @param func        Pointer to the task entry function.
 * @param name        Human-readable task name string (for debugging).
 * @param stack       Pointer to the user-allocated stack memory buffer.
 * @param stack_words Size of the stack memory buffer in 32-bit words.
 * @return int        Assigned Task ID (>= 0) on success, or negative value on error.
 */
int os_task_create(osThreadFunc_t func, const char *name, uint32_t *stack, uint32_t stack_words);

/**
 * @brief Starts the cooperative scheduler and yields control to the highest-priority/first task.
 *        Does not return under normal operating conditions.
 */
void os_start(void);

/**
 * @brief Blocks the current task for a specified duration in milliseconds.
 *
 * @param ms Duration to sleep in milliseconds.
 */
void os_sleep_ms(uint32_t ms);

/**
 * @brief Voluntarily yields CPU execution to allow other ready tasks to run.
 */
void os_yield(void);

/**
 * @brief Unblocks a specific task by ID and sets its state back to ready.
 *
 * @param id Task ID to unblock.
 */
void os_unblock_task(uint8_t id);

/**
 * @brief Blocks the current task until a absolute system time (in ms) is reached.
 *
 * @param until_ms Absolute target system timestamp in milliseconds.
 */
void os_block_current_until(uint32_t until_ms);

/**
 * @brief Retrieves the Task ID of the currently executing task.
 *
 * @return uint8_t ID of the running task.
 */
uint8_t os_current_id(void);

/**
 * @brief Gets the current system uptime in milliseconds.
 *
 * @return uint32_t Ticks elapsed since system boot.
 */
uint32_t os_now_ms(void);

/**
 * @brief Blocks the current task indefinitely until explicitly unblocked by another task or ISR.
 */
void os_block_current(void);

static inline uint32_t os_enter_critical(void)
{
    uint32_t primask;
    __asm volatile(
        "MRS %0, PRIMASK  \n"
        "CPSID I          \n"
        : "=r"(primask)::"memory");
    return primask;
}

static inline void os_exit_critical(uint32_t primask)
{
    __asm volatile(
        "MSR PRIMASK, %0  \n" ::"r"(primask) : "memory");
}

static inline int os_deadline_passed(uint32_t deadline)
{
    return (int32_t)(os_now_ms() - deadline) >= 0;
}

#endif
