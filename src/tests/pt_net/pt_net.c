/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever     *
 * you choose.                                                             *
 *                                                                         *
 * MPL 2.0:                                                                *
 *                                                                         *
 *   This Source Code Form is subject to the terms of the Mozilla Public   *
 *   License, v. 2.0. If a copy of the MPL was not distributed with this   *
 *   file, You can obtain one at http://mozilla.org/MPL/2.0/.              *
 *                                                                         *
 *                                                                         *
 * LGPL 2:                                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "pt_net.h"
#include "../utils/buf.h"
#include "../utils/err.h"
#include "../utils/utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*******************************************************************************
 *
 * PLATFORM DETECTION AND INCLUDES
 *
 *******************************************************************************/

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <mswsock.h>
    #pragma comment(lib, "ws2_32.lib")

    typedef SOCKET pt_net_socket_fd_t;
    #define INVALID_SOCKET_FD INVALID_SOCKET
    #define SOCKET_FD_CAST(x) (x)

#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <poll.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <errno.h>
    #include <signal.h>

    typedef int pt_net_socket_fd_t;
    #define INVALID_SOCKET_FD (-1)
    #define SOCKET_FD_CAST(x) (x)
    #define closesocket(x) close(x)

    /* Platform-specific defines */
    #if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        #define PT_NET_BSD 1
    #endif
#endif

/*******************************************************************************
 *
 * INTERNAL TYPE DEFINITIONS
 *
 *******************************************************************************/

typedef enum {
    PT_NET_SOCKET_UNUSED,
    PT_NET_SOCKET_TCP_LISTENER,
    PT_NET_SOCKET_TCP_CLIENT,
    PT_NET_SOCKET_UDP,
} pt_net_socket_type_t;

typedef enum {
    PT_NET_THREAD_RUNNABLE,
    PT_NET_THREAD_WAITING_IO,
    PT_NET_THREAD_WAITING_TIMER,
    PT_NET_THREAD_SUSPENDED,
    PT_NET_THREAD_COMPLETED,
} pt_net_thread_state_t;

typedef enum {
    PT_NET_WAIT_READ,
    PT_NET_WAIT_WRITE,
    PT_NET_WAIT_ACCEPT,
} pt_net_wait_type_t;

/*******************************************************************************
 *
 * INTERNAL STRUCTURE DEFINITIONS
 *
 *******************************************************************************/

/**
 * Socket structure - internal representation
 */
struct pt_net_socket_s {
    /* File descriptor */
    pt_net_socket_fd_t fd;
    pt_net_socket_type_t type;

    /* Reference to core */
    pt_net_core_t *core;

    /* Framer for TCP sockets */
    pt_net_framer_fn framer;
    void *framer_context;

    /* Receive buffer for framing */
    uint8_t *recv_buf_data;
    buf_t recv_buf;
    size_t recv_buf_capacity;

    /* Wait queue - threads waiting on this socket */
    pt_net_thread_s *waiting_threads;

    /* Socket state */
    bool nonblocking;
    bool connected;
    pt_net_addr_t local_addr;
    pt_net_addr_t remote_addr;
};

/**
 * Thread structure - internal representation
 */
struct pt_net_thread_s {
    /* Protothread continuation state */
    int line;
    bool suspended;
    bool cancelled;

    /* Reference to core */
    pt_net_core_t *core;

    /* Function and context */
    util_err_t (*func)(pt_net_thread_t *, void *);
    void *context;
    const char *name;

    /* Execution state */
    pt_net_thread_state_t state;
    util_err_t exit_status;

    /* What this thread is waiting on */
    pt_net_socket_s *waiting_socket;
    pt_net_wait_type_t wait_type;

    /* Timer expiry (if waiting on timer) */
    uint64_t timer_expiry_ms;

    /* Linked list pointers */
    pt_net_thread_s *next_waiting;
};

/**
 * Core reactor structure
 */
struct pt_net_core_s {
    /* Socket pool */
    pt_net_socket_t *sockets;
    size_t socket_count;
    size_t socket_capacity;

    /* Protothread pool */
    pt_net_thread_t *threads;
    size_t thread_count;
    size_t thread_capacity;

    /* Poll array (rebuilt each iteration) */
    struct pollfd *pollfds;
    size_t pollfd_count;
    size_t pollfd_capacity;

    /* Timer thread list */
    pt_net_thread_t *timer_threads;

    /* Control flags */
    bool shutdown;
    bool initialized;
};

/*******************************************************************************
 *
 * CORE LIFECYCLE FUNCTIONS
 *
 *******************************************************************************/

/**
 * Create and initialize the reactor core
 */
