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
 * Tag struct and op enum (design doc §14.7).  enip_session.c includes this
 * for the full enip_tag_t definition (scheduler fields, op/meta).
 */

#include <stdint.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/enip/enip_session.h>
#include <utils/bytes.h>

typedef enum {
    ENIP_OP_IDLE = 0,
    ENIP_OP_READ,
    ENIP_OP_WRITE,
    ENIP_OP_OPEN_PROBE, /* §11.2 count=1 probe */
    ENIP_OP_OPEN_BULK,  /* §11.2 remaining elements, single shot for the MVP */
} enip_op_t;

struct enip_tag_t {
    TAG_BASE_STRUCT; /* data, size, status, byte_order, vtable, api_mutex, ... */

    enip_connection_t *conn; /* holds an rc ref; released in destructor */

    /* scheduler membership -- conn->sched_mutex (§13) */
    struct enip_tag_t *sched_prev, *sched_next;
    int64_t op_time;
    uint8_t op; /* enip_op_t */
    uint8_t scheduled : 1;

    /* type/layout learned at open -- api_mutex (§11.4) */
    uint8_t type_header[4];
    uint8_t type_header_len; /* 2 or 4 */
    uint32_t elem_size, elem_count, window_elems, write_window_elems;
    uint32_t read_off; /* bulk cursor (elements); reused by OPEN_BULK, READ, and WRITE */
    uint8_t ready : 1;

    uint32_t frag_offset; /* ReadFrag continuation cursor (post-MVP) */

    struct enip_tag_t *batch_next; /* batch list linkage; IO-thread-only, no lock needed */

    Bytes path; /* encoded CIP IOI, into the tail */
    char *tag_name; /* into the tail */
    /* tail: tag_name (NUL), then the encoded CIP path bytes */

    /* @connection special tag (no path/tail). Drains the connection's conn-status
     * ring and fires PLCTAG_EVENT_CONN_STATUS_* events. */
    int32_t conn_status_read_idx;
    int32_t last_conn_state;
    uint8_t is_connection_tag : 1;
    uint8_t first_tickler_run : 1;
    uint8_t is_identity_tag : 1;
};

extern plc_tag_p enip_tag_create_impl(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                       void *userdata, plc_tag_p src_tag);
