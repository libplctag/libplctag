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
 * ENIP tag type definitions (plan §3).
 *
 * A tag is ONE rc_alloc block:
 *   TAG_BASE_STRUCT  — generic, owned by lib.c (data, size, status, byte_order)
 *   enip_tag_meta_t  — type info; owned by metadata code; read by getters
 *   enip_operation_t — transient I/O state; owned exclusively by engine thread
 *   tail bytes       — tag_name (NUL-terminated), then base encoded CIP path
 *
 * Never add heap pointers that enip_tag_t owns.  The destructor calls
 * mem_free(tag->data) and rc_dec(tag->conn) only.
 */

#include <libplctag/lib/tag.h>
#include <libplctag/protocols/enip/enip_op.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct enip_tag_t {
    TAG_BASE_STRUCT;                    /* data, size, status, byte_order, etc.  */

    struct enip_connection_t *conn;     /* back-pointer; holds an rc_inc ref     */

    enip_tag_meta_t meta;              /* type info + validity gate (plan §3.3) */
    enip_operation_t op;               /* transient I/O state (plan §3.2)       */

    bool in_active_tags;               /* true while linked in conn's queue     */

    char *tag_name;                    /* points into the tail; root symbol name */
    /* tail: tag_name bytes (NUL-terminated), then base encoded CIP path bytes  */
} enip_tag_t;

typedef struct enip_connection_tag_t {
    TAG_BASE_STRUCT;

    int32_t callback_latency_last_ms;
    int32_t callback_latency_max_ms;
    int32_t queue_depth;
} enip_connection_tag_t;
