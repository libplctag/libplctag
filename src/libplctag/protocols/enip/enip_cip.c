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

#include <libplctag/protocols/enip/enip_cip.h>
#include <inttypes.h>
#include <utils/debug.h>
#include <string.h>

/* Maximum encoded path length (bytes).  Generous for nested UDT member/array
 * paths; enip_cip_encode_path fails cleanly if a path would exceed this. */
#define ENIP_CIP_PATH_MAX_LEN ((size_t)512)

/* ============================================================================
 * Tag Path Encoding (ANSI Extended Symbol Format)
 *
 * Symbol segment:  0x91 [length_byte] [name_bytes] [pad_if_odd]
 * Array index:     0x28 [1-byte] | 0x29 [reserved][2-byte LE] | 0x2A [reserved][4-byte LE]
 * Member nav:      same as symbol segment (0x91) after a dot in the name
 * ============================================================================ */

/* Parse "[N]" starting at str[pos]; returns the position after ']', or pos
 * unchanged on a malformed index. */
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

/* Encode one array index segment (0x28/0x29/0x2A) into buf[buf_size].
 * Returns bytes written, or 0 if buf_size is too small. */
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
        buf[2] = (uint8_t)(index & 0xFFu);
        buf[3] = (uint8_t)((index >> 8) & 0xFFu);
        return 4;
    } else {
        if(buf_size < 6) { return 0; }
        buf[0] = 0x2A;
        buf[1] = 0x00; /* reserved */
        buf[2] = (uint8_t)(index & 0xFFu);
        buf[3] = (uint8_t)((index >> 8) & 0xFFu);
        buf[4] = (uint8_t)((index >> 16) & 0xFFu);
        buf[5] = (uint8_t)((index >> 24) & 0xFFu);
        return 6;
    }
}

/* Encode tag_name into buf[buf_size] in ANSI CIP Extended Symbol format.
 * Returns bytes written, or 0 on error (invalid syntax, segment too long, or
 * buffer too small). */
static size_t enip_cip_encode_tag_path(const char *tag_name, uint8_t *buf, size_t buf_size) {
    if(!tag_name || !buf) { return 0; }

    size_t total = 0;
    size_t pos = 0;

    while(tag_name[pos] != '\0') {
        if(pos > 0 && tag_name[pos] == '.') { pos++; }

        const char *bracket = strchr(&tag_name[pos], '[');
        const char *dot = strchr(&tag_name[pos], '.');

        size_t name_len;
        if(bracket && dot) {
            name_len = (bracket < dot) ? (size_t)(bracket - &tag_name[pos]) : (size_t)(dot - &tag_name[pos]);
        } else if(bracket) {
            name_len = (size_t)(bracket - &tag_name[pos]);
        } else if(dot) {
            name_len = (size_t)(dot - &tag_name[pos]);
        } else {
            name_len = strlen(&tag_name[pos]);
        }

        if(name_len == 0 || name_len > 255) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: invalid symbol length %zu at position %zu.", name_len, pos);
            return 0;
        }

        uint8_t pad = (uint8_t)(name_len & 1u);
        size_t symbol_size = 2 + name_len + pad;

        if(total + symbol_size > buf_size) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: buffer too small for symbol.");
            return 0;
        }

        buf[total] = 0x91;
        buf[total + 1] = (uint8_t)name_len;
        memcpy(&buf[total + 2], &tag_name[pos], name_len);
        if(pad) { buf[total + 2 + name_len] = 0x00; }

        total += symbol_size;
        pos += name_len;

        while(tag_name[pos] == '[') {
            uint32_t index = 0;
            size_t new_pos = enip_cip_parse_array_index(tag_name, pos, &index);

            if(new_pos == pos) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: invalid array index at position %zu.", pos);
                return 0;
            }

            size_t idx_size = enip_cip_encode_array_index(index, &buf[total], buf_size - total);
            if(idx_size == 0) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: failed to encode array index %" PRIu32 ".", index);
                return 0;
            }

            total += idx_size;
            pos = new_pos;
        }

        if(tag_name[pos] != '.' && tag_name[pos] != '\0') {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: unexpected character in tag path at position %zu.", pos);
            return 0;
        }
    }

    if(total == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: empty tag path.");
        return 0;
    }

    return total;
}

Bytes enip_cip_encode_path(Arena *a, const char *name) {
    if(!a || !name) { return bytes_null(); }

    uint8_t tmp[ENIP_CIP_PATH_MAX_LEN];

    size_t len = enip_cip_encode_tag_path(name, tmp, sizeof(tmp));
    if(len == 0) { return bytes_null(); }

    Bytes out = bytes_alloc(a, len);
    if(bytes_is_null(out)) { return bytes_null(); }

    memcpy(out.data, tmp, len);

    return out;
}

