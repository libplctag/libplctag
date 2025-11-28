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
    SOCKET_TYPE_STREAM,     /* TCP */
    SOCKET_TYPE_DGRAM,      /* UDP */
} socket_type_t;

/* Per-socket state */
typedef struct {
    socket_type_t socket_type;                          /* STREAM (TCP) or DGRAM (UDP) */
    socket_t sock;                                      /* Socket descriptor (INVALID_SOCKET if unused) */
    reactor_socket_cb_t callback;                       /* User callback function */
    void *context;                                      /* User context data */
    bitarray_t enabled;                                 /* Bitmask of enabled events */
    bitarray_t pending_events;                          /* Bitmask of active events waiting delivery */
    struct pollfd last_revents;                         /* Last poll revents for edge-triggered detection */
    util_err_t last_error;                              /* Last error status for ERROR event */
    bool connected;                                     /* For TCP sockets: true if connected or CONNECTED event fired */
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
#ifdef _WIN32
    SOCKET wake_pipe_read;
    SOCKET wake_pipe_write;
#else
    int wake_pipe[2];                           /* [0] = read, [1] = write */
#endif

    /* Synchronization */
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif

    /* Control flags */
    bool shutdown;  /* Signal reactor to stop */
};



/* ================================================================
 * Helper Functions
 * ================================================================ */

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
 * @brief Rebuild poll events for a single socket based on enabled[] mask.
 *
 * Called after reactor_set_event_enable_mask() to update the pollfd entry.
 */
