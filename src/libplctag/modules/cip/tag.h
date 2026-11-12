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
#include <libplctag/api/libplctag.h>
#include <stdint.h>
#include <utils/atomic_utils.h>
#include <utils/attr.h>

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


typedef struct cip_tag_t *cip_tag_p;

/*
 * A tag kind's wire format, set with its vtable when the family sets the tag up.  The
 * connection's handler calls these; NULL where the kind does not do that direction.
 *
 * encode: write one request into dest, which holds capacity bytes, and describe it in plan.
 * PLCTAG_ERR_TOO_SMALL when it does not fit.  No side effects on the tag's progress, so a
 * request that did not fit can be encoded again later.
 *
 * decode: apply one reply, the CIP message with the framing stripped.  OK when the operation
 * is done, PENDING when it needs another exchange.
 *
 * sent: optional.  The handler is putting the request just encoded at request on the wire,
 * under session_mutex.  For what can only be drawn once the request is sure to go: PCCC's TNS.
 */
typedef struct {
    int (*encode_read)(cip_tag_p tag, uint8_t *dest, int32_t capacity, tag_op_plan_t *plan);
    int (*decode_read)(cip_tag_p tag, uint8_t *reply, int32_t reply_size);
    int (*encode_write)(cip_tag_p tag, uint8_t *dest, int32_t capacity, tag_op_plan_t *plan);
    int (*decode_write)(cip_tag_p tag, uint8_t *reply, int32_t reply_size);
    void (*sent)(cip_tag_p tag, uint8_t *request);
} cip_tag_codec_t;

/*
 * The one tag struct every CIP family uses.
 *
 * The PCCC members are only meaningful to PCCC tags; the few bytes they cost a
 * Logix or OMRON tag are cheaper than a second struct that has to be kept in step.
 */
struct cip_tag_t {
    TAG_BASE_STRUCT;

    int32_t use_connected_msg;

    /* the encoded symbolic name */
    uint8_t encoded_name[MAX_TAG_NAME];
    int32_t encoded_name_size;

    /* the encoded type, as it came off the wire */
    uint8_t encoded_type_info[MAX_TAG_TYPE_INFO];
    int32_t encoded_type_info_size;

    cip_elem_type_t elem_type;

    bool special_tag;

    /* standard tags: how much data may one packet carry? */
    int32_t write_data_per_packet;

    /*
     * What this tag's family can do, set once at creation.  The shared engines read this
     * instead of asking which family this is.  See cip/standard_tag.h.
     */
    const struct cip_standard_tag_ops_t *std_ops;

    /* this tag kind's wire format; NULL until the family sets the tag up. */
    const cip_tag_codec_t *codec;

    /*
     * The module ID this tag's work is logged under.  Callers filter on these, so a
     * shared engine still has to log as the family it is serving.  Set at creation.
     */
    debug_module_t debug_module;

    /*
     * OMRON: a plain read was refused as too large, so the next read asks the PLC for the
     * variable's size (Get Attributes All) before reading it in sized segments.
     */
    bool size_probe;

    uint32_t next_id;       /* listing tags */
    uint8_t udt_get_fields; /* UDT tags */
    uint16_t udt_id;

    /*
     * Consecutive fragment responses that carried no payload.
     *
     * A partial-transfer status with zero bytes is legitimate: when several requests are
     * packed into one packet the earlier ones can consume all the room, leaving the later
     * ones only a bare CIP header.  It is also what a PLC would send forever to keep us
     * asking for the same fragment, so count them and give up rather than loop.  Reset
     * whenever a response actually delivers data.
     */
    int32_t fragment_retry_count;


    /*
     * PENDING while the create read is out, OK once it succeeded or when there is none, and
     * the error that failed it otherwise.  An error is final.  Its own field because the
     * library resets tag->status at the start of every operation.
     */
    plctag_error_code_t create_read_status;

