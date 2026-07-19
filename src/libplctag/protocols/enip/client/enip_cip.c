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

#include <libplctag/protocols/enip/client/enip_cip.h>
#include <libplctag/protocols/enip/common/cip_path.h>
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

/* Encode tag_name into buf[buf_size] in ANSI CIP Extended Symbol format.
 * Returns bytes written, or 0 on error (invalid syntax, segment too long, or
 * buffer too small). */
static size_t enip_cip_encode_tag_path(const char *tag_name, uint8_t *buf, size_t buf_size) {
    if(!tag_name || !buf) { return 0; }

    Bytes start = bytes_from_buf(buf, buf_size);
    Bytes rest = start;
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

        bool pad = (name_len & 1u) != 0;
        Bytes next = bytes_pack_into(rest, BYTES_LE, (uint8_t)0x91, (uint8_t)name_len,
                                     bytes_from_buf((const uint8_t *)&tag_name[pos], name_len));
        if(!bytes_is_null(next) && pad) { next = bytes_pack_into(next, BYTES_LE, (uint8_t)0x00); }
        if(bytes_is_null(next)) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: buffer too small for symbol.");
            return 0;
        }
        rest = next;
        pos += name_len;

        while(tag_name[pos] == '[') {
            uint32_t index = 0;
            size_t new_pos = enip_cip_parse_array_index(tag_name, pos, &index);

            if(new_pos == pos) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: invalid array index at position %zu.", pos);
                return 0;
            }

            size_t idx_size = cip_path_encode_index_into(index, rest.data, rest.len);
            if(idx_size == 0) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: failed to encode array index %" PRIu32 ".", index);
                return 0;
            }

            rest = bytes_slice(rest, idx_size, rest.len - idx_size);
            pos = new_pos;
        }

        if(tag_name[pos] != '.' && tag_name[pos] != '\0') {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: unexpected character in tag path at position %zu.", pos);
            return 0;
        }
    }

    size_t total = start.len - rest.len;
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

    return bytes_pack(a, BYTES_LE, bytes_from_buf(tmp, len));
}

Bytes enip_cip_encode_path_at(Arena *a, Bytes base_path, uint32_t index) {
    if(!a || bytes_is_null(base_path)) { return bytes_null(); }

    uint8_t idx_buf[6];
    size_t idx_len = cip_path_encode_index_into(index, idx_buf, sizeof(idx_buf));
    if(idx_len == 0) { return bytes_null(); }

    Bytes idx_bytes = bytes_pack(a, BYTES_LE, bytes_from_buf(idx_buf, idx_len));
    if(bytes_is_null(idx_bytes)) { return bytes_null(); }

    return bytes_concat(a, base_path, idx_bytes);
}

/* ============================================================================
 * Backplane route encoding ("1,0" -> port-segment byte pairs)
 * ============================================================================ */

Bytes enip_cip_encode_route(Arena *a, const char *route) {
    if(!a) { return bytes_null(); }

    /* No routing segment (direct connection -- e.g. a MicroLogix/SLC/PLC-5
     * reachable straight over Ethernet, no bridging backplane/DH+ hop): a
     * valid, empty path, not an error. Distinct from bytes_null(), which
     * callers (build_forward_open) treat as "could not encode". */
    if(!route || route[0] == '\0') { return bytes_alloc(a, 0); }

    uint8_t tmp[ENIP_CIP_PATH_MAX_LEN];
    Bytes start = bytes_from_buf(tmp, sizeof(tmp));
    Bytes rest = start;
    size_t pos = 0;

    while(route[pos] != '\0') {
        if(route[pos] < '0' || route[pos] > '9') {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: malformed route \"%s\" at position %zu.", route, pos);
            return bytes_null();
        }

        uint32_t port_value = 0;
        while(route[pos] >= '0' && route[pos] <= '9') {
            port_value = port_value * 10 + (uint32_t)(route[pos] - '0');
            if(port_value > 0xFF) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: route element out of range (0-255) in \"%s\".", route);
                return bytes_null();
            }
            pos++;
        }

        uint32_t link_value = 0; /* link address; 0 unless a comma-separated element follows */
        if(route[pos] == ',') {
            pos++;

            if(route[pos] < '0' || route[pos] > '9') {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: malformed route \"%s\" at position %zu.", route, pos);
                return bytes_null();
            }

            while(route[pos] >= '0' && route[pos] <= '9') {
                link_value = link_value * 10 + (uint32_t)(route[pos] - '0');
                if(link_value > 0xFF) {
                    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: route element out of range (0-255) in \"%s\".", route);
                    return bytes_null();
                }
                pos++;
            }

            if(route[pos] == ',') { pos++; }
        }

        Bytes next = bytes_pack_into(rest, BYTES_LE, (uint8_t)port_value, (uint8_t)link_value);
        if(bytes_is_null(next)) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "CIP: route \"%s\" is too long.", route);
            return bytes_null();
        }
        rest = next;
    }

    size_t total = start.len - rest.len;
    if(total == 0) { return bytes_null(); }

    return bytes_pack(a, BYTES_LE, bytes_from_buf(tmp, total));
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
 * CIP ReadTag Fragmented (0x52) / WriteTag Fragmented (0x53) requests (§16a.6)
 * ============================================================================ */

