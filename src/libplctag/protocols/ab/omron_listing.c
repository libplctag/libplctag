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
 * OMRON NJ/NX tag and UDT listing, @tags and @udt/N.  Omron has neither the Logix Symbol nor the
 * Template object, so both are built from Omron's own objects into the Logix layouts, and the
 * same decoding code reads either PLC's listing.  The wire formats follow aphyt
 * (aphyt/omron/n_series.py), which is known to work against NJ/NX hardware.
 *
 * Each read is a chain of requests.  The tickler decodes a reply, and a decoder that returns
 * PENDING has set up the state the next request is built from.
 */

#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/ab/ab_common.h>
#include <libplctag/protocols/ab/cip.h>
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/ab/error_codes.h>
#include <libplctag/protocols/ab/omron_listing.h>
#include <libplctag/protocols/ab/session.h>
#include <libplctag/protocols/ab/tag.h>
#include <platform.h>
#include <stdbool.h>
#include <stdint.h>
#include <utils/debug.h>


#define OMRON_SVC_GET_ATTRIBUTES_ALL ((uint8_t)0x01)
#define OMRON_SVC_GET_INSTANCE_LIST ((uint8_t)0x5F)
#define OMRON_CLASS_TAG_NAME_SERVER ((uint8_t)0x6A)
#define OMRON_CLASS_VARIABLE_TYPE ((uint8_t)0x6C)
#define CIP_SYMBOLIC_SEGMENT ((uint8_t)0x91)

#define OMRON_TYPE_STRUCT ((uint8_t)0xA2)
#define OMRON_TYPE_ABBREV_STRUCT ((uint8_t)0xA0)
#define OMRON_TYPE_ARRAY ((uint8_t)0xA3)

/* kind of variable in an instance list request. */
#define OMRON_KIND_SYSTEM ((uint16_t)1)
#define OMRON_KIND_USER ((uint16_t)2)

/* variables asked for per instance list request; the PLC returns fewer if they do not fit. */
#define OMRON_LIST_BATCH ((uint32_t)100)

/* where a @tags read is, kept in tag->udt_get_fields. */
#define OMRON_LIST_USER ((uint8_t)0)
#define OMRON_LIST_SYSTEM ((uint8_t)1)
#define OMRON_LIST_TYPES ((uint8_t)2)

/* the largest request built here: Get Attributes All on a symbolic segment of a 255-byte name. */
#define OMRON_MAX_REQUEST_SIZE (2 + 2 + 256)

/* the Logix tag listing entry ahead of each name. */
#define LIST_ENTRY_SIZE (22)
#define LIST_ENTRY_TYPE_OFFSET (4)
#define LIST_ENTRY_ELEM_SIZE_OFFSET (6)
#define LIST_ENTRY_DIMS_OFFSET (8)
#define LIST_ENTRY_NAME_LEN_OFFSET (20)

/* the Logix UDT header and member description. */
#define UDT_HEADER_SIZE (14)
#define UDT_MEMBER_SIZE (8)

#define LOGIX_TYPE_STRUCT ((uint16_t)0x8000)
#define LOGIX_TYPE_ARRAY ((uint16_t)0x2000)
#define LOGIX_TYPE_ID_MASK ((uint16_t)0x0FFF)

/* one Variable Type Object instance, parsed. */
typedef struct {
    uint32_t size;
    uint8_t type;
    uint8_t array_type;
    uint32_t elem_count;
    uint16_t num_members;
    uint16_t crc;
    uint8_t *name;
    int32_t name_len;
    uint32_t next_id;
    uint32_t nesting_id;
} omron_vto_t;