util_err_t pt_net_core_create(pt_net_core_t **core, size_t max_socks, size_t max_pts)
{
    if (!core) {
        return UTIL_EINVAL;
    }

    /* Ensure reasonable limits */
    if (max_socks == 0 || max_pts == 0) {
        return UTIL_EINVAL;
    }
    if (max_socks > 10000 || max_pts > 10000) {
        return UTIL_EINVAL;
    }

    /* Allocate core */
    pt_net_core_t *c = calloc(1, sizeof(pt_net_core_t));
    if (!c) {
        return UTIL_ERESOURCE;
    }

    /* Allocate socket pool */
    c->sockets = calloc(max_socks, sizeof(pt_net_socket_t));
    if (!c->sockets) {
        free(c);
        return UTIL_ERESOURCE;
    }

    /* Allocate thread pool */
    c->threads = calloc(max_pts, sizeof(pt_net_thread_t));
    if (!c->threads) {
        free(c->sockets);
        free(c);
        return UTIL_ERESOURCE;
    }

    /* Allocate poll array (needs space for all sockets) */
    c->pollfds = calloc(max_socks, sizeof(struct pollfd));
    if (!c->pollfds) {
        free(c->threads);
        free(c->sockets);
        free(c);
        return UTIL_ERESOURCE;
    }

    /* Initialize core fields */
    c->socket_capacity = max_socks;
    c->socket_count = 0;
    c->thread_capacity = max_pts;
    c->thread_count = 0;
    c->pollfd_capacity = max_socks;
    c->pollfd_count = 0;
    c->timer_threads = NULL;
    c->shutdown = false;
    c->initialized = true;

#ifdef _WIN32
    /* Initialize Winsock */
    WSADATA wsa_data;
    int wsa_err = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (wsa_err != 0) {
        free(c->pollfds);
        free(c->threads);
        free(c->sockets);
        free(c);
        return util_err_from_wsa(wsa_err);
    }
#endif

    *core = c;
    return UTIL_OK;
}

/**
 * Shutdown and free the reactor core
 */
void pt_net_core_shutdown(pt_net_core_t *core)
{
    if (!core || !core->initialized) {
        return;
    }

    /* Close all sockets */
    for (size_t i = 0; i < core->socket_count; i++) {
        pt_net_socket_t *sock = &core->sockets[i];
        if (sock->type != PT_NET_SOCKET_UNUSED && sock->fd != INVALID_SOCKET_FD) {
            closesocket(sock->fd);
            if (sock->recv_buf_data) {
                free(sock->recv_buf_data);
            }
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif

    /* Free pools */
    free(core->pollfds);
    free(core->threads);
    free(core->sockets);
    free(core);
}

/*******************************************************************************
 *
 * SOCKET LIFECYCLE HELPERS
 *
 *******************************************************************************/

/**
 * Find a free socket slot in the pool
 */
static pt_net_socket_t* socket_alloc(pt_net_core_t *core)
{
    if (core->socket_count >= core->socket_capacity) {
        return NULL;
    }

    pt_net_socket_t *sock = &core->sockets[core->socket_count];
    core->socket_count++;

    /* Initialize to unused */
    memset(sock, 0, sizeof(*sock));
    sock->type = PT_NET_SOCKET_UNUSED;
    sock->fd = INVALID_SOCKET_FD;
    sock->core = core;

    return sock;
}

/**
 * Set a socket to non-blocking mode
 */
static util_err_t set_nonblocking(pt_net_socket_fd_t fd)
{
#ifdef _WIN32
    unsigned long mode = 1;
    if (ioctlsocket(fd, FIONBIO, &mode) != 0) {
        return util_err_from_wsa(WSAGetLastError());
    }
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return util_err_from_errno(errno);
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return util_err_from_errno(errno);
    }
#endif
    return UTIL_OK;
}

/**
 * Set SIGPIPE handling (for POSIX)
 */
static util_err_t disable_sigpipe(pt_net_socket_fd_t fd)
{
#ifdef PT_NET_BSD
    /* BSD: use SO_NOSIGPIPE */
    int nosigpipe = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&nosigpipe, sizeof(nosigpipe)) < 0) {
        return util_err_from_errno(errno);
    }
#elif !defined(_WIN32)
    /* Linux: we'll use MSG_NOSIGNAL in send calls, no socket option needed */
#endif
    return UTIL_OK;
}

/*******************************************************************************
 *
 * ADDRESS FUNCTIONS
 *
 *******************************************************************************/

/**
 * Initialize an address structure
 */
util_err_t pt_net_init_addr(pt_net_addr_t *addr, const char *host, uint16_t port)
{
    if (!addr) {
        return UTIL_EINVAL;
    }

    /* Initialize structure */
    memset(addr, 0, sizeof(*addr));

    /* Set port */
    addr->port = port;

    /* Set host */
    if (host && *host) {
        /* Copy host string (bounded) */
        size_t len = strlen(host);
        if (len >= sizeof(addr->host)) {
            return UTIL_EINVAL;
        }
        strcpy(addr->host, host);
    } else {
        /* Empty string = INADDR_ANY */
        addr->host[0] = '\0';
    }

    return UTIL_OK;
}

/**
 * Get local address of a socket
 */