static void rebuild_pollfds_for_socket(reactor_t *r, size_t index) {
    if (index >= r->max_sockets) {
        return;
    }

    socket_entry_t *entry = &r->sockets[index];
    struct pollfd *pfd = &r->pollfds[index];

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

    /* POLLERR - socket error (may be spurious on Windows) */
    if ((current & POLLERR) && !(previous & POLLERR)) {
        /* Verify this is a real error by checking SO_ERROR */
#ifdef _WIN32
        int optval = 0;
        int optlen = sizeof(optval);
        if (getsockopt((SOCKET)entry->sock, SOL_SOCKET, SO_ERROR, (char *)&optval, &optlen) != SOCKET_ERROR) {
            if (optval != 0) {
                /* Real socket error - check if this is a failed async connect */
                if (entry->socket_type == SOCKET_TYPE_STREAM && !entry->connected) {
                    /* Failed async connect */
                    entry->last_error = util_err_from_errno(optval);
                    bitarray_set(&entry->pending_events, REACTOR_EVENT_ERROR);
                    /* Mark as "connected" to prevent further connect attempts */
                    entry->connected = true;
                } else {
                    /* Generic socket error */
                    entry->last_error = util_err_from_errno(optval);
                    bitarray_set(&entry->pending_events, REACTOR_EVENT_ERROR);
                }
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
                /* Real socket error - check if this is a failed async connect */
                if (entry->socket_type == SOCKET_TYPE_STREAM && !entry->connected) {
                    /* Failed async connect */
                    entry->last_error = util_err_from_errno(optval);
                    bitarray_set(&entry->pending_events, REACTOR_EVENT_ERROR);
                    /* Mark as "connected" to prevent further connect attempts */
                    entry->connected = true;
                } else {
                    /* Generic socket error */
                    entry->last_error = util_err_from_errno(optval);
                    bitarray_set(&entry->pending_events, REACTOR_EVENT_ERROR);
                }
            }
            /* else: spurious POLLERR, ignore */
        }
#endif
        /* Don't return early - process other events too */
    }

#ifdef POLLRDHUP
    /* POLLRDHUP - peer closed write side (Linux half-close detection) */
    if (entry->socket_type == SOCKET_TYPE_STREAM && (current & POLLRDHUP) && !(previous & POLLRDHUP)) {
        /* Peer sent FIN - read side is closed but write side may still be open */
        bitarray_set(&entry->pending_events, REACTOR_EVENT_CLOSED);
        /* Don't return early - there may be buffered data to read */
    }
#endif

    /* POLLHUP - peer closed (graceful close, only for TCP) */
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
#ifdef _WIN32
            /* On Windows, use MSG_PEEK to verify data is available and distinguish
             * between readable data, peer disconnect, and spurious wakeup */
            char peek_buf;
            int peek_result = recv((SOCKET)entry->sock, &peek_buf, 1, MSG_PEEK);
            if (peek_result > 0) {
                /* Data is available */
                bitarray_set(&entry->pending_events, REACTOR_EVENT_CAN_READ);
            } else if (peek_result == 0) {
                /* recv() returned 0 - peer closed connection gracefully */
                if (entry->socket_type == SOCKET_TYPE_STREAM) {
                    bitarray_set(&entry->pending_events, REACTOR_EVENT_CLOSED);
                }
            } else {
                /* recv() returned -1, check error */
                int recv_err = WSAGetLastError();
                if (recv_err == WSAEWOULDBLOCK) {
                    /* Spurious wakeup - no data available, ignore */
                } else {
                    /* Real error on socket */
                    entry->last_error = util_err_from_wsa(recv_err);
                    bitarray_set(&entry->pending_events, REACTOR_EVENT_ERROR);
                }
            }
#else
            /* On Unix, trust POLLIN for stream sockets but verify for robustness */
            if (entry->socket_type == SOCKET_TYPE_STREAM) {
                char peek_buf;
                ssize_t peek_result = recv(entry->sock, &peek_buf, 1, MSG_PEEK);
                if (peek_result > 0) {
                    bitarray_set(&entry->pending_events, REACTOR_EVENT_CAN_READ);
                } else if (peek_result == 0) {
                    /* Peer closed */
                    bitarray_set(&entry->pending_events, REACTOR_EVENT_CLOSED);
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    /* Real error */
                    entry->last_error = util_err_from_errno(errno);
                    bitarray_set(&entry->pending_events, REACTOR_EVENT_ERROR);
                }
            } else {
                /* UDP sockets - trust POLLIN */
                bitarray_set(&entry->pending_events, REACTOR_EVENT_CAN_READ);
            }
#endif
        }
    }

    /* POLLOUT - writable (edge-triggered) */
    if ((current & POLLOUT) && !(previous & POLLOUT)) {
        /* First check if this is a pending connect completion for TCP sockets */
        if (entry->socket_type == SOCKET_TYPE_STREAM && !entry->connected) {
            util_err_t conn_err = check_connect_completion(entry);
            if (conn_err == UTIL_OK) {
                entry->connected = true;
                bitarray_set(&entry->pending_events, REACTOR_EVENT_CONNECTED);
            } else {
                /* Connect failed */
                entry->last_error = conn_err;
                bitarray_set(&entry->pending_events, REACTOR_EVENT_ERROR);
            }
        } else if (bitarray_test(&entry->enabled, REACTOR_EVENT_CAN_WRITE)) {
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

#ifdef _WIN32
    SOCKET wake_sock = r->wake_pipe_read;
    int received = recv(wake_sock, (char *)buf, sizeof(buf), 0);
    while (received > 0) {
        received = recv(wake_sock, (char *)buf, sizeof(buf), 0);
    }
#else
    int fd = r->wake_pipe[0];
    ssize_t received = read(fd, buf, sizeof(buf));
    while (received > 0) {
        received = read(fd, buf, sizeof(buf));
    }
#endif
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

    /* Rebuild poll events to reflect the disabled event */
    rebuild_pollfds_for_socket(r, socket_index);

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
 */
static void deliver_pending_events(reactor_t *r) {
    /* Process sockets starting at index 1 (skip wake pipe at index 0) */
    for (size_t i = 1; i < r->max_sockets; i++) {
        socket_entry_t *entry = &r->sockets[i];

        if (entry->sock == INVALID_SOCKET) {
            continue;  /* Unused socket slot */
        }

        /* Process all pending events for this socket, one at a time */
        while(socket_has_pending_events(entry)) {
            /* priority 0: SHUTDOWN - trumps everything else. */
            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_SHUTDOWN)) {
                /* does not matter what events are still pending. */
                bitarray_clear_all(&entry->pending_events);

                deliver_event(r, i, REACTOR_EVENT_SHUTDOWN, UTIL_OK);
                continue;
            }

            /* Priority 1: FATAL events (ERROR, CLOSED) - deliver alone, mask others */
            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_ERROR) || bitarray_test(&entry->pending_events, REACTOR_EVENT_CLOSED)) {
                bitarray_clear_all(&entry->pending_events);

                /* TODO: Need to track error status per socket - for now use UTIL_EINTERNAL */
                deliver_event(r, i, REACTOR_EVENT_ERROR, entry->last_error);

                continue;
            }

            /* Priority 2: STATE CHANGE events (ACCEPT, CONNECTED) - deliver alone */
            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_CAN_ACCEPT)) {
                bitarray_clear(&entry->pending_events, REACTOR_EVENT_CAN_ACCEPT);
                deliver_event(r, i, REACTOR_EVENT_CAN_ACCEPT, UTIL_OK);
                continue;
            }

            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_CONNECTED)) {
                bitarray_clear(&entry->pending_events, REACTOR_EVENT_CONNECTED);
                deliver_event(r, i, REACTOR_EVENT_CONNECTED, UTIL_OK);
                continue;
            }

            /* Priority 3: DATA events (READABLE, WRITABLE, WRITE_COMPLETE) */
            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_CAN_READ)) {
                bitarray_clear(&entry->pending_events, REACTOR_EVENT_CAN_READ);
                /* Edge-triggered: event stays enabled, will fire again on next state transition */
                deliver_event(r, i, REACTOR_EVENT_CAN_READ, UTIL_OK);
                continue;
            }

            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_CAN_WRITE)) {
                bitarray_clear(&entry->pending_events, REACTOR_EVENT_CAN_WRITE);
                /* Edge-triggered: event stays enabled */
                deliver_event(r, i, REACTOR_EVENT_CAN_WRITE, UTIL_OK);
                continue;
            }

            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_WRITTEN)) {
                bitarray_clear(&entry->pending_events, REACTOR_EVENT_WRITTEN);
                deliver_event(r, i, REACTOR_EVENT_WRITTEN, UTIL_OK);
                continue;
            }

            /* Priority 4: PERIODIC events  */
            if (bitarray_test(&entry->pending_events, REACTOR_EVENT_TICK)) {
                bitarray_clear(&entry->pending_events, REACTOR_EVENT_TICK);
                deliver_event(r, i, REACTOR_EVENT_TICK, UTIL_OK);
                continue;
            }


            /* now deliver any other app events*/
            for(event_type_t evt = REACTOR_EVENT_MAX; evt < EVENT_TYPE_MAX; evt++) {
                if (bitarray_test(&entry->pending_events, evt)) {
                    bitarray_clear(&entry->pending_events, evt);
                    deliver_event(r, i, evt, UTIL_OK);
                }
            }
        }
    }
}

