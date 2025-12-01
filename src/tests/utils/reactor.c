#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include "bitarray.h"
#include "err.h"
#include "reactor.h"
#include "utils.h"
#include "log.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* Global reactor statistics for performance analysis */
static struct {
    int64_t total_poll_calls;
    int64_t total_poll_time_us;
    int64_t total_translate_time_us;
    int64_t total_deliver_time_us;
    int64_t total_events_delivered;
    int64_t total_callback_time_us;
} g_reactor_stats = {0};

void reactor_get_stats(int64_t *poll_calls, int64_t *poll_us, int64_t *translate_us,
                       int64_t *deliver_us, int64_t *events, int64_t *callback_us) {
    if (poll_calls) *poll_calls = g_reactor_stats.total_poll_calls;
    if (poll_us) *poll_us = g_reactor_stats.total_poll_time_us;
    if (translate_us) *translate_us = g_reactor_stats.total_translate_time_us;
    if (deliver_us) *deliver_us = g_reactor_stats.total_deliver_time_us;
    if (events) *events = g_reactor_stats.total_events_delivered;
    if (callback_us) *callback_us = g_reactor_stats.total_callback_time_us;
}

void reactor_reset_stats(void) {
    memset(&g_reactor_stats, 0, sizeof(g_reactor_stats));
}

/* ================================================================
 * Internal Data Structures
 * ================================================================ */

/* Socket type */
typedef enum {
    SOCKET_TYPE_UNKNOWN,
    SOCKET_TYPE_STREAM,     /* TCP socket (client or accepted connection) */
    SOCKET_TYPE_LISTENER,   /* TCP listening socket */
    SOCKET_TYPE_DGRAM,      /* UDP */
} socket_type_t;

/* Per-socket state */
typedef struct {
    socket_type_t socket_type;                          /* STREAM (TCP) or DGRAM (UDP) */
    socket_t sock;                                      /* Socket descriptor (INVALID_SOCKET if unused) */
    char *name;                                         /* Human-readable name for logging */
    reactor_socket_cb_t callback;                       /* User callback function */
    void *context;                                      /* User context data */
    bitarray_t enabled;                                 /* Bitmask of enabled events */
    bitarray_t pending_events;                          /* Bitmask of active events waiting delivery */
    struct pollfd last_revents;                         /* Last poll revents for edge-triggered detection */
    util_err_t last_error;                              /* Last error status for ERROR event */
    bool connected;                                     /* For TCP sockets: true if connected or CONNECTED event fired */
    bool pending_removal;                               /* Socket marked for removal (deferred until safe) */
} socket_entry_t;

/* Reactor instance */
struct reactor_s {
    /* Configuration */
    size_t max_sockets;

    /* Socket registry and poll array */
    socket_entry_t *sockets;                    /* Array of socket entries (index 0 reserved for wake pipe) */
    struct pollfd *pollfds;                     /* Array for poll/WSAPoll */
    size_t active_socket_count;                 /* Number of registered sockets (includes wake pipe) */

    /* Wake pipe for interrupting poll */
    socket_t wake_pipe[2];                      /* [0] = read, [1] = write */

    /* Synchronization */
#ifdef _WIN32
    CRITICAL_SECTION lock;
    DWORD reactor_thread_id;                    /* Thread ID of reactor loop (0 if not running) */
#else
    pthread_mutex_t lock;
    pthread_t reactor_thread_id;                /* Thread ID of reactor loop (0 if not running) */
#endif

    /* Control flags */
    bool shutdown;              /* Signal reactor to stop */
    bool pollfds_dirty;         /* pollfds array needs rebuild before next poll() */
};



/* ================================================================
 * Helper Functions
 * ================================================================ */

/* Forward declaration */
static util_err_t create_wake_pipe(socket_t wake_pipe[2]);

/**
 * @brief Lock the reactor mutex.
 *
 * @param r - Pointer to the reactor instance
 * @return util_err_t - UTIL_OK on success, error code on failure
 */
static inline util_err_t reactor_lock(reactor_t *r) {
#ifdef _WIN32
    EnterCriticalSection(&r->lock);
    return UTIL_OK;
#else
    int rc = pthread_mutex_lock(&r->lock);
    if (rc != 0) {
        return util_err_from_errno(rc);
    }
    return UTIL_OK;
#endif
}

/**
 * @brief Unlock the reactor mutex.
 *
 * @param r - Pointer to the reactor instance
 * @return util_err_t - UTIL_OK on success, error code on failure
 */
static inline util_err_t reactor_unlock(reactor_t *r) {
#ifdef _WIN32
    LeaveCriticalSection(&r->lock);
    return UTIL_OK;
#else
    int rc = pthread_mutex_unlock(&r->lock);
    if (rc != 0) {
        return util_err_from_errno(rc);
    }
    return UTIL_OK;
#endif
}

/**
 * @brief Check if the current thread is the reactor thread.
 *
 * @param r - Pointer to the reactor instance
 * @return true if current thread is running reactor_run, false otherwise
 */
static inline bool is_reactor_thread(reactor_t *r) {
#ifdef _WIN32
    return r->reactor_thread_id != 0 && GetCurrentThreadId() == r->reactor_thread_id;
#else
    return r->reactor_thread_id != 0 && pthread_equal(pthread_self(), r->reactor_thread_id);
#endif
}

/**
 * @brief Determine socket type (STREAM=TCP or DGRAM=UDP).
 *
 * Returns SOCKET_TYPE_UNKNOWN on error.
 */
static socket_type_t get_socket_type(socket_t sock) {
#ifdef _WIN32
    int type = 0;
    int type_len = sizeof(type);
    if (getsockopt(sock, SOL_SOCKET, SO_TYPE, (char *)&type, &type_len) == SOCKET_ERROR) {
        return SOCKET_TYPE_UNKNOWN;
    }
#else
    int type = 0;
    socklen_t type_len = sizeof(type);
    if (getsockopt(sock, SOL_SOCKET, SO_TYPE, &type, &type_len) < 0) {
        return SOCKET_TYPE_UNKNOWN;
    }
#endif

    if (type == SOCK_STREAM) {
        return SOCKET_TYPE_STREAM;
    } else if (type == SOCK_DGRAM) {
        return SOCKET_TYPE_DGRAM;
    }
    return SOCKET_TYPE_UNKNOWN;
}


/**
 * @brief Check if socket has pending events that are also enabled.
 */
static inline bool socket_has_pending_events(socket_entry_t *entry) {
    bitarray_t intersection = bitarray_and(&entry->enabled, &entry->pending_events);
    return bitarray_has_any(&intersection);
}


/**
 * @brief Build poll events for a single socket entry.
 *
 * Helper function that sets up the pollfd entry based on the socket's
 * enabled events mask.
 *
 * @param entry Socket entry
 * @param pfd Poll fd entry to populate
 */
