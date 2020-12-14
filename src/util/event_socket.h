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

#include <platform.h>


typedef enum { EVENT_SOCKET_CONNECT            = (1 << 0),
               EVENT_SOCKET_CLOSE              = (1 << 1),
               EVENT_SOCKET_CAN_READ           = (1 << 2),
               EVENT_SOCKET_CAN_WRITE          = (1 << 3)
             } event_socket_event_t;

typedef struct event_socket_s *event_socket_p;

extern int event_socket_create(event_socket_p *ps,
                                void *context,
                                event_socket_event_t event_mask,
                                void (*connect_callback)(event_socket_p esock, int64_t current_time, void *context),
                                int (*can_read_callback)(event_socket_p esock, int64_t current_time, void *context, sock_p sock),
                                int (*can_write_callback)(event_socket_p esock, int64_t current_time, void *context, sock_p sock),
                                void (*close_callback)(event_socket_p esock, int64_t current_time, void *context));
extern int event_socket_connect(event_socket_p event_sock, const char *host, int port);
extern int event_socket_close(event_socket_p event_sock);
extern int event_socket_destroy(event_socket_p *event_sock);
extern int event_socket_enable_events(event_socket_p event_sock, event_socket_event_t event);
extern int event_socket_disable_events(event_socket_p event_sock, event_socket_event_t event);


typedef struct timer_s *timer_p;

extern int timer_create(timer_p *timer,
                        void *context,
                        int(*timer_callback)(timer_p timer, int64_t current_time, void *context));
extern int timer_destroy(timer_p *timer);
extern int timer_set(timer_p timer, int64_t wake_time);
extern int timer_abort(timer_p timer);


extern void event_socket_tickler(void);

extern int event_socket_init(void);
extern void event_socket_teardown(void);
