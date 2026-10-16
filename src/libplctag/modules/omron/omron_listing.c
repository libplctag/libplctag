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
 * OMRON NJ/NX tag and UDT listing, @tags and @udt/N.  See omron_listing.h for the buffer
 * layouts.  The wire formats follow aphyt (aphyt/omron/n_series.py), which is known to work
 * against NJ/NX hardware.
 */

#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/error_codes.h>
#include <libplctag/modules/cip/services.h>
#include <libplctag/modules/cip/tag.h>
#include <libplctag/modules/cip/wire.h>
#include <libplctag/modules/omron/omron_common.h>
#include <libplctag/modules/omron/omron_listing.h>
#include <platform.h>
#include <stdbool.h>
#include <stdint.h>
#include <utils/debug.h>


#define OMRON_SVC_GET_INSTANCE_LIST ((uint8_t)0x5F)
#define OMRON_CLASS_TAG_NAME_SERVER ((uint8_t)0x6A)
#define OMRON_CLASS_VARIABLE_TYPE ((uint8_t)0x6C)

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

static int listing_tag_read_start(cip_tag_p tag);
static int listing_tag_encode_read(cip_tag_p tag, uint8_t *dest, int32_t capacity, tag_op_plan_t *plan);
static int listing_tag_decode_read(cip_tag_p tag, uint8_t *reply, int32_t reply_size);
static int decode_instance_list(cip_tag_p tag, uint8_t *data, int32_t data_size);
static int decode_variable_attributes(cip_tag_p tag, uint8_t *data, int32_t data_size);
static int udt_tag_read_start(cip_tag_p tag);
static int udt_tag_encode_read(cip_tag_p tag, uint8_t *dest, int32_t capacity, tag_op_plan_t *plan);
static int udt_tag_decode_read(cip_tag_p tag, uint8_t *reply, int32_t reply_size);
static int decode_udt_header(cip_tag_p tag, omron_vto_t *vto);
static int decode_udt_member(cip_tag_p tag, omron_vto_t *vto);
static int check_reply(cip_tag_p tag, uint8_t service, uint8_t *reply, int32_t reply_size);
static int parse_vto(uint8_t *data, int32_t data_size, omron_vto_t *vto);
static int append_bytes(cip_tag_p tag, uint8_t *src, int32_t len);
static void reset_read(cip_tag_p tag);

static struct tag_vtable_t listing_tag_vtable = {
    .abort = (tag_vtable_func)cip_data_tag_abort,
    .read = (tag_vtable_func)listing_tag_read_start,
    .status = (tag_vtable_func)cip_tag_status,
    .tickler = NULL,
    .write = NULL,
    .wake_plc = cip_tag_wake_plc,
    .activate = cip_tag_activate,
    .tag_data_written = cip_tag_wake_plc,
    .encode_read = (tag_encode_func)listing_tag_encode_read,
    .decode_read = (tag_decode_func)listing_tag_decode_read,
    .encode_write = NULL,
    .decode_write = NULL,
    .attribs = omron_attribs,
};

