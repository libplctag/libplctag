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



#ifdef __cplusplus
}
#endif

