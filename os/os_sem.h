#ifndef OS_SEM_H
#define OS_SEM_H

#include "os.h"

/**
 * @brief Special value indicating no task is currently blocked on the semaphore.
 */
#define OS_SEM_NO_WAITER 0xFF

/**
 * @brief Binary semaphore control block (SCB).
 */
typedef struct
{
    volatile uint8_t count;     /* 0 = not signalled, 1 = signalled */
    volatile uint8_t rx_waiter; /* task blocked in wait()            */
    volatile uint8_t tx_waiter; /* task blocked in signal_timeout()  */
} OS_Sem;

/**
 * @brief Initializes a binary semaphore control block.
 *
 * @param[out] sem      Pointer to the semaphore instance.
 * @param[in]  initial  Initial count state: 0 for unavailable (blocked), 1 for available (ready).
 */
void os_sem_init(OS_Sem *sem, uint8_t initial);

/**
 * @brief Acquires (waits on) a semaphore, blocking indefinitely until signaled.
 *
 * Must be called from task context only.
 *
 * @param[in,out] sem  Pointer to the semaphore instance.
 */
void os_sem_wait(OS_Sem *sem);

/**
 * @brief Acquires (waits on) a semaphore with a specified timeout limit.
 *
 * @param[in,out] sem         Pointer to the semaphore instance.
 * @param[in]     timeout_ms  Timeout in milliseconds, or OS_TIMEOUT_FOREVER to block indefinitely.
 *
 * @return  0 on success (signal consumed).
 * @return -1 on timeout (signal not received before expiration).
 * @return -2 on busy/error condition.
 */
int os_sem_wait_timeout(OS_Sem *sem, uint32_t timeout_ms);

/**
 * @brief Non-blocking attempt to acquire (wait on) a semaphore.
 *
 * @param[in,out] sem  Pointer to the semaphore instance.
 *
 * @return  0 on success (signal consumed).
 * @return -1 if semaphore is not signaled (would block).
 */
int os_sem_try_wait(OS_Sem *sem);

/**
 * @brief Signals (posts to) a semaphore from task context.
 *
 * If a task is waiting on the semaphore, it will be unblocked and a context switch/yield
 * will be triggered so the unblocked task can run immediately.
 *
 * @param[in,out] sem  Pointer to the semaphore instance.
 */
void os_sem_signal(OS_Sem *sem);

/**
 * @brief Signals a semaphore with a timeout if the previous signal was not consumed.
 *
 * If `count` is already 1, this function blocks up to `timeout_ms` waiting for the previous
 * signal to be consumed by an acquiring task before posting the new signal.
 *
 * @param[in,out] sem         Pointer to the semaphore instance.
 * @param[in]     timeout_ms  Timeout in milliseconds, or OS_TIMEOUT_FOREVER to wait indefinitely.
 *
 * @return  0 on success (signal successfully posted).
 * @return -1 if timeout elapsed while waiting for previous signal to be consumed.
 */
int os_sem_signal_timeout(OS_Sem *sem, uint32_t timeout_ms);

/**
 * @brief Signals a semaphore safely from an Interrupt Service Routine (ISR).
 *
 * Unblocks any waiting task without executing a context switch or blocking inside the ISR.
 *
 * @note Must only be called from ISR context.
 *
 * @param[in,out] sem  Pointer to the semaphore instance.
 */
void os_sem_signal_from_isr(OS_Sem *sem);

#endif /* OS_SEM_H */
