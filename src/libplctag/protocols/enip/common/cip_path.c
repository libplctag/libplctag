/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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

#include "cip_path.h"

extern bool cip_path_parse(Bytes path, cip_path_ids_t *out) {
    if(!out) { return false; }

    out->class_id = 0;
    out->instance_id = 0;
    out->attr_id = 0;
    out->has_class = false;
    out->has_instance = false;
    out->has_attr = false;

    Bytes rest = path;

    while(rest.len > 0) {
        uint8_t seg = 0;
        Bytes next = bytes_unpack(rest, BYTES_LE, &seg);
        if(bytes_is_null(next)) { return false; }
        rest = next;

        switch(seg) {
            case 0x20: { /* 8-bit class */
                uint8_t v = 0;
                if(bytes_is_null(next = bytes_unpack(rest, BYTES_LE, &v))) { return false; }
                rest = next;
                out->class_id = v;
                out->has_class = true;
                break;
            }
            case 0x21: { /* 16-bit class */
                uint16_t v = 0;
                if(bytes_is_null(next = bytes_unpack(rest, BYTES_LE, BYTES_SKIP(1) /* pad */, &v))) { return false; }
                rest = next;
                out->class_id = v;
                out->has_class = true;
                break;
            }
            case 0x22: { /* 32-bit class */
                uint32_t v = 0;
                if(bytes_is_null(next = bytes_unpack(rest, BYTES_LE, BYTES_SKIP(1) /* pad */, &v))) { return false; }
                rest = next;
                out->class_id = v;
                out->has_class = true;
                break;
            }
            case 0x24: { /* 8-bit instance */
                uint8_t v = 0;
                if(bytes_is_null(next = bytes_unpack(rest, BYTES_LE, &v))) { return false; }
                rest = next;
                out->instance_id = v;
                out->has_instance = true;
                break;
            }
            case 0x25: { /* 16-bit instance */
                uint16_t v = 0;
                if(bytes_is_null(next = bytes_unpack(rest, BYTES_LE, BYTES_SKIP(1) /* pad */, &v))) { return false; }
                rest = next;
                out->instance_id = v;
                out->has_instance = true;
                break;
            }
            case 0x26: { /* 32-bit instance */
                uint32_t v = 0;
                if(bytes_is_null(next = bytes_unpack(rest, BYTES_LE, BYTES_SKIP(1) /* pad */, &v))) { return false; }
                rest = next;
                out->instance_id = v;
                out->has_instance = true;
                break;
            }
            case 0x30: { /* 8-bit attribute */
                uint8_t v = 0;
                if(bytes_is_null(next = bytes_unpack(rest, BYTES_LE, &v))) { return false; }
                rest = next;
                out->attr_id = v;
                out->has_attr = true;
                break;
            }
            case 0x31: { /* 16-bit attribute */
                uint16_t v = 0;
                if(bytes_is_null(next = bytes_unpack(rest, BYTES_LE, BYTES_SKIP(1) /* pad */, &v))) { return false; }
                rest = next;
                out->attr_id = v;
                out->has_attr = true;
                break;
            }
            default:
                return false;
        }
    }

    return out->has_class && out->has_instance;
}


