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

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <utils/byte_slice.h>

typedef struct tcp_server *tcp_server_p;

/*
 * The handler returns what is left of input after the one request it handled, and sets
 * *reply to the bytes to send:
 *
 *   - error slice: close the connection.
 *   - same length as input: no whole request yet, read more.
 *   - shorter, *reply empty: request handled, send nothing.
 *   - shorter, *reply not empty: send *reply.
 *
 * The server keeps the leftover and calls the handler again before reading, so pipelined
 * requests are all answered.
 */
typedef byte_slice_t (*tcp_server_handler_t)(byte_slice_t input, byte_slice_t output, byte_slice_t *reply, void *context);

extern tcp_server_p tcp_server_create(const char *host, const char *port, tcp_server_handler_t handler, void *context,
                                      size_t context_size);
extern void tcp_server_start(tcp_server_p server, volatile sig_atomic_t *terminate);
extern void tcp_server_destroy(tcp_server_p server);