util_err_t pt_net_get_local_socket_addr(pt_net_addr_t *addr, pt_net_socket_t *sock)
{
    if (!addr || !sock) {
        return UTIL_EINVAL;
    }

    struct sockaddr_in sa;
    socklen_t sa_len = sizeof(sa);

    if (getsockname(sock->fd, (struct sockaddr *)&sa, &sa_len) < 0) {
#ifdef _WIN32
        return util_err_from_wsa(WSAGetLastError());
#else
        return util_err_from_errno(errno);
#endif
    }

    *addr = sock->local_addr;
    return UTIL_OK;
}

/**
 * Get remote address of a socket
 */
util_err_t pt_net_get_remote_socket_addr(pt_net_addr_t *addr, pt_net_socket_t *sock)
{
    if (!addr || !sock) {
        return UTIL_EINVAL;
    }

    struct sockaddr_in sa;
    socklen_t sa_len = sizeof(sa);

    if (getpeername(sock->fd, (struct sockaddr *)&sa, &sa_len) < 0) {
#ifdef _WIN32
        return util_err_from_wsa(WSAGetLastError());
#else
        return util_err_from_errno(errno);
#endif
    }

    *addr = sock->remote_addr;
    return UTIL_OK;
}

/*******************************************************************************
 *
 * GENERIC SOCKET FUNCTIONS
 *
 *******************************************************************************/

/**
 * Close a socket
 */
util_err_t pt_net_close_socket(pt_net_socket_t *sock)
{
    if (!sock) {
        return UTIL_EINVAL;
    }

    if (sock->fd != INVALID_SOCKET_FD) {
        closesocket(sock->fd);
        sock->fd = INVALID_SOCKET_FD;
    }

    if (sock->recv_buf_data) {
        free(sock->recv_buf_data);
        sock->recv_buf_data = NULL;
    }

    sock->type = PT_NET_SOCKET_UNUSED;
    return UTIL_OK;
}

/*******************************************************************************
 *
 * PROTOTHREAD FUNCTIONS
 *
 *******************************************************************************/

/**
 * Spawn a new protothread
 */
util_err_t pt_net_spawn_thread(
    pt_net_thread_t **pt,
    pt_net_core_t *core,
    util_err_t (*func)(pt_net_thread_t *, void *),
    void *context,
    const char *name
)
{
    if (!core || !func) {
        return UTIL_EINVAL;
    }

    if (core->thread_count >= core->thread_capacity) {
        return UTIL_ERESOURCE;
    }

    pt_net_thread_t *thread = &core->threads[core->thread_count];
    core->thread_count++;

    /* Initialize thread */
    memset(thread, 0, sizeof(*thread));
    thread->core = core;
    thread->func = func;
    thread->context = context;
    thread->name = name;
    thread->state = PT_NET_THREAD_RUNNABLE;
    thread->line = 0;
    thread->exit_status = UTIL_OK;

    if (pt) {
        *pt = thread;
    }

    return UTIL_OK;
}

/**
 * Wake a suspended thread
 */
util_err_t pt_net_wake_thread(pt_net_thread_t *pt)
{
    if (!pt) {
        return UTIL_EINVAL;
    }

    pt->suspended = false;
    pt->state = PT_NET_THREAD_RUNNABLE;
    return UTIL_OK;
}

/**
 * Request cancellation of a thread
 */
util_err_t pt_net_cancel_thread(pt_net_thread_t *pt)
{
    if (!pt) {
        return UTIL_EINVAL;
    }

    pt->cancelled = true;
    return UTIL_OK;
}

/**
 * Check if cancellation was requested
 */
bool pt_net_is_cancelled(pt_net_thread_t *this_pt)
{
    if (!this_pt) {
        return false;
    }

    return this_pt->cancelled;
}

/**
 * Try to join a thread (returns UTIL_EAGAIN if not done yet)
 */
util_err_t pt_net_try_join_thread(pt_net_thread_t *pt)
{
    if (!pt) {
        return UTIL_EINVAL;
    }

    if (pt->state == PT_NET_THREAD_COMPLETED) {
        return pt->exit_status;
    }

    return UTIL_EAGAIN;
}

/*******************************************************************************
 *
 * TIMER FUNCTIONS
 *
 *******************************************************************************/

/**
 * Try to sleep for a duration (returns UTIL_EAGAIN if would block)
 */
util_err_t pt_net_try_sleep(pt_net_thread_t *this_pt, uint32_t ms)
{
    if (!this_pt) {
        return UTIL_EINVAL;
    }

    /* Calculate expiry time */
    uint64_t now = util_get_time_ms();
    this_pt->timer_expiry_ms = now + ms;

    /* Set state and add to timer list */
    this_pt->state = PT_NET_THREAD_WAITING_TIMER;

    /* Add to front of timer list */
    this_pt->next_waiting = this_pt->core->timer_threads;
    this_pt->core->timer_threads = this_pt;

    return UTIL_EAGAIN;
}

/*******************************************************************************
 *
 * REACTOR LOOP HELPERS (declared forward)
 *
 *******************************************************************************/

