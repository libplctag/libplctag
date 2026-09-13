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
 * What the shared CIP tag code needs from a device module.
 *
 * ab/eip_cip.c and omron/omron_standard_tag.c were the same fourteen functions
 * over two tag structs that differ by three fields, two request structs that
 * differ by three, three connection functions with the same names, and two
 * connection fields with the same names.  That is the whole seam, so it is
 * expressed as two struct macros and one small ops table rather than as a
 * dialect vtable.
 *
 * AB is the base.  Where the two implementations differed for no functional
 * reason, AB's version was kept -- it is the more heavily exercised path.  The
 * one difference that is functional is fragmentation, and it is a flag.
 */

#include <libplctag/lib/tag.h>
#include <utils/atomic_utils.h>
#include <utils/spinlock.h>
#include <libplctag/protocols/cip/defs.h>
#include <stdint.h>


/* room for the encoded type information a CIP read returns. */
#ifndef MAX_TAG_TYPE_INFO
#    define MAX_TAG_TYPE_INFO (64)
#endif


/*
 * The request is fully shared: both modules used the same twelve fields plus the
 * three Omron added for packing decisions, so there is nothing left to diverge.
 * ab_request_p and omron_request_p are aliases of this.
 */

/*
 * The element type a tag holds.  Both modules carried this same list under
 * AB_TYPE_ and OMRON_TYPE_ prefixes; only the identity pseudo-type was AB's
 * alone, and an unused enumerator costs nothing.
 */
typedef enum {
    /* NOTE: BOOL is zero, as it was in both originals.  Do not insert ahead of it. */
    CIP_TYPE_BOOL = 0,
    CIP_TYPE_BOOL_ARRAY,
    CIP_TYPE_CONTROL,
    CIP_TYPE_COUNTER,
    CIP_TYPE_FLOAT32,
    CIP_TYPE_FLOAT64,
    CIP_TYPE_INT8,
    CIP_TYPE_INT16,
    CIP_TYPE_INT32,
    CIP_TYPE_INT64,
    CIP_TYPE_STRING,
    CIP_TYPE_SHORT_STRING,
    CIP_TYPE_TIMER,
    CIP_TYPE_TAG_ENTRY,   /* not a real CIP type, but a pseudo type for a tag listing entry. */
    CIP_TYPE_TAG_UDT,     /* as above, but for UDTs. */
    CIP_TYPE_TAG_RAW,     /* raw CIP tag */
    CIP_TYPE_TAG_IDENTITY /* CIP Identity Object data */
} cip_elem_type_t;


typedef struct cip_request_t *cip_request_p;

#define CIP_REQUEST_NULL ((cip_request_p)NULL)


/*
 * The tag fields the shared code touches, in the order both modules already had
 * them.  A module pastes this straight after TAG_BASE_STRUCT and adds its own
 * fields afterwards, so the two structs share a common initial sequence.
 *
 * NOTE: neither the connection pointer nor plc_type is here.  Both have module
 * specific types -- ab_session_p vs omron_conn_p, and two different plc_type_t
 * enums -- so each module declares its own after this macro.  No shared code
 * reads either: the connection arrives as a parameter, and nothing in the shared
 * set branches on PLC type.
 */
#define CIP_TAG_STRUCT                                                                       \
    int use_connected_msg;                                                                   \
                                                                                             \
    /* this contains the encoded name */                                                     \
    uint8_t encoded_name[MAX_TAG_NAME];                                                      \
    int encoded_name_size;                                                                   \
                                                                                             \
    /* storage for the encoded type. */                                                      \
    uint8_t encoded_type_info[MAX_TAG_TYPE_INFO];                                            \
    int encoded_type_info_size;                                                              \
                                                                                             \
    cip_elem_type_t elem_type;                                                                   \
    int elem_count;                                                                          \
    int elem_size;                                                                           \
                                                                                             \
    int special_tag;                                                                         \
                                                                                             \
    /* how much data can we send per packet? */                                              \
    int write_data_per_packet;                                                               \
                                                                                             \
    /* used for listing tags. */                                                             \
    uint32_t next_id;                                                                        \
                                                                                             \
    /* used for UDT tags. */                                                                 \
    uint8_t udt_get_fields;                                                                  \
    uint16_t udt_id;                                                                         \
                                                                                             \
    /*                                                                                       \
     * Consecutive fragment responses that carried no payload.                               \
     *                                                                                       \
     * A partial-transfer status with zero bytes is legitimate: when several requests are    \
     * packed into one packet the earlier ones can consume all the room, leaving the later   \
     * ones only a bare CIP header.  It is also what a PLC would send forever to keep us     \
     * asking for the same fragment, so count them and give up rather than loop.  Reset      \
     * whenever a response actually delivers data.                                           \
     */                                                                                      \
    int fragment_retry_count;                                                                \
                                                                                             \
    int pre_write_read;                                                                      \
    int first_read;                                                                          \
    cip_request_p req;                                                                       \
    int offset;                                                                              \
                                                                                             \
    int allow_packing;                                                                       \
                                                                                             \
    /*                                                                                       \
     * Whether the device can read a tag in fragments.  AB can; Omron NJ/NX cannot,          \
     * which is the one behavioural difference between the two implementations this          \
     * code was merged from.                                                                 \
     */                                                                                      \
    int supports_fragmented_read;                                                            \
                                                                                             \
    /* flags for operations */                                                               \
    int read_in_progress;                                                                    \
    int write_in_progress


