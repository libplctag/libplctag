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
 * Shared CIP Operations Implementation
 *
 * STATUS: KEEP AS-IS through Phase 4; minor addition in Phase 4.
 *
 * This file is correct and complete for Phases 1-3.  The only change needed
 * is in Phase 4 (Fragmentation): add a new builder for the OMRON trailing data
 * segment — service 0x4C/0x4D with the 8-byte 0x80 data segment appended
 * (plan §3 I).  That builder lives here because it is a CIP-layer concern.
 *
 * Phase 2: enip_cip_read_tag_request, enip_cip_write_tag_request,
 *          enip_cip_parse_response — used immediately.
 * Phase 4: enip_cip_read_tag_fragmented_request,
 *          enip_cip_write_tag_fragmented_request — used by AB encode_chunk.
 *          Add enip_cip_read_tag_omron_request / _write_tag_omron_request
 *          (service 0x4C/0x4D + 0x80 segment) for the OMRON encode_chunk.
 *
 * No debug-module change needed: this file already uses DEBUG_MODULE_ENIP.
 */

#include <libplctag/protocols/enip/enip.h>
#include <libplctag/protocols/enip/enip_cip.h>
#include <utils/arena.h>
#include <utils/bytes.h>
#include <utils/debug.h>
#include <string.h>

/* ============================================================================
 * Tag Path Encoding (ANSI Extended Symbol Format)
 * ============================================================================
 *
 * Symbol segment:  0x91 [length_byte] [name_bytes] [pad_if_odd]
 * Array index:     0x28 [1-byte] | 0x29 [2-byte LE] | 0x2A [4-byte LE]
 * Member nav:      same as symbol segment (0x91) after a dot in the name
 */

static size_t enip_cip_parse_array_index(const char *str, size_t pos, uint32_t *index_out);
static size_t enip_cip_encode_array_index(uint32_t index, uint8_t *buf, size_t buf_size);

/* Phase 2: internal helper — no changes needed. */
static size_t enip_cip_parse_array_index(const char *str, size_t pos, uint32_t *index_out) {
    if(!str || str[pos] != '[') { return pos; }

    pos++;
    uint32_t index = 0;
    int32_t digit_count = 0;

    while(str[pos] >= '0' && str[pos] <= '9') {
        index = (index * 10u) + (uint32_t)(str[pos] - '0');
        digit_count++;
        pos++;
        if(digit_count > 10) { return pos - (size_t)digit_count - 1; }
    }

    if(digit_count == 0 || str[pos] != ']') { return pos - (size_t)digit_count - 1; }

    *index_out = index;
    return pos + 1;
}

/* Phase 2: internal helper — no changes needed. */
static size_t enip_cip_encode_array_index(uint32_t index, uint8_t *buf, size_t buf_size) {
    if(!buf || buf_size == 0) { return 0; }

    if(index <= 0xFFu) {
        if(buf_size < 2) { return 0; }
        buf[0] = 0x28;
        buf[1] = (uint8_t)index;
        return 2;
    } else if(index <= 0xFFFFu) {
        if(buf_size < 4) { return 0; }
        buf[0] = 0x29;
        buf[1] = 0x00; /* reserved */
        buf[2] = (uint8_t)(index & 0xFF);
        buf[3] = (uint8_t)((index >> 8) & 0xFF);
        return 4;
    } else {
        if(buf_size < 6) { return 0; }
        buf[0] = 0x2A;
        buf[1] = 0x00; /* reserved */
        buf[2] = (uint8_t)(index & 0xFF);
        buf[3] = (uint8_t)((index >> 8) & 0xFF);
        buf[4] = (uint8_t)((index >> 16) & 0xFF);
        buf[5] = (uint8_t)((index >> 24) & 0xFF);
        return 6;
    }
}

/* Phase 2: correct as-is — no changes needed. */
size_t enip_cip_encode_tag_path(const char *tag_name, uint8_t *buf, size_t buf_size) {
    if(!tag_name || !buf) { return 0; }

    size_t total = 0;
    size_t pos = 0;

    while(tag_name[pos] != '\0') {
        if(pos > 0 && tag_name[pos] == '.') { pos++; }

        const char *bracket = strchr(&tag_name[pos], '[');
        const char *dot = strchr(&tag_name[pos], '.');

        size_t name_len;
        if(bracket && dot) {
            name_len = (bracket < dot) ? (size_t)(bracket - &tag_name[pos])
                                       : (size_t)(dot - &tag_name[pos]);
        } else if(bracket) {
            name_len = (size_t)(bracket - &tag_name[pos]);
        } else if(dot) {
            name_len = (size_t)(dot - &tag_name[pos]);
        } else {
            name_len = strlen(&tag_name[pos]);
        }

        if(name_len == 0 || name_len > 255) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                   "CIP: invalid symbol length %zu at position %zu", name_len, pos);
            return 0;
        }

        uint8_t pad = (uint8_t)(name_len & 1u);
        size_t symbol_size = 2 + name_len + pad;

        if(total + symbol_size > buf_size) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: buffer too small for symbol");
            return 0;
        }

        buf[total]     = 0x91;
        buf[total + 1] = (uint8_t)name_len;
        memcpy(&buf[total + 2], &tag_name[pos], name_len);
        if(pad) { buf[total + 2 + name_len] = 0x00; }

        total += symbol_size;
        pos += name_len;

        while(tag_name[pos] == '[') {
            uint32_t index = 0;
            size_t new_pos = enip_cip_parse_array_index(tag_name, pos, &index);

            if(new_pos == pos) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                       "CIP: invalid array index at position %zu", pos);
                return 0;
            }

            size_t idx_size = enip_cip_encode_array_index(index, &buf[total], buf_size - total);
            if(idx_size == 0) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                       "CIP: failed to encode array index %u", index);
                return 0;
            }

            total += idx_size;
            pos = new_pos;
        }

        if(tag_name[pos] != '.' && tag_name[pos] != '\0') {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                   "CIP: unexpected character in tag path at position %zu", pos);
            return 0;
        }
    }

    return total;
}