static util_err_t build_pollfds(pt_net_core_t *core);
static int calculate_next_timeout(pt_net_core_t *core);
static void resume_io_waiters(pt_net_core_t *core);
static void resume_timer_waiters(pt_net_core_t *core);
static void run_all_runnable(pt_net_core_t *core);

/*******************************************************************************
 *
 * REACTOR LOOP
 *
 *******************************************************************************/

/**
 * Run the reactor loop
 */
util_err_t pt_net_core_run(pt_net_core_t *core)
{
    if (!core || !core->initialized) {
        return UTIL_EINVAL;
    }

    while (!core->shutdown) {
        /* Build poll array */
        util_err_t err = build_pollfds(core);
        if (err != UTIL_OK) {
            return err;
        }

        /* Calculate timeout */
        int timeout_ms = calculate_next_timeout(core);
        if (timeout_ms > 100 || timeout_ms < 0) {
            timeout_ms = 100;
        }

        /* Poll for events */
#ifdef _WIN32
        int ret = WSAPoll(core->pollfds, (ULONG)core->pollfd_count, timeout_ms);
        if (ret == SOCKET_ERROR) {
            return util_err_from_wsa(WSAGetLastError());
        }
#else
        int ret = poll(core->pollfds, core->pollfd_count, timeout_ms);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return util_err_from_errno(errno);
        }
#endif

        /* Resume threads waiting on I/O */
        resume_io_waiters(core);

        /* Resume threads with expired timers */
        resume_timer_waiters(core);

        /* Clear poll array for next iteration */
        core->pollfd_count = 0;

        /* Run all runnable threads */
        run_all_runnable(core);
    }

    return UTIL_OK;
}

/**
 * Build the poll array from sockets with waiting threads
 */
static util_err_t build_pollfds(pt_net_core_t *core)
{
    core->pollfd_count = 0;

    for (size_t i = 0; i < core->socket_count; i++) {
        pt_net_socket_t *sock = &core->sockets[i];

        /* Skip unused sockets or sockets with no waiters */
        if (sock->type == PT_NET_SOCKET_UNUSED || !sock->waiting_threads) {
            continue;
        }

        /* Check we have space */
        if (core->pollfd_count >= core->pollfd_capacity) {
            return UTIL_ERESOURCE;
        }

        struct pollfd *pfd = &core->pollfds[core->pollfd_count];
        pfd->fd = sock->fd;
        pfd->events = 0;
        pfd->revents = 0;

        /* Determine what events we're waiting for */
        pt_net_thread_t *thread = sock->waiting_threads;
        while (thread) {
            if (thread->wait_type == PT_NET_WAIT_READ) {
                pfd->events |= POLLIN;
            } else if (thread->wait_type == PT_NET_WAIT_WRITE) {
                pfd->events |= POLLOUT;
            } else if (thread->wait_type == PT_NET_WAIT_ACCEPT) {
                pfd->events |= POLLIN;
            }
            thread = thread->next_waiting;
        }

        core->pollfd_count++;
    }

    return UTIL_OK;
}

/**
 * Calculate timeout for poll based on next timer expiry
 */
static int calculate_next_timeout(pt_net_core_t *core)
{
    if (!core->timer_threads) {
        return INT_MAX;
    }

    uint64_t now = util_get_time_ms();
    int min_timeout = INT_MAX;

    for (pt_net_thread_t *pt = core->timer_threads; pt != NULL; pt = pt->next_waiting) {
        if (pt->timer_expiry_ms <= now) {
            return 0;
        }

        int timeout = (int)(pt->timer_expiry_ms - now);
        if (timeout < min_timeout) {
            min_timeout = timeout;
        }
    }

    return min_timeout;
}

/**
 * Resume threads waiting on I/O events
 */
static void resume_io_waiters(pt_net_core_t *core)
{
    /* For each pollfd with events, wake waiting threads */
    for (size_t i = 0; i < core->pollfd_count; i++) {
        struct pollfd *pfd = &core->pollfds[i];

        if (pfd->revents == 0) {
            continue;
        }

        /* Find the socket for this pollfd */
        pt_net_socket_t *sock = NULL;
        for (size_t j = 0; j < core->socket_count; j++) {
            if (core->sockets[j].fd == pfd->fd) {
                sock = &core->sockets[j];
                break;
            }
        }

        if (!sock) {
            continue;
        }

        /* Wake threads waiting on this socket */
        pt_net_thread_t **pp = &sock->waiting_threads;
        while (*pp) {
            pt_net_thread_t *thread = *pp;

            bool should_wake = false;
            if ((thread->wait_type == PT_NET_WAIT_READ || thread->wait_type == PT_NET_WAIT_ACCEPT)
                && (pfd->revents & (POLLIN | POLLERR | POLLHUP))) {
                should_wake = true;
            }
            if (thread->wait_type == PT_NET_WAIT_WRITE && (pfd->revents & (POLLOUT | POLLERR))) {
                should_wake = true;
            }

            if (should_wake) {
                /* Remove from wait queue and mark runnable */
                *pp = thread->next_waiting;
                thread->next_waiting = NULL;
                thread->state = PT_NET_THREAD_RUNNABLE;
                thread->waiting_socket = NULL;
            } else {
                pp = &thread->next_waiting;
            }
        }
    }
}

