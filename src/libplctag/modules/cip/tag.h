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

#pragma once

#include <libplctag/lib/tag.h>
#include <utils/debug.h>
#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/cip/plc_type.h>
#include <stdint.h>

/*
 * State shared by every CIP tag, whatever the family.
 *
 * CIP_TAG_BASE_STRUCT follows the TAG_BASE_STRUCT idiom in lib/tag.h: a macro
 * expanded into each family's tag struct rather than a nested struct, so field
 * accesses stay direct.
 *
 * NOTE: expanding this into two family structs makes their layouts identical by
 * construction.  That is deliberate, but nothing may cast one family's tag to
 * another's -- the fields past this macro differ.  ab_common.c used to rely on
 * the layouts happening to match; that was a bug, not an interface.
 */

/* the longest encoded tag name, and the longest encoded type info, in bytes. */
#define MAX_TAG_NAME (260)
#define MAX_TAG_TYPE_INFO (64)

/*
 * The element type of a tag's data.
 *
 * The last four are not CIP data types at all, but markers for the special tag
 * kinds whose payload the library synthesises.
 */
typedef enum {
    CIP_TYPE_BOOL,
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
    CIP_TYPE_TAG_ENTRY,   /* pseudo type: an entry from a tag listing. */
    CIP_TYPE_TAG_UDT,     /* pseudo type: a UDT definition. */
    CIP_TYPE_TAG_RAW,     /* pseudo type: a raw CIP tag. */
    CIP_TYPE_TAG_IDENTITY /* pseudo type: CIP Identity Object data. */
} cip_elem_type_t;


/* defined in cip/standard_tag.h; only ever held here as a pointer. */
struct cip_standard_tag_ops_t;


/*
 * The state every CIP tag carries, whatever the family.
 *
 * One thing is deliberately NOT here: the PCCC fields (file_type,
 * req_pccc_seq_num), which belong to PCCC tags only.
 */
#define CIP_TAG_BASE_STRUCT                                                                \
    TAG_BASE_STRUCT;                                                                       \
                                                                                           \
    int use_connected_msg;                                                                 \
                                                                                           \
    /* the encoded symbolic name */                                                        \
    uint8_t encoded_name[MAX_TAG_NAME];                                                    \
    int encoded_name_size;                                                                 \
                                                                                           \
    /* the encoded type, as it came off the wire */                                        \
    uint8_t encoded_type_info[MAX_TAG_TYPE_INFO];                                          \
    int encoded_type_info_size;                                                            \
                                                                                           \
    cip_elem_type_t elem_type;                                                             \
    int elem_count;                                                                        \
    int elem_size;                                                                         \
                                                                                           \
    int special_tag;                                                                       \
                                                                                           \
    /* standard tags: how much data may one packet carry? */                               \
    int write_data_per_packet;                                                             \
                                                                                           \
    /*                                                                                     \
     * What this tag's family can do, set once at creation.  The shared engines read this  \
     * instead of asking what plc_type is.  See cip/standard_tag.h.                        \
     */                                                                                    \
    const struct cip_standard_tag_ops_t *std_ops;                                          \
                                                                                           \
    /*                                                                                     \
     * The module ID this tag's work is logged under.  Callers filter on these, so a       \
     * shared engine still has to log as the family it is serving.  Set at creation.       \
     */                                                                                    \
    debug_module_t debug_module;                                                           \
                                                                                           \
    uint32_t next_id;       /* listing tags */                                             \
    uint8_t udt_get_fields; /* UDT tags */                                                 \
    uint16_t udt_id;                                                                       \
                                                                                           \
    /*                                                                                     \
     * Consecutive fragment responses that carried no payload.                             \
     *                                                                                     \
     * A partial-transfer status with zero bytes is legitimate: when several requests are  \
     * packed into one packet the earlier ones can consume all the room, leaving the later \
     * ones only a bare CIP header.  It is also what a PLC would send forever to keep us   \
     * asking for the same fragment, so count them and give up rather than loop.  Reset    \
     * whenever a response actually delivers data.                                         \
     */                                                                                    \
    int fragment_retry_count;                                                              \
                                                                                           \
    int pre_write_read;                                                                    \
    int first_read;                                                                        \
    int offset;                                                                            \
    int allow_packing;                                                                     \
                                                                                           \
    int read_in_progress;                                                                  \
    int write_in_progress;                                                                 \
                                                                                           \
    /* how do we talk to this device? */                                                   \
    cip_plc_type_t plc_type;                                                               \
                                                                                           \
    /* pointer back to the connection */                                                   \
    cip_conn_p session;                                                                    \
                                                                                           \
    /* the in-flight request object */                                                     \
    cip_request_p req


/* The PCCC data file a tag addresses.  PCCC rides inside CIP, so this lives here. */
typedef enum {
    PCCC_FILE_UNKNOWN = 0x00, /* UNKNOWN! */
    PCCC_FILE_ASCII = 0x8e,
    PCCC_FILE_BCD = 0x8f,
    PCCC_FILE_BIT = 0x85,
    PCCC_FILE_BLOCK_TRANSFER = 0x00, /* UNKNOWN! */
    PCCC_FILE_CONTROL = 0x88,
    PCCC_FILE_COUNTER = 0x87,
    PCCC_FILE_FLOAT = 0x8a,
    PCCC_FILE_INPUT = 0x83,
    PCCC_FILE_INT = 0x89,
    PCCC_FILE_LONG_INT = 0x91,
    PCCC_FILE_MESSAGE = 0x92,
    PCCC_FILE_OUTPUT = 0x82,
    PCCC_FILE_PID = 0x93,
    PCCC_FILE_SFC = 0x00, /* UNKNOWN! */
    PCCC_FILE_STATUS = 0x84,
    PCCC_FILE_STRING = 0x8d,
    PCCC_FILE_TIMER = 0x86
} pccc_file_t;


