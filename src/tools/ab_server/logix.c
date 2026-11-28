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
 * Rockwell tag and UDT listing.  The layouts are what the library's @tags and @udt/N tags parse
 * (eip_cip_special.c) and what list_tags_logix decodes.
 */

#include "logix.h"
#include "cip.h"
#include "log.h"
#include "plc.h"
#include <string.h>
#include <utils/byte_slice.h>

/* symbol type bits: a structure, and the dimension count. */
#define SYMBOL_TYPE_STRUCT ((uint16_t)0x8000)
#define SYMBOL_TYPE_DIM_SHIFT (13)
#define SYMBOL_TYPE_TEMPLATE_MASK ((uint16_t)0x0FFF)

/* a template member that is an array. */
#define MEMBER_TYPE_ARRAY ((uint16_t)0x2000)

/* the template definition is attribute 4 32-bit words, less 23 bytes. */
#define TEMPLATE_SIZE_FUDGE (23)

/* ponytail: one buffer per call; UDT_MAX_MEMBERS members with long names still fit. */
#define TEMPLATE_DEF_MAX (4096)

#define CIP_ERR_ATTRIBUTE_NOT_SUPPORTED ((uint16_t)0x14)

static uint16_t symbol_type(tag_def_s *tag);
static uint16_t num_dims(tag_def_s *tag);
static udt_def_s *find_template(plc_s *plc, uint32_t template_id, uint16_t *handle);
static size_t atomic_size(tag_type_t type);
static size_t template_definition(udt_def_s *udt, uint8_t *buf, size_t capacity);

/* Logix STRING is a built-in UDT: DINT LEN, then SINT DATA[82]. */
static udt_def_s logix_string_udt = {
    .name = "STRING",
    .size = TAG_CIP_SIZE_LOGIX_STRING,
    .vto_id = TAG_CIP_STRUCT_HANDLE_STRING,
    .num_members = 2,
    .members = {{.name = "LEN", .type = TAG_CIP_TYPE_DINT, .size = 4, .offset = 0},
                {.name = "DATA", .type = TAG_CIP_TYPE_SINT, .size = 82, .offset = 4}},
};


/*
 * Get Instance Attribute List (0x55) on the Symbol object.  The path's instance is the first tag
 * to list; tags are numbered from 1 in the server's tag order.  Each entry is the instance (4),
 * then the requested attributes in the order asked: 1 name (2-byte count, characters), 2 symbol
 * type (2), 7 element size (2), 8 dimensions (3 x 4).  A reply that runs out of room ends with
 * status 0x06 and the client asks again from the instance after the last one it got.
 */
byte_slice_t logix_list_tags(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    uint16_t attr_count = 0;
    uint16_t attrs[8] = {0};
    uint32_t instance = 0;
    uint32_t listed = 0;
    bool more = false;
    byte_slice_t in = req->payload;
    byte_slice_t header = {0};
    byte_slice_t entries = {0};

    in = byte_slice_read_u16_le(in, &attr_count);
    if(attr_count == 0 || attr_count > (sizeof(attrs) / sizeof(attrs[0]))) {
        log_info("Tag listing asks for %u attributes!", attr_count);
        return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0);
    }

    for(uint16_t i = 0; i < attr_count; i++) { in = byte_slice_read_u16_le(in, &attrs[i]); }

    in = byte_slice_end(in);
    if(byte_slice_has_error(in)) {
        log_info("Tag listing request has the wrong length!");
        return cip_write_reply_header(out, req->service, cip_input_status(in), false, 0);
    }

    for(uint16_t i = 0; i < attr_count; i++) {
        if(attrs[i] != 1 && attrs[i] != 2 && attrs[i] != 7 && attrs[i] != 8) {
            log_info("Tag listing attribute %u is not supported!", attrs[i]);
            return cip_write_reply_header(out, req->service, CIP_ERR_ATTRIBUTE_NOT_SUPPORTED, false, 0);
        }
    }

    entries = byte_slice_take(out, (int32_t)CIP_RESPONSE_HEADER_SIZE, &header);
    if(byte_slice_has_error(entries)) { return cip_write_reply_header(out, req->service, CIP_ERR_REPLY_TOO_LARGE, false, 0); }

    for(tag_def_s *tag = plc->tags; tag; tag = tag->next_tag) {
        byte_slice_t trial = entries;
        uint16_t name_len = (uint16_t)strlen(tag->name);

        instance++;

        if(instance < req->instance) { continue; }

        /* a Micro800 leaves its BOOL arrays out of the listing. */
        if(plc->plc_type == PLC_MICRO800 && tag->tag_type == TAG_CIP_TYPE_BOOL && num_dims(tag) > 0) { continue; }

        trial = byte_slice_write_u32_le(trial, instance);

        for(uint16_t i = 0; i < attr_count; i++) {
            switch(attrs[i]) {
                case 1:
                    trial = byte_slice_write_u16_le(trial, name_len);
                    trial = byte_slice_write_bytes(trial, (const uint8_t *)tag->name, name_len);
                    break;

                case 2: trial = byte_slice_write_u16_le(trial, symbol_type(tag)); break;

                case 7: trial = byte_slice_write_u16_le(trial, (uint16_t)tag->elem_size); break;

                default: /* 8 */
                    for(size_t d = 0; d < 3; d++) {
                        uint32_t dim = (d < num_dims(tag)) ? (uint32_t)tag->dimensions[d] : 0;

                        trial = byte_slice_write_u32_le(trial, dim);
                    }
                    break;
            }
        }

        if(byte_slice_has_error(trial)) {
            more = true;
            break;
        }

        entries = trial;
        listed++;
    }

    /* not even one entry fits: the client cannot make progress. */
    if(more && listed == 0) { return cip_write_reply_header(out, req->service, CIP_ERR_REPLY_TOO_LARGE, false, 0); }

    log_info("Tag listing from instance %u: %u tags, more %d.", req->instance, listed, (int)more);

    cip_write_reply_header(header, req->service, more ? CIP_ERR_FRAG : CIP_OK, false, 0);

    return entries;
}