/**
 * Resume threads with expired timers
 */
static void resume_timer_waiters(pt_net_core_t *core)
{
    uint64_t now = util_get_time_ms();
    pt_net_thread_t **pp = &core->timer_threads;

    while (*pp != NULL) {
        pt_net_thread_t *pt = *pp;

        if (pt->timer_expiry_ms <= now) {
            /* Timer expired - remove from list and mark runnable */
            *pp = pt->next_waiting;
            pt->next_waiting = NULL;
            pt->timer_expiry_ms = 0;
            pt->state = PT_NET_THREAD_RUNNABLE;
        } else {
            /* Not expired yet, keep in list */
            pp = &pt->next_waiting;
        }
    }
}

/**
 * Run all runnable threads
 */
static void run_all_runnable(pt_net_core_t *core)
{
    for (size_t i = 0; i < core->thread_count; i++) {
        pt_net_thread_t *pt = &core->threads[i];

        if (pt->state != PT_NET_THREAD_RUNNABLE) {
            continue;
        }

        /* Call the protothread function */
        util_err_t err = pt->func(pt, pt->context);

        if (err == UTIL_EAGAIN) {
            /* Thread yielded - it should have set its state */
            continue;
        }

        /* Thread completed */
        pt->state = PT_NET_THREAD_COMPLETED;
        pt->exit_status = err;
    }
}

/*******************************************************************************
 *
 * TCP SERVER FUNCTIONS
 *
 *******************************************************************************/

/**
 * Open a TCP listener socket
 */
util_err_t pt_net_open_tcp_listener(
    pt_net_socket_t **sock,
    pt_net_core_t *core,
    pt_net_addr_t *addr,
    pt_net_framer_fn framer,
    void *context
)
{
    if (!sock || !core || !addr) {
        return UTIL_EINVAL;
    }

    /* Allocate socket */
    pt_net_socket_t *s = socket_alloc(core);
    if (!s) {
        return UTIL_ERESOURCE;
    }

    /* Create socket */
    s->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s->fd == INVALID_SOCKET_FD) {
#ifdef _WIN32
        return util_err_from_wsa(WSAGetLastError());
#else
        return util_err_from_errno(errno);
#endif
    }

    s->type = PT_NET_SOCKET_TCP_LISTENER;
    s->framer = framer;
    s->framer_context = context;

    /* Set non-blocking */
    util_err_t err = set_nonblocking(s->fd);
    if (err != UTIL_OK) {
        closesocket(s->fd);
        return err;
    }

    /* Disable SIGPIPE */
    err = disable_sigpipe(s->fd);
    if (err != UTIL_OK) {
        closesocket(s->fd);
        return err;
    }

    /* Allow address reuse */
    int reuse = 1;
    setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    /* Bind socket */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(addr->port);

    if (addr->host[0] == '\0') {
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (inet_pton(AF_INET, addr->host, &sa.sin_addr) <= 0) {
            closesocket(s->fd);
            return UTIL_EINVAL;
        }
    }

    if (bind(s->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
#ifdef _WIN32
        int wsa_err = WSAGetLastError();
        closesocket(s->fd);
        return util_err_from_wsa(wsa_err);
#else
        int err_code = errno;
        closesocket(s->fd);
        return util_err_from_errno(err_code);
#endif
    }

    /* Listen */
    if (listen(s->fd, 5) < 0) {
#ifdef _WIN32
        int wsa_err = WSAGetLastError();
        closesocket(s->fd);
        return util_err_from_wsa(wsa_err);
#else
        int err_code = errno;
        closesocket(s->fd);
        return util_err_from_errno(err_code);
#endif
    }

    s->local_addr = *addr;
    *sock = s;
    return UTIL_OK;
}

/**
 * Try to accept a TCP connection
 */