static int omron_special_read_start(plc_tag_p tag_arg);
static int omron_special_tickler(plc_tag_p tag_arg);
static int send_next_request(ab_tag_p tag);
static int32_t encode_listing_request(ab_tag_p tag, uint8_t *dest);
static int32_t encode_udt_request(ab_tag_p tag, uint8_t *dest);
static int decode_reply(ab_tag_p tag);
static int decode_instance_list(ab_tag_p tag, uint8_t *data, int32_t data_size);
static int decode_variable_attributes(ab_tag_p tag, uint8_t *data, int32_t data_size);
static int decode_udt_header(ab_tag_p tag, omron_vto_t *vto);
static int decode_udt_member(ab_tag_p tag, omron_vto_t *vto);
static int parse_vto(uint8_t *data, int32_t data_size, omron_vto_t *vto);
static int append_bytes(ab_tag_p tag, uint8_t *src, int32_t len);
static void reset_read(ab_tag_p tag);
static uint32_t get_u32(const uint8_t *data);

static struct tag_vtable_t omron_special_vtable = {
    .abort = ab_tag_vtable_abort,
    .read = omron_special_read_start,
    .status = ab_tag_status,
    .tickler = omron_special_tickler,
    .write = NULL,
    .wake_plc = NULL,
    .tag_data_written = NULL,
    .attribs = ab_attribs,
};

/* @tags names are counted by a 2-byte word and @udt/N names are zero terminated, as on Logix. */
static tag_byte_order_t listing_byte_order = {.is_allocated = 0,
                                              .int16_order = {0, 1},
                                              .int32_order = {0, 1, 2, 3},
                                              .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                              .float32_order = {0, 1, 2, 3},
                                              .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                              .str_is_defined = 1,
                                              .str_is_counted = 1,
                                              .str_is_fixed_length = 0,
                                              .str_is_zero_terminated = 0,
                                              .str_is_byte_swapped = 0,
                                              .str_pad_to_multiple_bytes = 1,
                                              .str_count_word_bytes = 2,
                                              .str_max_capacity = 0,
                                              .str_total_length = 0,
                                              .str_pad_bytes = 0};

static tag_byte_order_t udt_byte_order = {.is_allocated = 0,
                                          .int16_order = {0, 1},
                                          .int32_order = {0, 1, 2, 3},
                                          .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                          .float32_order = {0, 1, 2, 3},
                                          .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                          .str_is_defined = 1,
                                          .str_is_counted = 0,
                                          .str_is_fixed_length = 0,
                                          .str_is_zero_terminated = 1,
                                          .str_is_byte_swapped = 0,
                                          .str_pad_to_multiple_bytes = 1,
                                          .str_count_word_bytes = 0,
                                          .str_max_capacity = 0,
                                          .str_total_length = 0,
                                          .str_pad_bytes = 0};


int omron_setup_listing_tag(ab_tag_p tag) {
    tag->special_tag = 1;
    tag->elem_type = AB_TYPE_TAG_ENTRY;
    tag->elem_count = 1;
    tag->elem_size = 1;
    tag->byte_order = &listing_byte_order;
    tag->vtable = &omron_special_vtable;

    return PLCTAG_STATUS_OK;
}


int omron_setup_udt_tag(ab_tag_p tag, const char *name) {
    int udt_id = 0;

    if(str_to_int(name + str_length("@udt/"), &udt_id) != PLCTAG_STATUS_OK || udt_id < 1 || udt_id > UINT16_MAX) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "UDT id in %s must be a number from 1 to 65535!", name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    tag->udt_id = (uint16_t)udt_id;
    tag->special_tag = 1;
    tag->elem_type = AB_TYPE_TAG_UDT;
    tag->elem_count = 1;
    tag->elem_size = 1;
    tag->byte_order = &udt_byte_order;
    tag->vtable = &omron_special_vtable;

    return PLCTAG_STATUS_OK;
}


static int omron_special_read_start(plc_tag_p tag_arg) {
    ab_tag_p tag = (ab_tag_p)tag_arg;
    int rc = PLCTAG_STATUS_OK;

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "A write is in progress on a listing tag!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    reset_read(tag);

    tag->read_in_progress = 1;

    rc = send_next_request(tag);
    if(rc != PLCTAG_STATUS_OK) {
        tag->read_in_progress = 0;
        return rc;
    }

    return PLCTAG_STATUS_PENDING;
}