/*
 * Get Attribute List (0x03) on a template.  Request: count (2), attribute IDs (2 each).  Reply:
 * count (2), then per attribute its ID (2), status (2) and value: 1 structure handle (2),
 * 2 member count (2), 4 definition size in 32-bit words (4), 5 structure size in bytes (4).
 */
byte_slice_t logix_template_get_attribute_list(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    uint16_t attr_count = 0;
    uint16_t handle = 0;
    uint8_t def[TEMPLATE_DEF_MAX];
    byte_slice_t in = req->payload;
    byte_slice_t header = {0};
    byte_slice_t data = {0};
    udt_def_s *udt = find_template(plc, req->instance, &handle);

    if(!udt) {
        log_info("No template %u!", req->instance);
        return cip_write_reply_header(out, req->service, CIP_ERR_PATH_DEST_UNKNOWN, false, 0);
    }

    in = byte_slice_read_u16_le(in, &attr_count);
    if(byte_slice_has_error(in)) { return cip_write_reply_header(out, req->service, cip_input_status(in), false, 0); }

    data = byte_slice_take(out, (int32_t)CIP_RESPONSE_HEADER_SIZE, &header);
    data = byte_slice_write_u16_le(data, attr_count);

    for(uint16_t i = 0; i < attr_count; i++) {
        uint16_t attr = 0;

        in = byte_slice_read_u16_le(in, &attr);
        data = byte_slice_write_u16_le(data, attr);

        switch(attr) {
            case 1:
                data = byte_slice_write_u16_le(data, CIP_OK);
                data = byte_slice_write_u16_le(data, handle);
                break;

            case 2:
                data = byte_slice_write_u16_le(data, CIP_OK);
                data = byte_slice_write_u16_le(data, (uint16_t)udt->num_members);
                break;

            case 4:
                data = byte_slice_write_u16_le(data, CIP_OK);
                data = byte_slice_write_u32_le(
                    data, (uint32_t)((template_definition(udt, def, sizeof(def)) + TEMPLATE_SIZE_FUDGE + 3) / 4));
                break;

            case 5:
                data = byte_slice_write_u16_le(data, CIP_OK);
                data = byte_slice_write_u32_le(data, (uint32_t)udt->size);
                break;

            default: data = byte_slice_write_u16_le(data, CIP_ERR_ATTRIBUTE_NOT_SUPPORTED); break;
        }
    }

    in = byte_slice_end(in);
    if(byte_slice_has_error(in)) { return cip_write_reply_header(out, req->service, cip_input_status(in), false, 0); }
    if(byte_slice_has_error(data)) { return cip_write_reply_header(out, req->service, CIP_ERR_REPLY_TOO_LARGE, false, 0); }

    cip_write_reply_header(header, req->service, CIP_OK, false, 0);

    return data;
}


/*
 * Read Template (0x4C) on a template.  Request: byte offset (4), byte count (2).  Reply: as much
 * of the definition from the offset as fits, status 0x06 while more of it remains.
 */
