#ifndef OS_SEM_H
#define OS_SEM_H

#include <stdint.h>
#include "os.h"

#define OS_SEM_NO_WAITER 0xFF

typedef struct
{
    volatile uint8_t count;     /* 0 = not signalled, 1 = signalled */
    volatile uint8_t rx_waiter; /* task blocked in wait()            */
    volatile uint8_t tx_waiter; /* task blocked in signal_timeout()  */
} OS_Sem;

/* =============================================================
 * os_sem_init. initial = 0 (blocked) or 1 (ready)
 * ============================================================= */
void os_sem_init(OS_Sem *sem, uint8_t initial);

/* =============================================================
 * os_sem_wait - block forever until signalled
 * ============================================================= */
void os_sem_wait(OS_Sem *sem);

/* =============================================================
 * os_sem_wait_timeout
 * timeout_ms: OS_TIMEOUT_FOREVER to block indefinitely.
 * Returns 0 on success (signal consumed), -1 on timeout.
 * ============================================================= */
int os_sem_wait_timeout(OS_Sem *sem, uint32_t timeout_ms);

/* =============================================================
 * os_sem_try_wait - non-blocking
 * Returns 0 on success, -1 if not signalled.
 * ============================================================= */
int os_sem_try_wait(OS_Sem *sem);

/* =============================================================
 * os_sem_signal - from task context
 * Wakes a waiting task and yields so it runs promptly.
 * ============================================================= */
void os_sem_signal(OS_Sem *sem);

/* =============================================================
 * os_sem_signal_timeout
 * If count is already 1 (previous signal not yet consumed),
 * waits up to timeout_ms for it to be consumed, then signals.
 * Use OS_TIMEOUT_FOREVER to wait indefinitely for consumption.
 * Returns 0 on success (signal posted), -1 if timeout elapsed
 * while waiting for previous signal to be consumed.
 * ============================================================= */
int os_sem_signal_timeout(OS_Sem *sem, uint32_t timeout_ms);

/* =============================================================
 * os_sem_signal_from_isr - ISR-safe, non-blocking, no context switch
 * ============================================================= */
void os_sem_signal_from_isr(OS_Sem *sem);

#endif /* OS_SEM_H */