/**
 * @brief Tickle the wake pipe to interrupt poll().
 *
 * Called whenever reactor_wake() is invoked to wake the
 * reactor if it's blocked in poll().
 */
static void tickle_wake_pipe(reactor_t *r) {
#ifdef _WIN32
    uint8_t byte = 0;
    /* Non-blocking send - if pipe is full, wake already pending */
    int result = send(r->wake_pipe_write, (const char *)&byte, 1, 0);
    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            /* Wake pipe full - a wake is already pending, ignore */
        }
        /* Other errors don't matter - reactor will wake eventually */
    }
#else
    uint8_t byte = 0;
    /* Non-blocking write - if pipe is full, wake already pending */
    ssize_t result = write(r->wake_pipe[1], &byte, 1);
    if (result < 0 && errno == EAGAIN) {
        /* Wake pipe full - a wake is already pending, ignore */
    }
    /* Other errors don't matter - reactor will wake eventually */
#endif
}

/* ================================================================
 * Public API
 * ================================================================ */

reactor_t* reactor_create(size_t max_sockets) {
    if (max_sockets == 0) {
        return NULL;
    }

    reactor_t *r = (reactor_t *)malloc(sizeof(reactor_t));
    if (r == NULL) {
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
#ifdef _WIN32
    /* On Windows, create a UDP socket pair for waking */
    SOCKET listen_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (listen_sock == INVALID_SOCKET) {
        free(r->pollfds);
        free(r->sockets);
        DeleteCriticalSection(&r->lock);
        free(r);
        return NULL;
    }

    r->wake_pipe_read = listen_sock;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(listen_sock);
        free(r->pollfds);
        free(r->sockets);
        DeleteCriticalSection(&r->lock);
        free(r);
        return NULL;
    }

    /* Get bound address */
    struct sockaddr_in bound_addr;
    int addr_len = sizeof(bound_addr);
    if (getsockname(listen_sock, (struct sockaddr *)&bound_addr, &addr_len) != 0) {
        closesocket(listen_sock);
        free(r->pollfds);
        free(r->sockets);
        DeleteCriticalSection(&r->lock);
        free(r);
        return NULL;
    }

    /* Set listen socket non-blocking */
    if (socket_set_nonblocking(r->listen_sock, true) != UTIL_OK) {
        close(r->listen_sock);
        close(r->send_sock);
        free(r->pollfds);
        free(r->sockets);
        pthread_mutex_destroy(&r->lock);
        free(r);
        return NULL;
    }

    /* set listen socket no delay */
    if(socket_set_nodelay(r->listen_sock, true) != UTIL_OK) {
        close(r->listen_sock);
        close(r->send_sock);
        free(r->pollfds);
        free(r->sockets);
        pthread_mutex_destroy(&r->lock);
        free(r);
        return NULL;
    }



    /* Create socket that will send to listen_sock */
    SOCKET send_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (send_sock == INVALID_SOCKET) {
        closesocket(listen_sock);
        free(r->pollfds);
        free(r->sockets);
        DeleteCriticalSection(&r->lock);
        free(r);
        return NULL;
    }

    r->wake_pipe_read = send_sock;

    /* set send socket non-blocking */*/
    if(socket_set_nonblocking(send_sock) != UTIL_OK) {
        close(r->listen_sock);
        close(r->send_sock);
        free(r->pollfds);
        free(r->sockets);
        pthread_mutex_destroy(&r->lock);
        free(r);
        return NULL;
    }   

    /* set send socket no delay */
    if(socket_set_nodelay(send_sock) != UTIL_OK) {
        close(r->listen_sock);
        close(r->send_sock);
        free(r->pollfds);
        free(r->sockets);
        pthread_mutex_destroy(&r->lock);
        free(r);
        return NULL;
    }

    r->wake_pipe_read = listen_sock;
    r->wake_pipe_write = send_sock;
#else
    if (pipe(r->wake_pipe) != 0) {
        free(r->pollfds);
        free(r->sockets);
        pthread_mutex_destroy(&r->lock);
        free(r);
        return NULL;
    }

    /* Set read end to non-blocking */
    if (socket_set_nonblocking(r->wake_pipe[0], true) != UTIL_OK) {
        close(r->wake_pipe[0]);
        close(r->wake_pipe[1]);
        free(r->pollfds);
        free(r->sockets);
        pthread_mutex_destroy(&r->lock);
        free(r);
        return NULL;
    }

    if(socket_set_nodelay(r->wake_pipe[1], true) != UTIL_OK) {
        close(r->wake_pipe[0]);
        close(r->wake_pipe[1]);
        free(r->pollfds);
        free(r->sockets);
        pthread_mutex_destroy(&r->lock);
        free(r);
        return NULL;
    }   

    if(socket_set_nonblocking(r->wake_pipe[1], true) != UTIL_OK) {
        close(r->wake_pipe[0]);
        close(r->wake_pipe[1]);
        free(r->pollfds);
        free(r->sockets);
        pthread_mutex_destroy(&r->lock);
        free(r);
        return NULL;
    }

    if(socket_set_nodelay(r->wake_pipe[1], true) != UTIL_OK) {
        close(r->wake_pipe[0]);
        close(r->wake_pipe[1]);
        free(r->pollfds);
        free(r->sockets);
        pthread_mutex_destroy(&r->lock);
        free(r);
        return NULL;
    }
#endif

    /* Register wake pipe as socket 0 */
#ifdef _WIN32
    r->sockets[0].sock = r->wake_pipe_read;
    r->pollfds[0].fd = r->wake_pipe_read;
#else
    r->sockets[0].sock = (socket_t)r->wake_pipe[0];
    r->pollfds[0].fd = r->wake_pipe[0];
#endif
    r->sockets[0].callback = NULL;  /* No callback, just drains */
    r->sockets[0].context = r;
    bitarray_set(&r->sockets[0].enabled, REACTOR_EVENT_CAN_READ);

    r->pollfds[0].events = POLLIN;
    r->pollfds[0].revents = 0;

    r->active_socket_count = 1;  /* Start with wake pipe */

    /* Initialize shutdown flag */
    r->shutdown = false;

    return r;
}