static void build_pollfd_for_entry(socket_entry_t *entry, struct pollfd *pfd) {
    /* Defensive: ensure socket is valid before building poll events */
    if (entry->sock == INVALID_SOCKET) {
        pfd->fd = INVALID_SOCKET;
        pfd->events = 0;
        pfd->revents = 0;
        return;
    }

    pfd->fd = entry->sock;
    pfd->events = 0;
    pfd->revents = 0;

    /* For TCP client sockets pending async connection, always watch for both POLLIN and POLLOUT
     * to detect connection completion (POLLOUT) or connection failure (POLLERR/POLLIN) */
    if (entry->socket_type == SOCKET_TYPE_STREAM && !entry->connected) {
        pfd->events = POLLIN | POLLOUT;
        return;
    }

    /* Build poll events from enabled bitarray */
    for (unsigned int i = 0; i < REACTOR_EVENT_MAX; i++) {
        if (bitarray_test(&entry->enabled, i)) {
            switch (i) {
                case REACTOR_EVENT_CAN_READ:
                case REACTOR_EVENT_CAN_ACCEPT:
                    pfd->events |= POLLIN;
                    break;
                case REACTOR_EVENT_CAN_WRITE:
                case REACTOR_EVENT_CONNECTED:
                    pfd->events |= POLLOUT;
                    break;
                default:
                    /* Other events don't require explicit poll events */
                    break;
            }
        }
    }
}

/**
 * @brief Rebuild the pollfds array if needed.
 *
 * This function checks the pollfds_dirty flag and if set:
 * 1. Compacts the arrays by removing any sockets marked for removal
 *    (pending_removal == true) or with INVALID_SOCKET
 * 2. Rebuilds the poll events for all remaining sockets
 *
 * If pollfds_dirty is false, this function does nothing.
 *
 * MUST be called from the reactor thread before poll().
 * This function acquires the reactor lock internally.
 *
 * @param r Reactor instance
 */
static void rebuild_pollfds(reactor_t *r) {
    util_err_t rc = reactor_lock(r);
    if (rc != UTIL_OK) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "rebuild_pollfds: Failed to acquire lock");
        return;
    }

    if (!r->pollfds_dirty) {
        reactor_unlock(r);
        return;
    }

    size_t write_idx = 1;  /* Start at 1, index 0 is wake pipe */

    pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "rebuild_pollfds: Rebuilding pollfds array (active_count=%zu)", r->active_socket_count);

    /* First pass: compact the sockets array by removing marked/invalid entries */
    for (size_t read_idx = 1; read_idx < r->active_socket_count; read_idx++) {
        socket_entry_t *entry = &r->sockets[read_idx];

        /* Skip sockets marked for removal or already invalid */
        if (entry->pending_removal || entry->sock == INVALID_SOCKET) {
            const char *name = entry->name ? entry->name : "(unnamed)";
            pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "rebuild_pollfds: Removing socket[%s] at index %zu", name, read_idx);
            
            /* Free the name if allocated */
            if (entry->name) {
                free(entry->name);
                entry->name = NULL;
            }
            continue;
        }

        /* If we need to move this entry to a lower index, do so */
        if (write_idx != read_idx) {
            r->sockets[write_idx] = r->sockets[read_idx];
            /* Clear the old slot */
            memset(&r->sockets[read_idx], 0, sizeof(socket_entry_t));
            r->sockets[read_idx].sock = INVALID_SOCKET;
        }

        write_idx++;
    }

    /* Update active count */
    size_t old_count = r->active_socket_count;
    r->active_socket_count = write_idx;

    /* Clear any remaining slots */
    for (size_t i = write_idx; i < old_count; i++) {
        memset(&r->sockets[i], 0, sizeof(socket_entry_t));
        r->sockets[i].sock = INVALID_SOCKET;
        r->pollfds[i].fd = INVALID_SOCKET;
        r->pollfds[i].events = 0;
        r->pollfds[i].revents = 0;
    }

    /* Second pass: rebuild pollfds for all active sockets */
    for (size_t i = 0; i < r->active_socket_count; i++) {
        build_pollfd_for_entry(&r->sockets[i], &r->pollfds[i]);
    }

    pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "rebuild_pollfds: Done (new active_count=%zu)", r->active_socket_count);

    r->pollfds_dirty = false;
    reactor_unlock(r);
}

/**
 * @brief Check if an async connect has completed.
 *
 * For sockets with TCP and not yet connected, checks if the connection
 * succeeded or failed by calling getsockopt(SOL_SOCKET, SO_ERROR).
 *
 * Returns true if the connect completed successfully.
 * Returns false if the socket is not TCP or already connected or if connect failed.
 */