static struct tag_vtable_t udt_tag_vtable = {
    .abort = (tag_vtable_func)cip_data_tag_abort,
    .read = (tag_vtable_func)udt_tag_read_start,
    .status = (tag_vtable_func)cip_tag_status,
    .tickler = NULL,
    .write = NULL,
    .wake_plc = cip_tag_wake_plc,
    .activate = cip_tag_activate,
    .tag_data_written = cip_tag_wake_plc,
    .encode_read = (tag_encode_func)udt_tag_encode_read,
    .decode_read = (tag_decode_func)udt_tag_decode_read,
    .encode_write = NULL,
    .decode_write = NULL,
    .attribs = omron_attribs,
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


int omron_setup_listing_tag(cip_tag_p tag) {
    tag->next_id = 1;

    return cip_setup_special_tag(tag, CIP_TYPE_TAG_ENTRY, &listing_byte_order, &listing_tag_vtable,
                                 DEBUG_MODULE_OMRON_COMMON);
}


int omron_setup_udt_tag(cip_tag_p tag, const char *name) {
    int udt_id = 0;

    if(str_to_int(name + str_length("@udt/"), &udt_id) != PLCTAG_STATUS_OK || udt_id < 1 || udt_id > UINT16_MAX) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "UDT id in %s must be a number from 1 to 65535!", name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    tag->udt_id = (uint16_t)udt_id;

    return cip_setup_special_tag(tag, CIP_TYPE_TAG_UDT, &udt_byte_order, &udt_tag_vtable,
                                 DEBUG_MODULE_OMRON_COMMON);
}


/*
 * @tags
 *
 * Get Instance List (0x5F) pages through the user variables and then the system ones, which
 * gives only names.  Each name then gets Get Attributes All for its type and dimensions.  The
 * entries are built in place: the name pages append them with the type fields zero, and the
 * attribute replies fill those in, tag->offset walking the buffer.
 *
 * The create read is a probe: one page shows the Tag Name Server answers.  The full walk is a
 * request per variable, so it waits for the first real read.
 */
static int listing_tag_read_start(cip_tag_p tag) {
    reset_read(tag);

    return cip_readonly_read_start(tag, DEBUG_MODULE_OMRON_COMMON);
}


static int listing_tag_encode_read(cip_tag_p tag, uint8_t *dest, int32_t capacity, tag_op_plan_t *plan) {
    uint8_t *data = dest;

    if(tag->udt_get_fields == OMRON_LIST_TYPES) {
        uint8_t *entry = tag->data + tag->offset;
        int32_t name_len = (int32_t)entry[LIST_ENTRY_NAME_LEN_OFFSET] | ((int32_t)entry[LIST_ENTRY_NAME_LEN_OFFSET + 1] << 8);
        int32_t padded_len = name_len + (name_len & 1);

        if(2 + 2 + padded_len > capacity) { return PLCTAG_ERR_TOO_SMALL; }

        /* Get Attributes All on the variable's symbolic segment. */
        *data++ = CIP_GET_ATTRIBUTES_ALL;
        *data++ = (uint8_t)((2 + padded_len) / 2);
        *data++ = CIP_SYMBOLIC_SEGMENT;
        *data++ = (uint8_t)name_len;
        mem_copy(data, entry + LIST_ENTRY_SIZE, name_len);
        data += name_len;
        if(name_len & 1) { *data++ = 0; }
    } else {
        uint32_t count = (tag->create_read_status == PLCTAG_STATUS_PENDING) ? 1 : OMRON_LIST_BATCH;
        uint16_t kind = (tag->udt_get_fields == OMRON_LIST_USER) ? OMRON_KIND_USER : OMRON_KIND_SYSTEM;

        if(2 + 6 + 10 > capacity) { return PLCTAG_ERR_TOO_SMALL; }

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
        for(int32_t i = 0; i < 4; i++) { *data++ = (uint8_t)(count >> (8 * i)); }
        *data++ = (uint8_t)(kind & 0xFF);
        *data++ = (uint8_t)(kind >> 8);
    }

    plan->req_size = (int32_t)(data - dest);
    plan->reply_size = 0;
    plan->packable = (tag->allow_packing != 0);

    return PLCTAG_STATUS_OK;
}


static int listing_tag_decode_read(cip_tag_p tag, uint8_t *reply, int32_t reply_size) {
    uint8_t service = (tag->udt_get_fields == OMRON_LIST_TYPES) ? CIP_GET_ATTRIBUTES_ALL : OMRON_SVC_GET_INSTANCE_LIST;
    int rc = check_reply(tag, service, reply, reply_size);

    if(rc == PLCTAG_STATUS_OK) {
        uint8_t *data = reply + sizeof(cip_header);
        int32_t data_size = reply_size - (int32_t)sizeof(cip_header);

        rc = (tag->udt_get_fields == OMRON_LIST_TYPES) ? decode_variable_attributes(tag, data, data_size)
                                                       : decode_instance_list(tag, data, data_size);
    }

    if(rc_is_error(rc)) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "Tag listing failed: %s!", plc_tag_decode_error(rc));
        reset_read(tag);
    }

    return rc;
}


/*
 * Instance list reply: count (2), more-follow flag (1), pad (1), then per variable 4 bytes of
 * unknown meaning, the length of the rest (2), class (2), instance (4) and the name as a short
 * string.
 */
