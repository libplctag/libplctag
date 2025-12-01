#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "bitarray.h"
#include "err.h"
#include "socket.h"
#include "utils.h"

/* Forward */
typedef struct reactor_s reactor_t;

/**
 * @brief Reactor event constants.
 *
 * These are event_type_t values that the reactor uses to signal socket state changes.
 * Applications may define additional event types for their own use.
 */
typedef enum {
    REACTOR_EVENT_ERROR,
    REACTOR_EVENT_CAN_ACCEPT,
    REACTOR_EVENT_CAN_READ,
    REACTOR_EVENT_CAN_WRITE,
    REACTOR_EVENT_CLOSED,
    REACTOR_EVENT_CONNECTED,
    REACTOR_EVENT_WRITTEN,
    REACTOR_EVENT_TICK,
    REACTOR_EVENT_SHUTDOWN,

    REACTOR_EVENT_MAX
} reactor_event_type_t;


/**
 * @brief A reactor socket event module.
 *
 * Manages multiple sockets and dispatches events to registered callbacks per socket.
 * Each callback receives a SINGLE event at a time, ordered by priority.
 *
 * ONE-SHOT EVENT SEMANTICS:
 *
 * All events are ONE-SHOT: the reactor disables each event BEFORE invoking the callback.
 * Applications must re-enable events explicitly when ready for the next occurrence:
 *
 *   reactor_set_event_enable_mask(r, sock, REACTOR_EVENT_CAN_READ, true);
 *
 * This prevents race conditions where:
 *   - Callback is parsing/validating data when the next read edge arrives
 *   - Callback is composing headers when the next write edge arrives
 *
 * RE-ARMING PATTERN:
 *   1. Event fires → reactor disables it → callback invoked
 *   2. Callback processes the event (e.g., reads until EAGAIN, writes until EAGAIN)
 *   3. Callback calls reactor_set_event_enable_mask(r, sock, event, true) when ready
 *   4. Next state transition triggers the event again
 *
 * EXCEPTION - reactor_raise_event():
 * When reactor_raise_event() is called (immediate or deferred), the reactor
 * automatically re-enables the event atomically. This guarantees the edge is
 * preserved even if the event was previously disabled.
 *
 * EDGE-TRIGGERED EVENT SEMANTICS:
 *
 * Events fire when a socket transitions from one state to another (e.g., becomes readable,
 * writable, or has a new connection). After re-enabling, the event will fire again on the
 * next state transition.
 *
 * APPLICATION PATTERN:
 *   - Reading: Read in a loop until UTIL_EAGAIN, then re-enable CAN_READ
 *   - Writing: Write in a loop until UTIL_EAGAIN, then re-enable CAN_WRITE
 *   - Accepting: Accept in a loop until UTIL_EAGAIN, then re-enable CAN_ACCEPT
 *
 * EVENT PRIORITY (internal to reactor, affects delivery order):
 *
 * FATAL EVENTS (highest priority):
 *   REACTOR_EVENT_ERROR  - Socket error occurred (status param describes error)
 *   REACTOR_EVENT_CLOSED - Peer closed connection gracefully (POLLHUP)
 *   - Delivered immediately in their own callback
 *   - Mask all lower-priority events (ACCEPT, CONNECTED, CAN_READ, CAN_WRITE, etc.)
 *   - Pending lower-priority events are discarded; socket is unusable
 *   - Rationale: Socket error or closed; other event states are irrelevant
 *   - Example: If both CAN_READ and CLOSED events are pending, only CLOSED is delivered
 *
 * STATE CHANGE EVENTS (middle priority):
 *   REACTOR_EVENT_CAN_ACCEPT - Listening socket has incoming connection
 *   REACTOR_EVENT_CONNECTED  - Outgoing connection completed successfully
 *   - Delivered in their own callback, one at a time
 *   - Queued behind FATAL events; may mask pending DATA events
 *   - Rationale: State transitions must complete before data operations begin
 *   - Example: If CONNECTED and CAN_READ both fire, CONNECTED is delivered first,
 *     then CAN_READ is delivered in a subsequent callback
 *   - Allows FSM to establish new state before processing incoming data
 *
 * DATA OPERATION EVENTS (lower priority):
 *   REACTOR_EVENT_CAN_READ  - Socket has data available to read
 *   REACTOR_EVENT_CAN_WRITE - Socket is ready to accept more data
 *   REACTOR_EVENT_WRITTEN   - Previous write operation completed
 *   - Delivered one at a time
 *   - Queued after all FATAL and STATE CHANGE events have been processed
 *   - Rationale: Data operations can be performed after state is established
 *
 * PERIODIC EVENTS (lowest priority):
 *   REACTOR_EVENT_TICK     - Periodic timer event (fired per socket when enabled)
 *   REACTOR_EVENT_SHUTDOWN - Reactor is shutting down (clean up resources)
 *   - TICK: Fired for each socket when poll timeout expires (for timeouts/heartbeats)
 *   - SHUTDOWN: Fired for all sockets when reactor is stopping
 *
 * The reactor delivers events as a SINGLE event per callback, respecting priority.
 *
 * FSM INTEGRATION:
 *   This design naturally supports FSM-based protocols:
 *   - Each callback receives exactly one event
 *   - No need for FSM to parse bitmasks or check multiple conditions
 *   - FSM action can directly dispatch the event to the state machine
 *   - One-shot semantics prevent data races during parsing/validation
 *   - No race conditions when multiple threads call reactor operations
 */


