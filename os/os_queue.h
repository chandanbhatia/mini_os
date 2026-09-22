#ifndef OS_QUEUE_H
#define OS_QUEUE_H

#include <stdint.h>
#include "os.h"

#define OS_QUEUE_NO_WAITER 0xFF /* sentinel: no task waiting */

/* Dynamic Message Storage Layout */
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

/* =============================================================
 * os_queue_init
 * User provides backing memory buffer. Minimum buffer size needed:
 * queue_depth * (queue_size + 1) [1 byte extra per slot for payload len]
 * ============================================================= */
void os_queue_init(OS_Queue *q, void *buffer_mem, uint16_t queue_size, uint16_t queue_depth);

/* =============================================================
 * os_queue_send_timeout
 * Blocking send with timeout.
 * Returns 0 on success, -1 on timeout.
 * ============================================================= */
int os_queue_send_timeout(OS_Queue *q, const void *data, uint8_t len, uint32_t timeout_ms);

/* =============================================================
 * Non-blocking wrapper for ISRs or fast sends
 * ============================================================= */
int os_queue_send(OS_Queue *q, const void *data, uint8_t len);

/* =============================================================
 * os_queue_send_from_isr
 * Non-blocking, ISR-safe queue send.
 * Returns 0 on success, -1 if queue is full.
 * Set higher_prio_task_woken to non-null to notify scheduler.
 * ============================================================= */
int os_queue_send_from_isr(OS_Queue *q, const void *data, uint8_t len);

/* =============================================================
 * os_queue_receive_timeout
 * Blocking receive with timeout.
 * Returns 0 on success, -1 on timeout.
 * ============================================================= */
int os_queue_receive_timeout(OS_Queue *q, void *data, uint8_t *len, uint32_t timeout_ms);

/* =============================================================
 * os_queue_receive_from_isr
 * Non-blocking, ISR-safe queue receive.
 * Returns 0 on success, -1 if queue is empty.
 * ============================================================= */
int os_queue_receive_from_isr(OS_Queue *q, void *data, uint8_t *len);

/* =============================================================
 * os_queue_count
 * ============================================================= */
uint16_t os_queue_count(OS_Queue *q);

#if 0
/* -- Queue configuration --------------------------------------- */
#define OS_QUEUE_MSG_SIZE  32   /* bytes per message payload    */
#define OS_QUEUE_DEPTH     8    /* maximum queued messages      */
#define OS_QUEUE_NO_WAITER 0xFF /* sentinel: no task waiting    */

/* -- Message container ----------------------------------------- */
typedef struct
{
    uint8_t data[OS_QUEUE_MSG_SIZE];
    uint8_t len; /* actual payload length used   */
} OS_QueueMsg;

/* -- Queue control block --------------------------------------- */
typedef struct
{
    OS_QueueMsg buf[OS_QUEUE_DEPTH];
    uint8_t head;         /* next read  index             */
    uint8_t tail;         /* next write index             */
    uint8_t count;        /* current number of messages   */
    uint8_t waiting_task; /* task blocked on receive      */
    uint8_t queue_size;   /* bytes per message payload    */
    uint8_t queue_depth;  /* maximum queued messages      */
} OS_Queue;

/* -- API ------------------------------------------------------- */

/** Initialise a queue. Must be called before first use. */
void os_queue_init(OS_Queue *q, uint32_t queue_size, uint32_t queue_depth);

/**
 * Send a message (non-blocking).
 * Safe to call from task or ISR context.
 * Returns  0 on success, -1 if the queue is full.
 */
int os_queue_send(OS_Queue *q, const void *data, uint8_t len);

/**
 * Receive with a millisecond timeout.
 * Returns  0 on success, -1 on timeout.
 */
int os_queue_receive_timeout(OS_Queue *q,
                             void *data,
                             uint8_t *len,
                             uint32_t timeout_ms);

/** Number of messages currently in the queue. */
uint8_t os_queue_count(OS_Queue *q);
#endif
#endif /* OS_QUEUE_H */
