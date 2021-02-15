/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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

#pragma once

#include <stdint.h>

typedef int socket_t;

#ifndef INVALID_SOCKET
    #define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
    #define SOCKET_ERROR (-1)
#endif

typedef struct sock_t *sock_p;

/* direct calls. */
extern int socket_create(sock_p *s);
extern int socket_tcp_connect(sock_p s, const char *host, int port);
extern int socket_tcp_read(sock_p client, uint8_t *buf, int size);
extern int socket_tcp_write(sock_p client, uint8_t *buf, int size);
extern int socket_close(sock_p s);
extern int socket_destroy(sock_p *sock);
extern int socket_status(sock_p sock);

/* callback/event calls */
extern int socket_callback_when_connection_ready(sock_p sock, void (*callback)(void *context), void *context, const char *host, int port);
extern int socket_callback_when_read_done(sock_p, void (*callback)(void *context), void *context, uint8_t *buffer, int buffer_capacity, int *amount);
extern int socket_callback_when_write_done(sock_p, void (*callback)(void *context), void *context, uint8_t *buffer, int *amount);

/* socket event functions for the event loop. */
extern void socket_event_loop_wake(void);
extern void socket_event_loop_tickler(int64_t next_wake_time, int64_t current_time);
extern int socket_event_loop_init(void);
extern void socket_event_loop_teardown(void);
