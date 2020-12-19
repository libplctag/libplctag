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
extern int socket_create(sock_p *s);
extern int socket_connect_tcp(sock_p s, const char *host, int port);
extern int socket_read(sock_p s, uint8_t *buf, int size);
extern int socket_write(sock_p s, uint8_t *buf, int size);
extern int socket_close(sock_p s);

typedef enum {
    SOCKET_EVENT_NONE = 0,
    SOCKET_EVENT_ACCEPT = 1,
    SOCKET_EVENT_READ = 1,
    SOCKET_EVENT_CONNECT = 2,
    SOCKET_EVENT_WRITE = 2,
    SOCKET_EVENT_LAST
} socket_event_t;

extern int socket_event_enable(sock_p s, socket_event_t event, void (*callback)(sock_p sock, socket_event_t event, void *context), void *context);
extern int socket_event_disable(sock_p s);

/* socket event functions for the event loop. */
extern void socket_event_wake(void);
extern void socket_event_tickler(int64_t next_wake_time, int64_t current_time);
extern int socket_event_init(void);
extern void socket_event_teardown(void);