    /*
     * A copy of the tag string's attributes, kept while the tag waits for a probed connection
     * to identify the device and so pick its family.  NULL otherwise.
     */
    attr deferred_attribs;
    /* this tag's request in the exchange on the wire, -1 when it has none.  Under session_mutex. */
    int32_t reply_index;
    int32_t offset;
    bool allow_packing;

    /* PCCC only: the request prefix and reply checks, plain or DH+.  See ab/pccc.h. */
    const struct pccc_framing_t *pccc_framing;


    /* PCCC only: the data file this tag addresses. */
    pccc_file_t file_type;

    /*
     * PCCC only: TNS of the request we last put on the wire.  The response has to carry
     * the same one, otherwise a late reply to a request that already timed out gets
     * applied to whatever operation is in flight now.
     */
    uint16_t req_pccc_seq_num;

    /*
     * The request goes to the device at the gateway itself, with no Unconnected Send wrapper
     * and no route path: PCCC with no path and no DH+, and @identity with no path.  Fixed
     * once the tag is set up.
     */
    bool unrouted;

    /*
     * The library has finished creating the tag, so the connection's handler may see it.  A
     * request started before then waits for vtable->activate to queue it.  Atomic: a shutdown
     * destroying the tag reads it without the API mutex.
     */
    atomic_bool activated;
};

/* the tag's connection.  Every CIP tag's conn is a cip_conn_t: its base is the first member. */
static inline cip_conn_p cip_tag_conn(cip_tag_p tag) { return (cip_conn_p)tag->conn; }


/*
 * The vtable entries a tag carries until its family fills in its own.  Each returns
 * PLCTAG_ERR_NOT_IMPLEMENTED and logs; reaching one is a bug in that family's tag_create.
 */
extern int cip_tag_unimplemented_abort(plc_tag_p tag);
extern int cip_tag_unimplemented_read(plc_tag_p tag);
extern int cip_tag_unimplemented_status(plc_tag_p tag);
extern int cip_tag_unimplemented_write(plc_tag_p tag);

/*
 * End the create read, if one is out, with rc as its result: OK completes the create and
 * anything else fails it.  Call when a read finishes.
 */
extern void cip_tag_finish_create(cip_tag_p tag, int rc);

/*
 * End a create read that did not succeed, with rc as the result.  A failed create is final:
 * the tag reports rc on every later status, read and write until it is destroyed.
 */
extern void cip_tag_fail_create(cip_tag_p tag, plctag_error_code_t rc);

/*
 * The vtable's abort entry for tags with a create read: an abort while it is out fails the
 * create, and an abort after the create failed leaves that error alone.
 */
extern int cip_data_tag_abort(plc_tag_p tag);

/*
 * The vtable's status entry: the create error if the create failed, PENDING while an
 * operation is in flight, else tag->status.
 */
extern int cip_tag_status(plc_tag_p tag);

/*
 * The vtable's abort entry: take the tag off its connection's list and out of any exchange in
 * flight, and leave it at PLCTAG_ERR_ABORT.  It aborts even with nothing in flight.
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
extern int32_t cip_tag_get_connection_retry_max_delay_ms(plc_tag_p raw_tag, int32_t *result);
extern int32_t cip_tag_set_connection_retry_max_delay_ms(plc_tag_p raw_tag, int32_t value);
extern int32_t cip_tag_get_use_connected_msg(plc_tag_p raw_tag, int32_t *result);
extern int32_t cip_tag_get_allow_packing(plc_tag_p raw_tag, int32_t *result);
extern int32_t cip_tag_get_gateway(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length);
extern int32_t cip_tag_get_gateway_size(plc_tag_p raw_tag);
extern int32_t cip_tag_get_gateway_port(plc_tag_p raw_tag, int32_t *result);
extern int32_t cip_tag_get_path(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length);
extern int32_t cip_tag_get_path_size(plc_tag_p raw_tag);
extern int32_t cip_tag_get_conn_only_use_old_forward_open(plc_tag_p raw_tag, int32_t *result);
extern int32_t cip_tag_get_raw_tag_type_bytes(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length);
extern int32_t cip_tag_get_raw_tag_type_bytes_size(plc_tag_p raw_tag);
extern int32_t cip_tag_get_raw_tag_type_bytes_length(plc_tag_p raw_tag, int32_t *result);
extern int32_t cip_tag_get_plc(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length);
extern int32_t cip_tag_get_plc_size(plc_tag_p raw_tag);

/*
 * The attribute table entries every CIP tag shares, for a family table to start with.  Only
 * elem_type differs: a PCCC tag reports its data file type there.
 */
