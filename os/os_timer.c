#include "os_timer.h"

#include "os_queue.h"

/**
 * @brief Dedicated stack allocated for the Timer Daemon Task in 32-bit words (256 words = 1024 bytes).
 */
#define OS_TIMER_TASK_STACK_SIZE 256U

/**
 * @brief Maximum depth of the internal timer message queue, setting the limit for
 *        simultaneously pending timer callback events deferred from SysTick ISR context.
 */
#define OS_TIMER_QUEUE_DEPTH 16U

/**
 * @brief Head and tail container for the active timer linked list.
 */
typedef struct
{
    os_timer_t *head;
    os_timer_t *tail;
} os_timer_list_t;

/**
 * @brief Message queue payload structure representing an expired timer event.
 *        Pushed from SysTick ISR context and consumed by the Timer Daemon Task.
 */
typedef struct
{
    os_timer_cb_t cb;
    void *arg;
} os_timer_event_t;

/**
 * @brief Linked list container tracking all currently active/running timers.
 */
static os_timer_list_t g_active_timer_list = {NULL, NULL};

/**
 * @brief Queue Control Block (QCB) managing deferred timer callback events.
 */
static OS_Queue g_timer_queue;

/**
 * @brief Dedicated static stack memory for the high-priority Timer Daemon Task.
 */
static uint32_t g_timer_task_stack[OS_TIMER_TASK_STACK_SIZE];

/**
 * @brief Static backing memory array reserved for g_timer_queue slots.
 *        Uses OS_QUEUE_BUFFER_SIZE to ensure sufficient capacity for payload plus metadata.
 */
static uint8_t g_timer_queue_buf[OS_QUEUE_BUFFER_SIZE(sizeof(os_timer_event_t), OS_TIMER_QUEUE_DEPTH)];

/**
 * @brief Initializes a software timer control block.
 *
 * @param[out] timer  Pointer to the timer structure to initialize.
 * @param[in]  type   Timer operation mode (OS_TIMER_ONESHOT or OS_TIMER_PERIODIC).
 * @param[in]  cb     Callback routine triggered on expiration.
 * @param[in]  arg    User argument passed into callback routine.
 */
void os_timer_create(os_timer_t *timer,
                     os_timer_type_t type,
                     os_timer_cb_t cb,
                     void *arg)
{
    if (timer == NULL || cb == NULL)
    {
        return;
    }

    timer->type            = type;
    timer->state           = OS_TIMER_STOPPED;
    timer->period_ticks    = 0U;
    timer->remaining_ticks = 0U;
    timer->cb              = cb;
    timer->arg             = arg;
    timer->next            = NULL;
}

/**
 * @brief Starts or restarts a software timer with a specified tick duration.
 *
 * @param[in,out] timer         Pointer to the initialized timer.
 * @param[in]     period_ticks  Duration in system tick units (must be > 0).
 */
void os_timer_start(os_timer_t *timer, uint32_t period_ticks)
{
    if (timer == NULL || timer->cb == NULL || period_ticks == 0U)
    {
        return;
    }

    uint32_t primask = os_enter_critical();

    /* Reload reload value and countdown */
    timer->period_ticks    = period_ticks;
    timer->remaining_ticks = period_ticks;

    /* If not running, append to the active timer list */
    if (timer->state != OS_TIMER_RUNNING)
    {
        timer->state = OS_TIMER_RUNNING;
        timer->next  = NULL;

        if (g_active_timer_list.head == NULL)
        {
            g_active_timer_list.head = timer;
            g_active_timer_list.tail = timer;
        }
        else
        {
            g_active_timer_list.tail->next = timer;
            g_active_timer_list.tail       = timer;
        }
    }

    os_exit_critical(primask);
}

/**
 * @brief Stops an active software timer and removes it from the tick list.
 *
 * @param[in,out] timer  Pointer to the timer to stop.
 */
