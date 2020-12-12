
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
#include <util/slice.h>

typedef enum {PROTOCOL_STREAM_OPEN, PROTOCOL_STREAM_PACKET_READY, PROTOCOL_STREAM_WRITE_READY, PROTOCOL_STREAM_CLOSING} protocol_stream_event_t;

struct protocol_stream_callback_s {
    mutex_p mutex;
    struct protocol_stream_callback_s *next;
    void *context;
    slice_t (*callback_handler)(protocol_stream_event_t event, void *context, slice_t buffer);
};
typedef struct protocol_stream_callback_s *protocol_stream_callback_p;

struct protocol_stream_s {
    mutex_p mutex;
    protocol_stream_callback_p callbacks;
};
typedef struct protocol_stream_s *protocol_stream_p;

extern int protocol_stream_init(protocol_stream_p stream);
extern int protocol_stread_destroy(protocol_stream_p stream);

extern int protocol_stream_register_callback(protocol_stream_p stream, protocol_stream_callback_p callback);
extern int protocol_stream_remove_callback(protocol_stream_p stream, protocol_stream_callback_p callback);

