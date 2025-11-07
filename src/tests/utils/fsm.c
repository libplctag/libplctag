#include "fsm.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    event_type_t event;
    util_err_t status;
    void *event_ctx;
} fsm_queued_event_t;

struct fsm_s {
    const fsm_transition_t *table;
    size_t table_size;
    fsm_state_id_t current_state;
    void *fsm_ctx;
    
    /* Pending event ring_buf */
    size_t ring_capacity;
    size_t ring_head;
    size_t ring_tail;
    fsm_queued_event_t ring_buf[];
};



static const fsm_transition_t* fsm_lookup_transition(const fsm_t *fsm, fsm_state_id_t state, event_type_t event);
static const size_t event_queue_size(const fsm_t *fsm);
static bool enqueue_event(fsm_t *fsm, event_type_t event, util_err_t status, void *event_ctx);
static bool dequeue_event(fsm_t *fsm, event_type_t *out_event, util_err_t *out_status, void **out_ctx);


fsm_t* fsm_create(const fsm_transition_t *transition_table, size_t transition_count, fsm_state_id_t initial_state, 
                  size_t pending_queue_size, void *fsm_ctx) {
    if (!transition_table || transition_count == 0 || pending_queue_size == 0) {
        return NULL;
    }
    
    /* allocate FSM with event ring buffer */
    fsm_t *fsm = calloc(1, sizeof(fsm_t) + (sizeof(fsm_queued_event_t) * pending_queue_size));
    if (!fsm) {
        return NULL;
    }
        
    fsm->table = transition_table;
    fsm->table_size = transition_count;
    fsm->current_state = initial_state;
    fsm->fsm_ctx = fsm_ctx;
    fsm->ring_capacity = pending_queue_size;
    fsm->ring_head = 0;
    fsm->ring_tail = 0;
    
    return fsm;
}

void fsm_destroy(fsm_t *fsm) {
    if (fsm) {
        free(fsm);
    }
}

void *fsm_get_ctx(fsm_t *fsm) {
    return fsm ? fsm->fsm_ctx : NULL;
}

fsm_state_id_t fsm_get_state(const fsm_t *fsm) {
    return fsm ? fsm->current_state : FSM_STATE_ID_INVALID;
}


/**
 * @brief Queue an event for later processing.
 *
 * Adds the event to the ring buffer queue without processing it.
 * Enqueue may fail if the queue is full.
 *
 * @param fsm FSM instance
 * @param event Event type
 * @param status Event status
 * @param event_ctx Event context
 * @return util_err_t UTIL_OK on success, UTIL_EBUSY if queue full, UTIL_ENULL if fsm is NULL
 */
util_err_t fsm_queue_event(fsm_t *fsm, event_type_t event, util_err_t status, void *event_ctx) {
    if (!fsm) {
        return UTIL_ENULL;
    }

    log_info("FSM: Queueing event %u with status %s.", event, util_err_str(status));

    /* enqueue the event */
    if (!enqueue_event(fsm, event, status, event_ctx)) {
        return UTIL_EBUSY;  /* queue full */
    }

    return UTIL_OK;
}

/**
 * @brief Process all queued events until queue is empty.
 *
 * Drains the event queue in FIFO order, executing transitions and action functions.
 * Action functions may call fsm_queue_event() to queue additional events, which will
 * be processed in the same loop.
 *
 * @param fsm FSM instance
 * @return util_err_t UTIL_OK on success, error code on failure
 */