static util_err_t check_connect_completion(socket_entry_t *entry) {
    if (entry->socket_type != SOCKET_TYPE_STREAM || entry->connected) {
        return UTIL_OK;  /* Not applicable */
    }

#ifdef _WIN32
    int optval = 0;
    int optlen = sizeof(optval);
    if (getsockopt((SOCKET)entry->sock, SOL_SOCKET, SO_ERROR, (char *)&optval, &optlen) == SOCKET_ERROR) {
        return UTIL_EINTERNAL;
    }
#else
    int optval = 0;
    socklen_t optlen = sizeof(optval);
    if (getsockopt(entry->sock, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0) {
        return UTIL_EINTERNAL;
    }
#endif

    if (optval == 0) {
        /* Connection succeeded */
        return UTIL_OK;
    } else {
        /* Connection failed - return the error */
        return util_err_from_errno(optval);
    }
}

/**
 * @brief Translate poll revents bits to reactor pending_events (edge-triggered).
 *
 * Called after poll() returns to convert platform-specific poll results
 * into reactor event bits. Only fires events when state transitions from 0→1.
 */
static void translate_pollevents(reactor_t *r, size_t index) {
    socket_entry_t *entry = &r->sockets[index];
    struct pollfd *pfd = &r->pollfds[index];

    if (pfd->revents == 0) {
        /* No activity, but check for state changes from previous poll */
        if ((entry->last_revents.revents & POLLERR) && !(pfd->revents & POLLERR)) {
            /* Error cleared - don't trigger on this */
        }
        entry->last_revents.revents = pfd->revents;
        return;
    }

    short current = pfd->revents;
    short previous = entry->last_revents.revents;
    entry->last_revents.revents = current;

    /* POLLNVAL - invalid socket descriptor (socket closed/invalid) */
    if (current & POLLNVAL) {
        /* Socket is invalid - this is a fatal error */
        entry->last_error = UTIL_EINTERNAL;
        bitarray_set(&entry->pending_events, REACTOR_EVENT_ERROR);
        return;  /* Don't process other events on invalid socket */
    }

    /* For TCP sockets pending connection, handle connection completion specially */
    if (entry->socket_type == SOCKET_TYPE_STREAM && !entry->connected) {
        /* Check for connection error first via POLLERR or SO_ERROR */
        bool connection_failed = false;
        util_err_t conn_err = UTIL_OK;

        if (current & POLLERR) {
            /* Explicit error flag - check SO_ERROR */
            conn_err = check_connect_completion(entry);
            if (conn_err != UTIL_OK) {
                connection_failed = true;
            }
        } else if ((current & POLLOUT) || (current & POLLIN)) {
            /* POLLOUT usually means connected, but check SO_ERROR to be sure.
             * On some platforms, POLLIN can also indicate connection failure. */
            conn_err = check_connect_completion(entry);
            if (conn_err != UTIL_OK) {
                connection_failed = true;
            } else if (current & POLLOUT) {
                /* Connection succeeded */
                entry->connected = true;
                bitarray_set(&entry->pending_events, REACTOR_EVENT_CONNECTED);
                
                /* Mark pollfds dirty so it gets rebuilt before next poll() */
                r->pollfds_dirty = true;
                
                /* Reset last_revents since we're transitioning to connected state.
                 * This prevents spurious edge-triggered events on first poll after connect. */
                entry->last_revents.revents = 0;
            }
        }

        if (connection_failed) {
            entry->last_error = conn_err;
            bitarray_set(&entry->pending_events, REACTOR_EVENT_ERROR);
            entry->connected = true;  /* Prevent further connect attempts */
        }

        /* Don't process other events until connected */
        return;
    }

    /* POLLERR - socket error (may be spurious on Windows) */
    if ((current & POLLERR) && !(previous & POLLERR)) {
        /* Verify this is a real error by checking SO_ERROR */
#ifdef _WIN32
        int optval = 0;
        int optlen = sizeof(optval);
        if (getsockopt((SOCKET)entry->sock, SOL_SOCKET, SO_ERROR, (char *)&optval, &optlen) != SOCKET_ERROR) {
            if (optval != 0) {
                /* Real socket error */
                entry->last_error = util_err_from_errno(optval);
                bitarray_set(&entry->pending_events, REACTOR_EVENT_ERROR);
                /* Clear the error after reading it */
                optval = 0;
                setsockopt((SOCKET)entry->sock, SOL_SOCKET, SO_ERROR, (char *)&optval, sizeof(optval));
            }
            /* else: spurious POLLERR, ignore */
        }
#else
        int optval = 0;
        socklen_t optlen = sizeof(optval);
        if (getsockopt(entry->sock, SOL_SOCKET, SO_ERROR, &optval, &optlen) == 0) {
            if (optval != 0) {
                /* Real socket error */
                entry->last_error = util_err_from_errno(optval);
                bitarray_set(&entry->pending_events, REACTOR_EVENT_ERROR);
            }
            /* else: spurious POLLERR, ignore */
        }
#endif
        /* Don't return early - process other events too */
    }

#ifdef POLLRDHUP
    /* POLLRDHUP - peer closed write side (Linux half-close detection, only for TCP client sockets) */
    if (entry->socket_type == SOCKET_TYPE_STREAM && (current & POLLRDHUP) && !(previous & POLLRDHUP)) {
        /* Peer sent FIN - read side is closed but write side may still be open */
        bitarray_set(&entry->pending_events, REACTOR_EVENT_CLOSED);
        /* Don't return early - there may be buffered data to read */
    }
#endif

    /* POLLHUP - peer closed (graceful close, only for TCP client sockets) */
    if (entry->socket_type == SOCKET_TYPE_STREAM && (current & POLLHUP) && !(previous & POLLHUP)) {
        bitarray_set(&entry->pending_events, REACTOR_EVENT_CLOSED);
        /* Don't return early - there may be buffered data to read (POLLIN) */
    }

#ifdef POLLPRI
    /* POLLPRI - urgent/out-of-band data available */
    if ((current & POLLPRI) && !(previous & POLLPRI)) {
        /* OOB data available - treat as error condition since we don't handle OOB */
        /* Most applications don't use OOB, so log and ignore rather than error */
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Socket %d has OOB data (POLLPRI), ignoring", (int)entry->sock);
    }
#endif

    /* POLLIN - readable or acceptable (edge-triggered) */
    if ((current & POLLIN) && !(previous & POLLIN)) {
        /* For listening sockets, POLLIN means a connection is ready to accept */
        if (bitarray_test(&entry->enabled, REACTOR_EVENT_CAN_ACCEPT)) {
            bitarray_set(&entry->pending_events, REACTOR_EVENT_CAN_ACCEPT);
        }
        /* For connected sockets, verify data is actually available (not spurious) */
        else if (bitarray_test(&entry->enabled, REACTOR_EVENT_CAN_READ)) {
            /* Use MSG_PEEK to verify data is available and distinguish
             * between readable data, peer disconnect, and spurious wakeup */
            if (entry->socket_type == SOCKET_TYPE_STREAM) {
                char peek_buf;
                int peek_result = (int)recv(entry->sock, &peek_buf, 1, MSG_PEEK);
                if (peek_result > 0) {
                    /* Data is available */
                    bitarray_set(&entry->pending_events, REACTOR_EVENT_CAN_READ);
                } else if (peek_result == 0) {
                    /* recv() returned 0 - peer closed connection gracefully */
                    bitarray_set(&entry->pending_events, REACTOR_EVENT_CLOSED);
                } else {
                    /* recv() returned -1, check error */
                    util_err_t recv_err = socket_get_err();
                    if (recv_err != UTIL_EAGAIN) {
                        /* Real error on socket */
                        entry->last_error = recv_err;
                        bitarray_set(&entry->pending_events, REACTOR_EVENT_ERROR);
                    }
                    /* EAGAIN means spurious wakeup - no data available, ignore */
                }
            } else {
                /* UDP sockets - trust POLLIN */
                bitarray_set(&entry->pending_events, REACTOR_EVENT_CAN_READ);
            }
        }
    }

    /* POLLOUT - writable (edge-triggered) */
    if ((current & POLLOUT) && !(previous & POLLOUT)) {
        /* Connection already handled above for unconnected sockets */
        if (bitarray_test(&entry->enabled, REACTOR_EVENT_CAN_WRITE)) {
            bitarray_set(&entry->pending_events, REACTOR_EVENT_CAN_WRITE);
        }
    }
}

/**
 * @brief Drain the wake pipe (read all available data).
 *
 * Called when the wake pipe becomes readable. Just drains it,
 * no callbacks are invoked.
 */
static void drain_wake_pipe(reactor_t *r) {
    uint8_t buf[64];
    int total_drained = 0;
    int received;

    /* Use recv() on both platforms since wake_pipe is now a socket on both */
    do {
        received = (int)recv(r->wake_pipe[0], (char *)buf, sizeof(buf), 0);
        if (received > 0) {
            total_drained += received;
        }
    } while (received > 0);

    /* Check for unexpected errors (anything other than would-block) */
    if (received < 0) {
        util_err_t err = socket_get_err();
        if (err != UTIL_EAGAIN) {
            pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Unexpected error draining wake pipe: %s", util_err_str(err));
        }
    }

    if (total_drained > 0) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Drained %d bytes from wake pipe", total_drained);
    }
}