#define CIP_TAG_COMMON_ATTRIBS                                                                                          \
    {.name = "elem_size",                                                                                               \
     .type = ATTR_TYPE_INT,                                                                                             \
     .description = "The size in bytes of a single element of this tag.",                                               \
     .get_int = cip_tag_get_elem_size},                                                                                 \
        {.name = "elem_count",                                                                                          \
         .type = ATTR_TYPE_INT,                                                                                         \
         .description = "The number of elements this tag holds.",                                                       \
         .get_int = cip_tag_get_elem_count},                                                                            \
        {.name = "connection_status",                                                                                   \
         .type = ATTR_TYPE_INT,                                                                                         \
         .description = "The state of the connection this tag uses, as a plc_tag_conn_status_t.",                       \
         .get_int = cip_tag_get_connection_status},                                                                     \
        {.name = "connection_inactivity_timeout_ms",                                                                    \
         .type = ATTR_TYPE_INT,                                                                                         \
         .description = "Disconnect the connection after this many milliseconds without traffic.",                      \
         .get_int = cip_tag_get_connection_inactivity_timeout_ms,                                                       \
         .set_int = cip_tag_set_connection_inactivity_timeout_ms},                                                      \
        {.name = "connection_retry_max_delay_ms",                                                                       \
         .type = ATTR_TYPE_INT,                                                                                         \
         .description = "Longest delay between reconnect attempts after an error, in milliseconds.  Shared by every "   \
                        "tag on this connection.  A change applies from the next retry; a wait already running is not " \
                        "cut short.",                                                                                   \
         .get_int = cip_tag_get_connection_retry_max_delay_ms,                                                          \
         .set_int = cip_tag_set_connection_retry_max_delay_ms},                                                         \
        {.name = "raw_tag_type_bytes",                                                                                  \
         .type = ATTR_TYPE_BYTES,                                                                                       \
         .description = "The encoded CIP type information for this tag, empty when the PLC sends none.",                \
         .get_bytes = cip_tag_get_raw_tag_type_bytes,                                                                   \
         .get_bytes_size = cip_tag_get_raw_tag_type_bytes_size},                                                        \
        /* Deprecated: it predates plc_tag_get_attribute_size(). */                                                     \
        {.name = "raw_tag_type_bytes.length",                                                                           \
         .type = ATTR_TYPE_INT,                                                                                         \
         .description = "Deprecated, use plc_tag_get_attribute_size(). The size of raw_tag_type_bytes in bytes.",       \
         .get_int = cip_tag_get_raw_tag_type_bytes_length},                                                             \
        {.name = "plc",                                                                                                 \
         .type = ATTR_TYPE_STRING,                                                                                      \
         .description = "The PLC family this tag talks to, as a canonical name.",                                       \
         .get_bytes = cip_tag_get_plc,                                                                                  \
         .get_bytes_size = cip_tag_get_plc_size},                                                                       \
        {.name = "use_connected_msg",                                                                                   \
         .type = ATTR_TYPE_INT,                                                                                         \
         .description = "This tag uses CIP connected messaging.",                                                       \
         .get_int = cip_tag_get_use_connected_msg},                                                                     \
        {.name = "allow_packing",                                                                                       \
         .type = ATTR_TYPE_INT,                                                                                         \
         .description = "This tag's requests may be packed with others into one CIP request.",                          \
         .get_int = cip_tag_get_allow_packing},                                                                         \
        {.name = "gateway",                                                                                             \
         .type = ATTR_TYPE_STRING,                                                                                      \
         .description = "The host name or address of the gateway this tag's connection uses.",                          \
         .get_bytes = cip_tag_get_gateway,                                                                              \
         .get_bytes_size = cip_tag_get_gateway_size},                                                                   \
        {.name = "gateway_port",                                                                                        \
         .type = ATTR_TYPE_INT,                                                                                         \
         .description = "The TCP port this tag's connection uses.",                                                     \
         .get_int = cip_tag_get_gateway_port},                                                                          \
        {.name = "path",                                                                                                \
         .type = ATTR_TYPE_BYTES,                                                                                       \
         .description = "The encoded CIP path from the gateway to the PLC.",                                            \
         .get_bytes = cip_tag_get_path,                                                                                 \
         .get_bytes_size = cip_tag_get_path_size},                                                                      \
    {                                                                                                                   \
        .name = "conn_only_use_old_forward_open", .type = ATTR_TYPE_INT,                                                \
        .description = "This tag's connection uses the original Forward Open only, never the large one.",               \
        .get_int = cip_tag_get_conn_only_use_old_forward_open                                                           \
    }