util_err_t pt_net_try_accept_tcp(pt_net_socket_t **client_sock, pt_net_socket_t *listener_sock)
{
    if (!client_sock || !listener_sock) {
        return UTIL_EINVAL;
    }

    if (listener_sock->type != PT_NET_SOCKET_TCP_LISTENER) {
        return UTIL_EINVAL;
    }

    /* Allocate client socket */
    pt_net_socket_t *cs = socket_alloc(listener_sock->core);
    if (!cs) {
        return UTIL_ERESOURCE;
    }

    /* Accept connection */
    struct sockaddr_in sa;
    socklen_t sa_len = sizeof(sa);

    pt_net_socket_fd_t fd = accept(listener_sock->fd, (struct sockaddr *)&sa, &sa_len);
    if (fd == INVALID_SOCKET_FD) {
#ifdef _WIN32
        int wsa_err = WSAGetLastError();
        if (wsa_err == WSAEWOULDBLOCK) {
            return UTIL_EAGAIN;
        }
        return util_err_from_wsa(wsa_err);
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return UTIL_EAGAIN;
        }
        return util_err_from_errno(errno);
#endif
    }

    /* Set non-blocking */
    util_err_t err = set_nonblocking(fd);
    if (err != UTIL_OK) {
        closesocket(fd);
        return err;
    }

    /* Set up socket */
    cs->fd = fd;
    cs->type = PT_NET_SOCKET_TCP_CLIENT;
    cs->framer = listener_sock->framer;
    cs->framer_context = listener_sock->framer_context;
    cs->connected = true;

    /* Store remote address */
    inet_ntop(AF_INET, &sa.sin_addr, cs->remote_addr.host, sizeof(cs->remote_addr.host));
    cs->remote_addr.port = ntohs(sa.sin_port);

    /* Allocate receive buffer */
    cs->recv_buf_capacity = 8192;
    cs->recv_buf_data = malloc(cs->recv_buf_capacity);
    if (!cs->recv_buf_data) {
        closesocket(fd);
        return UTIL_ERESOURCE;
    }

    buf_init(&cs->recv_buf, cs->recv_buf_data, cs->recv_buf_capacity);

    *client_sock = cs;
    return UTIL_OK;
}

/**
 * Try to connect to a TCP server
 */
util_err_t pt_net_try_connect_tcp(pt_net_socket_t **sock, pt_net_core_t *core, pt_net_addr_t *addr)
{
    if (!sock || !core || !addr) {
        return UTIL_EINVAL;
    }

    /* Allocate socket */
    pt_net_socket_t *s = socket_alloc(core);
    if (!s) {
        return UTIL_ERESOURCE;
    }

    /* Create socket */
    s->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s->fd == INVALID_SOCKET_FD) {
#ifdef _WIN32
        return util_err_from_wsa(WSAGetLastError());
#else
        return util_err_from_errno(errno);
#endif
    }

    s->type = PT_NET_SOCKET_TCP_CLIENT;

    /* Set non-blocking */
    util_err_t err = set_nonblocking(s->fd);
    if (err != UTIL_OK) {
        closesocket(s->fd);
        return err;
    }

    /* Connect */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(addr->port);

    if (inet_pton(AF_INET, addr->host, &sa.sin_addr) <= 0) {
        closesocket(s->fd);
        return UTIL_EINVAL;
    }

    int ret = connect(s->fd, (struct sockaddr *)&sa, sizeof(sa));
    if (ret < 0) {
#ifdef _WIN32
        int wsa_err = WSAGetLastError();
        if (wsa_err == WSAEWOULDBLOCK || wsa_err == WSAEINPROGRESS) {
            /* Connection in progress - will complete when socket is writable */
            s->connected = false;
            *sock = s;
            return UTIL_EAGAIN;
        }
        closesocket(s->fd);
        return util_err_from_wsa(wsa_err);
#else
        if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
            /* Connection in progress */
            s->connected = false;
            *sock = s;
            return UTIL_EAGAIN;
        }
        closesocket(s->fd);
        return util_err_from_errno(errno);
#endif
    }

    s->connected = true;
    s->remote_addr = *addr;
    *sock = s;
    return UTIL_OK;
}

/**
 * Try to receive from TCP socket
 */
util_err_t pt_net_try_recv_tcp(buf_t *buf, pt_net_socket_t *sock, pt_net_framer_fn framer, void *context)
{
    if (!buf || !sock) {
        return UTIL_EINVAL;
    }

    if (sock->type != PT_NET_SOCKET_TCP_CLIENT) {
        return UTIL_EINVAL;
    }

    /* Try to recv into the socket's internal buffer */
    size_t space = buf_write_size(&sock->recv_buf);
    if (space == 0) {
        /* Buffer full, can't recv more */
        goto check_frame;
    }

    uint8_t *wp = buf_write_ptr(&sock->recv_buf);
    int n = recv(sock->fd, (char *)wp, (int)space, 0);

    if (n < 0) {
#ifdef _WIN32
        int wsa_err = WSAGetLastError();
        if (wsa_err == WSAEWOULDBLOCK) {
            goto check_frame;
        }
        return util_err_from_wsa(wsa_err);
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            goto check_frame;
        }
        return util_err_from_errno(errno);
#endif
    }

    if (n == 0) {
        /* Connection closed */
        return UTIL_ECLOSED;
    }

    /* Advance write cursor */
    buf_advance_write(&sock->recv_buf, n);