byte_slice_t logix_template_read(cip_request_t *req, byte_slice_t out, plc_s *plc) {
    uint32_t offset = 0;
    uint16_t count = 0;
    uint16_t handle = 0;
    uint8_t def[TEMPLATE_DEF_MAX];
    size_t def_size = 0;
    size_t n = 0;
    byte_slice_t in = req->payload;
    byte_slice_t header = {0};
    byte_slice_t data = {0};
    udt_def_s *udt = find_template(plc, req->instance, &handle);

    if(!udt) {
        log_info("No template %u!", req->instance);
        return cip_write_reply_header(out, req->service, CIP_ERR_PATH_DEST_UNKNOWN, false, 0);
    }

    in = byte_slice_read_u32_le(in, &offset);
    in = byte_slice_read_u16_le(in, &count);
    in = byte_slice_end(in);
    if(byte_slice_has_error(in)) { return cip_write_reply_header(out, req->service, cip_input_status(in), false, 0); }

    def_size = template_definition(udt, def, sizeof(def));
    if(offset > def_size) { return cip_write_reply_header(out, req->service, CIP_ERR_INVALID_PARAM, false, 0); }

    data = byte_slice_take(out, (int32_t)CIP_RESPONSE_HEADER_SIZE, &header);
    if(byte_slice_has_error(data)) { return cip_write_reply_header(out, req->service, CIP_ERR_REPLY_TOO_LARGE, false, 0); }

    n = def_size - offset;
    if(n > count) { n = count; }
    if(n > (size_t)byte_slice_len(data)) { n = (size_t)byte_slice_len(data); }

    data = byte_slice_write_bytes(data, def + offset, (int32_t)n);

    log_info("Template %u read from %u: %zu of %zu bytes.", req->instance, offset, n, def_size);

    cip_write_reply_header(header, req->service, (offset + n < def_size) ? CIP_ERR_FRAG : CIP_OK, false, 0);

    return data;
}


/* the dimensions a listing reports; NAME:TYPE[1] stands for a scalar. */
static uint16_t num_dims(tag_def_s *tag) {
    if(tag->num_dimensions == 1 && tag->dimensions[0] == 1) { return 0; }

    return (uint16_t)tag->num_dimensions;
}


static uint16_t symbol_type(tag_def_s *tag) {
    uint16_t type = 0;

    if(tag->udt) {
        type = SYMBOL_TYPE_STRUCT | (uint16_t)(tag->udt->vto_id & SYMBOL_TYPE_TEMPLATE_MASK);
    } else if(tag->tag_type == TAG_CIP_STRUCT_HANDLE_STRING) {
        type = SYMBOL_TYPE_STRUCT | TAG_CIP_STRUCT_HANDLE_STRING;
    } else {
        type = (uint16_t)(tag->tag_type & 0xFF);
    }

    return (uint16_t)(type | (uint16_t)(num_dims(tag) << SYMBOL_TYPE_DIM_SHIFT));
}


/* a UDT's template instance is its vto_id; the handle is what reads of it carry as the type. */
static udt_def_s *find_template(plc_s *plc, uint32_t template_id, uint16_t *handle) {
    if(plc->plc_type != PLC_CONTROL_LOGIX && plc->plc_type != PLC_MICRO800) { return NULL; }

    if(template_id == TAG_CIP_STRUCT_HANDLE_STRING) {
        *handle = TAG_CIP_STRUCT_HANDLE_STRING;
        return &logix_string_udt;
    }

    for(udt_def_s *udt = plc->udts; udt; udt = udt->next_udt) {
        if(udt->vto_id == template_id) {
            *handle = udt->crc;
            return udt;
        }
    }

    return NULL;
}


static size_t atomic_size(tag_type_t type) {
    switch(type) {
        case TAG_CIP_TYPE_INT: return 2;
        case TAG_CIP_TYPE_DINT:
        case TAG_CIP_TYPE_REAL: return 4;
        case TAG_CIP_TYPE_LINT:
        case TAG_CIP_TYPE_LREAL: return 8;
        default: return 1;
    }
}


/*
 * The template definition: per member its info (2: the element count of an array, else 0), type
 * (2) and offset (4); then "<name>;n" and each member's name, every string zero terminated.
 */
static size_t template_definition(udt_def_s *udt, uint8_t *buf, size_t capacity) {
    byte_slice_t s = byte_slice_make(buf, (int32_t)capacity);
    byte_slice_t start = s;

    for(size_t i = 0; i < udt->num_members; i++) {
        udt_member_s *m = &udt->members[i];
        size_t count = m->size / atomic_size(m->type);
        uint16_t type = (uint16_t)(m->type | ((count > 1) ? MEMBER_TYPE_ARRAY : 0));

        s = byte_slice_write_u16_le(s, (uint16_t)((count > 1) ? count : 0));
        s = byte_slice_write_u16_le(s, type);
        s = byte_slice_write_u32_le(s, (uint32_t)m->offset);
    }

    s = byte_slice_write_bytes(s, (const uint8_t *)udt->name, (int32_t)strlen(udt->name));
    s = byte_slice_write_bytes(s, (const uint8_t *)";n", 3);

    for(size_t i = 0; i < udt->num_members; i++) {
        s = byte_slice_write_bytes(s, (const uint8_t *)udt->members[i].name, (int32_t)strlen(udt->members[i].name) + 1);
    }

    if(byte_slice_has_error(s)) {
        log_info("Template %s does not fit %zu bytes!", udt->name, capacity);
        return 0;
    }

    return (size_t)(byte_slice_len(start) - byte_slice_len(s));
}