/**
 * @brief Deliver a single event to a socket's callback.
 *
 * Calls the socket's callback with the given event and status.
 * Called only for events in pending_events that passed priority checks.
 * 
 * ONE-SHOT SEMANTICS: Clears the enabled flag for the event BEFORE
 * invoking the callback, so the callback must explicitly re-enable
 * the event if it wants to receive it again.
 */
static void deliver_event(reactor_t *r, size_t socket_index, event_type_t event, util_err_t status) {
    socket_entry_t *entry = &r->sockets[socket_index];

    /* Clear the enabled flag BEFORE calling callback (one-shot) */
    bitarray_clear(&entry->enabled, event);

    /* Mark pollfds dirty so it gets rebuilt before next poll() */
    r->pollfds_dirty = true;

    if (entry->callback != NULL) {
        /* Time the callback execution */
        int64_t cb_start = util_time_us();
        entry->callback(r, entry->sock, event, status, entry->context);
        int64_t cb_end = util_time_us();
        g_reactor_stats.total_events_delivered++;
        g_reactor_stats.total_callback_time_us += (cb_end - cb_start);
    }
}


/**
 * @brief Process all pending events for all sockets, respecting priority.
 *
 * Iterates through sockets and delivers exactly one event per socket
 * per call, respecting priority (FATAL > STATE > DATA > PERIODIC).
 * 
 * NOTE: Callbacks may call reactor_remove_socket(), which compacts the arrays.
 * We handle this by tracking the socket fd and checking if it changed after
 * each event delivery.
 */
static void deliver_pending_events(reactor_t *r) {
    /* Process sockets starting at index 1 (skip wake pipe at index 0) 
     * Use a while loop since active_socket_count may change during iteration */
    size_t i = 1;
    while (i < r->active_socket_count) {
        socket_entry_t *entry = &r->sockets[i];
        socket_t current_sock = entry->sock;

        if (current_sock == INVALID_SOCKET) {
            i++;
            continue;  /* Shouldn't happen with compacted arrays, but be safe */
        }

        /* Process all pending events for this socket, one at a time */
        while(socket_has_pending_events(entry)) {
            /* priority 0: SHUTDOWN - trumps everything else. */
            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_SHUTDOWN)) {
                /* does not matter what events are still pending. */
                bitarray_clear_all(&entry->pending_events);

                deliver_event(r, i, REACTOR_EVENT_SHUTDOWN, UTIL_OK);
                
                /* Check if socket was removed (array compacted) */
                if (i >= r->active_socket_count || r->sockets[i].sock != current_sock) {
                    /* Socket was removed, don't increment i - a new socket slid into this slot */
                    break;
                }
                continue;
            }

            /* Priority 1: FATAL events (ERROR, CLOSED) - deliver alone, mask others */
            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_ERROR) || bitarray_test(&entry->pending_events, REACTOR_EVENT_CLOSED)) {
                bitarray_clear_all(&entry->pending_events);

                /* TODO: Need to track error status per socket - for now use UTIL_EINTERNAL */
                deliver_event(r, i, REACTOR_EVENT_ERROR, entry->last_error);
                
                /* Check if socket was removed (array compacted) */
                if (i >= r->active_socket_count || r->sockets[i].sock != current_sock) {
                    break;
                }
                continue;
            }

            /* Priority 2: STATE CHANGE events (ACCEPT, CONNECTED) - deliver alone */
            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_CAN_ACCEPT)) {
                bitarray_clear(&entry->pending_events, REACTOR_EVENT_CAN_ACCEPT);
                deliver_event(r, i, REACTOR_EVENT_CAN_ACCEPT, UTIL_OK);
                
                if (i >= r->active_socket_count || r->sockets[i].sock != current_sock) {
                    break;
                }
                continue;
            }

            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_CONNECTED)) {
                bitarray_clear(&entry->pending_events, REACTOR_EVENT_CONNECTED);
                deliver_event(r, i, REACTOR_EVENT_CONNECTED, UTIL_OK);
                
                if (i >= r->active_socket_count || r->sockets[i].sock != current_sock) {
                    break;
                }
                continue;
            }

            /* Priority 3: DATA events (READABLE, WRITABLE, WRITE_COMPLETE) */
            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_CAN_READ)) {
                bitarray_clear(&entry->pending_events, REACTOR_EVENT_CAN_READ);
                /* Edge-triggered: event stays enabled, will fire again on next state transition */
                deliver_event(r, i, REACTOR_EVENT_CAN_READ, UTIL_OK);
                
                if (i >= r->active_socket_count || r->sockets[i].sock != current_sock) {
                    break;
                }
                continue;
            }

            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_CAN_WRITE)) {
                bitarray_clear(&entry->pending_events, REACTOR_EVENT_CAN_WRITE);
                /* Edge-triggered: event stays enabled */
                deliver_event(r, i, REACTOR_EVENT_CAN_WRITE, UTIL_OK);
                
                if (i >= r->active_socket_count || r->sockets[i].sock != current_sock) {
                    break;
                }
                continue;
            }

            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_WRITTEN)) {
                bitarray_clear(&entry->pending_events, REACTOR_EVENT_WRITTEN);
                deliver_event(r, i, REACTOR_EVENT_WRITTEN, UTIL_OK);
                
                if (i >= r->active_socket_count || r->sockets[i].sock != current_sock) {
                    break;
                }
                continue;
            }

            /* Priority 4: PERIODIC events  */
            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_TICK)) {
                bitarray_clear(&entry->pending_events, REACTOR_EVENT_TICK);
                deliver_event(r, i, REACTOR_EVENT_TICK, UTIL_OK);
                
                if (i >= r->active_socket_count || r->sockets[i].sock != current_sock) {
                    break;
                }
                continue;
            }


            /* now deliver any other app events*/
            for(event_type_t evt = REACTOR_EVENT_MAX; evt < EVENT_TYPE_MAX; evt++) {
                if (bitarray_test(&entry->pending_events, evt)) {
                    bitarray_clear(&entry->pending_events, evt);
                    deliver_event(r, i, evt, UTIL_OK);
                    
                    if (i >= r->active_socket_count || r->sockets[i].sock != current_sock) {
                        break;
                    }
                }
            }
            
            /* Check again after app events */
            if (i >= r->active_socket_count || r->sockets[i].sock != current_sock) {
                break;
            }
        }
        
        /* Only increment if we didn't break out due to socket removal.
         * If socket was removed, a new socket slid into slot i, so we
         * need to process it without incrementing. */
        if (i < r->active_socket_count && r->sockets[i].sock == current_sock) {
            i++;
        }
        /* If socket was removed (current_sock no longer at i), don't increment -
         * either a new socket is now at i, or i >= active_socket_count */
        else if (i >= r->active_socket_count) {
            break;  /* No more sockets to process */
        }
        /* else: socket was removed and a new one slid into place, loop will process it */
    }
}