extern bool cip_path_parse_indexes(Bytes path, uint32_t *num_idx_out, uint32_t *indexes, uint32_t max_idx,
                                   Bytes *rest_out) {
    if(!num_idx_out || !indexes) { return false; }

    *num_idx_out = 0;
    Bytes rest = path;

    while(rest.len > 0) {
        uint8_t idx_type = 0;
        Bytes after_type = bytes_unpack(rest, BYTES_LE, &idx_type);
        if(bytes_is_null(after_type)) { break; }

        if(*num_idx_out >= max_idx) { return false; }

        switch(idx_type) {
            case 0x28: { /* 8-bit index */
                uint8_t v = 0;
                Bytes after_val = bytes_unpack(after_type, BYTES_LE, &v);
                if(bytes_is_null(after_val)) { return false; }
                indexes[(*num_idx_out)++] = (uint32_t)v;
                rest = after_val;
                break;
            }
            case 0x29: { /* 16-bit index */
                uint16_t v = 0;
                Bytes after_val = bytes_unpack(after_type, BYTES_LE, BYTES_SKIP(1) /* pad */, &v);
                if(bytes_is_null(after_val)) { return false; }
                indexes[(*num_idx_out)++] = (uint32_t)v;
                rest = after_val;
                break;
            }
            case 0x2A: { /* 32-bit index */
                uint32_t v = 0;
                Bytes after_val = bytes_unpack(after_type, BYTES_LE, BYTES_SKIP(1) /* pad */, &v);
                if(bytes_is_null(after_val)) { return false; }
                indexes[(*num_idx_out)++] = v;
                rest = after_val;
                break;
            }
            default:
                /* Not an index segment -- stop, leave it (and everything
                 * after it) for the caller via *rest_out. */
                if(rest_out) { *rest_out = rest; }
                return true;
        }
    }

    if(rest_out) { *rest_out = rest; }
    return true;
}


extern Bytes cip_path_encode(Arena *a, Bytes prefix, uint32_t class_id, uint32_t instance_id, int64_t attr_id) {
    if(!a) { return bytes_null(); }
    if(!bytes_is_null(prefix) && (prefix.len % 2) != 0) { return bytes_null(); }

    Bytes hdr;

    if(class_id <= 0xFFu) {
        hdr = bytes_pack(a, BYTES_LE, (uint8_t)0x20, (uint8_t)class_id);
    } else if(class_id <= 0xFFFFu) {
        hdr = bytes_pack(a, BYTES_LE, (uint8_t)0x21, (uint8_t)0, (uint16_t)class_id);
    } else {
        hdr = bytes_pack(a, BYTES_LE, (uint8_t)0x22, (uint8_t)0, class_id);
    }
    if(bytes_is_null(hdr)) { return bytes_null(); }

    /* Instance floors at 16-bit (never 8-bit) -- matches every existing
     * caller's wire bytes; only widens to 32-bit past 0xFFFF. */
    Bytes inst;
    if(instance_id <= 0xFFFFu) {
        inst = bytes_pack(a, BYTES_LE, (uint8_t)0x25, (uint8_t)0, (uint16_t)instance_id);
    } else {
        inst = bytes_pack(a, BYTES_LE, (uint8_t)0x26, (uint8_t)0, instance_id);
    }
    if(bytes_is_null(inst)) { return bytes_null(); }

    Bytes path = bytes_concat(a, hdr, inst);
    if(bytes_is_null(path)) { return bytes_null(); }

    if(attr_id >= 0) {
        Bytes attr;
        if((uint64_t)attr_id <= 0xFFu) {
            attr = bytes_pack(a, BYTES_LE, (uint8_t)0x30, (uint8_t)attr_id);
        } else {
            attr = bytes_pack(a, BYTES_LE, (uint8_t)0x31, (uint8_t)0, (uint16_t)attr_id);
        }
        if(bytes_is_null(attr)) { return bytes_null(); }
        path = bytes_concat(a, path, attr);
        if(bytes_is_null(path)) { return bytes_null(); }
    }

    if(bytes_is_null(prefix) || prefix.len == 0) { return path; }

    return bytes_concat(a, prefix, path);
}


extern size_t cip_path_encode_index_into(uint32_t index, uint8_t *buf, size_t buf_size) {
    if(!buf || buf_size == 0) { return 0; }

    Bytes dest = bytes_from_buf(buf, buf_size);
    Bytes rest;

    if(index <= 0xFFu) {
        rest = bytes_pack_into(dest, BYTES_LE, (uint8_t)0x28, (uint8_t)index);
    } else if(index <= 0xFFFFu) {
        rest = bytes_pack_into(dest, BYTES_LE, (uint8_t)0x29, (uint8_t)0x00 /* reserved */, (uint16_t)index);
    } else {
        rest = bytes_pack_into(dest, BYTES_LE, (uint8_t)0x2A, (uint8_t)0x00 /* reserved */, index);
    }

    if(bytes_is_null(rest)) { return 0; }
    return dest.len - rest.len;
}