/*
 * The one tag struct every CIP family uses.
 *
 * The PCCC members are only meaningful to PCCC tags; the few bytes they cost a
 * Logix or OMRON tag are cheaper than a second struct that has to be kept in step.
 */
struct cip_tag_t {
    CIP_TAG_BASE_STRUCT;

    /* PCCC only: the data file this tag addresses. */
    pccc_file_t file_type;

    /*
     * PCCC only: TNS of the request we last put on the wire.  The response has to carry
     * the same one, otherwise a late reply to a request that already timed out gets
     * applied to whatever operation is in flight now.
     */
    uint16_t req_pccc_seq_num;
};

typedef struct cip_tag_t *cip_tag_p;


/*
 * Abort the request this tag has in flight, if any, and leave the tag idle.
 * The _only form keeps tag->offset, so a fragmented transfer can resume; the other
 * resets it, discarding the partial transfer.
 */
extern int cip_tag_abort_request_only(cip_tag_p tag);
extern int cip_tag_abort_request(cip_tag_p tag);


/*
 * The vtable entries a tag carries until its family fills in its own.  Each returns
 * PLCTAG_ERR_NOT_IMPLEMENTED and logs; reaching one is a bug in that family's tag_create.
 */
extern int cip_tag_unimplemented_abort(plc_tag_p tag);
extern int cip_tag_unimplemented_read(plc_tag_p tag);
extern int cip_tag_unimplemented_status(plc_tag_p tag);
extern int cip_tag_unimplemented_tickler(plc_tag_p tag);
extern int cip_tag_unimplemented_write(plc_tag_p tag);

/* the vtable's status entry: PENDING while an operation is in flight, else tag->status. */
extern int cip_tag_status(cip_tag_p tag);

/*
 * The vtable's abort entry: stop anything in flight and leave the tag at
 * PLCTAG_ERR_ABORT.  Unlike cip_tag_abort_request() it also sets tag->status, and it
 * aborts even with no request in flight.
 */
extern int cip_tag_abort(cip_tag_p tag);

/*
 * The tag attributes every CIP family answers identically.  A family's attr_def_t table
 * points straight at these; only the attributes whose meaning really differs by family
 * stay in that family's own file.
 */
extern int32_t cip_tag_get_elem_size(plc_tag_p raw_tag, int32_t *result);
extern int32_t cip_tag_get_elem_count(plc_tag_p raw_tag, int32_t *result);
extern int32_t cip_tag_get_connection_status(plc_tag_p raw_tag, int32_t *result);
extern int32_t cip_tag_get_connection_inactivity_timeout_ms(plc_tag_p raw_tag, int32_t *result);
extern int32_t cip_tag_set_connection_inactivity_timeout_ms(plc_tag_p raw_tag, int32_t value);
extern int32_t cip_tag_get_use_connected_msg(plc_tag_p raw_tag, int32_t *result);
extern int32_t cip_tag_get_allow_packing(plc_tag_p raw_tag, int32_t *result);
extern int32_t cip_tag_get_gateway(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length);
extern int32_t cip_tag_get_gateway_size(plc_tag_p raw_tag);
extern int32_t cip_tag_get_gateway_port(plc_tag_p raw_tag, int32_t *result);
extern int32_t cip_tag_get_path(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length);
extern int32_t cip_tag_get_path_size(plc_tag_p raw_tag);
extern int32_t cip_tag_get_conn_only_use_old_forward_open(plc_tag_p raw_tag, int32_t *result);

/* true when a status code is a failure rather than OK or PENDING. */
#define rc_is_error(rc) ((rc) < PLCTAG_STATUS_OK)

/* Collect the result of the tag's in-flight request; PENDING while it is still out. */
extern int cip_check_request_status(cip_tag_p tag);

/* Encode a tag's symbolic name into tag->encoded_name as a CIP path. */
extern int cip_encode_tag_name(cip_tag_p tag, const char *name);

/*
 * Set up one of the single-element special tags (@raw, @identity).  They differ only in
 * the element type, the byte order and the vtable; everything else about them is the same
 * one-byte, one-element, no-name shape.
 */
/*
 * CIP SHORT_STRING: a one-byte count followed by exactly that many characters, with no
 * terminator and no padding out to a capacity, so the encoded size varies with the content.
 * The one-byte count puts the hard ceiling at 255 characters.
 *
 * This is the CIP type, not a PLC family's idea of a string.  Micro800 tags use it, and so
 * does the product name in an Identity object reply.  It is emphatically not the Logix
 * STRING UDT -- a four-byte count, 82 characters and two pad bytes, 88 bytes however short
 * the text -- and reading one as the other misdecodes even a single string.
 */
extern tag_byte_order_t cip_short_string_byte_order;

extern int cip_setup_special_tag(cip_tag_p tag, cip_elem_type_t elem_type, tag_byte_order_t *byte_order,
                                 tag_vtable_p vtable, debug_module_t debug_module);
