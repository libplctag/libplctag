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

    /* POLLERR - socket error */
    if ((current & POLLERR) && !(previous & POLLERR)) {
        /* Error occurred (transition) */
#ifdef _WIN32
        int optval = 0;
        int optlen = sizeof(optval);
        if (getsockopt((SOCKET)entry->sock, SOL_SOCKET, SO_ERROR, (char *)&optval, &optlen) != SOCKET_ERROR && optval != 0) {
            entry->last_error = util_err_from_errno(optval);
        }
#else
        int optval = 0;
        socklen_t optlen = sizeof(optval);
        if (getsockopt(entry->sock, SOL_SOCKET, SO_ERROR, &optval, &optlen) == 0 && optval != 0) {
            entry->last_error = util_err_from_errno(optval);
        }
#endif
        bitarray_set(&entry->pending_events, REACTOR_EVENT_ERROR);
        /* Store error status for delivery - we'll handle this in deliver_pending_events */
        /* For now, queue the error with OK status; will be updated in deliver function */
        return;
    }

    /* POLLHUP - peer closed (graceful close) */
    if ((current & POLLHUP) && !(previous & POLLHUP)) {
        bitarray_set(&entry->pending_events, REACTOR_EVENT_CLOSED);
        return;
    }

    /* POLLIN - readable or acceptable (edge-triggered) */
    if ((current & POLLIN) && !(previous & POLLIN)) {
        if (bitarray_test(&entry->enabled, REACTOR_EVENT_CAN_READ)) {
            bitarray_set(&entry->pending_events, REACTOR_EVENT_CAN_READ);
        }
        if (bitarray_test(&entry->enabled, REACTOR_EVENT_CAN_ACCEPT)) {
            bitarray_set(&entry->pending_events, REACTOR_EVENT_CAN_ACCEPT);
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
                /* Connect failed - raise ERROR with the connection error */
                bitarray_set(&entry->pending_events, REACTOR_EVENT_ERROR);
                /* Store the error status somehow... need to rethink this */
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
        entry->callback(r, entry->sock, event, status, entry->context);
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
    send(r->wake_pipe_write, (const char *)&byte, 1, 0);
#else
    uint8_t byte = 0;
    write(r->wake_pipe[1], &byte, 1);
#endif
    /* Fire and forget - errors don't matter */
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

    /* Allocate socket registry */
    r->sockets = (socket_entry_t *)malloc(max_sockets * sizeof(socket_entry_t));
    if (r->sockets == NULL) {
        free(r);
        return NULL;
    }
    memset(r->sockets, 0, max_sockets * sizeof(socket_entry_t));

    /* Initialize all socket fields to INVALID_SOCKET */
    for (size_t i = 0; i < max_sockets; i++) {
        r->sockets[i].sock = INVALID_SOCKET;
    }

    /* Allocate poll array */
    r->pollfds = (struct pollfd *)malloc(max_sockets * sizeof(struct pollfd));
    if (r->pollfds == NULL) {
        free(r->sockets);
        free(r);
        return NULL;
    }
    memset(r->pollfds, 0, max_sockets * sizeof(struct pollfd));

    r->max_sockets = max_sockets;

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

    /* Set non-blocking */
    u_long mode = 1;
    ioctlsocket(listen_sock, FIONBIO, &mode);

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
    int flags = fcntl(r->wake_pipe[0], F_GETFL, 0);
    if (flags == -1 || fcntl(r->wake_pipe[0], F_SETFL, flags | O_NONBLOCK) == -1) {
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
    if (r == NULL || sock == INVALID_SOCKET || cb == NULL) {
        return UTIL_EINVAL;
    }

    /* Set socket to non-blocking for reactor use */
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) {
        return util_err_from_wsa(WSAGetLastError());
    }
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) {
        return util_err_from_errno(errno);
    }
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        return util_err_from_errno(errno);
    }
#endif

    /* Detect socket type */
    socket_type_t sock_type = get_socket_type(sock);

#ifdef _WIN32
    EnterCriticalSection(&r->lock);
#else
    pthread_mutex_lock(&r->lock);