static int decode_instance_list(cip_tag_p tag, uint8_t *data, int32_t data_size) {
    uint8_t *end = data + data_size;
    uint8_t *cur = data + 4;
    uint16_t count = 0;
    bool more = false;

    if(data_size < 4) { return PLCTAG_ERR_TOO_SMALL; }

    count = (uint16_t)(data[0] | (data[1] << 8));
    more = (data[2] != 0);

    if(tag->create_read_status == PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Create probe answered.");
        reset_read(tag);
        tag->elem_count = 1;
        return PLCTAG_STATUS_OK;
    }

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
static int decode_variable_attributes(cip_tag_p tag, uint8_t *data, int32_t data_size) {
    uint8_t *entry = tag->data + tag->offset;
    int32_t name_len = (int32_t)entry[LIST_ENTRY_NAME_LEN_OFFSET] | ((int32_t)entry[LIST_ENTRY_NAME_LEN_OFFSET + 1] << 8);
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

    vto_id = (uint32_t)data[vto_offset] | ((uint32_t)data[vto_offset + 1] << 8) | ((uint32_t)data[vto_offset + 2] << 16)
             | ((uint32_t)data[vto_offset + 3] << 24);

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

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Listed %d variables.", tag->elem_count);

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
static int udt_tag_read_start(cip_tag_p tag) {
    reset_read(tag);

    return cip_readonly_read_start(tag, DEBUG_MODULE_OMRON_COMMON);
}


static int udt_tag_encode_read(cip_tag_p tag, uint8_t *dest, int32_t capacity, tag_op_plan_t *plan) {
    uint16_t instance = (tag->udt_get_fields == 0) ? tag->udt_id : (uint16_t)tag->next_id;

    if(capacity < 8) { return PLCTAG_ERR_TOO_SMALL; }

    dest[0] = CIP_GET_ATTRIBUTES_ALL;
    dest[1] = 3;
    dest[2] = 0x20;
    dest[3] = OMRON_CLASS_VARIABLE_TYPE;
    dest[4] = 0x25;
    dest[5] = 0x00;
    dest[6] = (uint8_t)(instance & 0xFF);
    dest[7] = (uint8_t)(instance >> 8);

    plan->req_size = 8;
    plan->reply_size = 0;
    plan->packable = (tag->allow_packing != 0);

    return PLCTAG_STATUS_OK;
}


static int udt_tag_decode_read(cip_tag_p tag, uint8_t *reply, int32_t reply_size) {
    omron_vto_t vto;
    int rc = check_reply(tag, CIP_GET_ATTRIBUTES_ALL, reply, reply_size);

    if(rc == PLCTAG_STATUS_OK) {
        rc = parse_vto(reply + sizeof(cip_header), reply_size - (int32_t)sizeof(cip_header), &vto);
    }

    if(rc == PLCTAG_STATUS_OK) { rc = (tag->udt_get_fields == 0) ? decode_udt_header(tag, &vto) : decode_udt_member(tag, &vto); }

    if(rc_is_error(rc)) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "UDT read failed: %s!", plc_tag_decode_error(rc));
        reset_read(tag);
    }

    return rc;
}


static int decode_udt_header(cip_tag_p tag, omron_vto_t *vto) {
    uint8_t header[UDT_HEADER_SIZE] = {0};
    uint8_t zero = 0;
    int rc = PLCTAG_STATUS_OK;

    /* the create read starts on the one-byte buffer every special tag is given. */
    tag->size = 0;

    if(vto->type != OMRON_TYPE_STRUCT) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "Variable Type Object %u is type 0x%02x, not a structure!",
               tag->udt_id, vto->type);
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
    for(int32_t i = 0; rc == PLCTAG_STATUS_OK && i < vto->num_members * UDT_MEMBER_SIZE; i++) { rc = append_bytes(tag, &zero, 1); }
    if(rc == PLCTAG_STATUS_OK) { rc = append_bytes(tag, vto->name, vto->name_len); }
    if(rc == PLCTAG_STATUS_OK) { rc = append_bytes(tag, &zero, 1); }
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    tag->udt_get_fields = 1;
    tag->next_id = vto->nesting_id;
    tag->offset = 0;

    return (vto->num_members > 0 && vto->nesting_id != 0) ? PLCTAG_STATUS_PENDING : PLCTAG_STATUS_OK;
}


/*
 * One member.  Omron does not report member offsets, so they are laid out the way aphyt does:
 * an atomic member at a multiple of its own size, anything else at an even offset.
 */