Bytes enip_cip_encode_path_at(Arena *a, Bytes base_path, uint32_t index) {
    if(!a || bytes_is_null(base_path)) { return bytes_null(); }

    uint8_t idx_buf[6];
    size_t idx_len = enip_cip_encode_array_index(index, idx_buf, sizeof(idx_buf));
    if(idx_len == 0) { return bytes_null(); }

    Bytes idx_bytes = bytes_alloc(a, idx_len);
    if(bytes_is_null(idx_bytes)) { return bytes_null(); }

    memcpy(idx_bytes.data, idx_buf, idx_len);

    return bytes_concat(a, base_path, idx_bytes);
}

/* ============================================================================
 * Backplane route encoding ("1,0" -> port-segment byte pairs)
 * ============================================================================ */

Bytes enip_cip_encode_route(Arena *a, const char *route) {
    if(!a || !route || route[0] == '\0') { return bytes_null(); }

    uint8_t tmp[ENIP_CIP_PATH_MAX_LEN];
    size_t total = 0;
    size_t pos = 0;

    while(route[pos] != '\0') {
        if(route[pos] < '0' || route[pos] > '9') {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: malformed route \"%s\" at position %zu.", route, pos);
            return bytes_null();
        }

        uint32_t value = 0;
        while(route[pos] >= '0' && route[pos] <= '9') {
            value = value * 10 + (uint32_t)(route[pos] - '0');
            if(value > 0xFF) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: route element out of range (0-255) in \"%s\".", route);
                return bytes_null();
            }
            pos++;
        }

        if(total + 2 > sizeof(tmp)) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: route \"%s\" is too long.", route);
            return bytes_null();
        }

        tmp[total++] = (uint8_t)value;
        tmp[total++] = 0; /* link address; filled by the next element below */

        if(route[pos] == ',') {
            pos++;

            if(route[pos] < '0' || route[pos] > '9') {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: malformed route \"%s\" at position %zu.", route, pos);
                return bytes_null();
            }

            value = 0;
            while(route[pos] >= '0' && route[pos] <= '9') {
                value = value * 10 + (uint32_t)(route[pos] - '0');
                if(value > 0xFF) {
                    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: route element out of range (0-255) in \"%s\".", route);
                    return bytes_null();
                }
                pos++;
            }

            tmp[total - 1] = (uint8_t)value;

            if(route[pos] == ',') { pos++; }
        }
    }

    if(total == 0) { return bytes_null(); }

    Bytes out = bytes_alloc(a, total);
    if(bytes_is_null(out)) { return bytes_null(); }

    memcpy(out.data, tmp, total);

    return out;
}

/* ============================================================================
 * CIP ReadTag (0x4C) request
 * ============================================================================ */

Bytes enip_cip_read(Arena *a, Bytes path, uint16_t count) {
    if(!a || bytes_is_null(path) || path.len == 0 || (path.len % 2) != 0 || path.len > 0xFF * 2) { return bytes_null(); }

    uint8_t path_size_words = (uint8_t)(path.len / 2);

    Bytes header = bytes_pack(a, BYTES_LE, (uint8_t)CIP_READ, path_size_words);
    if(bytes_is_null(header)) { return bytes_null(); }

    Bytes count_bytes = bytes_pack(a, BYTES_LE, count);
    if(bytes_is_null(count_bytes)) { return bytes_null(); }

    return bytes_concat(a, header, path, count_bytes);
}

/* ============================================================================
 * CIP WriteTag (0x4D) request
 * ============================================================================ */

Bytes enip_cip_write(Arena *a, Bytes path, Bytes type_header, uint16_t count, Bytes data) {
    if(!a || bytes_is_null(path) || path.len == 0 || (path.len % 2) != 0 || path.len > 0xFF * 2) { return bytes_null(); }
    if(bytes_is_null(type_header) || type_header.len == 0) { return bytes_null(); }
    if(bytes_is_null(data)) { return bytes_null(); }

    uint8_t path_size_words = (uint8_t)(path.len / 2);

    Bytes header = bytes_pack(a, BYTES_LE, (uint8_t)CIP_WRITE, path_size_words);
    if(bytes_is_null(header)) { return bytes_null(); }

    Bytes count_bytes = bytes_pack(a, BYTES_LE, count);
    if(bytes_is_null(count_bytes)) { return bytes_null(); }

    return bytes_concat(a, header, path, type_header, count_bytes, data);
}

/* ============================================================================
 * CIP reply parsing
 * ============================================================================ */

/* ============================================================================
 * CIP Multiple Service Packet (0x0A) request builder
 * ============================================================================ */

