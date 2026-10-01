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
 * One connection-status tag, shared by every protocol.
 *
 * A connection tag carries no PLC data.  It exposes one attribute,
 * "connection_status", and turns the events its connection publishes into tag
 * callbacks.  Nothing in here is protocol-specific: the protocol finds or
 * creates its own connection object and hands over the three things this code
 * reads from it.
 */

#include <libplctag/lib/conn_watch.h>
#include <libplctag/lib/tag.h>
#include <platform.h>
#include <stdbool.h>
#include <stdint.h>
#include <utils/attr.h>
#include <utils/debug.h>


typedef struct {
    TAG_BASE_STRUCT;

    void *conn;               /* the connection object, held by refcount; NULL if creation failed */
    conn_event_ring_t *watch; /* &conn->watch */
    int32_t last_conn_state;  /* last state delivered to the callback */
    int32_t ring_read_idx;    /* last ring entry this tag has processed */
    int32_t io_events;        /* 1 = report connection reads and writes, 0 = suppress */
    bool first_tickler_run;   /* true until the first post-CREATED tickler */
    debug_module_t debug_module;
} connection_tag_t;

typedef connection_tag_t *connection_tag_p;


/* what the protocol hands over about the connection it found or created */
typedef struct {
    tag_protocol_t protocol_type;
    debug_module_t debug_module;

    void *conn;               /* rc-held connection object, or NULL if it could not be obtained */
    conn_event_ring_t *watch; /* &conn->watch; unused when conn is NULL */
    mutex_p conn_mutex;       /* the connection's own mutex; unused when conn is NULL */
    bool conn_is_new;         /* true when this call created the connection rather than joining one */
    int32_t conn_rc;          /* the failure that left conn NULL */
} connection_tag_args_t;


extern plc_tag_p connection_tag_create(attr attribs, const connection_tag_args_t *args,
                                       void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                       void *userdata);
