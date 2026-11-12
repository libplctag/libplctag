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
 * Omron NJ/NX vendor services, as aphyt and libplctag use them.  The layouts follow what aphyt
 * parses (aphyt/omron/n_series.py); fields aphyt skips are zero here.
 */

#include "omron.h"
#include "cip.h"
#include "log.h"
#include "mutex.h"
#include "plc.h"
#include <libplctag/api/libplctag.h>
#include <utils/byte_slice.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define OMRON_SIMPLE_DATA_SEGMENT_WORDS ((uint8_t)0x03)

/* kind of variable in an instance list request. */
#define OMRON_KIND_SYSTEM ((uint16_t)1)

static bool is_scalar(tag_def_s *tag);
static bool is_fixed_string(tag_def_s *tag);
static uint8_t element_type_code(tag_def_s *tag);
static bool reply_data(byte_slice_t out, uint8_t service, int32_t data_size, byte_slice_t *data, byte_slice_t *rest);
static byte_slice_t write_variable_type(byte_slice_t out, uint8_t service, uint32_t size, uint8_t type_code,
                                        uint16_t num_members, uint16_t crc, const char *name, uint32_t next_id,
                                        uint32_t nesting_id);
static byte_slice_t segment_read(uint8_t service, tag_def_s *tag, uint32_t offset, uint16_t size, byte_slice_t out);
static byte_slice_t segment_write(uint8_t service, tag_def_s *tag, uint32_t offset, uint16_t size, byte_slice_t payload,
                                  byte_slice_t out);


/* ponytail: the tag syntax always has a dimension, so NAME:TYPE[1] stands for a scalar. */
static bool is_scalar(tag_def_s *tag) { return tag->num_dimensions == 1 && tag->dimensions[0] == 1; }


/* a fixed-capacity CIP STRING: the count word, then the characters. */
static bool is_fixed_string(tag_def_s *tag) { return tag->tag_type == TAG_CIP_TYPE_STRING && !tag->variable_elements; }


static uint8_t element_type_code(tag_def_s *tag) { return (uint8_t)(tag->tag_type & 0xFF); }


/*
 * An OK reply header and data_size zeroed bytes after it, which the caller fills in place.
 * False if out cannot hold them.
 */
static bool reply_data(byte_slice_t out, uint8_t service, int32_t data_size, byte_slice_t *data, byte_slice_t *rest) {
    *rest = byte_slice_take(cip_write_reply_header(out, service, CIP_OK, false, 0), data_size, data);

    if(byte_slice_has_error(*rest)) { return false; }

    memset(byte_slice_get_data(*data), 0, (size_t)data_size);

    return true;
}


/*
 * Get Instance List (0x5F) on the Tag Name Server.  Request: start index (4), count (4), kind
 * (2).  Variables whose names start with '_' are system variables, the rest are user ones, and
 * each kind is numbered from 1 in tag order.  Reply: count (2), more-follow flag (1), pad (1),
 * then per variable: 4 bytes aphyt skips, the length of the rest (2), class 0x6B (2), the
 * variable's instance (4), and its name as a short string.
 */