void reactor_destroy(reactor_t *r) {
    if (r == NULL) {
        return;
    }

    /* Signal to stop if running */
    reactor_stop(r);

#ifdef _WIN32
    /* Close wake pipe */
    closesocket(r->wake_pipe_read);
    closesocket(r->wake_pipe_write);

    DeleteCriticalSection(&r->lock);
#else
    /* Close wake pipe */
    close(r->wake_pipe[0]);
    close(r->wake_pipe[1]);

    pthread_mutex_destroy(&r->lock);
#endif

    free(r->pollfds);
    free(r->sockets);
    free(r);
}

util_err_t reactor_add_socket(reactor_t *r, socket_t sock,
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

    /* set socket to non-blocking for reactor use */
    if((rc = socket_set_nonblocking(sock, true)) != UTIL_OK) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to set socket fd=%d non-blocking.  Error %s.", sock, util_err_str(rc));
        return rc;
    }

    if((rc = socket_set_nodelay(sock, true)) != UTIL_OK) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to set socket fd=%d no-delay.  Error %s.", sock, util_err_str(rc));
        return rc;
    }

    /* Detect socket type */
    socket_type_t sock_type = get_socket_type(sock);

    int mutex_rc = 0;

#ifdef _WIN32
    mutex_rc = EnterCriticalSection(&r->lock);
    if(mutex_rc == 0) {
        rc = util_err_from_wsa(GetLastError());
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to add socket fd=%d.  Error %s.", sock, util_err_str(rc));
        return rc;
    }