static int omron_special_tickler(plc_tag_p tag_arg) {
    ab_tag_p tag = (ab_tag_p)tag_arg;
    int rc = check_request_status(tag);

    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Writing a listing tag is not supported!");
        ab_tag_abort_request(tag);
        tag->write_complete = 1;
        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(!tag->read_in_progress) { return tag->status; }

    rc = decode_reply(tag);

    /* the reply is used up whatever it said.  This also clears read_in_progress. */
    ab_tag_abort_request_only(tag);

    if(rc == PLCTAG_STATUS_PENDING) {
        tag->read_in_progress = 1;
        rc = send_next_request(tag);
    }

    if(rc == PLCTAG_STATUS_OK) {
        if(tag->req) { return (tag->status = (int8_t)PLCTAG_STATUS_PENDING); }

        tag->first_read = 0;
    } else {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "%s read failed: %s!",
               (tag->elem_type == AB_TYPE_TAG_UDT ? "UDT" : "Tag listing"), plc_tag_decode_error(rc));
        ab_tag_abort_request(tag);
        reset_read(tag);
    }

    tag->read_in_progress = 0;
    tag->read_complete = 1;
    tag->status = (int8_t)rc;

    return rc;
}


/* Build and queue the request the tag's state calls for.  tag->req is set on success. */
static int send_next_request(ab_tag_p tag) {
    uint8_t buf[OMRON_MAX_REQUEST_SIZE];
    ab_request_p req = NULL;
    int32_t size = (tag->elem_type == AB_TYPE_TAG_UDT) ? encode_udt_request(tag, buf) : encode_listing_request(tag, buf);
    int rc = PLCTAG_STATUS_OK;

    if(size > session_get_available_cip_payload_space(tag->session)) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Request of %d bytes does not fit in the connection!",
               (int)size);
        return PLCTAG_ERR_TOO_LARGE;
    }

    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    mem_copy(req->data, buf, size);
    req->request_size = (int)size;
    req->connected = (tag->use_connected_msg != 0);
    req->allow_packing = tag->allow_packing;

    rc = session_add_request(tag->session, req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        rc_dec(req);
        return rc;
    }

    tag->req = req;

    return PLCTAG_STATUS_OK;
}


/*
 * @tags
 *
 * Get Instance List (0x5F) pages through the user variables and then the system ones, which
 * gives only names.  Each name then gets Get Attributes All for its type and dimensions.  The
 * entries are built in place: the name pages append them with the type fields zero, and the
 * attribute replies fill those in, tag->offset walking the buffer.
 */
static int32_t encode_listing_request(ab_tag_p tag, uint8_t *dest) {
    uint8_t *data = dest;

    if(tag->udt_get_fields == OMRON_LIST_TYPES) {
        uint8_t *entry = tag->data + tag->offset;
        int32_t name_len = entry[LIST_ENTRY_NAME_LEN_OFFSET];

        /* Get Attributes All on the variable's symbolic segment. */
        *data++ = OMRON_SVC_GET_ATTRIBUTES_ALL;
        *data++ = (uint8_t)((2 + name_len + (name_len & 1)) / 2);
        *data++ = CIP_SYMBOLIC_SEGMENT;
        *data++ = (uint8_t)name_len;
        mem_copy(data, entry + LIST_ENTRY_SIZE, name_len);
        data += name_len;
        if(name_len & 1) { *data++ = 0; }
    } else {
        uint16_t kind = (tag->udt_get_fields == OMRON_LIST_USER) ? OMRON_KIND_USER : OMRON_KIND_SYSTEM;

        /* service, then class 0x6A instance 0 as a 16-bit instance. */
        *data++ = OMRON_SVC_GET_INSTANCE_LIST;
        *data++ = 3;
        *data++ = 0x20;
        *data++ = OMRON_CLASS_TAG_NAME_SERVER;
        *data++ = 0x25;
        *data++ = 0x00;
        *data++ = 0x00;
        *data++ = 0x00;

        /* first instance, how many, and which kind. */
        for(int32_t i = 0; i < 4; i++) { *data++ = (uint8_t)(tag->next_id >> (8 * i)); }
        for(int32_t i = 0; i < 4; i++) { *data++ = (uint8_t)(OMRON_LIST_BATCH >> (8 * i)); }
        *data++ = (uint8_t)(kind & 0xFF);
        *data++ = (uint8_t)(kind >> 8);
    }

    return (int32_t)(data - dest);
}


