#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief Event type identifier
 * 
 * Individual modules define their own event type constants.
 * Events are passed with a separate context pointer rather than
 * being wrapped in a struct.
 */
typedef uint32_t event_type_t;

#define EVENT_TYPE_MAX 64  /* Maximum number of event types supported */


/**
 * @brief Get current time in milliseconds.
 *
 * Used for deferred event timing. Uses platform-specific functions.
 */
extern uint64_t util_get_time_ms(void);

/**
 * @brief Get current time in microseconds.
 *
 * Used for performance analysis and latency measurements.
 */
extern int64_t util_time_us(void);

/**
 * @brief Set the interrupt handler function
 * 
 * @param handler Function pointer to the handler called when ^C or signals occur.
 * @return int 
 */
extern int util_set_interrupt_handler(void (*handler)(void));


/**
 * @brief  Sleep for the specified number of milliseconds.
 * 
 * @param ms The number of milliseconds to sleep.
 */
extern void util_sleep_ms(int ms);

#ifdef __cplusplus
}
#endif

