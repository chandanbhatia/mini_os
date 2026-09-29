#ifndef OS_TIMER_H
#define OS_TIMER_H

#include "os.h"

/**
 * @brief Software timer operating mode.
 */
typedef enum
{
    OS_TIMER_ONESHOT,
    OS_TIMER_PERIODIC
} os_timer_type_t;

/**
 * @brief Software timer run state.
 */
typedef enum
{
    OS_TIMER_STOPPED,
    OS_TIMER_RUNNING
} os_timer_state_t;

/**
 * @brief Callback function prototype for timer expiration events.
 * @param[in] arg  User context parameter provided during creation/start.
 */
typedef void (*os_timer_cb_t)(void *arg);

/**
 * @brief Software timer control block (TCB).
 */
typedef struct os_timer
{
    os_timer_type_t type;     /* ONESHOT or PERIODIC */
    os_timer_state_t state;   /* STOPPED or RUNNING */
    uint32_t period_ticks;    /* Configured timer duration */
    os_timer_cb_t cb;         /* Expiration callback function */
    uint32_t remaining_ticks; /* Current countdown value */
    void *arg;                /* Callback context argument */
    struct os_timer *next;    /* Active timer list linkage */
} os_timer_t;

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
                     void *arg);

/**
 * @brief Starts or restarts a software timer with a specified tick duration.
 *
 * @param[in,out] timer         Pointer to the initialized timer.
 * @param[in]     period_ticks  Duration in system tick units (must be > 0).
 */
void os_timer_start(os_timer_t *timer, uint32_t period_ticks);

/**
 * @brief Stops an active software timer and removes it from the tick list.
 *
 * @param[in,out] timer  Pointer to the timer to stop.
 */
void os_timer_stop(os_timer_t *timer);

/**
 * @brief Resets and re-arms a software timer countdown.
 *
 * @param[in,out] timer         Pointer to the timer.
 * @param[in]     period_ticks  New duration in tick units.
 */
void os_timer_reset(os_timer_t *timer, uint32_t period_ticks);

/**
 * @brief Checks if a timer is currently active.
 *
 * @param[in] timer  Pointer to the timer instance.
 * @return true if running, false if stopped or invalid.
 */
bool os_timer_is_running(const os_timer_t *timer);

/**
 * @brief Software timer tick handler. Must be called periodically from the RTOS tick interrupt.
 */
void os_timer_tick(void);

#endif /* OS_TIMER_H */
