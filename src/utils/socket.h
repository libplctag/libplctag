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
 * This replaces the socket section that used to live in the platform shims.
 * The API is unchanged; only the home of the declarations moved.
 *
 * A socket is an opaque handle carrying its file descriptor.  The transfer
 * calls take a timeout in milliseconds and sleep inside themselves until the
 * socket is ready or the timeout expires, which makes each socket the
 * property of one thread.  AB and Omron are the remaining callers and that is
 * the model they use.
 *
 * There is no wake channel and no readiness query here any more.  Both used
 * to exist for the Modbus protocol, which now drives its socket through
 * utils/socket_fd.h and utils/poller.h -- where the wake channel belongs to
 * the waiting thread rather than to each socket.  See 2.4 in
 * docs/deferred_fixes.md and docs/socket_layering_design.md; this API is the
 * L1 library adapter that step 4 puts onto socket_fd.
 */

#include <stdint.h>



typedef struct sock_t *sock_p;

extern int socket_create(sock_p *s);
extern int socket_connect_tcp_start(sock_p s, const char *host, int port);
extern int socket_connect_tcp_check(sock_p s, int timeout_ms);
extern int socket_read(sock_p s, uint8_t *buf, int size, int timeout_ms);
extern int socket_write(sock_p s, uint8_t *buf, int size, int timeout_ms);
extern int socket_close(sock_p s);
extern int socket_destroy(sock_p *s);