#else
    mutex_rc = pthread_mutex_lock(&r->lock);
    if(mutex_rc != 0) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to add socket fd=%d.  Error %s.", sock, util_err_str(util_err_from_errno(mutex_rc)));
        return util_err_from_errno(mutex_rc);
    }
#endif

    rc = UTIL_ERESOURCE;

    /* Find first available slot, skipping index 0 (reserved for wake pipe) */
    for (size_t i = 1; i < r->max_sockets; i++) {
        if (r->sockets[i].sock == INVALID_SOCKET) {
            pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Adding socket fd=%d at index %zu", sock, i);
            r->sockets[i].sock = sock;
            r->sockets[i].callback = cb;
            r->sockets[i].context = ctx;
            r->sockets[i].socket_type = sock_type;
            r->sockets[i].connected = (sock_type == SOCKET_TYPE_DGRAM);  /* UDP is always "connected" */

            /* Use provided initial events, or enable all if not provided */
            if (initial_events != NULL) {
                r->sockets[i].enabled = *initial_events;
                pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Set initial event mask for socket at index %zu", i);
            } else {
                bitarray_set_all(&r->sockets[i].enabled);  /* All events enabled by default */
                pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Enabled all events for socket at index %zu", i);
            }

            bitarray_clear_all(&r->sockets[i].pending_events);
            memset(&r->sockets[i].last_revents, 0, sizeof(r->sockets[i].last_revents));

            r->pollfds[i].fd = sock;
            r->pollfds[i].events = 0;
            r->pollfds[i].revents = 0;

            /* Rebuild poll events */
            rebuild_pollfds_for_socket(r, i);

            r->active_socket_count++;

            rc = UTIL_OK;

            break;
        }
    }

#ifdef _WIN32
    LeaveCriticalSection(&r->lock);
#else
    pthread_mutex_unlock(&r->lock);
#endif

    if(rc != UTIL_OK) {
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "No available slots to add socket fd=%d", sock);
    }

    return rc;
}