check_frame:
    /* Check if framer sees a complete frame */
    if (framer) {
        util_err_t err = framer(&sock->recv_buf, context);
        if (err == UTIL_OK) {
            /* Complete frame available - copy to output buffer */
            size_t available = buf_size(&sock->recv_buf);
            if (available == 0) {
                return UTIL_EAGAIN;
            }

            size_t space = buf_write_size(buf);
            if (space == 0) {
                return UTIL_EAGAIN;
            }

            size_t to_copy = available < space ? available : space;
            uint8_t *src = buf_read_ptr(&sock->recv_buf);
            uint8_t *dst = buf_write_ptr(buf);

            memcpy(dst, src, to_copy);
            buf_advance_read(&sock->recv_buf, to_copy);
            buf_advance_write(buf, to_copy);

            return UTIL_OK;
        } else if (err == UTIL_EBOUNDS) {
            /* Incomplete frame, wait for more data */
            return UTIL_EAGAIN;
        } else {
            /* Framing error */
            return err;
        }
    } else {
        /* No framer - fill output buffer from received data */
        size_t available = buf_size(&sock->recv_buf);
        if (available == 0) {
            return UTIL_EAGAIN;
        }

        size_t space = buf_write_size(buf);
        if (space == 0) {
            return UTIL_EAGAIN;
        }

        size_t to_copy = available < space ? available : space;
        uint8_t *src = buf_read_ptr(&sock->recv_buf);
        uint8_t *dst = buf_write_ptr(buf);

        memcpy(dst, src, to_copy);
        buf_advance_read(&sock->recv_buf, to_copy);
        buf_advance_write(buf, to_copy);

        return UTIL_OK;
    }
}

/**
 * Try to send on TCP socket
 */
util_err_t pt_net_try_send_tcp(buf_t *buf, pt_net_socket_t *sock)
{
    if (!buf || !sock) {
        return UTIL_EINVAL;
    }

    if (sock->type != PT_NET_SOCKET_TCP_CLIENT) {
        return UTIL_EINVAL;
    }

    size_t to_send = buf_read_size(buf);
    if (to_send == 0) {
        return UTIL_OK;
    }

    uint8_t *rp = buf_read_ptr(buf);
    int flags = 0;

#ifndef _WIN32
    flags |= MSG_NOSIGNAL;
#endif

    int n = send(sock->fd, (const char *)rp, (int)to_send, flags);

    if (n < 0) {
#ifdef _WIN32
        int wsa_err = WSAGetLastError();
        if (wsa_err == WSAEWOULDBLOCK) {
            return UTIL_EAGAIN;
        }
        return util_err_from_wsa(wsa_err);
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return UTIL_EAGAIN;
        }
        return util_err_from_errno(errno);
#endif
    }

    if (n == 0) {
        return UTIL_EAGAIN;
    }

    buf_advance_read(buf, n);

    if (buf_read_size(buf) > 0) {
        return UTIL_EAGAIN;
    }

    return UTIL_OK;
}

/*******************************************************************************
 *
 * UDP FUNCTIONS
 *
 *******************************************************************************/

/**
 * Open a UDP socket
 */
util_err_t pt_net_open_udp(
    pt_net_socket_t **sock,
    pt_net_core_t *core,
    pt_net_addr_t *bind_addr
)
{
    if (!sock || !core) {
        return UTIL_EINVAL;
    }

    /* Allocate socket */
    pt_net_socket_t *s = socket_alloc(core);
    if (!s) {
        return UTIL_ERESOURCE;
    }

    /* Create socket */
    s->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s->fd == INVALID_SOCKET_FD) {
#ifdef _WIN32
        return util_err_from_wsa(WSAGetLastError());
#else
        return util_err_from_errno(errno);
#endif
    }

    s->type = PT_NET_SOCKET_UDP;

    /* Set non-blocking */
    util_err_t err = set_nonblocking(s->fd);
    if (err != UTIL_OK) {
        closesocket(s->fd);
        return err;
    }

    /* Bind if address provided */
    if (bind_addr) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(bind_addr->port);

        if (bind_addr->host[0] == '\0') {
            sa.sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            if (inet_pton(AF_INET, bind_addr->host, &sa.sin_addr) <= 0) {
                closesocket(s->fd);
                return UTIL_EINVAL;
            }
        }

        if (bind(s->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
#ifdef _WIN32
            int wsa_err = WSAGetLastError();
            closesocket(s->fd);
            return util_err_from_wsa(wsa_err);
#else
            int err_code = errno;
            closesocket(s->fd);
            return util_err_from_errno(err_code);
#endif
        }

        s->local_addr = *bind_addr;
    }

    *sock = s;
    return UTIL_OK;
}

/**
 * Enable broadcast on UDP socket
 */
util_err_t pt_net_udp_enable_broadcast(pt_net_socket_t *sock)
{
    if (!sock || sock->type != PT_NET_SOCKET_UDP) {
        return UTIL_EINVAL;
    }

    int broadcast = 1;
    if (setsockopt(sock->fd, SOL_SOCKET, SO_BROADCAST, (const char *)&broadcast, sizeof(broadcast)) < 0) {
#ifdef _WIN32
        return util_err_from_wsa(WSAGetLastError());
#else
        return util_err_from_errno(errno);
#endif
    }

    return UTIL_OK;
}