/**
 * @brief Callback invoked for each socket event.
 *
 * The callback is invoked with a single event at a time, according to the priority
 * rules documented in the reactor module description (see above).
 *
 * ONE-SHOT SEMANTICS:
 * The event has already been disabled by the reactor before this callback is invoked.
 * To receive the event again, call reactor_set_event_enable_mask(r, sock, event, true)
 * after you are ready for the next occurrence.
 *
 * IMPORTANT: Multiple callbacks may be invoked per reactor cycle if different
 * priority tiers have pending events. For example:
 *   1. First callback with ERROR (fatal event)
 *   2. Second callback with CAN_READ (data event, queued behind ERROR)
 *
 * @param r - Pointer to the reactor instance
 * @param sock - The socket on which the event occurred
 * @param event - The event that occurred (single event, not a bitmask)
 * @param status - UTIL_OK or error code/additional info associated with the event
 * @param socket_ctx_data - Context associated with the socket
 * @return void
 */
typedef void (*reactor_socket_cb_t)(reactor_t *r, socket_t sock, event_type_t event, util_err_t status, void *socket_ctx_data);

/**
 * @brief Create a new reactor instance.
 *
 * @param max_sockets - Maximum number of sockets to handle
 * @return reactor_t* - Pointer to the new reactor instance or null on failure.
 */
reactor_t* reactor_create(size_t max_sockets);

/**
 * @brief Destroy a reactor instance.
 * 
 * This will join the worker thread and free all associated resources.
 * 
 * @param r - Pointer to the reactor instance to destroy
 */
void reactor_destroy(reactor_t *r);

/**
 * @brief Register a socket with the reactor.
 *
 * Registers a socket to be monitored by the reactor. The socket is automatically
 * set to non-blocking mode (required for event-driven I/O). All events are enabled
 * by default; use reactor_set_event_enable_mask() to selectively disable events.
 *
 * For TCP client sockets: the reactor tracks connection state and will raise
 * CONNECTED event when the socket becomes writable (on successful connect).
 * For TCP server sockets: CONNECTED is raised immediately for accepted connections.
 * For UDP sockets: no CONNECTED tracking (UDP is connectionless).
 *
 * Thread safe.
 *
 * @param r - Pointer to the reactor instance
 * @param sock - The socket to register (will be set to non-blocking)
 * @param name - Human-readable name for logging (e.g., peer address). May be NULL.
 * @param cb - Callback to invoke for socket events
 * @param ctx - User context associated with the socket
 * @param initial_events - Bitarray of initial enabled events (or NULL for all enabled)
 * @return util_err_t - Error code indicating success or failure
 */
util_err_t reactor_add_socket(reactor_t *r, socket_t sock, const char *name, reactor_socket_cb_t cb, void *ctx, const bitarray_t *initial_events);

/**
 * @brief Remove a socket from the reactor.
 * 
 * Unregisters a socket from the reactor, stopping monitoring for events.
 * 
 * Thread safe.
 * 
 * @param r - Pointer to the reactor instance
 * @param sock - The socket to remove
 * @return util_err_t - Error code indicating success or failure
 */
