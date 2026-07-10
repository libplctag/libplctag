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
#include <libplctag/protocols/enip/client/enip_pccc_addr.h> /* pccc_addr_t for PCCC data tags */
#include <libplctag/protocols/enip/client/enip_session.h>
#include <utils/bytes.h>

typedef enum {
    ENIP_OP_IDLE = 0,
    ENIP_OP_READ,
    ENIP_OP_WRITE,
    ENIP_OP_OPEN_PROBE, /* §11.2 count=1 probe */
    ENIP_OP_OPEN_BULK,  /* §11.2 remaining elements, single shot for the MVP */
    ENIP_OP_LIST,       /* @tags: class 0x6B Get_Instance_Attribute_List, id continuation */
    ENIP_OP_UDT_META,   /* @udt: class 0x6C Get_Attribute_List (template metadata) */
    ENIP_OP_UDT_FIELDS, /* @udt: class 0x6C CIP read of the field definition, offset continuation */
} enip_op_t;

/* Which variant of the per-tag union is active. Exactly one applies. */
typedef enum {
    ENIP_TAG_KIND_DATA = 0, /* normal data tag: scheduler + type/layout + path */
    ENIP_TAG_KIND_CONNECTION, /* @connection */
    ENIP_TAG_KIND_IDENTITY,   /* @identity */
    ENIP_TAG_KIND_LISTING,    /* @tags: a data-variant tag driven by ENIP_OP_LIST */
    ENIP_TAG_KIND_UDT,        /* @udt/<id>: a data-variant tag driven by ENIP_OP_UDT_* */
    ENIP_TAG_KIND_PCCC,       /* PLC-5/SLC/MicroLogix: data-variant tag, Execute-PCCC (no probe) */
} enip_tag_kind_t;

struct enip_tag_t {
    TAG_BASE_STRUCT; /* data, size, status, byte_order, vtable, api_mutex, ... */
    /* TAG_BASE_STRUCT must stay first: enip_tag_t "is a" plc_tag_t. */

    enip_connection_t *conn; /* holds an rc ref; released in destructor; common to all variants */

    /* Per-variant fields. The active member is selected by `kind` below: a
     * data tag uses the data variant; @connection uses the conn_status variant;
     * @identity uses neither (it reads conn's cached payload).
     * Fields within each variant are ordered by decreasing size. */
    union {
        /* data tag: scheduler membership, learned type/layout, path/tail. */
        struct {
            Bytes path; /* encoded CIP IOI, into the tail */
            struct enip_tag_t *sched_prev, *sched_next; /* conn->sched_mutex (§13) */
            struct enip_tag_t *batch_next;              /* batch list; IO-thread-only */
            char *tag_name;                             /* into the tail */
            int64_t op_time;
            /* type/layout learned at open -- api_mutex (§11.4) */
            uint32_t elem_size, elem_count, window_elems, write_window_elems;
            uint32_t read_off;    /* bulk cursor (elements); reused by OPEN_BULK, READ, WRITE */
            uint32_t frag_offset; /* ReadFrag continuation cursor (post-MVP) */
            /* @tags/@udt only (ENIP_TAG_KIND_LISTING/UDT): list_next_id is the
             * next symbol instance id for @tags, or the fixed template id for
             * @udt; list_total is the @udt field-definition byte target. */
            uint32_t list_next_id, list_total;
            /* PCCC only (ENIP_TAG_KIND_PCCC): the parsed logical address (N7:0,
             * F8:0, ...) and which encoder/function family to use. Filled at
             * create; the probe is skipped (elem_size comes from the address). */
            pccc_addr_t pccc_addr;
            uint8_t type_header[4];
            uint8_t type_header_len; /* 2 or 4 */
            uint8_t op;              /* enip_op_t */
            uint8_t scheduled : 1;
            uint8_t ready : 1;
            uint8_t pccc_plc5 : 1; /* PCCC: PLC-5 encoding/functions (else SLC/MicroLogix) */
            /* tail: tag_name (NUL), then the encoded CIP path bytes */
        };

        /* @connection tag (no path/tail): drains the conn-status ring and fires
         * PLCTAG_EVENT_CONN_STATUS_* events. */
        struct {
            int32_t conn_status_read_idx;
            int32_t last_conn_state;
            uint8_t first_tickler_run : 1;
        };
    };

    enip_tag_kind_t kind; /* selects the active union variant */
};

extern plc_tag_p enip_tag_create_impl(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                       void *userdata, plc_tag_p src_tag);