/**
 * Join a multicast group
 */
util_err_t pt_net_udp_join_multicast(
    pt_net_socket_t *sock,
    pt_net_addr_t *multicast_addr,
    pt_net_addr_t *interface_addr
)
{
    if (!sock || !multicast_addr || sock->type != PT_NET_SOCKET_UDP) {
        return UTIL_EINVAL;
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));

    if (inet_pton(AF_INET, multicast_addr->host, &mreq.imr_multiaddr) <= 0) {
        return UTIL_EINVAL;
    }

    if (interface_addr && interface_addr->host[0] != '\0') {
        if (inet_pton(AF_INET, interface_addr->host, &mreq.imr_interface) <= 0) {
            return UTIL_EINVAL;
        }
    } else {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    }

    if (setsockopt(sock->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, sizeof(mreq)) < 0) {
#ifdef _WIN32
        return util_err_from_wsa(WSAGetLastError());
#else
        return util_err_from_errno(errno);
#endif
    }

    return UTIL_OK;
}

/**
 * Leave a multicast group
 */
util_err_t pt_net_udp_leave_multicast(
    pt_net_socket_t *sock,
    pt_net_addr_t *multicast_addr,
    pt_net_addr_t *interface_addr
)
{
    if (!sock || !multicast_addr || sock->type != PT_NET_SOCKET_UDP) {
        return UTIL_EINVAL;
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));

    if (inet_pton(AF_INET, multicast_addr->host, &mreq.imr_multiaddr) <= 0) {
        return UTIL_EINVAL;
    }

    if (interface_addr && interface_addr->host[0] != '\0') {
        if (inet_pton(AF_INET, interface_addr->host, &mreq.imr_interface) <= 0) {
            return UTIL_EINVAL;
        }
    } else {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    }

    if (setsockopt(sock->fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const char *)&mreq, sizeof(mreq)) < 0) {
#ifdef _WIN32
        return util_err_from_wsa(WSAGetLastError());
#else
        return util_err_from_errno(errno);
#endif
    }

    return UTIL_OK;
}

/**
 * Try to receive from UDP socket
 */
util_err_t pt_net_try_recvfrom_udp(buf_t *buf, pt_net_addr_t *from_addr, pt_net_socket_t *sock)
{
    if (!buf || !sock) {
        return UTIL_EINVAL;
    }

    if (sock->type != PT_NET_SOCKET_UDP) {
        return UTIL_EINVAL;
    }

    size_t space = buf_write_size(buf);
    if (space == 0) {
        return UTIL_EAGAIN;
    }

    struct sockaddr_in sa;
    socklen_t sa_len = sizeof(sa);
    uint8_t *wp = buf_write_ptr(buf);

    int n = recvfrom(sock->fd, (char *)wp, (int)space, 0, (struct sockaddr *)&sa, &sa_len);

    if (n < 0) {
#ifdef _WIN32
        int wsa_err = WSAGetLastError();
        if (wsa_err == WSAEWOULDBLOCK) {
            return UTIL_EAGAIN;
        }
        return util_err_from_wsa(wsa_err);
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return UTIL_EAGAIN;
        }
        return util_err_from_errno(errno);
#endif
    }

    if (n == 0) {
        return UTIL_EAGAIN;
    }

    /* Store source address if requested */
    if (from_addr) {
        inet_ntop(AF_INET, &sa.sin_addr, from_addr->host, sizeof(from_addr->host));
        from_addr->port = ntohs(sa.sin_port);
    }

    buf_advance_write(buf, n);
    return UTIL_OK;
}

/**
 * Try to send UDP datagram
 */
util_err_t pt_net_try_sendto_udp(buf_t *buf, pt_net_socket_t *sock, const pt_net_addr_t *to_addr)
{
    if (!buf || !sock || !to_addr) {
        return UTIL_EINVAL;
    }

    if (sock->type != PT_NET_SOCKET_UDP) {
        return UTIL_EINVAL;
    }

    size_t to_send = buf_read_size(buf);
    if (to_send == 0) {
        return UTIL_OK;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(to_addr->port);

    if (inet_pton(AF_INET, to_addr->host, &sa.sin_addr) <= 0) {
        return UTIL_EINVAL;
    }

    uint8_t *rp = buf_read_ptr(buf);
    int n = sendto(sock->fd, (const char *)rp, (int)to_send, 0, (struct sockaddr *)&sa, sizeof(sa));

    if (n < 0) {
#ifdef _WIN32
        int wsa_err = WSAGetLastError();
        if (wsa_err == WSAEWOULDBLOCK) {
            return UTIL_EAGAIN;
        }
        return util_err_from_wsa(wsa_err);
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return UTIL_EAGAIN;
        }
        return util_err_from_errno(errno);
#endif
    }

    if (n != (int)to_send) {
        return UTIL_EAGAIN;
    }

    buf_advance_read(buf, n);
    return UTIL_OK;
}