/* ============================================================================
 * Internal: copy pre-encoded path into arena and compute path_size_words
 * ============================================================================ */

/* Phase 2: internal helper — no changes needed. */
static Bytes enip_cip_arena_copy_path(Arena *arena, const uint8_t *encoded_path,
                                      size_t encoded_path_len, uint8_t *path_size_words_out) {
    if(!arena || !encoded_path || encoded_path_len == 0 || !path_size_words_out) {
        return bytes_null();
    }

    /* path_size_words is the number of 16-bit words; round up */
    *path_size_words_out = (uint8_t)((encoded_path_len + 1) / 2);

    uint8_t *buf = arena_alloc(arena, encoded_path_len);
    if(!buf) { return bytes_null(); }

    memcpy(buf, encoded_path, encoded_path_len);
    return (Bytes){buf, encoded_path_len};
}

/* ============================================================================
 * CIP ReadTag (0x4C)
 *
 * Wire format:
 *   service(1)=0x4C + path_size_words(1) + path(N) + element_count(2)
 * ============================================================================ */

/* Phase 2: correct as-is — no changes needed. */
Bytes enip_cip_read_tag_request(Arena *arena, const uint8_t *encoded_path,
                                size_t encoded_path_len, uint16_t element_count) {
    if(!arena || !encoded_path || encoded_path_len == 0 || element_count == 0) {
        return bytes_null();
    }

    uint8_t path_size_words = 0;
    Bytes path = enip_cip_arena_copy_path(arena, encoded_path, encoded_path_len,
                                          &path_size_words);
    if(bytes_is_null(path)) { return bytes_null(); }

    Bytes header = bytes_pack(arena, BYTES_LE, (uint8_t)CIP_SVC_READ_TAG, path_size_words);
    if(bytes_is_null(header)) { return bytes_null(); }

    Bytes count = bytes_pack(arena, BYTES_LE, element_count);
    if(bytes_is_null(count)) { return bytes_null(); }

    return bytes_concat(arena, header, path, count);
}

/* ============================================================================
 * CIP WriteTag (0x4D)
 *
 * Wire format:
 *   service(1)=0x4D + path_size_words(1) + path(N) + data_type(2)
 *   + element_count(2) + data
 * ============================================================================ */

/* Phase 2: correct as-is — no changes needed. */
Bytes enip_cip_write_tag_request(Arena *arena, const uint8_t *encoded_path,
                                 size_t encoded_path_len, uint16_t data_type,
                                 uint16_t element_count, const uint8_t *write_data,
                                 size_t write_data_len) {
    if(!arena || !encoded_path || encoded_path_len == 0 || !write_data || write_data_len == 0) {
        return bytes_null();
    }

    uint8_t path_size_words = 0;
    Bytes path = enip_cip_arena_copy_path(arena, encoded_path, encoded_path_len,
                                          &path_size_words);
    if(bytes_is_null(path)) { return bytes_null(); }

    Bytes header = bytes_pack(arena, BYTES_LE, (uint8_t)CIP_SVC_WRITE_TAG, path_size_words);
    if(bytes_is_null(header)) { return bytes_null(); }

    Bytes meta = bytes_pack(arena, BYTES_LE, data_type, element_count);
    if(bytes_is_null(meta)) { return bytes_null(); }

    uint8_t *data_buf = arena_alloc(arena, write_data_len);
    if(!data_buf) { return bytes_null(); }
    memcpy(data_buf, write_data, write_data_len);
    Bytes data = {data_buf, write_data_len};

    return bytes_concat(arena, header, path, meta, data);
}

/* ============================================================================
 * AB ReadTagFragmented (0x52)
 *
 * Wire format:
 *   service(1)=0x52 + path_size_words(1) + path(N) + element_count(2)
 *   + byte_offset(4)
 * ============================================================================ */

/* Phase 4 (AB encode_chunk): correct as-is — used by AB encode_chunk to start/continue
 * a fragmented read.  No changes needed. */