/*
 * The request fields the shared code touches.  Same arrangement: a module pastes
 * this and adds its own afterwards.
 */
#define CIP_REQUEST_STRUCT                                                                   \
    /* used to force interlocks with other threads. */                                       \
    lock_t lock;                                                                             \
                                                                                             \
    int status;                                                                              \
                                                                                             \
    /* flags for communicating with the background thread */                                 \
    int resp_received;                                                                       \
    atomic_int32_t abort_request;                                                            \
                                                                                             \
    /* debugging info */                                                                     \
    int tag_id;                                                                              \
                                                                                             \
    /* allow requests to be packed together */                                               \
    int allow_packing;                                                                       \
    int packing_num;                                                                         \
                                                                                             \
    /* time stamp for debugging output */                                                    \
    int64_t time_sent;                                                                       \
                                                                                             \
    /* used by the background thread for incrementally getting data */                       \
    int request_size; /* total bytes, not just data */                                       \
    int request_capacity;                                                                    \
    uint8_t *data;                                                                           \
                                                                                             \
    /*                                                                                       \
     * Packing decisions need to know how much the response may be.  A first read does        \
     * not know the tag's size yet, and a device that cannot fragment cannot recover from     \
     * guessing wrong, so both are carried here.                                              \
     */                                                                                      \
    int response_size;                                                                       \
    int first_read;                                                                          \
    int supports_fragmented_read


/*
 * Everything the shared code needs from a connection.  Three calls and one
 * accessor -- that is the entire surface the fourteen functions used.
 */
struct cip_request_t {
    CIP_REQUEST_STRUCT;
};


typedef struct {
    int (*create_request)(void *conn, int32_t tag_id, cip_request_p *req);
    int (*add_request)(void *conn, cip_request_p req);
    int (*get_available_cip_payload_space)(void *conn);

    /* the encoded connection path and its length, for building a request header. */
    const uint8_t *(*get_conn_path)(void *conn, int *path_size);
} cip_conn_ops_t;


/*
 * A tag holding just the shared fields.  Both modules' structs begin with this
 * sequence, so shared code takes a pointer to one of these and each module casts.
 */
struct cip_tag_t {
    TAG_BASE_STRUCT;
    CIP_TAG_STRUCT;
};

typedef struct cip_tag_t *cip_tag_p;


/* a device's request builder, as the shared code sees it. */
typedef int (*cip_build_request_func)(cip_tag_p tag);


extern int cip_default_tag_status(plc_tag_p tag);
extern int cip_tag_status(cip_tag_p tag, void *conn);
extern int cip_raw_tag_write_start(cip_tag_p tag, cip_build_request_func build_connected,
                                   cip_build_request_func build_unconnected);

/* fill in the tag's encoded name, size, is_bit and bit from a tag name string. */
extern int cip_fill_tag_name(cip_tag_p tag, const char *name);

/*
 * Check that a response's CPF framing matches the request it answers.  The
 * connected form needs the two connection IDs, whose home is the module's own
 * connection struct.
 */
extern int cip_check_cpf_unconnected(cip_tag_p tag, cip_request_p request);
extern int cip_check_cpf_connected(cip_tag_p tag, cip_request_p request, uint32_t orig_connection_id,
                                   uint32_t targ_connection_id);