/* the table for every CIP tag but PCCC data files: elem_type is the CIP element type. */
extern const attr_def_t cip_tag_attribs[];

/* true when a status code is a failure rather than OK or PENDING. */
#define rc_is_error(rc) ((rc) < PLCTAG_STATUS_OK)


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
                                 tag_vtable_p vtable, const cip_tag_codec_t *codec, debug_module_t debug_module);




/* the vtable's activate: the library has finished the tag, so the handler may now see it. */
extern int cip_tag_activate(plc_tag_p tag_arg);

/* the vtable's wake_plc and tag_data_written: make the tag due so its handler looks at it. */
extern int cip_tag_wake_plc(plc_tag_p tag_arg);

/* How the handler ends an operation: tag_op_end(), then the create if this was its read. */
extern void cip_tag_op_done(cip_tag_p tag, int rc);


/*
 * The vtable's read and write for every CIP tag kind: check the op can start.  The caller has
 * started the op, and queues the tag with wake_plc once it lets go of the API mutex; the
 * handler's own auto-sync ops are already on its list.  A write waits for the create to finish.
 */
extern int cip_tag_read_start(plc_tag_p tag_arg);
extern int cip_tag_write_start(plc_tag_p tag_arg);


/*
 * The steps of CIP tag creation that do not vary between PLC families.
 *
 * Every CIP constructor runs the same skeleton: allocate, initialise the generic parts,
 * resolve the element count, then size the tag either from the attributes or by leaving it
 * for the PLC to say.  Only the debug module and the starting vtable differ, so they are
 * parameters, and the rest lives here.  A family that skips one of these is a family whose
 * tags are missing a check, which is how the Rockwell side came to have no bound on the
 * element count at all.
 */
extern cip_tag_p cip_tag_alloc(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                               void *userdata, tag_vtable_p default_vtable,
                               debug_module_t debug_module);
extern int cip_tag_resolve_elem_count(cip_tag_p tag, attr attribs, debug_module_t debug_module);

/* for a tag whose size the PLC reports on the first read. */
extern int cip_tag_size_from_plc(cip_tag_p tag, debug_module_t debug_module);

/* for a tag whose size the caller states through elem_size.  Allocates the tag data buffer. */
extern int cip_tag_size_from_attributes(cip_tag_p tag, attr attribs, debug_module_t debug_module);


/*
 * The element type and size a symbolic CIP tag string states, if any.
 *
 * elem_type names the type, and its size follows.  Without it, elem_size is taken as given,
 * and a family that learns its size from the PLC may legitimately have neither.  One copy
 * for Rockwell and OMRON: the two lists were identical.
 */
extern int cip_tag_parse_elem_type(cip_tag_p tag, attr attribs, debug_module_t debug_module);