util_err_t reactor_remove_socket(reactor_t *r, socket_t sock) {
    if (r == NULL || sock == INVALID_SOCKET) {
        return UTIL_EINVAL;
    }

    int mutex_rc = 0;

#ifdef _WIN32
    mutex_rc = EnterCriticalSection(&r->lock);
    if(mutex_rc == 0) {
        util_err_t rc = util_err_from_wsa(GetLastError());
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to remove socket fd=%d.  Error %s.", sock, util_err_str(rc));
        return rc;
    }
#else
    mutex_rc = pthread_mutex_lock(&r->lock);
    if(mutex_rc != 0) {
        util_err_t rc = util_err_from_errno(mutex_rc);
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to remove socket fd=%d.  Error %s.", sock, util_err_str(rc));
        return rc;
    }
#endif

    /* Find socket, skipping index 0 (reserved for wake pipe) */
    for (size_t i = 1; i < r->max_sockets; i++) {
        if (r->sockets[i].sock == sock) {
            pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "reactor_remove_socket: Removing socket fd=%d from index %zu", sock, i);
            r->sockets[i].sock = INVALID_SOCKET;
            r->sockets[i].callback = NULL;
            r->sockets[i].context = NULL;
            bitarray_clear_all(&r->sockets[i].pending_events);
            bitarray_clear_all(&r->sockets[i].enabled);

            r->pollfds[i].fd = INVALID_SOCKET;
            r->pollfds[i].events = 0;
            r->pollfds[i].revents = 0;

            r->active_socket_count--;

#ifdef _WIN32
            LeaveCriticalSection(&r->lock);
#else
            pthread_mutex_unlock(&r->lock);
#endif
            return UTIL_OK;
        }
    }

#ifdef _WIN32
    LeaveCriticalSection(&r->lock);
#else
    pthread_mutex_unlock(&r->lock);
#endif
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

    int mutex_rc = 0;

    /* Time lock acquisition start*/
    t0 = util_time_us();
#ifdef _WIN32
    mutex_rc = EnterCriticalSection(&r->lock);
    if(mutex_rc == 0) {
        int rc = util_err_from_wsa(GetLastError());
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to set event mask for socket fd=%d.  Error %s.", sock, util_err_str(rc));
        return rc;
    }
#else
    mutex_rc = pthread_mutex_lock(&r->lock);
    if(mutex_rc != 0) {
        util_err_t rc = util_err_from_errno(mutex_rc);
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to set event mask for socket fd=%d.  Error %s.", sock, util_err_str(rc));
        return rc;
    }
#endif

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

            /* Time log_detail */
            t0 = util_time_us();
            pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_SPEW, "Found socket fd=%d at index %zu", sock, i);
            t1 = util_time_us();
            g_set_mask_stats.log_time_us += (t1 - t0);

            /* Atomically replace the entire event mask */
            r->sockets[i].enabled = event_mask;

            /* Time rebuild */
            t0 = util_time_us();
            rebuild_pollfds_for_socket(r, i);
            t1 = util_time_us();
            g_set_mask_stats.rebuild_time_us += (t1 - t0);

            /* Time second log_detail */
            t0 = util_time_us();
            pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_SPEW, "Rebuilt events for socket at index %zu, new events=0x%x", i, r->pollfds[i].events);
            t1 = util_time_us();
            g_set_mask_stats.log_time_us += (t1 - t0);

            /* Time unlock */
            t0 = util_time_us();
#ifdef _WIN32
            LeaveCriticalSection(&r->lock);
#else
            pthread_mutex_unlock(&r->lock);
#endif
            t1 = util_time_us();
            g_set_mask_stats.unlock_time_us += (t1 - t0);

            /* Time wake pipe */
            t0 = util_time_us();
            tickle_wake_pipe(r);
            t1 = util_time_us();
            g_set_mask_stats.wake_time_us += (t1 - t0);

            return UTIL_OK;
        }
    }

    t1 = util_time_us();
    g_set_mask_stats.search_time_us += (t1 - t0);

#ifdef _WIN32
    LeaveCriticalSection(&r->lock);
#else
    pthread_mutex_unlock(&r->lock);
#endif
    return UTIL_ENOTFOUND;
}


util_err_t reactor_stop(reactor_t *r) {
    if (r == NULL) {
        return UTIL_EINVAL;
    }

    int mutex_rc = 0;

#ifdef _WIN32
    if((mutex_rc = EnterCriticalSection(&r->lock)) == 0) {
        int rc = util_err_from_wsa(GetLastError());
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to stop reactor.  Error %s.", util_err_str(rc));
        return rc;
    }
#else
    mutex_rc = pthread_mutex_lock(&r->lock);
    if(mutex_rc != 0) {
        util_err_t rc = util_err_from_errno(mutex_rc);
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to stop reactor.  Error %s.", util_err_str(rc));
        return rc;
    }
#endif

    r->shutdown = true;

#ifdef _WIN32
    LeaveCriticalSection(&r->lock);
#else
    pthread_mutex_unlock(&r->lock);
#endif

    tickle_wake_pipe(r);
    return UTIL_OK;
}