/* Get Attributes All on Variable Type Object instance N, the UDT or one of its members. */
static int32_t encode_udt_request(ab_tag_p tag, uint8_t *dest) {
    uint16_t instance = (tag->udt_get_fields == 0) ? tag->udt_id : (uint16_t)tag->next_id;

    dest[0] = OMRON_SVC_GET_ATTRIBUTES_ALL;
    dest[1] = 3;
    dest[2] = 0x20;
    dest[3] = OMRON_CLASS_VARIABLE_TYPE;
    dest[4] = 0x25;
    dest[5] = 0x00;
    dest[6] = (uint8_t)(instance & 0xFF);
    dest[7] = (uint8_t)(instance >> 8);

    return 8;
}


/* OK when the read is done, PENDING when there is another request to send, or an error. */
static int decode_reply(ab_tag_p tag) {
    uint8_t *data = tag->req->data + sizeof(cip_header);
    uint8_t *data_end = tag->req->data + tag->req->request_size;
    int32_t data_size = (int32_t)(data_end - data);
    bool is_udt = (tag->elem_type == AB_TYPE_TAG_UDT);
    uint8_t service = (is_udt || tag->udt_get_fields == OMRON_LIST_TYPES) ? OMRON_SVC_GET_ATTRIBUTES_ALL : OMRON_SVC_GET_INSTANCE_LIST;
    omron_vto_t vto;
    int rc = PLCTAG_STATUS_OK;

    if(data_size < 0) { return PLCTAG_ERR_TOO_SMALL; }

    rc = cip_check_reply(tag->tag_id, tag->req->data, data_end, service, NULL);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(!is_udt) {
        return (tag->udt_get_fields == OMRON_LIST_TYPES) ? decode_variable_attributes(tag, data, data_size)
                                                         : decode_instance_list(tag, data, data_size);
    }

    rc = parse_vto(data, data_size, &vto);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    return (tag->udt_get_fields == 0) ? decode_udt_header(tag, &vto) : decode_udt_member(tag, &vto);
}


/*
 * Instance list reply: count (2), more-follow flag (1), pad (1), then per variable 4 bytes of
 * unknown meaning, the length of the rest (2), class (2), instance (4) and the name as a short
 * string.
 */
static int decode_instance_list(ab_tag_p tag, uint8_t *data, int32_t data_size) {
    uint8_t *end = data + data_size;
    uint8_t *cur = data + 4;
    uint16_t count = 0;
    bool more = false;

    if(data_size < 4) { return PLCTAG_ERR_TOO_SMALL; }

    count = (uint16_t)(data[0] | (data[1] << 8));
    more = (data[2] != 0);

    for(uint16_t i = 0; i < count; i++) {
        uint8_t entry[LIST_ENTRY_SIZE] = {0};
        int32_t rest_len = 0;
        int32_t name_len = 0;
        int rc = PLCTAG_STATUS_OK;

        if(end - cur < 13) { return PLCTAG_ERR_OUT_OF_BOUNDS; }

        rest_len = cur[4] | (cur[5] << 8);
        name_len = cur[12];
        if(rest_len < 7 + name_len || end - cur < 6 + rest_len) { return PLCTAG_ERR_OUT_OF_BOUNDS; }

        mem_copy(entry, cur + 8, 4); /* the variable's instance. */
        entry[LIST_ENTRY_NAME_LEN_OFFSET] = (uint8_t)name_len;

        rc = append_bytes(tag, entry, LIST_ENTRY_SIZE);
        if(rc == PLCTAG_STATUS_OK) { rc = append_bytes(tag, cur + 13, name_len); }
        if(rc != PLCTAG_STATUS_OK) { return rc; }

        tag->elem_count++;
        cur += 6 + rest_len;
    }

    tag->next_id += count;

    if(more && count > 0) { return PLCTAG_STATUS_PENDING; }

    if(tag->udt_get_fields == OMRON_LIST_USER) {
        tag->udt_get_fields = OMRON_LIST_SYSTEM;
        tag->next_id = 1;
        return PLCTAG_STATUS_PENDING;
    }

    tag->udt_get_fields = OMRON_LIST_TYPES;
    tag->offset = 0;

    return (tag->size > 0) ? PLCTAG_STATUS_PENDING : PLCTAG_STATUS_OK;
}