Bytes enip_cip_read_frag(Arena *a, Bytes path, uint16_t count, uint32_t byte_offset) {
    if(!a || bytes_is_null(path) || path.len == 0 || (path.len % 2) != 0 || path.len > 0xFF * 2) { return bytes_null(); }

    uint8_t path_size_words = (uint8_t)(path.len / 2);

    Bytes header = bytes_pack(a, BYTES_LE, (uint8_t)CIP_READ_FRAG, path_size_words);
    if(bytes_is_null(header)) { return bytes_null(); }

    Bytes tail = bytes_pack(a, BYTES_LE, count, byte_offset);
    if(bytes_is_null(tail)) { return bytes_null(); }

    return bytes_concat(a, header, path, tail);
}

Bytes enip_cip_write_frag(Arena *a, Bytes path, Bytes type_header, uint16_t count, uint32_t byte_offset, Bytes data) {
    if(!a || bytes_is_null(path) || path.len == 0 || (path.len % 2) != 0 || path.len > 0xFF * 2) { return bytes_null(); }
    if(bytes_is_null(type_header) || type_header.len == 0) { return bytes_null(); }
    if(bytes_is_null(data)) { return bytes_null(); }

    uint8_t path_size_words = (uint8_t)(path.len / 2);

    Bytes header = bytes_pack(a, BYTES_LE, (uint8_t)CIP_WRITE_FRAG, path_size_words);
    if(bytes_is_null(header)) { return bytes_null(); }

    Bytes tail = bytes_pack(a, BYTES_LE, count, byte_offset);
    if(bytes_is_null(tail)) { return bytes_null(); }

    return bytes_concat(a, header, path, type_header, tail, data);
}

/* ============================================================================
 * CIP reply parsing
 * ============================================================================ */

/* ============================================================================
 * CIP tag/UDT listing requests (class 0x6B / 0x6C)
 * ============================================================================ */

Bytes enip_cip_list_tags(Arena *a, Bytes prefix, uint16_t instance_id) {
    if(!a) { return bytes_null(); }

    Bytes path = cip_path_encode(a, prefix, (uint8_t)0x6B, instance_id, -1);
    if(bytes_is_null(path) || path.len > 0xFF * 2) { return bytes_null(); }

    Bytes header = bytes_pack(a, BYTES_LE, (uint8_t)CIP_LIST_TAGS, (uint8_t)(path.len / 2));
    if(bytes_is_null(header)) { return bytes_null(); }

    /* num_attributes + attrs 0x02, 0x07, 0x08, 0x01 */
    Bytes attrs =
        bytes_pack(a, BYTES_LE, (uint16_t)4, (uint16_t)0x02, (uint16_t)0x07, (uint16_t)0x08, (uint16_t)0x01);
    if(bytes_is_null(attrs)) { return bytes_null(); }

    return bytes_concat(a, header, path, attrs);
}

Bytes enip_cip_udt_meta(Arena *a, uint16_t udt_id) {
    if(!a) { return bytes_null(); }

    Bytes path = cip_path_encode(a, bytes_null(), (uint8_t)0x6C, udt_id, -1);
    if(bytes_is_null(path)) { return bytes_null(); }

    Bytes header = bytes_pack(a, BYTES_LE, (uint8_t)CIP_GET_ATTR_LIST, (uint8_t)(path.len / 2));
    if(bytes_is_null(header)) { return bytes_null(); }

    /* num_attributes + attrs 0x04, 0x05, 0x02, 0x01 */
    Bytes attrs =
        bytes_pack(a, BYTES_LE, (uint16_t)4, (uint16_t)0x04, (uint16_t)0x05, (uint16_t)0x02, (uint16_t)0x01);
    if(bytes_is_null(attrs)) { return bytes_null(); }

    return bytes_concat(a, header, path, attrs);
}

