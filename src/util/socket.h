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
#include <util/slice.h>

typedef int socket_t;

#ifndef INVALID_SOCKET
    #define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
    #define SOCKET_ERROR (-1)
#endif

typedef struct sock_t *sock_p;
extern int socket_create(sock_p *s);
extern int socket_tcp_connect(sock_p s, const char *host, int port);
extern int socket_tcp_read(sock_p client, uint8_t *buf, int size);
extern int socket_tcp_write(sock_p client, uint8_t *buf, int size);
extern int socket_close(sock_p s);
extern int socket_destroy(sock_p *sock);


typedef enum {
    SOCKET_EVENT_ACCEPT_READY        = (1 << 0),
    SOCKET_EVENT_CONNECTION_READY    = (1 << 1),
    SOCKET_EVENT_CLOSING             = (1 << 2),
    SOCKET_EVENT_READ_READY          = (1 << 3),
    SOCKET_EVENT_WRITE_READY         = (1 << 4)
} socket_event_t;

extern int socket_callback_when_read_ready(sock_p, void (*callback)(sock_p sock, void *context), void *context);
extern int socket_callback_when_write_ready(sock_p, void (*callback)(sock_p sock, void *context), void *context);


// extern int socket_event_set_callback(sock_p sock, void (*callback)(sock_p sock, int events, void *context), void *context);
// extern int socket_event_set_mask(sock_p sock, int event_mask);
// extern int socket_event_get_mask(sock_p sock, int *event_mask);

// extern int socket_bind(sock_p sock, const char *interface, int port);
// extern int socket_tcp_listen(sock_p sock, int listen_queue_size);
// extern int socket_tcp_client(sock_p *client, const char *host, int port);
// extern int socket_tcp_server(sock_p *server, const char *interface, int port, int listen_queue_size);
// extern int socket_tcp_accept(sock_p server, sock_p *client);

// extern int socket_udp_open(sock_p *udp, const char *interface, int port)
// extern int socket_udp_send(sock_p udp, uint8_t *data, int data_len, )
// extern int socket_cancel(sock_p s);


// extern int socket_on_tcp_accept_ready(sock_p s, bool (*callback)(sock_p sock, int status, void *context), void *context);
// extern int socket_on_tcp_connection_ready(sock_p s, bool (*callback)(sock_p sock, int status, void *context), void *context);
// extern int socket_on_tcp_read_ready(sock_p s, bool (*callback)(sock_p sock, int status, void *context), void *context);
// extern int socket_on_tcp_write_ready(sock_p s, bool (*callback)(sock_p sock, int status, void *context), void *context);
// extern int socket_on_cancel(sock_p s, int (*callback)(sock_p sock, int status, void *context), void *context);


// typedef enum {
//     SOCKET_EVENT_NONE,
//     SOCKET_EVENT_ACCEPT,
//     SOCKET_EVENT_READ,
//     SOCKET_EVENT_CONNECT,
//     SOCKET_EVENT_WRITE,
//     SOCKET_EVENT_LAST
// } socket_event_t;

// extern int socket_event_enable(sock_p sock, socket_event_t event, void (*callback)(sock_p sock, socket_event_t event, void *context), void *context);
// extern int socket_event_disable(sock_p sock);

/* socket event functions for the event loop. */
extern void socket_event_loop_wake(void);
extern void socket_event_loop_tickler(int64_t next_wake_time, int64_t current_time);
extern int socket_event_loop_init(void);
extern void socket_event_loop_teardown(void);
