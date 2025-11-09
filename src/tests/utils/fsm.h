#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "err.h"
#include "utils.h"

/* Types */
typedef struct fsm_s fsm_t;
typedef uint32_t fsm_state_id_t;

/* Invalid state ID */
#define FSM_STATE_ID_INVALID ((fsm_state_id_t)0)

/* Any state ID (wildcard) */
#define FSM_STATE_ID_ANY ((fsm_state_id_t)(0xFFFFFFFFu))

/**
 * @brief Action function signature for state transitions.
 * 
 * Called while the FSM is still in current_state. The transition to
 * next_state is committed only after this function returns.
 * 
 * @param fsm - The state machine instance
 * @param current_state - State before transition
 * @param event - Event type that triggered this transition
 * @param status - Error status associated with the event
 * @param next_state - State after transition (from table)
 * @param user_data - Application context pointer
 */
typedef void (*fsm_action_fn)(fsm_t *fsm, fsm_state_id_t current_state, event_type_t event, util_err_t status, fsm_state_id_t next_state, void *user_data);

/**
 * @brief Transition row struct: on event -> optional action, next_state
 * 
 * Defines a single state transition in the FSM.  Make static arrays
 * of these to define the FSM's behavior.  Rows must have a unique combination
 * of current_state and event.
 * 
 * Use FSM_STATE_ID_ANY as current_state to match any state.  This can be used
 * for defaults or error handling.
 * 
 * Fields:
 *  current_state - State in which this transition is valid
 *  event - Event that triggers this transition
 *  action - Action function to execute on transition (may be NULL)
 *  next_state - State to transition to (may equal current_state to stay)
 */
typedef struct {
    fsm_state_id_t current_state;
    event_type_t   event;
    fsm_action_fn  action;        /* may be NULL */
    fsm_state_id_t next_state;   /* may equal current to stay */
} fsm_transition_t;

/**
 * @brief Create a new finite state machine (FSM).
 *
 * @param transition_table Array of state transition definitions.
 * @param transition_count Number of transitions in the table.
 * @param initial_state Initial state of the FSM.
 * @param pending_queue_size Maximum number of events that can be queued (typically 4-8).
 * @param fsm_ctx User data pointer associated with the FSM (may be NULL).
 * @return fsm_t* Pointer to the created FSM instance or NULL on failure.
 */
fsm_t* fsm_create(fsm_transition_t *transition_table, size_t transition_count, fsm_state_id_t initial_state, size_t pending_queue_size, void *fsm_ctx);

/**
 * @brief Destroy a finite state machine.
 *
 * @param fsm Pointer to the FSM instance to destroy.
 */
void fsm_destroy(fsm_t *fsm);

/**
 * @brief Get the context data pointer associated with the FSM.
 *
 * @param fsm Pointer to the FSM instance.
 * @return void* Context data pointer, or NULL if not set.
 */
void *fsm_get_ctx(fsm_t *fsm);

/**
 * @brief Get the current state of the FSM.
 *
 * @param fsm Pointer to the FSM instance.
 * @return fsm_state_id_t Current state ID.
 */
fsm_state_id_t fsm_get_state(const fsm_t *fsm);


/**
 * @brief Queue an event for processing.
 *
 * Adds an event to the FSM's event queue without processing it.
 * The event will be processed when fsm_process_events() is called.
 * Returns UTIL_EBUSY if the event queue is full.
 *
 * @param fsm Pointer to the FSM instance.
 * @param event Event type to queue.
 * @param status Error status associated with the event.
 * @param event_ctx Event-specific context pointer.
 * @return util_err_t UTIL_OK on success, UTIL_EBUSY if queue full, error code otherwise.
 */
util_err_t fsm_queue_event(fsm_t *fsm, event_type_t event, util_err_t status, void *event_ctx);

/**
 * @brief Process all queued events until queue is empty.
 *
 * Drains the event queue in FIFO order, executing state transitions and action
 * functions. Action functions may call fsm_queue_event() to queue additional
 * events, which will be processed in the same call.
 *
 * @param fsm Pointer to the FSM instance.
 * @return util_err_t UTIL_OK on success, error code otherwise.
 */
util_err_t fsm_process_events(fsm_t *fsm);

/**
 * @brief Dispatch an event to the FSM (deprecated).
 *
 * This function is deprecated. Use fsm_queue_event() + fsm_process_events() instead.
 * For backward compatibility, this internally queues and processes events.
 *
 * @param fsm Pointer to the FSM instance.
 * @param event Event type to dispatch.
 * @param status Error status associated with the event.
 * @param event_ctx Event-specific context pointer.
 * @return util_err_t UTIL_OK on success, error code otherwise.
 */
util_err_t fsm_dispatch_event(fsm_t *fsm, event_type_t event, util_err_t status, void *event_ctx);


#ifdef __cplusplus
}
#endif
