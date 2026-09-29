#ifndef OS_QUEUE_H
#define OS_QUEUE_H

#include "os.h"

/**
 * @brief Special value indicating no task is currently blocked on the queue.
 */
#define OS_QUEUE_NO_WAITER 0xFF /* sentinel: no task waiting */

/**
 * @brief Queue Control Block (QCB) for dynamic message storage.
 */
typedef struct
{
    uint8_t *buffer;         /* Pointer to raw backing memory buffer */
    uint16_t head;           /* Read index */
    uint16_t tail;           /* Write index */
    uint16_t count;          /* Current item count */
    uint16_t queue_depth;    /* Maximum queued messages */
    uint16_t queue_size;     /* Max payload size per message */
    uint8_t waiting_rx_task; /* Task blocked on receive (queue empty) */
    uint8_t waiting_tx_task; /* Task blocked on send (queue full) */
} OS_Queue;

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
void os_queue_init(OS_Queue *q, void *buffer_mem, uint16_t queue_size, uint16_t queue_depth);

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
int os_queue_send_timeout(OS_Queue *q, const void *data, uint8_t len, uint32_t timeout_ms);

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
int os_queue_send(OS_Queue *q, const void *data, uint8_t len);

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
int os_queue_send_from_isr(OS_Queue *q, const void *data, uint8_t len);

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
int os_queue_receive_timeout(OS_Queue *q, void *data, uint8_t *len, uint32_t timeout_ms);

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
int os_queue_receive_from_isr(OS_Queue *q, void *data, uint8_t *len);

/**
 * @brief Returns the current number of messages stored in the queue.
 *
 * @param[in] q  Pointer to the queue control block instance.
 *
 * @return Number of queued messages.
 */
uint16_t os_queue_count(OS_Queue *q);

#endif /* OS_QUEUE_H */