/*
 * Get Attributes All on a variable, with d dimensions (0 for a scalar): size (4), type (1),
 * array element type (1), d (1), pad, then the count of each dimension (4 each), and the
 * Variable Type Object instance at 20 + 4d.  A scalar's type instance is read from bytes
 * 8-11, which is where aphyt finds it on hardware.
 */
static int decode_variable_attributes(ab_tag_p tag, uint8_t *data, int32_t data_size) {
    uint8_t *entry = tag->data + tag->offset;
    int32_t name_len = entry[LIST_ENTRY_NAME_LEN_OFFSET];
    bool is_array = false;
    int32_t dims = 0;
    uint8_t elem_type = 0;
    uint32_t vto_id = 0;
    uint16_t symbol_type = 0;
    int32_t vto_offset = 0;

    if(data_size < 12) { return PLCTAG_ERR_TOO_SMALL; }

    is_array = (data[4] == OMRON_TYPE_ARRAY);
    dims = is_array ? data[6] : 0;
    elem_type = is_array ? data[5] : data[4];
    vto_offset = is_array ? 20 + (4 * dims) : 8;

    if(dims > 3 || data_size < vto_offset + 4) { return PLCTAG_ERR_BAD_DATA; }

    vto_id = get_u32(data + vto_offset);

    /* ponytail: Logix type ids are 12 bits, so a type instance above 4095 is truncated here; @udt/N takes all 16. */
    if(elem_type == OMRON_TYPE_STRUCT || elem_type == OMRON_TYPE_ABBREV_STRUCT) {
        symbol_type = (uint16_t)(LOGIX_TYPE_STRUCT | (vto_id & LOGIX_TYPE_ID_MASK));
    } else {
        symbol_type = elem_type;
    }

    symbol_type = (uint16_t)(symbol_type | ((uint16_t)dims << 13));

    entry[LIST_ENTRY_TYPE_OFFSET] = (uint8_t)(symbol_type & 0xFF);
    entry[LIST_ENTRY_TYPE_OFFSET + 1] = (uint8_t)(symbol_type >> 8);
    mem_copy(entry + LIST_ENTRY_ELEM_SIZE_OFFSET, data, 2);
    if(dims > 0) { mem_copy(entry + LIST_ENTRY_DIMS_OFFSET, data + 8, 4 * dims); }

    tag->offset += LIST_ENTRY_SIZE + name_len;

    if(tag->offset < tag->size) { return PLCTAG_STATUS_PENDING; }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Listed %d variables.", tag->elem_count);

    tag->udt_get_fields = OMRON_LIST_USER;
    tag->offset = 0;
    tag->next_id = 1;

    return PLCTAG_STATUS_OK;
}


/*
 * @udt/N
 *
 * Get Attributes All on Variable Type Object instance N gives the UDT's size, member count,
 * CRC, name and first member.  Each member's own instance gives its type, size, name and the
 * next member.  tag->udt_get_fields counts the members read, tag->next_id is the next member's
 * instance and tag->offset the byte offset the next member lands at.
 */