/**
 * @brief Tickle the wake pipe to interrupt poll().
 *
 * Called whenever reactor_wake() is invoked to wake the
 * reactor if it's blocked in poll().
 */
static void tickle_wake_pipe(reactor_t *r) {
    uint8_t byte = 0;
    /* Non-blocking send - if socket is full, wake already pending */
    int result = (int)send(r->wake_pipe[1], (const char *)&byte, 1, 0);
    if (result < 0) {
        util_err_t err = socket_get_err();
        if (err == UTIL_EAGAIN) {
            /* Wake pipe full - a wake is already pending, ignore */
        }
        /* Other errors don't matter - reactor will wake eventually */
    }
}

/**
 * @brief Create and configure a wake pipe for the reactor.
 *
 * On Windows, creates a pair of TCP sockets connected via loopback.
 * On POSIX, creates a socketpair using AF_UNIX.
 * Both socket ends are set to non-blocking mode.
 *
 * @param wake_pipe Array to store the wake pipe descriptors [0]=read, [1]=write
 * @return util_err_t UTIL_OK on success, error code on failure (wake_pipe unchanged on error)
 */
static util_err_t create_wake_pipe(socket_t wake_pipe[2]) {
#ifdef _WIN32
    /* On Windows, create a TCP socket pair using connect/accept pattern.
     * This is more secure than UDP as only the connected socket can wake the reactor. */
    SOCKET listener = INVALID_SOCKET;
    SOCKET read_sock = INVALID_SOCKET;
    SOCKET write_sock = INVALID_SOCKET;
    util_err_t rc = UTIL_OK;

    do {
        /* Create listener socket */
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            rc = util_err_from_wsa(WSAGetLastError());
            break;
        }

        /* Bind to loopback with ephemeral port */
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            rc = util_err_from_wsa(WSAGetLastError());
            break;
        }

        /* Get bound address and port */
        int addr_len = sizeof(addr);
        if (getsockname(listener, (struct sockaddr *)&addr, &addr_len) != 0) {
            rc = util_err_from_wsa(WSAGetLastError());
            break;
        }

        /* Listen for connections */
        if (listen(listener, 1) != 0) {
            rc = util_err_from_wsa(WSAGetLastError());
            break;
        }

        /* Create read-side socket and connect to listener */
        read_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (read_sock == INVALID_SOCKET) {
            rc = util_err_from_wsa(WSAGetLastError());
            break;
        }

        if (connect(read_sock, (struct sockaddr *)&addr, addr_len) != 0) {
            rc = util_err_from_wsa(WSAGetLastError());
            break;
        }

        /* Accept the connection - this becomes the write side */
        write_sock = accept(listener, NULL, NULL);
        if (write_sock == INVALID_SOCKET) {
            rc = util_err_from_wsa(WSAGetLastError());
            break;
        }

        /* Set both sockets to non-blocking */
        rc = socket_set_nonblocking(read_sock, true);
        if (rc != UTIL_OK) {
            break;
        }

        rc = socket_set_nonblocking(write_sock, true);
        if (rc != UTIL_OK) {
            break;
        }

        /* Set TCP_NODELAY to avoid latency */
        rc = socket_set_nodelay(read_sock, true);
        if (rc != UTIL_OK) {
            break;
        }

        rc = socket_set_nodelay(write_sock, true);
        if (rc != UTIL_OK) {
            break;
        }

        /* Success - assign to output array */
        wake_pipe[0] = read_sock;
        wake_pipe[1] = write_sock;

    } while (0);

    /* Clean up listener socket (no longer needed) */
    if (listener != INVALID_SOCKET) {
        socket_close(listener);
    }

    /* Clean up on failure */
    if (rc != UTIL_OK) {
        if (read_sock != INVALID_SOCKET) {
            socket_close(read_sock);
        }
        if (write_sock != INVALID_SOCKET) {
            socket_close(write_sock);
        }
    }

    return rc;

#else
    /* On POSIX, use socketpair() for consistency with Windows */
    int sock_fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sock_fds) != 0) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "socketpair() failed: %s", strerror(errno));
        return util_err_from_errno(errno);
    }

    /* Set read end to non-blocking */
    util_err_t rc = socket_set_nonblocking((socket_t)sock_fds[0], true);
    if (rc != UTIL_OK) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to set wake pipe read end non-blocking: %s", util_err_str(rc));
        close(sock_fds[0]);
        close(sock_fds[1]);
        return rc;
    }

    /* Set write end to non-blocking */
    rc = socket_set_nonblocking((socket_t)sock_fds[1], true);
    if (rc != UTIL_OK) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to set wake pipe write end non-blocking: %s", util_err_str(rc));
        close(sock_fds[0]);
        close(sock_fds[1]);
        return rc;
    }

    // /* Set TCP_NODELAY on read end to avoid latency */
    // rc = socket_set_nodelay((socket_t)sock_fds[0], true);
    // if (rc != UTIL_OK) {
    //     pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to set wake pipe read end TCP_NODELAY: %s", util_err_str(rc));
    //     close(sock_fds[0]);
    //     close(sock_fds[1]);
    //     return rc;
    // }

    // /* Set TCP_NODELAY on write end to avoid latency */
    // rc = socket_set_nodelay((socket_t)sock_fds[1], true);
    // if (rc != UTIL_OK) {
    //     pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to set wake pipe write end TCP_NODELAY: %s", util_err_str(rc));
    //     close(sock_fds[0]);
    //     close(sock_fds[1]);
    //     return rc;
    // }

    /* Success - assign to output array */
    wake_pipe[0] = (socket_t)sock_fds[0];
    wake_pipe[1] = (socket_t)sock_fds[1];
    return UTIL_OK;
#endif
}

/* ================================================================
 * Public API
 * ================================================================ */