void os_timer_stop(os_timer_t *timer)
{
    if (timer == NULL || timer->state != OS_TIMER_RUNNING)
    {
        return;
    }

    uint32_t primask = os_enter_critical();

    os_timer_t *curr = g_active_timer_list.head;
    os_timer_t *prev = NULL;

    while (curr != NULL)
    {
        if (curr == timer)
        {
            if (prev == NULL)
            {
                g_active_timer_list.head = curr->next;
                if (g_active_timer_list.head == NULL)
                {
                    g_active_timer_list.tail = NULL;
                }
            }
            else
            {
                prev->next = curr->next;
            }

            if (curr == g_active_timer_list.tail)
            {
                g_active_timer_list.tail = prev;
            }

            curr->next  = NULL;
            curr->state = OS_TIMER_STOPPED;
            break;
        }

        prev = curr;
        curr = curr->next;
    }

    os_exit_critical(primask);
}

/**
 * @brief Resets and re-arms a software timer countdown.
 *
 * @param[in,out] timer         Pointer to the timer.
 * @param[in]     period_ticks  New duration in tick units.
 */
void os_timer_reset(os_timer_t *timer, uint32_t period_ticks)
{
    os_timer_start(timer, period_ticks);
}

/**
 * @brief Checks if a timer is currently active.
 *
 * @param[in] timer  Pointer to the timer instance.
 * @return true if running, false if stopped or invalid.
 */
bool os_timer_is_running(const os_timer_t *timer)
{
    if (timer == NULL)
    {
        return false;
    }

    return (timer->state == OS_TIMER_RUNNING);
}

/**
 * @brief Software timer tick handler. Must be called periodically from the RTOS tick interrupt.
 */
void os_timer_tick(void)
{
    uint32_t primask = os_enter_critical();

    os_timer_t *curr = g_active_timer_list.head;
    os_timer_t *prev = NULL;

    while (curr != NULL)
    {
        os_timer_t *next_node = curr->next;

        if (curr->remaining_ticks > 0U)
        {
            curr->remaining_ticks--;
        }

        if (curr->remaining_ticks == 0U)
        {
            if (curr->cb != NULL)
            {
                /* Defer callback execution to the Timer Daemon Task via queue.
                 * Offloading from SysTick ISR context keeps ISR latency minimal and
                 * allows callbacks to safely invoke blocking RTOS APIs. */
                os_timer_event_t evt = {
                    .cb  = curr->cb,
                    .arg = curr->arg
                };
                (void)os_queue_send_from_isr(&g_timer_queue, &evt, sizeof(evt));
            }

            if (curr->type == OS_TIMER_PERIODIC)
            {
                /* Auto-reload periodic timer */
                curr->remaining_ticks = curr->period_ticks;
                prev                  = curr;
            }
            else
            {
                /* Oneshot expiration: Unlink node */
                curr->state = OS_TIMER_STOPPED;

                if (prev == NULL)
                {
                    g_active_timer_list.head = next_node;
                    if (g_active_timer_list.head == NULL)
                    {
                        g_active_timer_list.tail = NULL;
                    }
                }
                else
                {
                    prev->next = next_node;
                }

                if (curr == g_active_timer_list.tail)
                {
                    g_active_timer_list.tail = prev;
                }

                curr->next = NULL;
            }
        }
        else
        {
            prev = curr;
        }

        curr = next_node;
    }

    os_exit_critical(primask);
}

/**
 * @brief Private Daemon Task routine executing deferred callbacks.
 */
static void os_timer_task(void)
{
    os_timer_event_t evt;
    uint8_t len = 0U;

    for (;;)
    {
        if (os_queue_receive_timeout(&g_timer_queue, &evt, &len, OS_WAIT_FOREVER) == 0)
        {
            if (evt.cb != NULL)
            {
                /* Executed in Task Context with interrupts fully enabled */
                evt.cb(evt.arg);
            }
        }
    }
}

/**
 * @brief Initializes timer subsystem, message queue, and spawns the Timer Daemon Task.
 * @param priority Task priority to assign to the Timer Daemon.
 */
void os_timer_subsystem_init(uint8_t priority)
{
    int ret = os_queue_init(&g_timer_queue,
                            g_timer_queue_buf,
                            sizeof(g_timer_queue_buf),
                            sizeof(os_timer_event_t),
                            OS_TIMER_QUEUE_DEPTH);

    dev_assert(ret == 0);

    int task_id = os_task_create(os_timer_task,
                                 "timer_daemon",
                                 g_timer_task_stack,
                                 OS_TIMER_TASK_STACK_SIZE,
                                 priority);

    dev_assert(task_id > 0);
}
