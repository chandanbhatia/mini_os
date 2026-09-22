#include "os_sem.h"

/* =============================================================
 * os_sem_init
 * ============================================================= */
void os_sem_init(OS_Sem *sem, uint8_t initial)
{
    sem->count     = (initial > 0) ? 1U : 0U;
    sem->rx_waiter = OS_SEM_NO_WAITER;
    sem->tx_waiter = OS_SEM_NO_WAITER;
}

/* =============================================================
 * os_sem_wait - block forever until signalled
 * ============================================================= */
void os_sem_wait(OS_Sem *sem)
{
    (void)os_sem_wait_timeout(sem, OS_WAIT_FOREVER);
}

/* =============================================================
 * os_sem_wait_timeout
 * ============================================================= */
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

/* =============================================================
 * os_sem_try_wait - non-blocking
 * ============================================================= */
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

/* =============================================================
 * os_sem_signal - from task context
 * ============================================================= */
void os_sem_signal(OS_Sem *sem)
{
    (void)os_sem_signal_timeout(sem, OS_WAIT_FOREVER);
}

/* =============================================================
 * os_sem_signal_timeout
 * ============================================================= */
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

/* =============================================================
 * os_sem_signal_from_isr - ISR-safe, non-blocking, no context switch
 * ============================================================= */
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
