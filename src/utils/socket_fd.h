#pragma once

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
 * LGPL 2:                                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 ***************************************************************************/

/*
 * L0 of the socket stack: non-blocking operations on a bare handle.
 *
 * No buffer type, no timeouts, no opinions.  Nothing here ever sleeps.  A
 * call returns PLCTAG_STATUS_OK, PLCTAG_STATUS_PENDING for would-block, or an
 * error, and it returns it now.  Waiting is the caller's business and belongs in poller.h.
 *
 * A partial transfer is normal here, not an error: socket_fd_recv() and
 * socket_fd_send() report how many bytes actually moved and leave the rest
 * to the next call.
 *
 * This is the entire portability surface of the socket stack -- Winsock
 * startup, INVALID_SOCKET, EAGAIN vs WSAEWOULDBLOCK, EINPROGRESS vs
 * WSAEINPROGRESS, the socket options.  It is meant to be the only file in
 * the stack carrying a #ifdef _WIN32.
 *
 * See docs/socket_layering_design.md.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
/* KEEP THE SPACES BETWEEN THE INCLUDES.  The order is required! */
#    include <winsock2.h>

#    include <windows.h>

#    include <ws2tcpip.h>

typedef SOCKET socket_fd_t;

#    define SOCKET_FD_INVALID (INVALID_SOCKET)
#else
typedef int32_t socket_fd_t;

#    define SOCKET_FD_INVALID ((socket_fd_t)-1)
#endif

/*
 * A resolved peer address, large enough for IPv4 or IPv6.  Opaque to the
 * caller; pass it back to socket_fd_sendto() or read it with
 * socket_fd_addr_str().
 */
typedef struct {
    uint8_t storage[128];
    uint32_t length;
} socket_fd_addr_t;


/*
 * Winsock needs an explicit startup call and a matching cleanup.  Both are
 * reference counted and safe to call repeatedly; on POSIX they do nothing.
 * Every socket_fd_open_*() calls startup itself, so a caller only needs
 * these to bracket a process that wants the cleanup to happen on exit.
 */
extern int32_t socket_fd_lib_startup(void);
extern int32_t socket_fd_lib_shutdown(void);

/* handle and setup */
extern int32_t socket_fd_open_tcp(socket_fd_t *fd);
extern int32_t socket_fd_open_udp(socket_fd_t *fd);
extern int32_t socket_fd_close(socket_fd_t *fd);
extern int32_t socket_fd_set_nonblocking(socket_fd_t fd, bool on);
extern int32_t socket_fd_set_nodelay(socket_fd_t fd, bool on);
extern int32_t socket_fd_set_reuseaddr(socket_fd_t fd, bool on);
extern int32_t socket_fd_set_broadcast(socket_fd_t fd, bool on);

/*
 * Reads the pending error off the socket with SO_ERROR and clears it.  This
 * is how a poller distinguishes "writable because the connect finished" from
 * "writable because the connect failed".
 */
extern int32_t socket_fd_get_error(socket_fd_t fd);

/* client */
extern int32_t socket_fd_connect_start(socket_fd_t fd, const char *host, int32_t port);
extern int32_t socket_fd_connect_check(socket_fd_t fd);

/* server */
extern int32_t socket_fd_bind_listen(socket_fd_t fd, const char *host, int32_t port, int32_t backlog);
extern int32_t socket_fd_accept(socket_fd_t listener, socket_fd_t *client_fd, socket_fd_addr_t *client_addr);

/*
 * Transfer.  Never sleeps.  *count is set to the number of bytes moved, which
 * may be less than len and may be zero with PLCTAG_STATUS_PENDING.  A recv
 * of zero bytes with PLCTAG_STATUS_OK means the peer closed the connection.
 */
extern int32_t socket_fd_recv(socket_fd_t fd, uint8_t *buf, int32_t len, int32_t *count);
extern int32_t socket_fd_send(socket_fd_t fd, const uint8_t *buf, int32_t len, int32_t *count);

/* UDP */
extern int32_t socket_fd_recvfrom(socket_fd_t fd, uint8_t *buf, int32_t len, socket_fd_addr_t *from, int32_t *count);
extern int32_t socket_fd_sendto(socket_fd_t fd, const uint8_t *buf, int32_t len, const socket_fd_addr_t *to, int32_t *count);

/*
 * A connected pair of handles, used as a wake channel: a write to one end
 * makes the other readable.  socketpair() on POSIX, a self-connected TCP
 * pair on Windows, which has no socketpair().  Both ends come back
 * non-blocking.
 *
 * This lives here rather than in poller.c so that poller.c has no platform
 * code in it at all.
 */
extern int32_t socket_fd_pair(socket_fd_t *fd_read, socket_fd_t *fd_write);

/*
 * Readiness, as a thin portable wrapper over poll()/WSAPoll().  This is the
 * primitive poller.h is built on; most callers want the poller, not this.
 *
 * Set `want` before the call and read `got` after it.  A socket may report
 * ERR or HUP without either having been asked for.
 */
/*
 * Ceiling on one socket_fd_poll() call.  The native array is built on the
 * stack, so this bounds that: 1024 entries is 8KB on POSIX.  It is a limit
 * per call, not per process -- a poller with more sockets than this would
 * call in batches.  Raise it if that ever happens; nothing here assumes the
 * number is small.
 */
#define SOCKET_FD_POLL_MAX (1024)

#define SOCKET_FD_POLL_READ ((int16_t)(1 << 0))
#define SOCKET_FD_POLL_WRITE ((int16_t)(1 << 1))
#define SOCKET_FD_POLL_ERR ((int16_t)(1 << 2))
#define SOCKET_FD_POLL_HUP ((int16_t)(1 << 3))

typedef struct {
    socket_fd_t fd;
    int16_t want;
    int16_t got;
} socket_fd_poll_item_t;

extern int32_t socket_fd_poll(socket_fd_poll_item_t *items, int32_t item_count, int32_t timeout_ms, int32_t *ready_count);

/* address helpers */
extern int32_t socket_fd_addr_init(socket_fd_addr_t *addr, const char *host, int32_t port);
extern int32_t socket_fd_addr_str(const socket_fd_addr_t *addr, char *buf, int32_t buf_capacity);
extern int32_t socket_fd_addr_port(const socket_fd_addr_t *addr);
