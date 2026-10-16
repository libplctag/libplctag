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
#include "slice.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define OMRON_SRV_GET_INSTANCE_LIST ((uint8_t)0x5f)

#define OMRON_CLASS_TAG_NAME_SERVER ((uint8_t)0x6a)
#define OMRON_CLASS_VARIABLE ((uint16_t)0x006b)
#define OMRON_CLASS_VARIABLE_TYPE ((uint8_t)0x6c)

#define OMRON_SIMPLE_DATA_SEGMENT ((uint8_t)0x80)
#define OMRON_SIMPLE_DATA_SEGMENT_WORDS ((uint8_t)0x03)
#define OMRON_SIMPLE_DATA_SEGMENT_SIZE ((size_t)8)

/* kind of variable in an instance list request. */
#define OMRON_KIND_SYSTEM ((uint16_t)1)
#define OMRON_KIND_USER ((uint16_t)2)

#define OMRON_INSTANCE_LIST_REQUEST_SIZE ((size_t)10)
#define OMRON_INSTANCE_LIST_HEADER_SIZE ((size_t)4)
#define OMRON_INSTANCE_ENTRY_FIXED_SIZE ((size_t)13) /* 4 unknown, 2 length, 2 class, 4 instance, 1 name length */

static bool is_scalar(tag_def_s *tag);
static bool is_fixed_string(tag_def_s *tag);
static uint8_t element_type_code(tag_def_s *tag);
static slice_s handle_instance_list(uint8_t cip_service, slice_s payload, slice_s output, plc_s *plc);
static slice_s handle_variable_attributes(uint8_t cip_service, tag_def_s *tag, slice_s output);
static slice_s handle_variable_type_attributes(uint8_t cip_service, uint32_t instance, slice_s output, plc_s *plc);
static slice_s write_variable_type(slice_s output, uint8_t cip_service, uint32_t size, uint8_t type_code,
                                   uint16_t num_members, uint16_t crc, const char *name, uint32_t next_id, uint32_t nesting_id);
static slice_s handle_segment_read(uint8_t cip_service, tag_def_s *tag, uint32_t offset, uint16_t size, slice_s output);
static slice_s handle_segment_write(uint8_t cip_service, tag_def_s *tag, uint32_t offset, uint16_t size, slice_s payload,
                                    slice_s output);


bool omron_dispatch_request(uint8_t cip_service, slice_s cip_service_path, slice_s cip_service_payload, slice_s output,
                            plc_s *plc, slice_s *response) {
    tag_def_s *tag = NULL;
    size_t end_offset = 0;
    uint32_t instance = 0;

    switch(cip_service) {
        case OMRON_SRV_GET_INSTANCE_LIST:
            if(!cip_path_is_class(cip_service_path, OMRON_CLASS_TAG_NAME_SERVER, &instance)) { return false; }
            *response = handle_instance_list(cip_service, cip_service_payload, output, plc);
            return true;

        case CIP_SRV_GET_ATTRIBUTES_ALL:
            if(cip_path_is_class(cip_service_path, OMRON_CLASS_VARIABLE_TYPE, &instance)) {
                *response = handle_variable_type_attributes(cip_service, instance, output, plc);
                return true;
            }

            if(slice_get_uint8(cip_service_path, 0) != CIP_SYMBOLIC_SEGMENT_MARKER) { return false; }

            tag = cip_find_symbol_tag(cip_service_path, plc, &end_offset);
            if(!tag || end_offset != slice_len(cip_service_path)) {
                log_info("Omron Get Attributes All: no such variable.");
                *response = make_cip_log_error(output, cip_service, CIP_ERR_PATH_DEST_UNKNOWN, false, 0);
                return true;
            }

            *response = handle_variable_attributes(cip_service, tag, output);
            return true;

        case CIP_SRV_READ_NAMED_TAG:
        case CIP_SRV_WRITE_NAMED_TAG:
            tag = cip_find_symbol_tag(cip_service_path, plc, &end_offset);
            if(!tag || end_offset >= slice_len(cip_service_path)
               || slice_get_uint8(cip_service_path, end_offset) != OMRON_SIMPLE_DATA_SEGMENT) {
                return false;
            }

            if(slice_len(cip_service_path) != end_offset + OMRON_SIMPLE_DATA_SEGMENT_SIZE
               || slice_get_uint8(cip_service_path, end_offset + 1) != OMRON_SIMPLE_DATA_SEGMENT_WORDS) {
                log_info("Malformed Omron simple data segment!");
                *response = make_cip_log_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
                return true;
            }

            if(cip_service == CIP_SRV_READ_NAMED_TAG) {
                *response = handle_segment_read(cip_service, tag, slice_get_uint32_le(cip_service_path, end_offset + 2),
                                                slice_get_uint16_le(cip_service_path, end_offset + 6), output);
            } else {
                *response = handle_segment_write(cip_service, tag, slice_get_uint32_le(cip_service_path, end_offset + 2),
                                                 slice_get_uint16_le(cip_service_path, end_offset + 6), cip_service_payload,
                                                 output);
            }
            return true;

        default: return false;
    }
}