util_err_t fsm_process_events(fsm_t *fsm) {
    event_type_t current_event;
    util_err_t current_status;
    void *current_ctx;

    if (!fsm) {
        return UTIL_ENULL;
    }

    log_info("FSM: Processing events.");

    /* dequeue all the pending events in FIFO order. */
    while(dequeue_event(fsm, &current_event, &current_status, &current_ctx)) {
        const fsm_transition_t *trans = fsm_lookup_transition(fsm, fsm->current_state, current_event);

        if (!trans) {
            /* No matching transition; ignore event */
            log_warn("FSM: No transition for state %u, event %u!", fsm->current_state, current_event);
            return UTIL_ENOTFOUND;
        }

        /* Execute action while still in current_state */
        if (trans->action) {
            log_detail("FSM: State %u --(%u)--> State %u", fsm->current_state, current_event, trans->next_state);

            trans->action(fsm, fsm->current_state, current_event, current_status, trans->next_state, fsm->fsm_ctx);
        } else {
            log_detail("FSM: State %u --(%u)--> State %u (no action)", fsm->current_state, current_event, trans->next_state);
        }

        /* Commit state transition */
        fsm->current_state = trans->next_state;
    }

    log_info("FSM: All events processed.");

    return UTIL_OK;
}

/**
 * @brief Dispatch an event to the FSM (deprecated).
 *
 * Backward-compatible wrapper that queues and processes events.
 */
util_err_t fsm_dispatch_event(fsm_t *fsm, event_type_t event, util_err_t status, void *event_ctx) {
    util_err_t err = fsm_queue_event(fsm, event, status, event_ctx);
    if (err != UTIL_OK) {
        return err;
    }
    return fsm_process_events(fsm);
}


/* helper functions */

static const size_t event_queue_size(const fsm_t *fsm) {
    if (!fsm) {
        return 0;
    }

    return ((fsm->ring_head + fsm->ring_capacity) - fsm->ring_tail) % fsm->ring_capacity;
}


static const fsm_transition_t* fsm_lookup_transition(const fsm_t *fsm, fsm_state_id_t state, event_type_t event) {
    /* Try exact match first */
    for (size_t i = 0; i < fsm->table_size; i++) {
        if (fsm->table[i].current_state == state && fsm->table[i].event == event) {
            return &fsm->table[i];
        }
    }
    
    /* Try wildcard state match */
    for (size_t i = 0; i < fsm->table_size; i++) {
        if (fsm->table[i].current_state == FSM_STATE_ID_ANY && fsm->table[i].event == event) {
            return &fsm->table[i];
        }
    }
    
    return NULL;
}


/**
 * @brief Enqueue an event for later processing.
 * 
 * Push into the ring buffer at the head index.
 * 
 * @param fsm FSM instance
 * @param event Event type
 * @param status event status
 * @param event_ctx event context
 * @return True if enqueued successfully, false if the queue is full.
 */
static bool enqueue_event(fsm_t *fsm, event_type_t event, util_err_t status, void *event_ctx) {
    size_t next_head = (fsm->ring_head + 1) % fsm->ring_capacity;
    if (next_head == fsm->ring_tail) {
        /* Ring buffer full */
        return false;
    }
    
    fsm->ring_buf[fsm->ring_head].event = event;
    fsm->ring_buf[fsm->ring_head].status = status;
    fsm->ring_buf[fsm->ring_head].event_ctx = event_ctx;
    
    fsm->ring_head = next_head;
    
    return true;
}


/**
 * @brief Dequeue an event from the ring buffer 
 * 
 * Pop from the ring buffer at the tail index.
 * 
 * @param fsm FSM instance
 * @param out_event out event type
 * @param out_status out event status
 * @param out_ctx out event context
 * @return True if dequeued successfully, false if the queue is empty.
 */
static bool dequeue_event(fsm_t *fsm, event_type_t *out_event, util_err_t *out_status, void **out_ctx) {
    if (fsm->ring_tail == fsm->ring_head) {
        /* Ring buffer empty */
        return false;
    }
    
    *out_event = fsm->ring_buf[fsm->ring_tail].event;
    *out_status = fsm->ring_buf[fsm->ring_tail].status;
    *out_ctx = fsm->ring_buf[fsm->ring_tail].event_ctx;
    
    fsm->ring_tail = (fsm->ring_tail + 1) % fsm->ring_capacity;
    
    return true;
}