static int decode_udt_member(cip_tag_p tag, omron_vto_t *vto) {
    uint16_t num_members = (uint16_t)(tag->data[10] | (tag->data[11] << 8));
    uint8_t *desc = tag->data + UDT_HEADER_SIZE + ((int32_t)(tag->udt_get_fields - 1) * UDT_MEMBER_SIZE);
    uint8_t elem_type = (vto->type == OMRON_TYPE_ARRAY) ? vto->array_type : vto->type;
    uint32_t count = (vto->type == OMRON_TYPE_ARRAY) ? vto->elem_count : 0;
    uint16_t logix_type = elem_type;
    uint32_t align = 2;
    uint32_t offset = (uint32_t)tag->offset;
    uint32_t total_size = 0;
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

    desc[0] = (uint8_t)(count & 0xFF);
    desc[1] = (uint8_t)((count >> 8) & 0xFF);
    desc[2] = (uint8_t)(logix_type & 0xFF);
    desc[3] = (uint8_t)(logix_type >> 8);
    for(int32_t i = 0; i < 4; i++) { desc[4 + i] = (uint8_t)(offset >> (8 * i)); }

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
    {
        uint32_t words = ((uint32_t)(tag->size - UDT_HEADER_SIZE) + 23 + 3) / 4;

        for(int32_t i = 0; i < 4; i++) { tag->data[2 + i] = (uint8_t)(words >> (8 * i)); }
    }

    tag->elem_count = 1;
    tag->elem_size = tag->size;
    tag->udt_get_fields = 0;
    tag->offset = 0;
    tag->next_id = 0;

    return PLCTAG_STATUS_OK;
}


static int check_reply(cip_tag_p tag, uint8_t service, uint8_t *reply, int32_t reply_size) {
    cip_header *resp = (cip_header *)reply;

    if(reply_size < (int32_t)sizeof(cip_header)) { return PLCTAG_ERR_TOO_SMALL; }

    if(resp->reply_service != (service | CIP_SVC_REPLY)) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "Unexpected reply service 0x%02x!", resp->reply_service);
        return PLCTAG_ERR_BAD_DATA;
    }

    if(resp->status != CIP_STATUS_OK) {
        size_t status_size = cip_error_data_size(&resp->status, reply + reply_size);

        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "Request failed with status 0x%x %s.", resp->status,
               decode_cip_error_short(&resp->status, status_size));
        return decode_cip_error_code(&resp->status, status_size);
    }

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

    vto->size = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    vto->type = data[5];
    vto->array_type = data[6];
    vto->elem_count = 1;
    for(int32_t i = 0; i < dims; i++) {
        vto->elem_count *= (uint32_t)data[8 + (4 * i)] | ((uint32_t)data[9 + (4 * i)] << 8) | ((uint32_t)data[10 + (4 * i)] << 16)
                           | ((uint32_t)data[11 + (4 * i)] << 24);
    }
    vto->num_members = (uint16_t)(data[8 + base] | (data[9 + base] << 8));
    vto->crc = (uint16_t)(data[14 + base] | (data[15 + base] << 8));
    vto->name_len = data[16 + base];
    vto->name = data + 17 + base;

    name_end = 17 + base + vto->name_len + ((vto->name_len & 1) ? 0 : 1);
    if(data_size < name_end + 8) { return PLCTAG_ERR_TOO_SMALL; }

    vto->next_id = (uint32_t)data[name_end] | ((uint32_t)data[name_end + 1] << 8) | ((uint32_t)data[name_end + 2] << 16)
                   | ((uint32_t)data[name_end + 3] << 24);
    vto->nesting_id = (uint32_t)data[name_end + 4] | ((uint32_t)data[name_end + 5] << 8) | ((uint32_t)data[name_end + 6] << 16)
                      | ((uint32_t)data[name_end + 7] << 24);

    return PLCTAG_STATUS_OK;
}


static int append_bytes(cip_tag_p tag, uint8_t *src, int32_t len) {
    uint8_t *new_data = NULL;

    if(tag->size + len > CIP_MAX_TAG_DATA_SIZE) { return PLCTAG_ERR_TOO_LARGE; }

    new_data = (uint8_t *)mem_realloc(tag->data, tag->size + len);
    if(!new_data) { return PLCTAG_ERR_NO_MEM; }

    mem_copy(new_data + tag->size, src, len);
    tag->data = new_data;
    tag->size += len;

    return PLCTAG_STATUS_OK;
}


/* forget a read's progress and its partial buffer. */
static void reset_read(cip_tag_p tag) {
    tag->udt_get_fields = 0;
    tag->next_id = 1;
    tag->offset = 0;
    tag->size = 0;
    tag->elem_count = 0;
}