/* ponytail: the tag syntax always has a dimension, so NAME:TYPE[1] stands for a scalar. */
static bool is_scalar(tag_def_s *tag) { return tag->num_dimensions == 1 && tag->dimensions[0] == 1; }


/* a fixed-capacity CIP STRING: the count word, then the characters. */
static bool is_fixed_string(tag_def_s *tag) { return tag->tag_type == TAG_CIP_TYPE_STRING && !tag->variable_elements; }


static uint8_t element_type_code(tag_def_s *tag) { return (uint8_t)(tag->tag_type & 0xFF); }


/*
 * Get Instance List (0x5F) on the Tag Name Server.  Request: start index (4), count (4), kind
 * (2).  Variables whose names start with '_' are system variables, the rest are user ones, and
 * each kind is numbered from 1 in tag order.  Reply: count (2), more-follow flag (1), pad (1),
 * then per variable: 4 bytes aphyt skips, the length of the rest (2), class 0x6B (2), the
 * variable's instance (4), and its name as a short string.
 */
static slice_s handle_instance_list(uint8_t cip_service, slice_s payload, slice_s output, plc_s *plc) {
    uint32_t start = 0;
    uint32_t max_count = 0;
    uint16_t kind = 0;
    uint32_t index = 0;
    uint32_t instance = 0;
    uint16_t count = 0;
    bool more = false;
    size_t offset = CIP_RESPONSE_HEADER_SIZE + OMRON_INSTANCE_LIST_HEADER_SIZE;

    if(slice_len(payload) != OMRON_INSTANCE_LIST_REQUEST_SIZE) {
        log_info("Omron instance list request must be %zu bytes!", OMRON_INSTANCE_LIST_REQUEST_SIZE);
        return make_cip_log_error(output, cip_service, CIP_ERR_INSUFFICIENT_DATA, false, 0);
    }

    start = slice_get_uint32_le(payload, 0);
    max_count = slice_get_uint32_le(payload, 4);
    kind = slice_get_uint16_le(payload, 8);

    if(slice_len(output) < offset) { return make_cip_log_error(output, cip_service, CIP_ERR_REPLY_TOO_LARGE, false, 0); }

    for(tag_def_s *tag = plc->tags; tag; tag = tag->next_tag) {
        bool is_system = (tag->name[0] == '_');
        size_t name_len = strlen(tag->name);

        instance++;

        if(is_system != (kind == OMRON_KIND_SYSTEM)) { continue; }

        index++;

        if(index < start) { continue; }

        if(count >= max_count || offset + OMRON_INSTANCE_ENTRY_FIXED_SIZE + name_len > slice_len(output)) {
            more = true;
            break;
        }

        slice_set_uint32_le(output, offset, instance);
        slice_set_uint16_le(output, offset + 4, (uint16_t)(2 + 4 + 1 + name_len));
        slice_set_uint16_le(output, offset + 6, OMRON_CLASS_VARIABLE);
        slice_set_uint32_le(output, offset + 8, instance);
        slice_set_uint8(output, offset + 12, (uint8_t)name_len);
        slice_copy_data_in(slice_from_slice(output, offset + 13, name_len), (uint8_t *)tag->name, name_len);

        offset += OMRON_INSTANCE_ENTRY_FIXED_SIZE + name_len;
        count++;
    }

    log_info("Omron instance list: kind %u from %u, returning %u, more %d.", kind, start, count, (int)more);

    make_cip_log_error(output, cip_service, CIP_OK, false, 0);
    slice_set_uint16_le(output, CIP_RESPONSE_HEADER_SIZE, count);
    slice_set_uint8(output, CIP_RESPONSE_HEADER_SIZE + 2, more ? 1 : 0);
    slice_set_uint8(output, CIP_RESPONSE_HEADER_SIZE + 3, 0);

    return slice_from_slice(output, 0, offset);
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
static slice_s handle_variable_attributes(uint8_t cip_service, tag_def_s *tag, slice_s output) {
    size_t dims = is_scalar(tag) ? 0 : tag->num_dimensions;
    size_t data_size = 24 + (8 * dims);
    slice_s data = slice_from_slice(output, CIP_RESPONSE_HEADER_SIZE, data_size);
    uint32_t vto_id = tag->udt ? tag->udt->vto_id : 0;
    uint32_t size = (uint32_t)tag->elem_size; /* zero for packed strings, which have no stride. */

    if(slice_len(data) != data_size) { return make_cip_log_error(output, cip_service, CIP_ERR_REPLY_TOO_LARGE, false, 0); }

    if(is_fixed_string(tag)) { size = (uint32_t)(tag->elem_size - TAG_CIP_STRING_COUNT_WORD_BYTES); }

    memset(data.data, 0, slice_len(data));
    make_cip_log_error(output, cip_service, CIP_OK, false, 0);

    slice_set_uint32_le(data, 0, size);

    if(dims == 0) {
        slice_set_uint8(data, 4, element_type_code(tag));
        slice_set_uint32_le(data, 8, vto_id);
    } else {
        slice_set_uint8(data, 4, TAG_CIP_TYPE_ARRAY);
        slice_set_uint8(data, 5, element_type_code(tag));
        slice_set_uint8(data, 6, (uint8_t)dims);
        for(size_t i = 0; i < dims; i++) { slice_set_uint32_le(data, 8 + (4 * i), (uint32_t)tag->dimensions[i]); }
    }

    slice_set_uint32_le(data, 20 + (4 * dims), vto_id);

    return slice_from_slice(output, 0, CIP_RESPONSE_HEADER_SIZE + data_size);
}


/*
 * Get Attributes All on the Variable Type Object.  Instance 0 is the class: revision (2) and
 * the highest instance (2).  A UDT's own instance names it and points at its first member; each
 * member's instance names the member and points at the next.
 */
static slice_s handle_variable_type_attributes(uint8_t cip_service, uint32_t instance, slice_s output, plc_s *plc) {
    if(instance == 0) {
        if(slice_len(output) < CIP_RESPONSE_HEADER_SIZE + 4) {
            return make_cip_log_error(output, cip_service, CIP_ERR_REPLY_TOO_LARGE, false, 0);
        }

        make_cip_log_error(output, cip_service, CIP_OK, false, 0);
        slice_set_uint16_le(output, CIP_RESPONSE_HEADER_SIZE, 1);
        slice_set_uint16_le(output, CIP_RESPONSE_HEADER_SIZE + 2, (uint16_t)plc->max_vto_id);

        return slice_from_slice(output, 0, CIP_RESPONSE_HEADER_SIZE + 4);
    }

    for(udt_def_s *udt = plc->udts; udt; udt = udt->next_udt) {
        if(instance == udt->vto_id) {
            return write_variable_type(output, cip_service, (uint32_t)udt->size, (uint8_t)(TAG_CIP_TYPE_STRUCT & 0xFF),
                                       (uint16_t)udt->num_members, udt->crc, udt->name, 0, udt->vto_id + 1);
        }

        if(instance > udt->vto_id && instance <= udt->vto_id + udt->num_members) {
            size_t i = instance - udt->vto_id - 1;
            uint32_t next_id = (i + 1 < udt->num_members) ? instance + 1 : 0;

            return write_variable_type(output, cip_service, (uint32_t)udt->members[i].size,
                                       (uint8_t)(udt->members[i].type & 0xFF), 0, 0, udt->members[i].name, next_id, 0);
        }
    }

    log_info("No Variable Type Object instance %u!", instance);
    return make_cip_log_error(output, cip_service, CIP_ERR_OBJECT_DOES_NOT_EXIST, false, 0);
}


/*
 * One Variable Type Object instance, for a type with no array dimensions:
 *   0 size (4), 4 pad, 5 type (1), 6 array type (1), 7 dimensions (1), 8 member count (2),
 *   10 reserved (4), 14 CRC (2), 16 name as a short string padded to even, then the next
 *   instance (4) and the nested (first member) instance (4).
 */
static slice_s write_variable_type(slice_s output, uint8_t cip_service, uint32_t size, uint8_t type_code,
                                   uint16_t num_members, uint16_t crc, const char *name, uint32_t next_id, uint32_t nesting_id) {
    size_t name_len = strlen(name);
    size_t name_end = 17 + name_len + ((name_len & 1) ? 0 : 1);
    size_t data_size = name_end + 8;
    slice_s data = slice_from_slice(output, CIP_RESPONSE_HEADER_SIZE, data_size);

    if(slice_len(data) != data_size) { return make_cip_log_error(output, cip_service, CIP_ERR_REPLY_TOO_LARGE, false, 0); }

    memset(data.data, 0, slice_len(data));
    make_cip_log_error(output, cip_service, CIP_OK, false, 0);

    slice_set_uint32_le(data, 0, size);
    slice_set_uint8(data, 5, type_code);
    slice_set_uint16_le(data, 8, num_members);
    slice_set_uint16_le(data, 14, crc);
    slice_set_uint8(data, 16, (uint8_t)name_len);
    slice_copy_data_in(slice_from_slice(data, 17, name_len), (uint8_t *)name, name_len);
    slice_set_uint32_le(data, name_end, next_id);
    slice_set_uint32_le(data, name_end + 4, nesting_id);

    return slice_from_slice(output, 0, CIP_RESPONSE_HEADER_SIZE + data_size);
}


/*
 * A read with a Simple Data Segment: <size> bytes at byte <offset> of the variable.  The reply
 * is the type, any additional info (a structure's CRC), then the bytes.  For a string, offset
 * and size count characters, and the bytes are led by a count word holding <size>.
 */
static slice_s handle_segment_read(uint8_t cip_service, tag_def_s *tag, uint32_t offset, uint16_t size, slice_s output) {
    bool is_string = is_fixed_string(tag);
    size_t base = is_string ? TAG_CIP_STRING_COUNT_WORD_BYTES : 0;
    size_t extent = tag->data_size - base;
    size_t header_size = 0;
    uint8_t header[TAG_TYPE_INFO_MAX + TAG_CIP_STRING_COUNT_WORD_BYTES] = {0};
    slice_s payload = {0};

    if(tag->variable_elements) {
        log_info("Segmented access to packed string arrays is not supported!");
        return make_cip_log_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    if((size_t)offset + size > extent) {
        log_info("Segment read of %u bytes at %u runs past the %zu bytes of %s!", size, offset, extent, tag->name);
        return make_cip_log_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    if(tag->udt) {
        memcpy(header, tag->type_info, tag->type_info_size);
        header_size = tag->type_info_size;
    } else {
        header[0] = element_type_code(tag);
        header_size = 2;
    }

    if(is_string) {
        header[2] = (uint8_t)(size & 0xFF);
        header[3] = (uint8_t)(size >> 8);
        header_size += TAG_CIP_STRING_COUNT_WORD_BYTES;
    }

    payload = slice_from_slice(output, CIP_RESPONSE_HEADER_SIZE + header_size, size);
    if(slice_len(payload) != size) {
        log_info("Segment read of %u bytes does not fit the response!", size);
        return make_cip_log_error(output, cip_service, CIP_ERR_REPLY_TOO_LARGE, false, 0);
    }

    make_cip_log_error(output, cip_service, CIP_OK, false, 0);
    slice_copy_data_in(slice_from_slice(output, CIP_RESPONSE_HEADER_SIZE, header_size), header, header_size);

    critical_block(tag->data_mutex) { slice_copy_data_in(payload, tag->data + base + offset, size); }

    return slice_from_slice(output, 0, CIP_RESPONSE_HEADER_SIZE + header_size + size);
}


/*
 * A write with a Simple Data Segment.  Payload: type (1), additional info length (1) and info,
 * element count (2), then <size> bytes, led by a count word for a string.  A structure must be
 * written as an abbreviated structure carrying the UDT's CRC, an array as its element type.
 */
static slice_s handle_segment_write(uint8_t cip_service, tag_def_s *tag, uint32_t offset, uint16_t size, slice_s payload,
                                    slice_s output) {
    bool is_string = is_fixed_string(tag);
    size_t base = is_string ? TAG_CIP_STRING_COUNT_WORD_BYTES : 0;
    size_t extent = tag->data_size - base;
    size_t info_len = slice_get_uint8(payload, 1);
    size_t data_start = 2 + info_len + 2 + base;
    uint8_t want_type = tag->udt ? TAG_CIP_TYPE_ABBREV_STRUCT : element_type_code(tag);
    size_t want_info_len = tag->udt ? 2 : 0;

    if(tag->variable_elements) {
        log_info("Segmented access to packed string arrays is not supported!");
        return make_cip_log_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    if(slice_len(payload) < 2 || slice_get_uint8(payload, 0) != want_type || info_len != want_info_len
       || (tag->udt && slice_get_uint16_le(payload, 2) != tag->udt->crc)) {
        log_info("Segment write type, or structure CRC, does not match %s!", tag->name);
        return make_cip_log_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    if(slice_len(payload) != data_start + size) {
        log_info("Segment write carries %zu bytes, not the %u its segment says!",
                 slice_len(payload) > data_start ? slice_len(payload) - data_start : 0, size);
        return make_cip_log_error(output, cip_service,
                                  slice_len(payload) < data_start + size ? CIP_ERR_INSUFFICIENT_DATA : CIP_ERR_TOO_MUCH_DATA,
                                  false, 0);
    }

    if((size_t)offset + size > extent) {
        log_info("Segment write of %u bytes at %u runs past the %zu bytes of %s!", size, offset, extent, tag->name);
        return make_cip_log_error(output, cip_service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    critical_block(tag->data_mutex) {
        slice_copy_data_out(tag->data + base + offset, size, slice_from_slice(payload, data_start, size));

        if(is_string) {
            size_t len = strnlen((const char *)(tag->data + base), extent);

            tag->data[0] = (uint8_t)(len & 0xFF);
            tag->data[1] = (uint8_t)(len >> 8);
        }
    }

    return make_cip_log_error(output, cip_service, CIP_OK, false, 0);
}