util_err_t reactor_wake(reactor_t *r) {
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
    int mutex_rc = 0;

    if (r == NULL) {
        return UTIL_EINVAL;
    }

    /* Main event loop: runs continuously until reactor_stop() is called */
    while (!r->shutdown) {
        /* Use poll_timeout_ms directly for TICK event generation */
        int timeout = (int)poll_timeout_ms;

        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Calling poll() with %zu sockets, timeout=%dms", r->active_socket_count, timeout);

        /* Log poll fd setup for listener sockets (typically indices 1 and 2) */
        for (size_t i = 1; i < r->active_socket_count && i < 3; i++) {
            if (r->sockets[i].sock != INVALID_SOCKET) {
                bool has_events = bitarray_has_any(&r->sockets[i].enabled);
                pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_DETAIL, "Socket[%zu] fd=%d events=0x%x has_enabled=%d", i, r->pollfds[i].fd, r->pollfds[i].events, has_events ? 1 : 0);
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

        /* Detect timeout to raise TICK events */
        bool poll_timeout_expired = (ret == 0);

        /* Time translate_pollevents */
        t0 = util_time_us();

        /* Translate poll results to pending events */
        for (size_t i = 0; i < r->active_socket_count; i++) {
            if (i == 0) {
                /* Wake pipe: special handling */
                if (r->pollfds[i].revents & POLLIN) {
                    drain_wake_pipe(r);
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
#ifdef _WIN32
            if((mutex_rc = EnterCriticalSection(&r->lock)) == 0) {
                int rc = util_err_from_wsa(GetLastError());
                pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to set TICK events.  Error %s.", util_err_str(rc));
                return rc;
            }
#else
            if((mutex_rc = pthread_mutex_lock(&r->lock)) != 0) {
                util_err_t rc = util_err_from_errno(mutex_rc);
                pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to set TICK events.  Error %s.", util_err_str(rc));
                return rc;
            }
#endif
            for (size_t i = 1; i < r->max_sockets; i++) {
                if (r->sockets[i].sock != INVALID_SOCKET && bitarray_test(&r->sockets[i].enabled, REACTOR_EVENT_TICK)) {
                    bitarray_set(&r->sockets[i].pending_events, REACTOR_EVENT_TICK);
                }
            }
#ifdef _WIN32
            LeaveCriticalSection(&r->lock);
#else
            pthread_mutex_unlock(&r->lock);
#endif
        }

        /* Time event delivery */
        t0 = util_time_us();

        /* Deliver pending events in priority order */
        deliver_pending_events(r);

        t1 = util_time_us();
        g_reactor_stats.total_deliver_time_us += (t1 - t0);
    }

    /* After shutdown, deliver SHUTDOWN events to all sockets */
#ifdef _WIN32
    if((mutex_rc = EnterCriticalSection(&r->lock)) == 0) {
        int rc = util_err_from_wsa(GetLastError());
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to set SHUTDOWN events.  Error %s.", util_err_str(rc));
        return rc;
    }
#else
    if((mutex_rc = pthread_mutex_lock(&r->lock)) != 0) {
        util_err_t rc = util_err_from_errno(mutex_rc);
        pdlog(LOG_MODULE_REACTOR, LOG_LEVEL_WARN, "Failed to acquire reactor lock to set SHUTDOWN events.  Error %s.", util_err_str(rc));
        return rc;
    }
#endif

    for (size_t i = 1; i < r->max_sockets; i++) {
        if (r->sockets[i].sock != INVALID_SOCKET) {
            bitarray_set(&r->sockets[i].pending_events, REACTOR_EVENT_SHUTDOWN);
        }
    }

#ifdef _WIN32
    LeaveCriticalSection(&r->lock);
#else
    pthread_mutex_unlock(&r->lock);
#endif

    deliver_pending_events(r);

    return UTIL_OK;
}
