#include "os_sem.h"

/**
 * @brief Initializes a binary semaphore control block.
 *
 * @param[out] sem      Pointer to the semaphore instance.
 * @param[in]  initial  Initial count state: 0 for unavailable (blocked), 1 for available (ready).
 */
void os_sem_init(OS_Sem *sem, uint8_t initial)
{
    sem->count     = (initial > 0) ? 1U : 0U;
    sem->rx_waiter = OS_SEM_NO_WAITER;
    sem->tx_waiter = OS_SEM_NO_WAITER;
}

/**
 * @brief Acquires (waits on) a semaphore, blocking indefinitely until signaled.
 *
 * Must be called from task context only.
 *
 * @param[in,out] sem  Pointer to the semaphore instance.
 */
void os_sem_wait(OS_Sem *sem)
{
    (void)os_sem_wait_timeout(sem, OS_WAIT_FOREVER);
}

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
int os_sem_wait_timeout(OS_Sem *sem, uint32_t timeout_ms)
{
    uint32_t deadline = os_now_ms() + timeout_ms;

    while (1)
    {
        uint32_t primask = os_enter_critical();

        if (sem->count > 0)
        {
            sem->count = 0;

            if (sem->tx_waiter != OS_SEM_NO_WAITER)
            {
                os_unblock_task(sem->tx_waiter);
                sem->tx_waiter = OS_SEM_NO_WAITER;
            }
            os_exit_critical(primask);
            return 0;
        }

        /* Register as receiver waiter */
        if (sem->rx_waiter != OS_SEM_NO_WAITER && sem->rx_waiter != os_current_id())
        {
            /* Reject request immediately with an error code */
            os_exit_critical(primask);
            return -2; /* BUSY */
        }

        /* Check deadline if non-infinite timeout */
        if (timeout_ms != OS_WAIT_FOREVER)
        {
            if ((int32_t)(os_now_ms() - deadline) >= 0)
            {
                if (sem->rx_waiter == os_current_id())
                {
                    sem->rx_waiter = OS_SEM_NO_WAITER; /* clear stale waiter */
                }
                os_exit_critical(primask);
                return -1;
            }
        }

        /* Set waiter BEFORE state change */
        sem->rx_waiter = os_current_id();

        if (timeout_ms == OS_WAIT_FOREVER)
        {
            os_block_current();
        }
        else
        {
            os_block_current_until(deadline);
        }

        os_exit_critical(primask);
        os_yield();
    }
}

/**
 * @brief Non-blocking attempt to acquire (wait on) a semaphore.
 *
 * @param[in,out] sem  Pointer to the semaphore instance.
 *
 * @return  0 on success (signal consumed).
 * @return -1 if semaphore is not signaled (would block).
 */
int os_sem_try_wait(OS_Sem *sem)
{
    uint32_t primask = os_enter_critical();
    int result       = -1;
    if (sem->count > 0)
    {
        sem->count = 0;
        if (sem->tx_waiter != OS_SEM_NO_WAITER)
        {
            os_unblock_task(sem->tx_waiter);
            sem->tx_waiter = OS_SEM_NO_WAITER;
        }
        result = 0;
    }
    os_exit_critical(primask);
    return result;
}

/**
 * @brief Signals (posts to) a semaphore from task context.
 *
 * If a task is waiting on the semaphore, it will be unblocked and a context switch/yield
 * will be triggered so the unblocked task can run immediately.
 *
 * @param[in,out] sem  Pointer to the semaphore instance.
 */
void os_sem_signal(OS_Sem *sem)
{
    (void)os_sem_signal_timeout(sem, OS_WAIT_FOREVER);
}

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
int os_sem_signal_timeout(OS_Sem *sem, uint32_t timeout_ms)
{
    uint32_t deadline = os_now_ms() + timeout_ms;

    while (1)
    {
        uint32_t primask = os_enter_critical();

        if (sem->count == 0)
        {
            sem->count = 1;

            if (sem->rx_waiter != OS_SEM_NO_WAITER)
            {
                os_unblock_task(sem->rx_waiter);
                sem->rx_waiter = OS_SEM_NO_WAITER;
            }
            os_exit_critical(primask);
            os_yield();
            return 0;
        }

        /* Check deadline if non-infinite timeout */
        if (timeout_ms != OS_WAIT_FOREVER)
        {
            if ((int32_t)(os_now_ms() - deadline) >= 0)
            {
                if (sem->tx_waiter == os_current_id())
                {
                    sem->tx_waiter = OS_SEM_NO_WAITER; /* clear stale waiter */
                }
                os_exit_critical(primask);
                return -1;
            }
        }

        sem->tx_waiter = os_current_id();

        if (timeout_ms == OS_WAIT_FOREVER)
        {
            os_block_current();
        }
        else
        {
            os_block_current_until(deadline);
        }

        os_exit_critical(primask);
        os_yield();
    }
}

/**
 * @brief Signals a semaphore safely from an Interrupt Service Routine (ISR).
 *
 * Unblocks any waiting task without executing a context switch or blocking inside the ISR.
 *
 * @note Must only be called from ISR context.
 *
 * @param[in,out] sem  Pointer to the semaphore instance.
 */
void os_sem_signal_from_isr(OS_Sem *sem)
{
    sem->count = 1;

    if (sem->rx_waiter != OS_SEM_NO_WAITER)
    {
        os_unblock_task(sem->rx_waiter);
        sem->rx_waiter = OS_SEM_NO_WAITER;
    }
    /* Do NOT call os_yield() - illegal from ISR context */
}