static int decode_udt_header(ab_tag_p tag, omron_vto_t *vto) {
    uint8_t header[UDT_HEADER_SIZE] = {0};
    uint8_t zero = 0;
    int rc = PLCTAG_STATUS_OK;

    if(vto->type != OMRON_TYPE_STRUCT) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Variable Type Object %u is type 0x%02x, not a structure!",
               (unsigned int)tag->udt_id, (unsigned int)vto->type);
        return PLCTAG_ERR_BAD_PARAM;
    }

    header[0] = (uint8_t)(tag->udt_id & 0xFF);
    header[1] = (uint8_t)(tag->udt_id >> 8);
    for(int32_t i = 0; i < 4; i++) { header[6 + i] = (uint8_t)(vto->size >> (8 * i)); }
    header[10] = (uint8_t)(vto->num_members & 0xFF);
    header[11] = (uint8_t)(vto->num_members >> 8);
    header[12] = (uint8_t)(vto->crc & 0xFF);
    header[13] = (uint8_t)(vto->crc >> 8);

    /* the header, room for each member's description, then the name. */
    rc = append_bytes(tag, header, UDT_HEADER_SIZE);
    if(rc == PLCTAG_STATUS_OK) { rc = append_bytes(tag, NULL, vto->num_members * UDT_MEMBER_SIZE); }
    if(rc == PLCTAG_STATUS_OK) { rc = append_bytes(tag, vto->name, vto->name_len); }
    if(rc == PLCTAG_STATUS_OK) { rc = append_bytes(tag, &zero, 1); }
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    tag->udt_get_fields = 1;
    tag->next_id = vto->nesting_id;
    tag->offset = 0;

    if(vto->num_members > 0 && vto->nesting_id != 0) { return PLCTAG_STATUS_PENDING; }

    tag->elem_count = 1;
    tag->elem_size = tag->size;
    tag->udt_get_fields = 0;

    return PLCTAG_STATUS_OK;
}


/*
 * One member.  Omron does not report member offsets, so they are laid out the way aphyt does:
 * an atomic member at a multiple of its own size, anything else at an even offset.
 */
static int decode_udt_member(ab_tag_p tag, omron_vto_t *vto) {
    uint16_t num_members = (uint16_t)(tag->data[10] | (tag->data[11] << 8));
    uint8_t *desc = NULL;
    uint8_t elem_type = (vto->type == OMRON_TYPE_ARRAY) ? vto->array_type : vto->type;
    uint32_t count = (vto->type == OMRON_TYPE_ARRAY) ? vto->elem_count : 0;
    uint16_t logix_type = elem_type;
    uint32_t align = 2;
    uint32_t offset = (uint32_t)tag->offset;
    uint32_t total_size = 0;
    uint32_t words = 0;
    uint8_t zero = 0;
    int rc = PLCTAG_STATUS_OK;

    if(elem_type == OMRON_TYPE_STRUCT || elem_type == OMRON_TYPE_ABBREV_STRUCT) {
        logix_type = (uint16_t)(LOGIX_TYPE_STRUCT | (vto->nesting_id & LOGIX_TYPE_ID_MASK));
    } else if(vto->size == 1 || vto->size == 2 || vto->size == 4 || vto->size == 8) {
        align = vto->size;
    }

    if(count > 0) { logix_type = (uint16_t)(logix_type | LOGIX_TYPE_ARRAY); }

    offset = (offset + align - 1) / align * align;
    total_size = vto->size * (count > 0 ? count : 1);

    desc = tag->data + UDT_HEADER_SIZE + ((int32_t)(tag->udt_get_fields - 1) * UDT_MEMBER_SIZE);
    desc[0] = (uint8_t)(count & 0xFF);
    desc[1] = (uint8_t)((count >> 8) & 0xFF);
    desc[2] = (uint8_t)(logix_type & 0xFF);
    desc[3] = (uint8_t)(logix_type >> 8);
    for(int32_t i = 0; i < 4; i++) { desc[4 + i] = (uint8_t)(offset >> (8 * i)); }

    /* desc points into tag->data, which these appends may move. */
    rc = append_bytes(tag, vto->name, vto->name_len);
    if(rc == PLCTAG_STATUS_OK) { rc = append_bytes(tag, &zero, 1); }
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    tag->offset = (int)(offset + total_size);
    tag->next_id = vto->next_id;

    if(tag->udt_get_fields < num_members && vto->next_id != 0) {
        tag->udt_get_fields++;
        return PLCTAG_STATUS_PENDING;
    }

    /* the member description size, in the Logix formula's 32-bit words: bytes = 4 * words - 23. */
    words = ((uint32_t)(tag->size - UDT_HEADER_SIZE) + 23 + 3) / 4;
    for(int32_t i = 0; i < 4; i++) { tag->data[2 + i] = (uint8_t)(words >> (8 * i)); }

    tag->elem_count = 1;
    tag->elem_size = tag->size;
    tag->udt_get_fields = 0;
    tag->offset = 0;
    tag->next_id = 0;

    return PLCTAG_STATUS_OK;
}