reactor_t* reactor_create(size_t max_sockets) {
    pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_INFO, "Creating reactor with max_sockets=%zu", max_sockets);
    if (max_sockets == 0) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Invalid max_sockets=0");
        return NULL;
    }

    reactor_t *r = (reactor_t *)malloc(sizeof(reactor_t));
    if (r == NULL) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_ERROR, "Failed to allocate reactor instance");
        return NULL;
    }

    memset(r, 0, sizeof(*r));

    /* Initialize reactor fields */
    r->max_sockets = max_sockets + 1;  /* +1 for wake pipe */
    r->active_socket_count = 1;        /* Start with wake pipe registered */
    r->shutdown = false;
    
    /* Allocate socket registry, one extra for wake pipe */
    r->sockets = (socket_entry_t *)malloc(r->max_sockets * sizeof(socket_entry_t));
    if (r->sockets == NULL) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_ERROR, "Failed to allocate reactor socket registry");
        free(r);
        return NULL;
    }
    memset(r->sockets, 0, r->max_sockets * sizeof(socket_entry_t));

    /* Initialize all socket fields to INVALID_SOCKET */
    for (size_t i = 0; i < r->max_sockets; i++) {
        r->sockets[i].sock = INVALID_SOCKET;
    }

    /* Allocate poll array */
    r->pollfds = (struct pollfd *)malloc(r->max_sockets * sizeof(struct pollfd));
    if (r->pollfds == NULL) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_ERROR, "Failed to allocate reactor pollfd array");
        free(r->sockets);
        free(r);
        return NULL;
    }
    memset(r->pollfds, 0, r->max_sockets * sizeof(struct pollfd));

    /* Initialize synchronization */
#ifdef _WIN32
    InitializeCriticalSection(&r->lock);
#else
    pthread_mutex_init(&r->lock, NULL);
#endif

    /* Create wake pipe */
    if (create_wake_pipe(r->wake_pipe) != UTIL_OK) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to create reactor wake pipe");
        free(r->pollfds);
        free(r->sockets);
#ifdef _WIN32
        DeleteCriticalSection(&r->lock);
#else
        pthread_mutex_destroy(&r->lock);
#endif
        free(r);
        return NULL;
    }

    /* Register wake pipe as socket 0 */
    r->sockets[0].sock = r->wake_pipe[0];
    r->pollfds[0].fd = r->wake_pipe[0];
    r->sockets[0].callback = NULL;  /* No callback, just drains */
    r->sockets[0].context = r;
    bitarray_set(&r->sockets[0].enabled, REACTOR_EVENT_CAN_READ);

    r->pollfds[0].events = POLLIN;
    r->pollfds[0].revents = 0;

    r->active_socket_count = 1;  /* Start with wake pipe */

    pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Wake pipe created: read_fd=%d, write_fd=%d", (int)r->wake_pipe[0], (int)r->wake_pipe[1]);

    /* Initialize shutdown flag */
    r->shutdown = false;

    pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_INFO, "Reactor created successfully");

    return r;
}

void reactor_destroy(reactor_t *r) {
    if (r == NULL) {
        return;
    }

    /* Signal to stop if running */
    reactor_stop(r);

    /* Close wake pipe */
    socket_close(r->wake_pipe[0]);
    socket_close(r->wake_pipe[1]);

#ifdef _WIN32
    DeleteCriticalSection(&r->lock);
#else
    pthread_mutex_destroy(&r->lock);
#endif

    free(r->pollfds);
    free(r->sockets);
    free(r);
}

util_err_t reactor_add_socket(reactor_t *r, socket_t sock, const char *name,
                             reactor_socket_cb_t cb, void *ctx,
                             const bitarray_t *initial_events) {
    util_err_t rc = UTIL_OK;

    if(r == NULL) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Invalid reactor instance (NULL)");
        return UTIL_ENULL;
    }

    if(cb == NULL) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Invalid socket callback (NULL) for socket fd=%d", sock);
        return UTIL_ENULL;
    }

    if(sock == INVALID_SOCKET) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Invalid socket descriptor (INVALID_SOCKET)");
        return UTIL_EINVAL;
    }

    const char *sock_name = name ? name : "(unnamed)";

    /* set socket to non-blocking for reactor use */
    if((rc = socket_set_nonblocking(sock, true)) != UTIL_OK) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to set socket fd=%d non-blocking.  Error %s.", sock, util_err_str(rc));
        return rc;
    }

    if((rc = socket_set_nodelay(sock, true)) != UTIL_OK) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to set socket fd=%d no-delay.  Error %s.", sock, util_err_str(rc));
        return rc;
    }

    /* Detect socket type and connection status */
    socket_type_t sock_type = get_socket_type(sock);
    bool already_connected = false;

    if (sock_type == SOCKET_TYPE_STREAM) {
        /* For STREAM sockets, determine if listener or client (outbound/accepted) */
        if (initial_events != NULL && bitarray_test(initial_events, REACTOR_EVENT_CAN_ACCEPT)) {
            /* Has CAN_ACCEPT enabled - this is a listening socket */
            sock_type = SOCKET_TYPE_LISTENER;
            already_connected = true;  /* Listeners don't need to connect */
        } else if (initial_events == NULL || !bitarray_test(initial_events, REACTOR_EVENT_CONNECTED)) {
            /* CONNECTED event NOT enabled - socket is already connected (from accept() or pre-connected) */
            already_connected = true;
        } else {
            /* CONNECTED event IS enabled - this is an outbound client socket pending async connect */
            already_connected = false;
        }
    } else if (sock_type == SOCKET_TYPE_DGRAM) {
        already_connected = true;  /* UDP doesn't have connection state */
    }

    rc = reactor_lock(r);
    if(rc != UTIL_OK) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to add socket fd=%d.  Error %s.", sock, util_err_str(rc));
        return rc;
    }

    /* With compacted arrays, new sockets go at active_socket_count */
    if (r->active_socket_count >= r->max_sockets) {
        reactor_unlock(r);
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "No available slots to add socket fd=%d (active=%zu, max=%zu)", sock, r->active_socket_count, r->max_sockets);
        return UTIL_ERESOURCE;
    }

    size_t i = r->active_socket_count;
    pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Adding socket[%s] fd=%d at index %zu", sock_name, sock, i);
    r->sockets[i].sock = sock;
    r->sockets[i].name = name ? strdup(name) : NULL;
    r->sockets[i].callback = cb;
    r->sockets[i].context = ctx;
    r->sockets[i].socket_type = sock_type;
    r->sockets[i].connected = already_connected;

    /* Use provided initial events, or enable all if not provided */
    if (initial_events != NULL) {
        r->sockets[i].enabled = *initial_events;
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Set initial event mask for socket[%s] at index %zu", sock_name, i);
    } else {
        bitarray_set_all(&r->sockets[i].enabled);  /* All events enabled by default */
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Enabled all events for socket[%s] at index %zu", sock_name, i);
    }

    bitarray_clear_all(&r->sockets[i].pending_events);
    memset(&r->sockets[i].last_revents, 0, sizeof(r->sockets[i].last_revents));

    r->pollfds[i].fd = sock;
    r->pollfds[i].events = 0;
    r->pollfds[i].revents = 0;

    /* Mark pollfds dirty so it gets rebuilt before next poll() */
    r->pollfds_dirty = true;

    r->active_socket_count++;

    reactor_unlock(r);

    return UTIL_OK;
}

