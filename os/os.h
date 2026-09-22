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

void os_tick(void);
void os_kernel_init(void);
int os_task_create(osThreadFunc_t func, const char *name, uint32_t *stack, uint32_t stack_words);
void os_start(void);
void os_sleep_ms(uint32_t ms);
void os_yield(void);

void os_unblock_task(uint8_t id);
void os_block_current_until(uint32_t until_ms);

uint8_t os_current_id(void);
uint32_t os_now_ms(void);
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