Bytes enip_cip_multi_service(Arena *a, Bytes *sub_reqs, uint16_t count) {
    if(!a || !sub_reqs || count == 0) { return bytes_null(); }

    /* Compute total size: fixed header(8) + offset table(2*count) + sub-requests. */
    size_t total = ENIP_MS_REQ_FIXED + (size_t)2 * count;
    for(uint16_t i = 0; i < count; i++) {
        if(bytes_is_null(sub_reqs[i])) { return bytes_null(); }
        total += sub_reqs[i].len;
    }

    uint8_t *buf = (uint8_t *)arena_alloc(a, total);
    if(!buf) { return bytes_null(); }

    /* service + path */
    buf[0] = CIP_MULTI_SVC;
    buf[1] = (uint8_t)0x02; /* path_size_words = 2 words */
    buf[2] = (uint8_t)0x20; /* class segment */
    buf[3] = (uint8_t)0x02; /* Message Router class 0x02 */
    buf[4] = (uint8_t)0x24; /* instance segment */
    buf[5] = (uint8_t)0x01; /* instance 1 */

    /* request_count LE16 */
    buf[6] = (uint8_t)(count & 0xFFu);
    buf[7] = (uint8_t)(count >> 8);

    /* Offset table: each offset is relative to the start of the Number_of_Services
     * field (CIP Vol 1 §3-5.5), i.e. it includes the 2-byte count itself.
     * offset[0] = 2 (count) + 2*count (offset table). */
    uint16_t off = (uint16_t)(2u + (uint16_t)2 * count);
    for(uint16_t i = 0; i < count; i++) {
        buf[8 + (size_t)2 * i]     = (uint8_t)(off & 0xFFu);
        buf[8 + (size_t)2 * i + 1] = (uint8_t)(off >> 8);
        off = (uint16_t)(off + (uint16_t)sub_reqs[i].len);
    }

    /* Sub-requests */
    size_t pos = ENIP_MS_REQ_FIXED + (size_t)2 * count;
    for(uint16_t i = 0; i < count; i++) {
        memcpy(buf + pos, sub_reqs[i].data, sub_reqs[i].len);
        pos += sub_reqs[i].len;
    }

    return bytes_from_buf(buf, total);
}

/* ============================================================================
 * CIP Multiple Service Packet reply parser
 * ============================================================================ */

bool enip_cip_parse_multi_service_reply(Bytes data, uint16_t *count_out,
                                        Bytes *sub_replies, uint16_t max_count) {
    /* data = the `data` slice from enip_cip_parse_reply of the outer MS reply.
     * Layout: [response_count:u16le][offset[0]:u16le]...[offset[N-1]:u16le][sub-replies...]
     * Offsets are from the start of the offset array (data.data + 2). */
    if(!count_out || !sub_replies || bytes_is_null(data) || data.len < 2) { return false; }

    uint16_t count = (uint16_t)((uint16_t)data.data[0] | (uint16_t)((uint16_t)data.data[1] << 8));
    if(count == 0 || count > max_count) { return false; }
    if(data.len < (size_t)2 + (size_t)2 * count) { return false; }

    *count_out = count;

    for(uint16_t i = 0; i < count; i++) {
        size_t tbl = (size_t)2 + (size_t)2 * i;
        uint16_t start_off = (uint16_t)((uint16_t)data.data[tbl] | (uint16_t)((uint16_t)data.data[tbl + 1] << 8));
        uint16_t end_off;

        if(i + 1 < count) {
            size_t tbl_next = (size_t)2 + (size_t)2 * (i + 1);
            end_off = (uint16_t)((uint16_t)data.data[tbl_next] | (uint16_t)((uint16_t)data.data[tbl_next + 1] << 8));
        } else {
            /* Last sub-reply: extends to the end of data. */
            end_off = (uint16_t)data.len;
        }

        if(start_off >= end_off) { return false; }
        if((size_t)end_off > data.len) { return false; }

        /* Offsets are from Number_of_Services start (= data.data[0]). */
        sub_replies[i] = bytes_from_buf(data.data + start_off, (size_t)(end_off - start_off));
    }

    return true;
}

/* ============================================================================
 * CIP reply parsing
 * ============================================================================ */

bool enip_cip_parse_reply(Bytes in, cip_reply_t *out) {
    if(!out) { return false; }

    *out = (cip_reply_t){0};

    if(bytes_is_null(in) || in.len < CIP_READ_REPLY_OVERHEAD) { return false; }

    uint8_t service = 0, reserved = 0, status = 0, ext_size = 0;

    Bytes rest = bytes_unpack(in, BYTES_LE, &service, &reserved, &status, &ext_size);
    if(bytes_is_null(rest)) { return false; }

    out->service = service;
    out->status = status;

    if(ext_size > 0) {
        size_t ext_bytes = (size_t)ext_size * 2u;
        if(rest.len < ext_bytes) { return false; }

        uint16_t ext_status = 0;
        Bytes ext_rest = bytes_unpack(rest, BYTES_LE, &ext_status);
        if(bytes_is_null(ext_rest)) { return false; }

        out->ext_status = ext_status;
        rest = bytes_slice(rest, ext_bytes, rest.len - ext_bytes);
        if(bytes_is_null(rest)) { return false; }
    }

    out->data = rest;

    return true;
}