util_err_t reactor_remove_socket(reactor_t *r, socket_t sock) {
    if (r == NULL || sock == INVALID_SOCKET) {
        return UTIL_EINVAL;
    }

    util_err_t rc = reactor_lock(r);
    if(rc != UTIL_OK) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to remove socket fd=%d.  Error %s.", sock, util_err_str(rc));
        return rc;
    }

    /* Find socket, skipping index 0 (reserved for wake pipe) */
    for (size_t i = 1; i < r->active_socket_count; i++) {
        if (r->sockets[i].sock == sock) {
            const char *sock_name = r->sockets[i].name ? r->sockets[i].name : "(unnamed)";
            
            pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "reactor_remove_socket: Marking socket[%s] fd=%d for removal", sock_name, sock);
            
            /* Mark for removal - rebuild_pollfds will handle compaction */
            r->sockets[i].pending_removal = true;
            
            /* Disable all events so poll() won't report events for this socket */
            bitarray_clear_all(&r->sockets[i].enabled);
            r->pollfds[i].events = 0;
            
            /* Set dirty flag so rebuild_pollfds will compact the arrays */
            r->pollfds_dirty = true;
            
            /* If not on reactor thread, wake it so rebuild happens promptly */
            if (!is_reactor_thread(r)) {
                reactor_unlock(r);
                tickle_wake_pipe(r);
                return UTIL_OK;
            }

            reactor_unlock(r);
            return UTIL_OK;
        }
    }

    reactor_unlock(r);
    return UTIL_ENOTFOUND;
}

/* Statistics for reactor_set_event_mask breakdown */
static struct {
    int64_t calls;
    int64_t lock_time_us;
    int64_t search_time_us;
    int64_t rebuild_time_us;
    int64_t log_time_us;
    int64_t unlock_time_us;
    int64_t wake_time_us;
} g_set_mask_stats = {0};

void reactor_get_set_mask_stats(int64_t *calls, int64_t *lock_us, int64_t *search_us,
                                 int64_t *rebuild_us, int64_t *log_us, int64_t *unlock_us,
                                 int64_t *wake_us) {
    if (calls) *calls = g_set_mask_stats.calls;
    if (lock_us) *lock_us = g_set_mask_stats.lock_time_us;
    if (search_us) *search_us = g_set_mask_stats.search_time_us;
    if (rebuild_us) *rebuild_us = g_set_mask_stats.rebuild_time_us;
    if (log_us) *log_us = g_set_mask_stats.log_time_us;
    if (unlock_us) *unlock_us = g_set_mask_stats.unlock_time_us;
    if (wake_us) *wake_us = g_set_mask_stats.wake_time_us;
}

util_err_t reactor_set_event_mask(reactor_t *r, socket_t sock, bitarray_t event_mask) {
    int64_t t0, t1;

    if (r == NULL || sock == INVALID_SOCKET) {
        return UTIL_EINVAL;
    }

    g_set_mask_stats.calls++;

    /* Time lock acquisition start*/
    t0 = util_time_us();
    util_err_t rc = reactor_lock(r);
    if(rc != UTIL_OK) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to set event mask for socket fd=%d.  Error %s.", sock, util_err_str(rc));
        return rc;
    }

    /* Time lock acquisition end */
    t1 = util_time_us();
    g_set_mask_stats.lock_time_us += (t1 - t0);

    /* Time socket search */
    t0 = util_time_us();

    /* Find socket, skipping index 0 (reserved for wake pipe) */
    for (size_t i = 1; i < r->max_sockets; i++) {
        if (r->sockets[i].sock == sock) {
            t1 = util_time_us();
            g_set_mask_stats.search_time_us += (t1 - t0);

            const char *sock_name = r->sockets[i].name ? r->sockets[i].name : "(unnamed)";

            /* Time log_detail */
            t0 = util_time_us();
            pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_SPEW, "Found socket[%s] fd=%d at index %zu", sock_name, sock, i);
            t1 = util_time_us();
            g_set_mask_stats.log_time_us += (t1 - t0);

            /* Check if the mask is actually changing - if not, skip update entirely */
            if (bitarray_equal(&r->sockets[i].enabled, &event_mask)) {
                /* Mask unchanged - no need to update or wake */
                reactor_unlock(r);
                return UTIL_OK;
            }

            /* Detect newly enabled events (transitioning from 0→1) to clear last_revents */
            bool read_newly_enabled = (bitarray_test(&event_mask, REACTOR_EVENT_CAN_READ) && !bitarray_test(&r->sockets[i].enabled, REACTOR_EVENT_CAN_READ)) ||
                                      (bitarray_test(&event_mask, REACTOR_EVENT_CAN_ACCEPT) && !bitarray_test(&r->sockets[i].enabled, REACTOR_EVENT_CAN_ACCEPT));
            bool write_newly_enabled = (bitarray_test(&event_mask, REACTOR_EVENT_CAN_WRITE) && !bitarray_test(&r->sockets[i].enabled, REACTOR_EVENT_CAN_WRITE)) ||
                                       (bitarray_test(&event_mask, REACTOR_EVENT_CONNECTED) && !bitarray_test(&r->sockets[i].enabled, REACTOR_EVENT_CONNECTED));

            /* Clear last_revents bits for newly enabled events to allow edge retrigger */
            if (read_newly_enabled) {
                r->sockets[i].last_revents.revents &= ~POLLIN;
            }
            if (write_newly_enabled) {
                r->sockets[i].last_revents.revents &= ~POLLOUT;
            }

            /* Atomically replace the entire event mask */
            r->sockets[i].enabled = event_mask;

            /* Mark pollfds dirty so it gets rebuilt before next poll() */
            r->pollfds_dirty = true;

            /* Time second log_detail */
            t0 = util_time_us();
            pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_SPEW, "Set event mask for socket[%s] at index %zu", sock_name, i);
            t1 = util_time_us();
            g_set_mask_stats.log_time_us += (t1 - t0);

            /* Time unlock */
            t0 = util_time_us();
            reactor_unlock(r);
            t1 = util_time_us();
            g_set_mask_stats.unlock_time_us += (t1 - t0);

            /* Only tickle wake pipe if called from outside the reactor thread.
             * If we're on the reactor thread (inside a callback), the changes 
             * will be picked up on the next poll iteration without needing to 
             * wake ourselves - avoiding a busy loop. */
            if (!is_reactor_thread(r)) {
                /* Time wake pipe */
                t0 = util_time_us();
                tickle_wake_pipe(r);
                t1 = util_time_us();
                g_set_mask_stats.wake_time_us += (t1 - t0);
            }

            return UTIL_OK;
        }
    }

    t1 = util_time_us();
    g_set_mask_stats.search_time_us += (t1 - t0);

    reactor_unlock(r);
    return UTIL_ENOTFOUND;
}