/*
 * A Variable Type Object instance, with d dimensions: size (4), pad, type (1), array type (1),
 * d (1), the count of each dimension (4 each), member count (2), 4 reserved, CRC (2), the name
 * as a short string padded to even, the next instance (4) and the nested instance (4).
 */
static int parse_vto(uint8_t *data, int32_t data_size, omron_vto_t *vto) {
    int32_t dims = 0;
    int32_t base = 0;
    int32_t name_end = 0;

    mem_set(vto, 0, (int)sizeof(*vto));

    if(data_size < 8) { return PLCTAG_ERR_TOO_SMALL; }

    dims = data[7];
    base = 4 * dims;
    if(dims > 3 || data_size < 17 + base) { return PLCTAG_ERR_TOO_SMALL; }

    vto->size = get_u32(data);
    vto->type = data[5];
    vto->array_type = data[6];
    vto->elem_count = 1;
    for(int32_t i = 0; i < dims; i++) { vto->elem_count *= get_u32(data + 8 + (4 * i)); }
    vto->num_members = (uint16_t)(data[8 + base] | (data[9 + base] << 8));
    vto->crc = (uint16_t)(data[14 + base] | (data[15 + base] << 8));
    vto->name_len = data[16 + base];
    vto->name = data + 17 + base;

    name_end = 17 + base + vto->name_len + ((vto->name_len & 1) ? 0 : 1);
    if(data_size < name_end + 8) { return PLCTAG_ERR_TOO_SMALL; }

    vto->next_id = get_u32(data + name_end);
    vto->nesting_id = get_u32(data + name_end + 4);

    return PLCTAG_STATUS_OK;
}


/* A NULL src appends len zeros.  tag->size is the bytes in use. */
static int append_bytes(ab_tag_p tag, uint8_t *src, int32_t len) {
    int32_t new_size = tag->size + len;
    uint8_t *new_data = NULL;

    if(new_size > AB_MAX_TAG_DATA_SIZE) { return PLCTAG_ERR_TOO_LARGE; }

    new_data = (uint8_t *)mem_realloc(tag->data, new_size);
    if(!new_data) { return PLCTAG_ERR_NO_MEM; }

    if(src) {
        mem_copy(new_data + tag->size, src, len);
    } else {
        mem_set(new_data + tag->size, 0, len);
    }

    tag->data = new_data;
    tag->size = new_size;

    return PLCTAG_STATUS_OK;
}


/* forget a read's progress and its partial data. */
static void reset_read(ab_tag_p tag) {
    tag->udt_get_fields = 0;
    tag->next_id = 1;
    tag->offset = 0;
    tag->size = 0;
    tag->elem_count = 0;
}


static uint32_t get_u32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}
