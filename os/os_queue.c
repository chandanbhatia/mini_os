#include "os_queue.h"

/* Helper macro to calculate memory slot offset */
#define QUEUE_SLOT_PTR(q, idx) ((uint8_t *)(q)->buffer + ((idx) * ((q)->queue_size + 1)))

/* =============================================================
 * os_queue_init
 * User provides backing memory buffer. Minimum buffer size needed:
 * queue_depth * (queue_size + 1) [1 byte extra per slot for payload len]
 * ============================================================= */
void os_queue_init(OS_Queue *q, void *buffer_mem, uint16_t queue_size, uint16_t queue_depth)
{
    if (!q || !buffer_mem || !queue_size || !queue_depth)
    {
        return;
    }

    q->buffer          = (uint8_t *)buffer_mem;
    q->head            = 0;
    q->tail            = 0;
    q->count           = 0;
    q->queue_size      = queue_size;
    q->queue_depth     = queue_depth;
    q->waiting_rx_task = OS_QUEUE_NO_WAITER;
    q->waiting_tx_task = OS_QUEUE_NO_WAITER;
}

/* =============================================================
 * os_queue_send_timeout
 * Blocking send with timeout.
 * Returns 0 on success, -1 on timeout.
 * ============================================================= */
int os_queue_send_timeout(OS_Queue *q, const void *data, uint8_t len, uint32_t timeout_ms)
{
    uint32_t start_time = os_now_ms();
    uint32_t deadline   = start_time + timeout_ms;

    if (!q || !data)
    {
        return -1;
    }

    while (1)
    {
        uint32_t primask = os_enter_critical();

        if (q->count < q->queue_depth)
        {
            /* Calculate slot destination address */
            uint8_t *slot = QUEUE_SLOT_PTR(q, q->tail);
            uint8_t l     = (len > q->queue_size) ? q->queue_size : len;

            /* Memory Layout per Slot: [Data Bytes (queue_size)] + [Length (1 byte)] */
            memcpy(slot, data, l);
            slot[q->queue_size] = l; /* Store payload length at end of slot */

            q->tail = (q->tail + 1) % q->queue_depth;
            q->count++;

            /* Wake up task waiting for data */
            if (q->waiting_rx_task != OS_QUEUE_NO_WAITER)
            {
                os_unblock_task(q->waiting_rx_task);
                q->waiting_rx_task = OS_QUEUE_NO_WAITER;
            }

            os_exit_critical(primask);
            return 0;
        }

        /* Check deadline if non-infinite timeout */
        if (timeout_ms != OS_WAIT_FOREVER)
        {
            if ((int32_t)(os_now_ms() - deadline) >= 0)
            {
                if (q->waiting_tx_task == os_current_id())
                {
                    q->waiting_tx_task = OS_QUEUE_NO_WAITER;
                }
                os_exit_critical(primask);
                return -1;
            }
        }

        /* Block task until queue has space */
        q->waiting_tx_task = os_current_id();

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
 * os_queue_send_from_isr
 * Non-blocking, ISR-safe queue send.
 * Returns 0 on success, -1 if queue is full.
 * Set higher_prio_task_woken to non-null to notify scheduler.
 * ============================================================= */
int os_queue_send_from_isr(OS_Queue *q, const void *data, uint8_t len)
{
    uint32_t primask = os_enter_critical();

    if (!q || !data)
    {
        os_exit_critical(primask);
        return -1;
    }

    if (q->count >= q->queue_depth)
    {
        os_exit_critical(primask);
        return -1; /* Queue full - ISR cannot block! */
    }

    /* Copy data into next available slot */
    uint8_t *slot = QUEUE_SLOT_PTR(q, q->tail);
    uint8_t l     = (len > q->queue_size) ? q->queue_size : len;

    memcpy(slot, data, l);
    slot[q->queue_size] = l;

    q->tail = (q->tail + 1) % q->queue_depth;
    q->count++;

    /* Wake receiver if waiting */
    if (q->waiting_rx_task != OS_QUEUE_NO_WAITER)
    {
        os_unblock_task(q->waiting_rx_task);
        q->waiting_rx_task = OS_QUEUE_NO_WAITER;
    }

    os_exit_critical(primask);
    /* NO os_yield() here! */
    return 0;
}

/* Non-blocking wrapper for ISRs or fast sends */
int os_queue_send(OS_Queue *q, const void *data, uint8_t len)
{
    if (!q || !data)
    {
        return -1;
    }
    return os_queue_send_timeout(q, data, len, 0);
}

/* =============================================================
 * os_queue_receive_timeout
 * Blocking receive with timeout.
 * Returns 0 on success, -1 on timeout, -2 on Busy..
 * ============================================================= */
int os_queue_receive_timeout(OS_Queue *q, void *data, uint8_t *len, uint32_t timeout_ms)
{
    uint32_t start_time = os_now_ms();
    uint32_t deadline   = start_time + timeout_ms;

    if (!q || !data || !len)
    {
        return -1;
    }

    while (1)
    {
        uint32_t primask = os_enter_critical();

        /* Register as receiver waiter */
        if (q->waiting_rx_task != OS_QUEUE_NO_WAITER && q->waiting_rx_task != os_current_id())
        {
            /* Reject request immediately with an error code */
            os_exit_critical(primask);
            return -2; /* BUSY */
        }

        if (q->count > 0)
        {
            uint8_t *slot    = QUEUE_SLOT_PTR(q, q->head);
            uint8_t slot_len = slot[q->queue_size];

            if (len)
            {
                *len = slot_len;
            }
            if (data)
            {
                memcpy(data, slot, slot_len);
            }

            q->head = (q->head + 1) % q->queue_depth;
            q->count--;

            /* Wake up task blocked on queue full */
            if (q->waiting_tx_task != OS_QUEUE_NO_WAITER)
            {
                os_unblock_task(q->waiting_tx_task);
                q->waiting_tx_task = OS_QUEUE_NO_WAITER;
            }

            os_exit_critical(primask);
            return 0;
        }

        /* Check deadline if non-infinite timeout */
        if (timeout_ms != OS_WAIT_FOREVER)
        {
            if ((int32_t)(os_now_ms() - deadline) >= 0)
            {
                if (q->waiting_rx_task == os_current_id())
                {
                    q->waiting_rx_task = OS_QUEUE_NO_WAITER;
                }
                os_exit_critical(primask);
                return -1;
            }
        }

        /* Register as receiver waiter */
        q->waiting_rx_task = os_current_id();

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
 * os_queue_receive_from_isr
 * Non-blocking, ISR-safe queue receive.
 * Returns 0 on success, -1 if queue is empty.
 * ============================================================= */
int os_queue_receive_from_isr(OS_Queue *q, void *data, uint8_t *len)
{
    uint32_t primask = os_enter_critical();

    if (!q || !data || !len || q->count == 0)
    {
        os_exit_critical(primask);
        return -1; /* Queue empty - ISR cannot block! */
    }

    uint8_t *slot    = QUEUE_SLOT_PTR(q, q->head);
    uint8_t slot_len = slot[q->queue_size];

    if (len)
    {
        *len = slot_len;
    }
    if (data)
    {
        memcpy(data, slot, slot_len);
    }

    q->head = (q->head + 1) % q->queue_depth;
    q->count--;

    /* Wake transmitter if a task was waiting for space */
    if (q->waiting_tx_task != OS_QUEUE_NO_WAITER)
    {
        os_unblock_task(q->waiting_tx_task);
        q->waiting_tx_task = OS_QUEUE_NO_WAITER;
    }

    os_exit_critical(primask);
    /* NO os_yield() here! */
    return 0;
}

/* =============================================================
 * os_queue_count
 * ============================================================= */
uint16_t os_queue_count(OS_Queue *q)
{
    if (q)
    {
        uint32_t primask = os_enter_critical();
        uint16_t c       = q->count;
        os_exit_critical(primask);
        return c;
    }
    else
    {
        return 0;
    }
}

#if 0
/* =============================================================
 * os_queue_init
 * ============================================================= */
void os_queue_init(OS_Queue *q, uint32_t queue_size, uint32_t queue_depth)
{
    q->head         = 0;
    q->tail         = 0;
    q->count        = 0;
    q->waiting_task = OS_QUEUE_NO_WAITER;
    q->queue_size   = queue_size;
    q->queue_depth  = queue_depth;
}

/* =============================================================
 * os_queue_send  -  non-blocking, ISR-safe
 * ============================================================= */
int os_queue_send(OS_Queue *q, const void *data, uint8_t len)
{
    uint32_t primask = os_enter_critical();

    if (q->count >= q->queue_depth)
    {
        os_exit_critical(primask);
        return -1;
    }

    uint8_t l         = (len > q->queue_size) ? q->queue_size : len;
    OS_QueueMsg *slot = &q->buf[q->tail];
    memcpy(slot->data, data, l);
    slot->len = l;

    q->tail = (uint8_t)((q->tail + 1) % q->queue_depth);
    q->count++;

    /* Wake blocked receiver if present */
    if (q->waiting_task != OS_QUEUE_NO_WAITER)
    {
        os_unblock_task(q->waiting_task);
        q->waiting_task = OS_QUEUE_NO_WAITER;
    }

    os_exit_critical(primask);
    return 0;
}

/* =============================================================
 * os_queue_receive_timeout
 * Returns 0 on success, -1 on timeout.
 * ============================================================= */
int os_queue_receive_timeout(OS_Queue *q,
                             void *data,
                             uint8_t *len,
                             uint32_t timeout_ms)
{
    uint32_t deadline = os_now_ms() + timeout_ms;

    while (1)
    {
        uint32_t primask = os_enter_critical();

        if (q->count > 0)
        {
            OS_QueueMsg *slot = &q->buf[q->head];
            if (len)
            {
                *len = slot->len;
            }
            if (data)
            {
                memcpy(data, slot->data, slot->len);
            }
            q->head = (uint8_t)((q->head + 1) % q->queue_depth);
            q->count--;
            /* No need to clear waiting_task - we got data, not timeout */
            os_exit_critical(primask);
            return 0;
        }

        if (os_now_ms() >= deadline)
        {
            /*Clear stale waiter ID before giving up */
            q->waiting_task = OS_QUEUE_NO_WAITER;
            os_exit_critical(primask);
            return -1;
        }

        /* Register as waiter BEFORE marking state sleeping */
        q->waiting_task = os_current_id();

        /* Use SLEEPING (not BLOCKED) so os_tick() also wakes us at
         * the deadline without needing a separate timer primitive.
         * os_queue_send wakes us early if data arrives first.       */
        os_block_current_until(deadline); /* state ? SLEEPING  [Fix 8] */

        os_exit_critical(primask);
        os_yield();
        /* Woken either by send (early) or by os_tick (deadline).
         * Loop to determine which case and act accordingly.          */
    }
}

/* =============================================================
 * os_queue_count
 * ============================================================= */
uint8_t os_queue_count(OS_Queue *q)
{
    uint32_t primask = os_enter_critical();
    uint8_t c        = q->count;
    os_exit_critical(primask);
    return c;
}
#endif
