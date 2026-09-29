#include "os_queue.h"

/**
 * @brief Helper macro to calculate memory slot offse.
 */
#define QUEUE_SLOT_PTR(q, idx) ((uint8_t *)(q)->buffer + ((idx) * ((q)->queue_size + 1)))

/**
 * @brief Initializes a message queue instance with user-allocated backing memory.
 *
 * @note The required backing memory buffer size must be at least:
 *       `queue_depth * (queue_size + 1)` bytes (allocating 1 byte overhead per slot for length).
 *
 * @param[out] q           Pointer to the queue control block instance.
 * @param[in]  buffer_mem  Pointer to raw allocated memory matching minimum required size.
 * @param[in]  queue_size  Maximum payload length allowed per message in bytes.
 * @param[in]  queue_depth Maximum number of messages the queue can hold.
 */
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

/**
 * @brief Sends a message to the queue from task context with a blocking timeout.
 *
 * If the queue is full, the calling task blocks until space becomes available or the timeout expires.
 *
 * @param[in,out] q           Pointer to the queue control block instance.
 * @param[in]     data        Pointer to message payload buffer.
 * @param[in]     len         Length of message payload in bytes (must be <= queue_size).
 * @param[in]     timeout_ms  Timeout in milliseconds, or OS_TIMEOUT_FOREVER to block indefinitely.
 *
 * @return  0 on success (message posted to queue).
 * @return -1 on timeout (queue remained full).
 */
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

/**
 * @brief Non-blocking, ISR-safe function to post a message to the queue.
 *
 * @note Must only be called from ISR context. Does not block or invoke scheduler directly.
 *
 * @param[in,out] q     Pointer to the queue control block instance.
 * @param[in]     data  Pointer to message payload buffer.
 * @param[in]     len   Length of message payload in bytes (must be <= queue_size).
 *
 * @return  0 on success (message posted to queue).
 * @return -1 if queue is full.
 */
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

/**
 * @brief Non-blocking wrapper to send a message to the queue from task context.
 *
 * Returns immediately without blocking if the queue is full.
 *
 * @param[in,out] q     Pointer to the queue control block instance.
 * @param[in]     data  Pointer to message payload buffer.
 * @param[in]     len   Length of message payload in bytes (must be <= queue_size).
 *
 * @return  0 on success (message posted to queue).
 * @return -1 if queue is full.
 */
int os_queue_send(OS_Queue *q, const void *data, uint8_t len)
{
    if (!q || !data)
    {
        return -1;
    }
    return os_queue_send_timeout(q, data, len, 0);
}

/**
 * @brief Receives a message from the queue with a blocking timeout.
 *
 * If the queue is empty, the calling task blocks until a message is posted or the timeout expires.
 *
 * @param[in,out] q           Pointer to the queue control block instance.
 * @param[out]    data        Pointer to buffer where the received message will be copied.
 * @param[out]    len         Pointer to variable that receives the actual payload length.
 * @param[in]     timeout_ms  Timeout in milliseconds, or OS_TIMEOUT_FOREVER to block indefinitely.
 *
 * @return  0 on success (message retrieved).
 * @return -1 on timeout (queue remained empty).
 * @return -2 on busy/error condition.
 */
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

/**
 * @brief Non-blocking, ISR-safe function to receive a message from the queue.
 *
 * @note Must only be called from ISR context.
 *
 * @param[in,out] q     Pointer to the queue control block instance.
 * @param[out]    data  Pointer to buffer where the received message will be copied.
 * @param[out]    len   Pointer to variable that receives the actual payload length.
 *
 * @return  0 on success (message retrieved).
 * @return -1 if queue is empty.
 */
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

/**
 * @brief Returns the current number of messages stored in the queue.
 *
 * @param[in] q  Pointer to the queue control block instance.
 *
 * @return Number of queued messages.
 */
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