byte_slice_t omron_get_instance_list(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    uint32_t start = 0;
    uint32_t max_count = 0;
    uint16_t kind = 0;
    uint32_t index = 0;
    uint32_t instance = 0;
    uint16_t count = 0;
    bool more = false;
    byte_slice_t in = req->payload;
    byte_slice_t header = {0};
    byte_slice_t entries = {0};

    in = byte_slice_read_u32_le(in, &start);
    in = byte_slice_read_u32_le(in, &max_count);
    in = byte_slice_read_u16_le(in, &kind);

    in = byte_slice_end(in);


    if(byte_slice_has_error(in)) {
        log_info("Omron instance list request must be 10 bytes!");
        return cip_write_reply_header(out, req->service, cip_input_status(in), false, 0);
    }

    entries = byte_slice_take(out, (int32_t)CIP_RESPONSE_HEADER_SIZE + 4, &header);
    if(byte_slice_has_error(entries)) { return cip_write_reply_header(out, req->service, CIP_ERR_REPLY_TOO_LARGE, false, 0); }

    for(tag_def_s *tag = plc->tags; tag; tag = tag->next_tag) {
        bool is_system = (tag->name[0] == '_');
        uint8_t name_len = (uint8_t)strlen(tag->name);
        byte_slice_t trial = entries;

        instance++;

        if(is_system != (kind == OMRON_KIND_SYSTEM)) { continue; }

        index++;

        if(index < start) { continue; }

        if(count >= max_count) {
            more = true;
            break;
        }

        trial = byte_slice_write_u32_le(trial, instance);
        trial = byte_slice_write_u16_le(trial, (uint16_t)(2 + 4 + 1 + name_len));
        trial = byte_slice_write_u16_le(trial, CIP_CLASS_SYMBOL);
        trial = byte_slice_write_u32_le(trial, instance);
        trial = byte_slice_write_u8(trial, name_len);
        trial = byte_slice_write_bytes(trial, (const uint8_t *)tag->name, name_len);

        if(byte_slice_has_error(trial)) {
            more = true;
            break;
        }

        entries = trial;
        count++;
    }

    log_info("Omron instance list: kind %u from %u, returning %u, more %d.", kind, start, count, (int)more);

    header = cip_write_reply_header(header, req->service, CIP_OK, false, 0);
    header = byte_slice_write_u16_le(header, count);
    header = byte_slice_write_u8(header, more ? 1 : 0);
    byte_slice_write_u8(header, 0);

    return entries;
}


/*
 * Get Attributes All on a variable.  Layout, with d dimensions (0 for a scalar):
 *   0 size (4): the scalar's size, an array element's size, or a string's capacity.
 *   4 type (1), 5 array element type (1), 6 d (1), 7 pad.
 *   8 element count per dimension (4 each).
 *   then 8 reserved, the bit number (1), 3 pad, the Variable Type Object instance (4), and the
 *   start index per dimension (4 each).
 * aphyt reads a scalar structure's or string's type instance from bytes 8-11 instead, which
 * are reserved for a scalar, so it goes in both places.
 */
byte_slice_t omron_variable_get_attributes_all(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    tag_def_s *tag = NULL;
    byte_slice_t data = {0};
    byte_slice_t rest = cip_find_symbol_tag(req->path, plc, &tag);

    if(!tag || byte_slice_has_error(byte_slice_end(rest))) {
        log_info("Omron Get Attributes All: no such variable.");
        return cip_write_reply_header(out, req->service, CIP_ERR_PATH_DEST_UNKNOWN, false, 0);
    }

    int32_t dims = is_scalar(tag) ? 0 : (int32_t)tag->num_dimensions;
    uint32_t vto_id = tag->udt ? tag->udt->vto_id : 0;
    uint32_t size = (uint32_t)tag->elem_size; /* zero for packed strings, which have no stride. */

    if(is_fixed_string(tag)) { size = (uint32_t)(tag->elem_size - TAG_CIP_STRING_COUNT_WORD_BYTES); }

    if(!reply_data(out, req->service, 24 + (8 * dims), &data, &rest)) {
        return cip_write_reply_header(out, req->service, CIP_ERR_REPLY_TOO_LARGE, false, 0);
    }

    byte_slice_write_u32_le(data, size);

    if(dims == 0) {
        byte_slice_write_u8(byte_slice_skip(data, 4), element_type_code(tag));
        byte_slice_write_u32_le(byte_slice_skip(data, 8), vto_id);
    } else {
        byte_slice_t d = byte_slice_skip(data, 4);

        d = byte_slice_write_u8(d, TAG_CIP_TYPE_ARRAY);
        d = byte_slice_write_u8(d, element_type_code(tag));
        d = byte_slice_write_u8(d, (uint8_t)dims);
        d = byte_slice_skip(d, 1);
        for(int32_t i = 0; i < dims; i++) { d = byte_slice_write_u32_le(d, (uint32_t)tag->dimensions[i]); }
    }

    byte_slice_write_u32_le(byte_slice_skip(data, 20 + (4 * dims)), vto_id);

    return rest;
}


/*
 * Get Attributes All on the Variable Type Object.  Instance 0 is the class: revision (2) and
 * the highest instance (2).  A UDT's own instance names it and points at its first member; each
 * member's instance names the member and points at the next.
 */