util_err_t reactor_stop(reactor_t *r) {
    if (r == NULL) {
        return UTIL_EINVAL;
    }

    util_err_t rc = reactor_lock(r);
    if(rc != UTIL_OK) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to stop reactor.  Error %s.", util_err_str(rc));
        return rc;
    }

    r->shutdown = true;

    reactor_unlock(r);

    tickle_wake_pipe(r);
    return UTIL_OK;
}

util_err_t reactor_wake(reactor_t *r) {
    pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Waking reactor");

    if (r == NULL) {
        return UTIL_EINVAL;
    }

    tickle_wake_pipe(r);

    return UTIL_OK;
}

/* ================================================================
 * Main Event Loop
 * ================================================================ */

util_err_t reactor_run(reactor_t *r, uint32_t poll_timeout_ms) {
    int64_t t0, t1;

    if (r == NULL) {
        return UTIL_EINVAL;
    }

    /* Record reactor thread ID so we can detect calls from within callbacks.
     * This prevents unnecessary wake pipe tickles when reactor_set_event_mask 
     * is called from within callbacks running on this thread. */
#ifdef _WIN32
    r->reactor_thread_id = GetCurrentThreadId();
#else
    r->reactor_thread_id = pthread_self();
#endif

    /* Main event loop: runs continuously until reactor_stop() is called */
    while (!r->shutdown) {
        /* Rebuild pollfds array if needed.
         * This handles: compacting arrays after socket removal,
         * updating poll events after mask changes, and cleaning up
         * sockets marked for deferred removal.
         * MUST happen before poll() so the arrays are valid. */
        rebuild_pollfds(r);

        /* Use poll_timeout_ms directly for TICK event generation */
        int timeout = (int)poll_timeout_ms;

        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Calling poll() with %zu sockets, timeout=%dms", r->active_socket_count, timeout);

        /* Log poll fd setup for listener sockets (typically indices 1 and 2) */
        for (size_t i = 1; i < r->active_socket_count && i < 3; i++) {
            if (r->sockets[i].sock != INVALID_SOCKET) {
                const char *sock_name = r->sockets[i].name ? r->sockets[i].name : "(unnamed)";
                bool has_events = bitarray_has_any(&r->sockets[i].enabled);
                pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Socket[%s] fd=%d events=0x%x has_enabled=%d", sock_name, r->pollfds[i].fd, r->pollfds[i].events, has_events ? 1 : 0);
            }
        }

        /* Time the poll() call */
        t0 = util_time_us();

        /* Poll for socket activity */
#ifdef _WIN32
        int ret = WSAPoll(r->pollfds, (ULONG)r->active_socket_count, timeout);
        if (ret == SOCKET_ERROR) {
            return util_err_from_wsa(WSAGetLastError());
        }
#else
        int ret = poll(r->pollfds, (nfds_t)r->active_socket_count, timeout);
        if (ret < 0) {
            /* EINTR is expected and should be retried */
            if (errno == EINTR) {
                pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "poll() interrupted by signal, retrying");
                continue;
            }
            pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "poll() returned error: %d (errno=%d)", ret, errno);
            return util_err_from_errno(errno);
        }
#endif

        t1 = util_time_us();
        g_reactor_stats.total_poll_calls++;
        g_reactor_stats.total_poll_time_us += (t1 - t0);

        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "poll() returned %d ready sockets", ret);

        /* Log which sockets have events */
        if (ret > 0) {
            for (size_t i = 0; i < r->active_socket_count; i++) {
                if (r->pollfds[i].revents != 0) {
                    const char *sock_name = r->sockets[i].name ? r->sockets[i].name : "(unnamed)";
                    pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Socket[%s] fd=%d has revents=0x%x", sock_name, (int)r->pollfds[i].fd, r->pollfds[i].revents);
                }
            }
        }

        /* Detect timeout to raise TICK events */
        bool poll_timeout_expired = (ret == 0);

        /* Time translate_pollevents */
        t0 = util_time_us();

        /* Translate poll results to pending events */
        for (size_t i = 0; i < r->active_socket_count; i++) {
            if (i == 0) {
                /* Wake pipe: special handling */
                if (r->pollfds[i].revents & POLLIN) {
                    pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Draining wake pipe fd=%d", (int)r->wake_pipe[0]);
                    drain_wake_pipe(r);
                    pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Wake pipe drained");
                }
            } else {
                /* Regular socket */
                translate_pollevents(r, i);
            }
        }

        t1 = util_time_us();
        g_reactor_stats.total_translate_time_us += (t1 - t0);

        /* Raise TICK events for all sockets with TICK enabled (if poll timed out) */
        if (poll_timeout_expired) {
            util_err_t rc = reactor_lock(r);
            if(rc != UTIL_OK) {
                pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to set TICK events.  Error %s.", util_err_str(rc));
                return rc;
            }
            for (size_t i = 1; i < r->max_sockets; i++) {
                if (r->sockets[i].sock != INVALID_SOCKET && bitarray_test(&r->sockets[i].enabled, REACTOR_EVENT_TICK)) {
                    bitarray_set(&r->sockets[i].pending_events, REACTOR_EVENT_TICK);
                }
            }
            reactor_unlock(r);
        }

        /* Time event delivery */
        t0 = util_time_us();

        /* Deliver pending events in priority order */
        deliver_pending_events(r);

        t1 = util_time_us();
        g_reactor_stats.total_deliver_time_us += (t1 - t0);
    }

    /* After shutdown, deliver SHUTDOWN events to all sockets */
    util_err_t rc = reactor_lock(r);
    if(rc != UTIL_OK) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to set SHUTDOWN events.  Error %s.", util_err_str(rc));
        return rc;
    }

    for (size_t i = 1; i < r->max_sockets; i++) {
        if (r->sockets[i].sock != INVALID_SOCKET) {
            bitarray_set(&r->sockets[i].pending_events, REACTOR_EVENT_SHUTDOWN);
        }
    }

    reactor_unlock(r);

    deliver_pending_events(r);

    /* Clear reactor thread ID before returning */
#ifdef _WIN32
    r->reactor_thread_id = 0;
#else
    r->reactor_thread_id = 0;
#endif

    return UTIL_OK;
}