#endif

    /* Find first available slot, skipping index 0 (reserved for wake pipe) */
    for (size_t i = 1; i < r->max_sockets; i++) {
        if (r->sockets[i].sock == INVALID_SOCKET) {
            log_detail("reactor_add_socket: Adding socket fd=%d at index %zu", sock, i);
            r->sockets[i].sock = sock;
            r->sockets[i].callback = cb;
            r->sockets[i].context = ctx;
            r->sockets[i].socket_type = sock_type;
            r->sockets[i].connected = (sock_type == SOCKET_TYPE_DGRAM);  /* UDP is always "connected" */

            /* Use provided initial events, or enable all if not provided */
            if (initial_events != NULL) {
                r->sockets[i].enabled = *initial_events;
                log_detail("reactor_add_socket: Set initial event mask for socket at index %zu", i);
            } else {
                bitarray_set_all(&r->sockets[i].enabled);  /* All events enabled by default */
                log_detail("reactor_add_socket: Enabled all events for socket at index %zu", i);
            }

            bitarray_clear_all(&r->sockets[i].pending_events);
            memset(&r->sockets[i].last_revents, 0, sizeof(r->sockets[i].last_revents));

            r->pollfds[i].fd = sock;
            r->pollfds[i].events = 0;
            r->pollfds[i].revents = 0;

            /* Rebuild poll events */
            rebuild_pollfds_for_socket(r, i);

            r->active_socket_count++;

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
    return UTIL_ERESOURCE;
}

util_err_t reactor_remove_socket(reactor_t *r, socket_t sock) {
    if (r == NULL || sock == INVALID_SOCKET) {
        return UTIL_EINVAL;
    }

#ifdef _WIN32
    EnterCriticalSection(&r->lock);
#else
    pthread_mutex_lock(&r->lock);
#endif

    /* Find socket, skipping index 0 (reserved for wake pipe) */
    for (size_t i = 1; i < r->max_sockets; i++) {
        if (r->sockets[i].sock == sock) {
            log_detail("reactor_remove_socket: Removing socket fd=%d from index %zu", sock, i);
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

util_err_t reactor_set_event_mask(reactor_t *r, socket_t sock, bitarray_t event_mask) {
    if (r == NULL || sock == INVALID_SOCKET) {
        return UTIL_EINVAL;
    }

#ifdef _WIN32
    EnterCriticalSection(&r->lock);
#else
    pthread_mutex_lock(&r->lock);
#endif

    /* Find socket, skipping index 0 (reserved for wake pipe) */
    for (size_t i = 1; i < r->max_sockets; i++) {
        if (r->sockets[i].sock == sock) {
            log_detail("reactor_set_event_mask: Found socket fd=%d at index %zu", sock, i);
            /* Atomically replace the entire event mask */
            r->sockets[i].enabled = event_mask;

            /* Rebuild poll events for this socket */
            rebuild_pollfds_for_socket(r, i);
            log_detail("reactor_set_event_mask: Rebuilt events for socket at index %zu, new events=0x%x", i, r->pollfds[i].events);

#ifdef _WIN32
            LeaveCriticalSection(&r->lock);
#else
            pthread_mutex_unlock(&r->lock);
#endif

            /* Wake the reactor to restart poll() with the new event mask */
            tickle_wake_pipe(r);

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


util_err_t reactor_stop(reactor_t *r) {
    if (r == NULL) {
        return UTIL_EINVAL;
    }

#ifdef _WIN32
    EnterCriticalSection(&r->lock);
#else
    pthread_mutex_lock(&r->lock);
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
    if (r == NULL) {
        return UTIL_EINVAL;
    }

    /* Main event loop: runs continuously until reactor_stop() is called */
    while (!r->shutdown) {
        /* Use poll_timeout_ms directly for TICK event generation */
        int timeout = (int)poll_timeout_ms;

        log_detail("reactor_run: Calling poll() with %zu sockets, timeout=%dms", r->active_socket_count, timeout);

        /* Log poll fd setup for listener sockets (typically indices 1 and 2) */
        for (size_t i = 1; i < r->active_socket_count && i < 3; i++) {
            if (r->sockets[i].sock != INVALID_SOCKET) {
                bool has_events = bitarray_has_any(&r->sockets[i].enabled);
                log_detail("reactor_run: Socket[%zu] fd=%d events=0x%x has_enabled=%d", i, r->pollfds[i].fd, r->pollfds[i].events, has_events ? 1 : 0);
            }
        }

        /* Poll for socket activity */
#ifdef _WIN32
        int ret = WSAPoll(r->pollfds, (ULONG)r->active_socket_count, timeout);
        if (ret == SOCKET_ERROR) {
            return util_err_from_wsa(WSAGetLastError());
        }
#else
        int ret = poll(r->pollfds, (nfds_t)r->active_socket_count, timeout);
        if (ret < 0) {
            log_error("reactor_run: poll() returned error: %d (errno=%d)", ret, errno);
            return util_err_from_errno(errno);
        }
#endif

        log_detail("reactor_run: poll() returned %d ready sockets", ret);

        /* Detect timeout to raise TICK events */
        bool poll_timeout_expired = (ret == 0);

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

        /* Raise TICK events for all sockets with TICK enabled (if poll timed out) */
        if (poll_timeout_expired) {
#ifdef _WIN32
            EnterCriticalSection(&r->lock);
#else
            pthread_mutex_lock(&r->lock);
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

        /* Deliver pending events in priority order */
        deliver_pending_events(r);
    }

    /* After shutdown, deliver SHUTDOWN events to all sockets */
#ifdef _WIN32
    EnterCriticalSection(&r->lock);
#else
    pthread_mutex_lock(&r->lock);
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
