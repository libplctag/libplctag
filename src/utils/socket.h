#pragma once

/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
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

/*
 * TCP client sockets.
 *
 * This replaces the socket section that used to live in the platform shims
 * (src/platform/posix/platform.[ch] and src/platform/windows/platform.[ch]).
 * The API is unchanged; only the home of the declarations moved.
 *
 * A socket is an opaque handle carrying its file descriptor plus a private
 * wake channel -- a self-connected TCP pair on Windows, a socketpair() on
 * POSIX -- that lets one thread break another out of socket_wait_event().
 * Only the Modbus protocol uses that channel today; see
 * docs/socket_layering_design.md for where this API is headed.
 *
 * The transfer calls take a timeout in milliseconds and sleep inside
 * themselves until the socket is ready or the timeout expires.  That makes
 * each socket the property of one thread.
 */

#include <stdint.h>

#include <libplctag/lib/libplctag.h>


typedef struct sock_t *sock_p;

/*
 * Readiness and status bits.  Passed as a mask to socket_wait_event() and
 * returned by it as the set of things that actually happened.
 */
typedef enum {
    SOCK_EVENT_NONE = 0,
    SOCK_EVENT_TIMEOUT = (1 << 0),
    SOCK_EVENT_DISCONNECT = (1 << 1),
    SOCK_EVENT_ERROR = (1 << 2),
    SOCK_EVENT_CAN_READ = (1 << 3),
    SOCK_EVENT_CAN_WRITE = (1 << 4),
    SOCK_EVENT_WAKE_UP = (1 << 5),
    SOCK_EVENT_CONNECT = (1 << 6),

    SOCK_EVENT_DEFAULT_MASK = (SOCK_EVENT_TIMEOUT | SOCK_EVENT_DISCONNECT | SOCK_EVENT_ERROR | SOCK_EVENT_WAKE_UP)
} sock_event_t;

extern int socket_create(sock_p *s);
extern int socket_connect_tcp_start(sock_p s, const char *host, int port);
extern int socket_connect_tcp_check(sock_p s, int timeout_ms);
extern int socket_wait_event(sock_p sock, int events, int timeout_ms);
extern int socket_wake(sock_p sock);
extern int socket_read(sock_p s, uint8_t *buf, int size, int timeout_ms);
extern int socket_write(sock_p s, uint8_t *buf, int size, int timeout_ms);
extern int socket_close(sock_p s);
extern int socket_destroy(sock_p *s);