util_err_t reactor_remove_socket(reactor_t *r, socket_t sock);

/**
 * @brief Replace the entire event mask for a registered socket.
 *
 * Atomically replaces the event mask with a new set of enabled events.
 * This is typically used when an FSM transitions to a new state with
 * a different set of accepted events.
 *
 * Thread safe - takes the reactor mutex internally.
 *
 * The reactor will:
 * 1. Replace the event mask
 * 2. Rebuild the poll() file descriptor masks
 * 3. Wake up the reactor to restart poll() with new masks
 *
 * USAGE:
 * Typically called from an FSM state change callback:
 *   reactor_set_event_mask(r, sock, new_event_mask)
 *
 * This ensures the reactor only monitors for events the current FSM state
 * is ready to handle, preventing spurious event deliveries.
 *
 * @param r - Pointer to the reactor instance
 * @param sock - The socket to update
 * @param event_mask - New event mask (bitarray with bits set for enabled events)
 * @return util_err_t - UTIL_OK on success, error code on failure
 */
util_err_t reactor_set_event_mask(reactor_t *r, socket_t sock, bitarray_t event_mask);


/**
 * @brief Run the reactor event loop.
 *
 * Runs the reactor in a continuous loop, processing socket activity and raising events
 * to callbacks. The function blocks in poll() for the specified timeout, allowing
 * periodic TICK events to be raised for connected sockets.
 *
 * The loop continues until reactor_stop() is called from a callback or another thread.
 * Use reactor_wake() to interrupt the reactor from other threads for shutdown or events.
 *
 * Thread-safe: Call from the main application thread or a dedicated reactor thread.
 *
 * @param r - Pointer to the reactor instance
 * @param poll_timeout_ms - Timeout in milliseconds for poll(); TICK events fire after this interval
 * @return util_err_t - UTIL_OK on normal stop, error code on failure
 */
util_err_t reactor_run(reactor_t *r, uint32_t poll_timeout_ms);

/**
 * @brief Stop the reactor event loop.
 *
 * Signals the reactor to stop. The next reactor_run() cycle will exit after raising
 * REACTOR_EVENT_SHUTDOWN to all sockets.
 *
 * Thread safe. Can be called from a callback or another thread.
 *
 * @param r - Pointer to the reactor instance
 * @return util_err_t - UTIL_OK on success, error code on failure
 */
util_err_t reactor_stop(reactor_t *r);

/**
 * @brief Wake the reactor from poll().
 *
 * Interrupts the reactor's poll() call without waiting for socket activity.
 * Useful for signaling shutdown from signal handlers or other threads.
 *
 * Thread safe. Can be called from any thread at any time.
 *
 * @param r - Pointer to the reactor instance
 * @return util_err_t - UTIL_OK on success, error code on failure
 */
util_err_t reactor_wake(reactor_t *r);

/**
 * @brief Get reactor performance statistics.
 *
 * Returns accumulated timing statistics for the reactor.
 * Any output parameter may be NULL to skip that statistic.
 *
 * @param poll_calls Total number of poll() calls
 * @param poll_us Total time in poll() (microseconds)
 * @param translate_us Total time translating poll events (microseconds)
 * @param deliver_us Total time in deliver_pending_events (microseconds)
 * @param events Total events delivered to callbacks
 * @param callback_us Total time in callbacks (microseconds)
 */
void reactor_get_stats(int64_t *poll_calls, int64_t *poll_us, int64_t *translate_us,
                       int64_t *deliver_us, int64_t *events, int64_t *callback_us);

/**
 * @brief Reset reactor performance statistics to zero.
 */
void reactor_reset_stats(void);

/**
 * @brief Get detailed timing breakdown for reactor_set_event_mask().
 *
 * @param calls Total number of calls
 * @param lock_us Time acquiring mutex lock
 * @param search_us Time searching for socket
 * @param rebuild_us Time rebuilding poll events
 * @param log_us Time in log_detail calls
 * @param unlock_us Time releasing mutex lock
 * @param wake_us Time waking the reactor
 */
void reactor_get_set_mask_stats(int64_t *calls, int64_t *lock_us, int64_t *search_us,
                                 int64_t *rebuild_us, int64_t *log_us, int64_t *unlock_us,
                                 int64_t *wake_us);

#ifdef __cplusplus
}
#endif