byte_slice_t omron_variable_type_get_attributes_all(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    uint32_t instance = req->instance;

    if(instance == 0) {
        out = cip_write_reply_header(out, req->service, CIP_OK, false, 0);
        out = byte_slice_write_u16_le(out, 1);
        return byte_slice_write_u16_le(out, (uint16_t)plc->max_vto_id);
    }

    for(udt_def_s *udt = plc->udts; udt; udt = udt->next_udt) {
        if(instance == udt->vto_id) {
            return write_variable_type(out, req->service, (uint32_t)udt->size, (uint8_t)(TAG_CIP_TYPE_STRUCT & 0xFF),
                                       (uint16_t)udt->num_members, udt->crc, udt->name, 0, udt->vto_id + 1);
        }

        if(instance > udt->vto_id && instance <= udt->vto_id + udt->num_members) {
            size_t i = instance - udt->vto_id - 1;
            uint32_t next_id = (i + 1 < udt->num_members) ? instance + 1 : 0;

            return write_variable_type(out, req->service, (uint32_t)udt->members[i].size,
                                       (uint8_t)(udt->members[i].type & 0xFF), 0, 0, udt->members[i].name, next_id, 0);
        }
    }

    log_info("No Variable Type Object instance %u!", instance);
    return cip_write_reply_header(out, req->service, CIP_ERR_OBJECT_DOES_NOT_EXIST, false, 0);
}


/*
 * One Variable Type Object instance, for a type with no array dimensions:
 *   0 size (4), 4 pad, 5 type (1), 6 array type (1), 7 dimensions (1), 8 member count (2),
 *   10 reserved (4), 14 CRC (2), 16 name as a short string padded to even, then the next
 *   instance (4) and the nested (first member) instance (4).
 */
static byte_slice_t write_variable_type(byte_slice_t out, uint8_t service, uint32_t size, uint8_t type_code,
                                        uint16_t num_members, uint16_t crc, const char *name, uint32_t next_id,
                                        uint32_t nesting_id) {
    uint8_t name_len = (uint8_t)strlen(name);
    int32_t name_end = 17 + name_len + ((name_len & 1) ? 0 : 1);
    byte_slice_t data = {0};
    byte_slice_t rest = {0};

    if(!reply_data(out, service, name_end + 8, &data, &rest)) {
        return cip_write_reply_header(out, service, CIP_ERR_REPLY_TOO_LARGE, false, 0);
    }

    byte_slice_write_u32_le(data, size);
    byte_slice_write_u8(byte_slice_skip(data, 5), type_code);
    byte_slice_write_u16_le(byte_slice_skip(data, 8), num_members);
    byte_slice_write_u16_le(byte_slice_skip(data, 14), crc);
    byte_slice_write_bytes(byte_slice_write_u8(byte_slice_skip(data, 16), name_len), (const uint8_t *)name, name_len);
    byte_slice_write_u32_le(byte_slice_write_u32_le(byte_slice_skip(data, name_end), next_id), nesting_id);

    return rest;
}