Bytes enip_cip_udt_fields(Arena *a, uint16_t udt_id, uint32_t offset, uint16_t total) {
    if(!a) { return bytes_null(); }

    Bytes path = cip_path_encode(a, bytes_null(), (uint8_t)0x6C, udt_id, -1);
    if(bytes_is_null(path)) { return bytes_null(); }

    Bytes header = bytes_pack(a, BYTES_LE, (uint8_t)CIP_READ, (uint8_t)(path.len / 2));
    if(bytes_is_null(header)) { return bytes_null(); }

    Bytes body = bytes_pack(a, BYTES_LE, (uint32_t)offset, (uint16_t)total);
    if(bytes_is_null(body)) { return bytes_null(); }

    return bytes_concat(a, header, path, body);
}

Bytes enip_cip_omron_list_tags(Arena *a, uint32_t start_instance, uint32_t count, uint16_t kind) {
    if(!a) { return bytes_null(); }

    Bytes path = cip_path_encode(a, bytes_null(), (uint8_t)0x6A, 0, -1);
    if(bytes_is_null(path)) { return bytes_null(); }

    Bytes header = bytes_pack(a, BYTES_LE, (uint8_t)CIP_GET_INSTANCE_LIST_EX2, (uint8_t)(path.len / 2));
    if(bytes_is_null(header)) { return bytes_null(); }

    Bytes body = bytes_pack(a, BYTES_LE, (uint32_t)start_instance, (uint32_t)count, (uint16_t)kind);
    if(bytes_is_null(body)) { return bytes_null(); }

    return bytes_concat(a, header, path, body);
}

Bytes enip_cip_omron_udt_get_all(Arena *a, uint32_t type_instance_id) {
    if(!a) { return bytes_null(); }

    /* Real template ids stay in the existing 16-bit (0x25) form (unchanged
     * wire bytes for every pre-existing top-level request); the synthetic
     * member ids omron_listing.c hands back via next_instance_id are always
     * > 0xFFFF (see its member_id_encode) and get the 32-bit (0x26) form --
     * cip_path_encode already floors instance encoding at 16-bit and only
     * widens past 0xFFFF, so no special case is needed here. */
    Bytes path = cip_path_encode(a, bytes_null(), (uint8_t)0x6C, type_instance_id, -1);
    if(bytes_is_null(path)) { return bytes_null(); }

    Bytes header = bytes_pack(a, BYTES_LE, (uint8_t)CIP_GET_ATTR_ALL, (uint8_t)(path.len / 2));
    if(bytes_is_null(header)) { return bytes_null(); }

    return bytes_concat(a, header, path);
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

    uint16_t count = 0;
    if(bytes_is_null(bytes_unpack(data, BYTES_LE, &count))) { return false; }
    if(count == 0 || count > max_count) { return false; }
    if(data.len < (size_t)2 + (size_t)2 * count) { return false; }

    *count_out = count;

    for(uint16_t i = 0; i < count; i++) {
        size_t tbl = (size_t)2 + (size_t)2 * i;
        uint16_t start_off = 0;
        if(bytes_is_null(bytes_unpack(bytes_slice(data, tbl, data.len - tbl), BYTES_LE, &start_off))) { return false; }

        uint16_t end_off;
        if(i + 1 < count) {
            size_t tbl_next = tbl + 2;
            if(bytes_is_null(bytes_unpack(bytes_slice(data, tbl_next, data.len - tbl_next), BYTES_LE, &end_off))) {
                return false;
            }
        } else {
            /* Last sub-reply: extends to the end of data. */
            end_off = (uint16_t)data.len;
        }

        if(start_off >= end_off) { return false; }
        if((size_t)end_off > data.len) { return false; }

        /* Offsets are from Number_of_Services start (= data.data[0]). */
        sub_replies[i] = bytes_slice(data, start_off, (size_t)(end_off - start_off));
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