Bytes enip_cip_read_tag_fragmented_request(Arena *arena, const uint8_t *encoded_path,
                                           size_t encoded_path_len, uint16_t element_count,
                                           uint32_t byte_offset) {
    if(!arena || !encoded_path || encoded_path_len == 0 || element_count == 0) {
        return bytes_null();
    }

    uint8_t path_size_words = 0;
    Bytes path = enip_cip_arena_copy_path(arena, encoded_path, encoded_path_len,
                                          &path_size_words);
    if(bytes_is_null(path)) { return bytes_null(); }

    Bytes header = bytes_pack(arena, BYTES_LE, (uint8_t)CIP_SVC_READ_TAG_FRAGMENTED,
                              path_size_words);
    if(bytes_is_null(header)) { return bytes_null(); }

    Bytes tail = bytes_pack(arena, BYTES_LE, element_count, byte_offset);
    if(bytes_is_null(tail)) { return bytes_null(); }

    return bytes_concat(arena, header, path, tail);
}

/* ============================================================================
 * AB WriteTagFragmented (0x53)
 *
 * Wire format:
 *   service(1)=0x53 + path_size_words(1) + path(N) + data_type(2)
 *   + element_count(2) + byte_offset(4) + data
 * ============================================================================ */

/* Phase 4 (AB encode_chunk): correct as-is — used by AB encode_chunk for fragmented writes.
 * No changes needed. */
Bytes enip_cip_write_tag_fragmented_request(Arena *arena, const uint8_t *encoded_path,
                                            size_t encoded_path_len, uint16_t data_type,
                                            uint16_t element_count, uint32_t byte_offset,
                                            const uint8_t *write_data, size_t write_data_len) {
    if(!arena || !encoded_path || encoded_path_len == 0 || !write_data || write_data_len == 0) {
        return bytes_null();
    }

    uint8_t path_size_words = 0;
    Bytes path = enip_cip_arena_copy_path(arena, encoded_path, encoded_path_len,
                                          &path_size_words);
    if(bytes_is_null(path)) { return bytes_null(); }

    Bytes header = bytes_pack(arena, BYTES_LE, (uint8_t)CIP_SVC_WRITE_TAG_FRAGMENTED,
                              path_size_words);
    if(bytes_is_null(header)) { return bytes_null(); }

    Bytes meta = bytes_pack(arena, BYTES_LE, data_type, element_count, byte_offset);
    if(bytes_is_null(meta)) { return bytes_null(); }

    uint8_t *data_buf = arena_alloc(arena, write_data_len);
    if(!data_buf) { return bytes_null(); }
    memcpy(data_buf, write_data, write_data_len);
    Bytes data = {data_buf, write_data_len};

    return bytes_concat(arena, header, path, meta, data);
}

/* ============================================================================
 * CIP Response Parsing
 *
 * Wire format:
 *   reply_service(1) + reserved(1) + general_status(1) + ext_status_size(1)
 *   + ext_status_words(2 * ext_status_size) + data
 * ============================================================================ */

/* Phase 2: correct as-is.
 * NOTE (plan §3 G): the returned *data_out begins with a 2- or 4-byte CIP type code
 * for ReadTag replies.  Callers (mfg_ab accept_chunk, Phase 4) must strip that prefix
 * before copying into tag->data.  This function itself does NOT need to change. */
Bytes enip_cip_parse_response(Bytes response, uint8_t *general_status,
                              uint8_t *extended_status_size, Bytes *data_out) {
    if(data_out) { *data_out = bytes_null(); }
    if(general_status) { *general_status = 0xFF; }
    if(extended_status_size) { *extended_status_size = 0; }

    if(bytes_is_null(response) || !general_status || !extended_status_size || !data_out) {
        return bytes_null();
    }

    /* Minimum: reply_service(1) + reserved(1) + general_status(1) + ext_sz(1) = 4 bytes */
    if(response.len < 4) { return bytes_null(); }

    uint8_t reply_service = 0;
    uint8_t reserved = 0;
    uint8_t cip_status = 0;
    uint8_t ext_sz = 0;

    Bytes remaining = bytes_unpack(response, BYTES_LE, &reply_service, &reserved,
                                   &cip_status, &ext_sz);

    if(bytes_is_null(remaining)) { return bytes_null(); }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_SPEW, 0,
           "CIP response: service=0x%02x status=0x%02x ext_sz=%u",
           reply_service, cip_status, ext_sz);

    *general_status = cip_status;
    *extended_status_size = ext_sz;

    /* Skip extended status words */
    if(ext_sz > 0) {
        size_t ext_bytes = (size_t)ext_sz * 2u;
        if(remaining.len < ext_bytes) { return bytes_null(); }
        remaining = bytes_slice(remaining, ext_bytes, remaining.len - ext_bytes);
        if(bytes_is_null(remaining)) { return bytes_null(); }
    }

    *data_out = remaining;
    return remaining;
}