/* the segment is 0x80, its size in words (3), the byte offset (4) and the byte count (2). */
byte_slice_t omron_segment_access(cip_request_t *req, tag_def_s *tag, byte_slice_t segment, byte_slice_t out) {
    uint8_t marker = 0;
    uint8_t words = 0;
    uint32_t offset = 0;
    uint16_t size = 0;

    segment = byte_slice_read_u8(segment, &marker);
    segment = byte_slice_read_u8(segment, &words);
    segment = byte_slice_read_u32_le(segment, &offset);
    segment = byte_slice_read_u16_le(segment, &size);

    if(byte_slice_has_error(byte_slice_end(segment)) || words != OMRON_SIMPLE_DATA_SEGMENT_WORDS) {
        log_info("Malformed Omron simple data segment!");
        return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    if(tag->variable_elements) {
        log_info("Segmented access to packed string arrays is not supported!");
        return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    if(req->service == CIP_SRV_READ_NAMED_TAG) { return segment_read(req->service, tag, offset, size, out); }

    return segment_write(req->service, tag, offset, size, req->payload, out);
}


/*
 * A read with a Simple Data Segment: <size> bytes at byte <offset> of the variable.  The reply
 * is the type, any additional info (a structure's CRC), then the bytes.  For a string, offset
 * and size count characters, and the bytes are led by a count word holding <size>.
 */
static byte_slice_t segment_read(uint8_t service, tag_def_s *tag, uint32_t offset, uint16_t size, byte_slice_t out) {
    bool is_string = is_fixed_string(tag);
    size_t base = is_string ? TAG_CIP_STRING_COUNT_WORD_BYTES : 0;
    size_t extent = tag->data_size - base;
    byte_slice_t rest = {0};
    byte_slice_t dest = {0};

    if((size_t)offset + size > extent) {
        log_info("Segment read of %u bytes at %u runs past the %zu bytes of %s!", size, offset, extent, tag->name);
        return cip_write_reply_header(out, service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    rest = cip_write_reply_header(out, service, CIP_OK, false, 0);

    if(tag->udt) {
        rest = byte_slice_write_bytes(rest, tag->type_info, (int32_t)tag->type_info_size);
    } else {
        rest = byte_slice_write_u8(rest, element_type_code(tag));
        rest = byte_slice_write_u8(rest, 0);
    }

    if(is_string) { rest = byte_slice_write_u16_le(rest, size); }

    rest = byte_slice_take(rest, size, &dest);

    if(byte_slice_has_error(rest)) {
        log_info("Segment read of %u bytes does not fit the response!", size);
        return cip_write_reply_header(out, service, CIP_ERR_REPLY_TOO_LARGE, false, 0);
    }

    critical_block(tag->data_mutex) { byte_slice_write_bytes(dest, tag->data + base + offset, size); }

    return rest;
}


/*
 * A write with a Simple Data Segment.  Payload: type (1), additional info length (1) and info,
 * element count (2), then <size> bytes, led by a count word for a string.  A structure must be
 * written as an abbreviated structure carrying the UDT's CRC, an array as its element type.
 */
static byte_slice_t segment_write(uint8_t service, tag_def_s *tag, uint32_t offset, uint16_t size, byte_slice_t payload,
                                  byte_slice_t out) {
    bool is_string = is_fixed_string(tag);
    size_t base = is_string ? TAG_CIP_STRING_COUNT_WORD_BYTES : 0;
    size_t extent = tag->data_size - base;
    uint8_t want_type = tag->udt ? TAG_CIP_TYPE_ABBREV_STRUCT : element_type_code(tag);
    uint8_t want_info_len = tag->udt ? 2 : 0;
    uint8_t type = 0;
    uint8_t info_len = 0;
    uint16_t crc = 0;
    byte_slice_t data = {0};
    byte_slice_t in = payload;

    in = byte_slice_read_u8(in, &type);
    in = byte_slice_read_u8(in, &info_len);

    if(byte_slice_has_error(in) || type != want_type || info_len != want_info_len) {
        log_info("Segment write type does not match %s!", tag->name);
        return cip_write_reply_header(out, service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    if(tag->udt) { in = byte_slice_read_u16_le(in, &crc); }
    in = byte_slice_skip(in, 2 + (int32_t)base); /* element count, and a string's count word */
    in = byte_slice_take(in, size, &data);

    in = byte_slice_end(in);


    if(byte_slice_has_error(in)) {
        log_info("Segment write does not carry the %u bytes its segment says!", size);
        return cip_write_reply_header(out, service, cip_input_status(in), false, 0);
    }

    if(tag->udt && crc != tag->udt->crc) {
        log_info("Segment write structure CRC does not match %s!", tag->name);
        return cip_write_reply_header(out, service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    if((size_t)offset + size > extent) {
        log_info("Segment write of %u bytes at %u runs past the %zu bytes of %s!", size, offset, extent, tag->name);
        return cip_write_reply_header(out, service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    critical_block(tag->data_mutex) {
        memcpy(tag->data + base + offset, byte_slice_get_data(data), size);

        if(is_string) {
            size_t len = strnlen((const char *)(tag->data + base), extent);

            tag->data[0] = (uint8_t)(len & 0xFF);
            tag->data[1] = (uint8_t)(len >> 8);
        }
    }

    return cip_write_reply_header(out, service, CIP_OK, false, 0);
}
