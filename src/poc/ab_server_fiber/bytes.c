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
 * Bytes implementation.  Copied from ~/Projects/data_table and modified:
 *   - Added bytes_alloc(), bytes_zero().
 *   - Removed helpers not needed by modbus_server3 (b_str, bytes_repeat,
 *     pack_uint8, pack_uint16_le, pack_uint32_le).
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bytes.h"


/* ============================================================================
 * New helpers
 * ============================================================================ */

Bytes bytes_alloc(Arena *a, size_t len) {
    void *mem = arena_alloc(a, len);
    if(!mem) { return (Bytes){NULL, 0}; }
    return (Bytes){(uint8_t *)mem, len};
}


void bytes_zero(Bytes b) {
    if(b.data) { memset(b.data, 0, b.len); }
}


/* ============================================================================
 * Concatenation
 * ============================================================================ */

Bytes bytes_concat_impl(Arena *a, int count, ...) {
    va_list args;
    size_t total_len = 0;

    va_start(args, count);
    for(int i = 0; i < count; i++) {
        Bytes b = va_arg(args, Bytes);
        total_len += b.len;
    }
    va_end(args);

    uint8_t *mem = (uint8_t *)arena_alloc(a, total_len);
    if(!mem) { return (Bytes){NULL, 0}; }

    size_t offset = 0;
    va_start(args, count);
    for(int i = 0; i < count; i++) {
        Bytes b = va_arg(args, Bytes);
        if(b.data) { memcpy(mem + offset, b.data, b.len); }
        offset += b.len;
    }
    va_end(args);

    return (Bytes){mem, total_len};
}


/* ============================================================================
 * Struct pack / unpack helpers
 * ============================================================================ */

/* Returns 1 for little-endian, -1 for big-endian, 0 for native. */
static int get_byte_order(const char *fmt) {
    if(!fmt || !*fmt) { return 0; }
    if(*fmt == '<') { return 1; }
    if(*fmt == '>') { return -1; }
    return 0;
}


static const char *skip_byte_order(const char *fmt) {
    if(!fmt || !*fmt) { return fmt; }
    if(*fmt == '<' || *fmt == '>' || *fmt == '=' || *fmt == '@') { return fmt + 1; }
    return fmt;
}


/* Pack a single value into buf at the current position.
 * Returns the number of bytes written, or 0 on unknown format. */
static size_t pack_value(uint8_t *buf, uint64_t value, const char *str, char fmt_char, int byte_order) {
    if(byte_order == 0) { byte_order = 1; }

    switch(fmt_char) {
        case 'b':
        case 'B': buf[0] = (uint8_t)value; return 1;

        case 'h':
        case 'H': {
            uint16_t v = (uint16_t)value;
            if(byte_order > 0) {
                buf[0] = (uint8_t)(v & 0xFF);
                buf[1] = (uint8_t)((v >> 8) & 0xFF);
            } else {
                buf[0] = (uint8_t)((v >> 8) & 0xFF);
                buf[1] = (uint8_t)(v & 0xFF);
            }
            return 2;
        }

        case 'i':
        case 'I': {
            uint32_t v = (uint32_t)value;
            if(byte_order > 0) {
                buf[0] = (uint8_t)(v & 0xFF);
                buf[1] = (uint8_t)((v >> 8) & 0xFF);
                buf[2] = (uint8_t)((v >> 16) & 0xFF);
                buf[3] = (uint8_t)((v >> 24) & 0xFF);
            } else {
                buf[0] = (uint8_t)((v >> 24) & 0xFF);
                buf[1] = (uint8_t)((v >> 16) & 0xFF);
                buf[2] = (uint8_t)((v >> 8) & 0xFF);
                buf[3] = (uint8_t)(v & 0xFF);
            }
            return 4;
        }

        case 'q':
        case 'Q': {
            uint64_t v = value;
            if(byte_order > 0) {
                for(int i = 0; i < 8; i++) { buf[i] = (uint8_t)((v >> (i * 8)) & 0xFF); }
            } else {
                for(int i = 0; i < 8; i++) { buf[i] = (uint8_t)((v >> ((7 - i) * 8)) & 0xFF); }
            }
            return 8;
        }

        case 'f': {
            uint32_t v = (uint32_t)value;
            if(byte_order > 0) {
                buf[0] = (uint8_t)(v & 0xFF);
                buf[1] = (uint8_t)((v >> 8) & 0xFF);
                buf[2] = (uint8_t)((v >> 16) & 0xFF);
                buf[3] = (uint8_t)((v >> 24) & 0xFF);
            } else {
                buf[0] = (uint8_t)((v >> 24) & 0xFF);
                buf[1] = (uint8_t)((v >> 16) & 0xFF);
                buf[2] = (uint8_t)((v >> 8) & 0xFF);
                buf[3] = (uint8_t)(v & 0xFF);
            }
            return 4;
        }

        case 'd': {
            uint64_t v = value;
            if(byte_order > 0) {
                for(int i = 0; i < 8; i++) { buf[i] = (uint8_t)((v >> (i * 8)) & 0xFF); }
            } else {
                for(int i = 0; i < 8; i++) { buf[i] = (uint8_t)((v >> ((7 - i) * 8)) & 0xFF); }
            }
            return 8;
        }

        case 'z': {
            if(!str) { return 0; }
            size_t len = strlen(str);
            memcpy(buf, str, len);
            return len;
        }

        default: return 0;
    }
}


static uint64_t unpack_value(const uint8_t *buf, char fmt_char, int byte_order) {
    if(byte_order == 0) { byte_order = 1; }

    switch(fmt_char) {
        case 'b':
        case 'B': return (uint64_t)buf[0];

        case 'h':
        case 'H':
            if(byte_order > 0) {
                return (uint64_t)buf[0] | ((uint64_t)buf[1] << 8);
            } else {
                return ((uint64_t)buf[0] << 8) | (uint64_t)buf[1];
            }

        case 'i':
        case 'I':
            if(byte_order > 0) {
                return (uint64_t)buf[0] | ((uint64_t)buf[1] << 8) | ((uint64_t)buf[2] << 16) | ((uint64_t)buf[3] << 24);
            } else {
                return ((uint64_t)buf[0] << 24) | ((uint64_t)buf[1] << 16) | ((uint64_t)buf[2] << 8) | (uint64_t)buf[3];
            }

        case 'q':
        case 'Q': {
            uint64_t v = 0;
            if(byte_order > 0) {
                for(int i = 0; i < 8; i++) { v |= ((uint64_t)buf[i] << (i * 8)); }
            } else {
                for(int i = 0; i < 8; i++) { v |= ((uint64_t)buf[i] << ((7 - i) * 8)); }
            }
            return v;
        }

        case 'f':
        case 'd': {
            uint64_t v = 0;
            int size = (fmt_char == 'f') ? 4 : 8;
            if(byte_order > 0) {
                for(int i = 0; i < size; i++) { v |= ((uint64_t)buf[i] << (i * 8)); }
            } else {
                for(int i = 0; i < size; i++) { v |= ((uint64_t)buf[i] << ((size - 1 - i) * 8)); }
            }
            return v;
        }

        default: return 0;
    }
}


/* ============================================================================
 * Dynamic-array helpers for '*T' format
 * ============================================================================ */

static size_t elem_size_for(char c) {
    switch(c) {
        case 'b':
        case 'B': return 1;
        case 'h':
        case 'H': return 2;
        case 'i':
        case 'I':
        case 'f': return 4;
        case 'q':
        case 'Q':
        case 'd': return 8;
        default: return 0;
    }
}

/* Read element i from a native-endian array into a uint64_t. */
static uint64_t read_array_elem(const void *ptr, size_t i, char c) {
    const uint8_t *p = (const uint8_t *)ptr;
    switch(c) {
        case 'b': {
            int8_t v;
            memcpy(&v, p + i, 1);
            return (uint64_t)(uint8_t)v;
        }
        case 'B': {
            uint8_t v;
            memcpy(&v, p + i, 1);
            return v;
        }
        case 'h': {
            int16_t v;
            memcpy(&v, p + i * 2, 2);
            return (uint64_t)(uint16_t)v;
        }
        case 'H': {
            uint16_t v;
            memcpy(&v, p + i * 2, 2);
            return v;
        }
        case 'i': {
            int32_t v;
            memcpy(&v, p + i * 4, 4);
            return (uint64_t)(uint32_t)v;
        }
        case 'I': {
            uint32_t v;
            memcpy(&v, p + i * 4, 4);
            return v;
        }
        case 'q': {
            int64_t v;
            memcpy(&v, p + i * 8, 8);
            return (uint64_t)v;
        }
        case 'Q': {
            uint64_t v;
            memcpy(&v, p + i * 8, 8);
            return v;
        }
        case 'f': {
            float v;
            uint32_t bits;
            memcpy(&v, p + i * 4, 4);
            memcpy(&bits, &v, 4);
            return bits;
        }
        case 'd': {
            double v;
            uint64_t bits;
            memcpy(&v, p + i * 8, 8);
            memcpy(&bits, &v, 8);
            return bits;
        }
        default: return 0;
    }
}

/* Write element i into a native-endian array from a uint64_t. */
static void write_array_elem(void *ptr, size_t i, char c, uint64_t val) {
    uint8_t *p = (uint8_t *)ptr;
    switch(c) {
        case 'b': {
            int8_t v = (int8_t)val;
            memcpy(p + i, &v, 1);
            break;
        }
        case 'B': {
            uint8_t v = (uint8_t)val;
            memcpy(p + i, &v, 1);
            break;
        }
        case 'h': {
            int16_t v = (int16_t)val;
            memcpy(p + i * 2, &v, 2);
            break;
        }
        case 'H': {
            uint16_t v = (uint16_t)val;
            memcpy(p + i * 2, &v, 2);
            break;
        }
        case 'i': {
            int32_t v = (int32_t)val;
            memcpy(p + i * 4, &v, 4);
            break;
        }
        case 'I': {
            uint32_t v = (uint32_t)val;
            memcpy(p + i * 4, &v, 4);
            break;
        }
        case 'q': {
            int64_t v = (int64_t)val;
            memcpy(p + i * 8, &v, 8);
            break;
        }
        case 'Q': {
            uint64_t v = val;
            memcpy(p + i * 8, &v, 8);
            break;
        }
        case 'f': {
            uint32_t bits = (uint32_t)val;
            float v;
            memcpy(&v, &bits, 4);
            memcpy(p + i * 4, &v, 4);
            break;
        }
        case 'd': {
            uint64_t bits = val;
            double v;
            memcpy(&v, &bits, 8);
            memcpy(p + i * 8, &v, 8);
            break;
        }
        default: break;
    }
}


/* ============================================================================
 * bytes_pack
 * ============================================================================ */

Bytes bytes_pack(Arena *a, const char *fmt, ...) {
    if(!fmt || !*fmt) { return (Bytes){NULL, 0}; }

    int byte_order = get_byte_order(fmt);
    const char *format_chars = skip_byte_order(fmt);

    /* First pass: compute total byte count. */
    va_list args_count;
    va_start(args_count, fmt);
    size_t total_size = 0;

    for(const char *p = format_chars; *p;) {
        size_t count = 1;
        if(*p >= '0' && *p <= '9') {
            count = 0;
            while(*p >= '0' && *p <= '9') { count = count * 10 + (size_t)(*p++ - '0'); }
            if(!*p) { break; }
        }

        char c = *p++;
        if(c == 'x') {
            total_size += count;
        } else if(c == '*') {
            /* Dynamic array: next format char is element type. */
            if(!*p) { break; }
            char elem = *p++;
            size_t dyn_count = va_arg(args_count, size_t);
            if(elem == 'x') {
                total_size += dyn_count; /* *x: no pointer arg */
            } else {
                va_arg(args_count, void *); /* skip array pointer */
                total_size += dyn_count * elem_size_for(elem);
            }
        } else if(c == 'z') {
            const char *str = va_arg(args_count, const char *);
            if(str) { total_size += strlen(str); }
        } else if(c == 'b' || c == 'B') {
            total_size += count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, uint64_t); }
        } else if(c == 'h' || c == 'H') {
            total_size += 2 * count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, uint64_t); }
        } else if(c == 'i' || c == 'I') {
            total_size += 4 * count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, uint64_t); }
        } else if(c == 'f') {
            total_size += 4 * count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, double); }
        } else if(c == 'q' || c == 'Q') {
            total_size += 8 * count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, uint64_t); }
        } else if(c == 'd') {
            total_size += 8 * count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, double); }
        } else {
            va_arg(args_count, uint64_t);
        }
    }
    va_end(args_count);

    if(total_size == 0) { return (Bytes){NULL, 0}; }

    uint8_t *buf = (uint8_t *)arena_alloc(a, total_size);
    if(!buf) { return (Bytes){NULL, 0}; }

    /* Second pass: pack values. */
    va_list args;
    va_start(args, fmt);
    size_t offset = 0;

    for(const char *p = format_chars; *p;) {
        size_t count = 1;
        if(*p >= '0' && *p <= '9') {
            count = 0;
            while(*p >= '0' && *p <= '9') { count = count * 10 + (size_t)(*p++ - '0'); }
            if(!*p) { break; }
        }

        char c = *p++;
        if(c == 'x') {
            memset(buf + offset, 0, count);
            offset += count;
        } else if(c == '*') {
            /* Dynamic array: next format char is element type. */
            if(!*p) { break; }
            char elem = *p++;
            size_t dyn_count = va_arg(args, size_t);
            if(elem == 'x') {
                memset(buf + offset, 0, dyn_count);
                offset += dyn_count;
            } else {
                void *arr = va_arg(args, void *);
                if(arr) {
                    for(size_t i = 0; i < dyn_count; i++) {
                        uint64_t val = read_array_elem(arr, i, elem);
                        offset += pack_value(buf + offset, val, NULL, elem, byte_order);
                    }
                } else {
                    offset += dyn_count * elem_size_for(elem);
                }
            }
        } else if(c == 'z') {
            const char *str = va_arg(args, const char *);
            size_t written = pack_value(buf + offset, 0, str, 'z', byte_order);
            offset += written;
        } else {
            for(size_t i = 0; i < count; i++) {
                uint64_t val;
                if(c == 'f') {
                    double d = va_arg(args, double);
                    float f = (float)d;
                    uint32_t bits;
                    memcpy(&bits, &f, 4);
                    val = bits;
                } else if(c == 'd') {
                    double d = va_arg(args, double);
                    memcpy(&val, &d, 8);
                } else {
                    val = va_arg(args, uint64_t);
                }
                size_t written = pack_value(buf + offset, val, NULL, c, byte_order);
                offset += written;
            }
        }
    }
    va_end(args);

    return (Bytes){buf, offset};
}


/* ============================================================================
 * bytes_pack_into
 * ============================================================================ */

Bytes bytes_pack_into(Bytes buf, const char *fmt, ...) {
    if(!fmt || !*fmt || !buf.data) { return (Bytes){NULL, 0}; }

    int byte_order = get_byte_order(fmt);
    const char *format_chars = skip_byte_order(fmt);

    /* First pass: compute total bytes needed. */
    va_list args_count;
    va_start(args_count, fmt);
    size_t total_size = 0;

    for(const char *p = format_chars; *p;) {
        size_t count = 1;
        if(*p >= '0' && *p <= '9') {
            count = 0;
            while(*p >= '0' && *p <= '9') { count = count * 10 + (size_t)(*p++ - '0'); }
            if(!*p) { break; }
        }
        char c = *p++;
        if(c == 'x') {
            total_size += count;
        } else if(c == '*') {
            if(!*p) { break; }
            char elem = *p++;
            size_t dyn_count = va_arg(args_count, size_t);
            if(elem == 'x') {
                total_size += dyn_count;
            } else {
                va_arg(args_count, void *);
                total_size += dyn_count * elem_size_for(elem);
            }
        } else if(c == 'z') {
            const char *str = va_arg(args_count, const char *);
            if(str) { total_size += strlen(str); }
        } else if(c == 'b' || c == 'B') {
            total_size += count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, uint64_t); }
        } else if(c == 'h' || c == 'H') {
            total_size += 2 * count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, uint64_t); }
        } else if(c == 'i' || c == 'I') {
            total_size += 4 * count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, uint64_t); }
        } else if(c == 'f') {
            total_size += 4 * count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, double); }
        } else if(c == 'q' || c == 'Q') {
            total_size += 8 * count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, uint64_t); }
        } else if(c == 'd') {
            total_size += 8 * count;
            for(size_t i = 0; i < count; i++) { va_arg(args_count, double); }
        } else {
            va_arg(args_count, uint64_t);
        }
    }
    va_end(args_count);

    if(total_size > buf.len) { return (Bytes){NULL, 0}; }

    /* Second pass: pack values into buf. */
    va_list args;
    va_start(args, fmt);
    size_t offset = 0;

    for(const char *p = format_chars; *p;) {
        size_t count = 1;
        if(*p >= '0' && *p <= '9') {
            count = 0;
            while(*p >= '0' && *p <= '9') { count = count * 10 + (size_t)(*p++ - '0'); }
            if(!*p) { break; }
        }
        char c = *p++;
        if(c == 'x') {
            memset(buf.data + offset, 0, count);
            offset += count;
        } else if(c == '*') {
            if(!*p) { break; }
            char elem = *p++;
            size_t dyn_count = va_arg(args, size_t);
            if(elem == 'x') {
                memset(buf.data + offset, 0, dyn_count);
                offset += dyn_count;
            } else {
                void *arr = va_arg(args, void *);
                if(arr) {
                    for(size_t i = 0; i < dyn_count; i++) {
                        uint64_t val = read_array_elem(arr, i, elem);
                        offset += pack_value(buf.data + offset, val, NULL, elem, byte_order);
                    }
                } else {
                    offset += dyn_count * elem_size_for(elem);
                }
            }
        } else if(c == 'z') {
            const char *str = va_arg(args, const char *);
            size_t written = pack_value(buf.data + offset, 0, str, 'z', byte_order);
            offset += written;
        } else {
            for(size_t i = 0; i < count; i++) {
                uint64_t val;
                if(c == 'f') {
                    double d = va_arg(args, double);
                    float f = (float)d;
                    uint32_t bits;
                    memcpy(&bits, &f, 4);
                    val = bits;
                } else if(c == 'd') {
                    double d = va_arg(args, double);
                    memcpy(&val, &d, 8);
                } else {
                    val = va_arg(args, uint64_t);
                }
                offset += pack_value(buf.data + offset, val, NULL, c, byte_order);
            }
        }
    }
    va_end(args);

    return bytes_slice(buf, offset, buf.len - offset);
}


/* ============================================================================
 * bytes_unpack
 * ============================================================================ */

Bytes bytes_unpack(Bytes data, const char *fmt, ...) {
    if(!fmt || !*fmt || !data.data) { return (Bytes){NULL, 0}; }

    int byte_order = get_byte_order(fmt);
    const char *format_chars = skip_byte_order(fmt);

    /* Compute expected minimum size. */
    size_t expected_size = 0;
    for(const char *p = format_chars; *p;) {
        size_t count = 1;
        if(*p >= '0' && *p <= '9') {
            count = 0;
            while(*p >= '0' && *p <= '9') { count = count * 10 + (size_t)(*p++ - '0'); }
        }
        char c = *p++;
        if(c == 'x') {
            expected_size += count;
        } else if(c == 'b' || c == 'B') {
            expected_size += count;
        } else if(c == 'h' || c == 'H') {
            expected_size += count * 2;
        } else if(c == 'i' || c == 'I' || c == 'f') {
            expected_size += count * 4;
        } else if(c == 'q' || c == 'Q' || c == 'd') {
            expected_size += count * 8;
        } else if(c == '*') {
            if(*p) { p++; } /* skip elem type; dynamic size */
        }
    }

    if(data.len < expected_size) { return (Bytes){NULL, 0}; }

    va_list args;
    va_start(args, fmt);
    size_t offset = 0;

    for(const char *p = format_chars; *p;) {
        size_t count = 1;
        if(*p >= '0' && *p <= '9') {
            count = 0;
            while(*p >= '0' && *p <= '9') { count = count * 10 + (size_t)(*p++ - '0'); }
        }
        char c = *p++;

        if(c == 'x') {
            offset += count;
            continue;
        }

        if(c == '*') {
            /* Dynamic array: next format char is element type. */
            if(!*p) { break; }
            char elem = *p++;
            size_t dyn_count = va_arg(args, size_t);
            if(elem == 'x') {
                /* *x: skip dyn_count bytes, no output arg */
                if(offset + dyn_count > data.len) {
                    va_end(args);
                    return (Bytes){NULL, 0};
                }
                offset += dyn_count;
            } else {
                void *arr = va_arg(args, void *);
                size_t esz = elem_size_for(elem);
                if(offset + dyn_count * esz > data.len) {
                    va_end(args);
                    return (Bytes){NULL, 0};
                }
                if(arr) {
                    for(size_t i = 0; i < dyn_count; i++) {
                        uint64_t val = unpack_value(data.data + offset, elem, byte_order);
                        write_array_elem(arr, i, elem, val);
                        offset += esz;
                    }
                } else {
                    offset += dyn_count * esz;
                }
            }
            continue;
        }

        for(size_t i = 0; i < count; i++) {
            void *ptr = va_arg(args, void *);
            uint64_t val = unpack_value(data.data + offset, c, byte_order);

            if(c == 'b') {
                *(int8_t *)ptr = (int8_t)val;
                offset += 1;
            } else if(c == 'B') {
                *(uint8_t *)ptr = (uint8_t)val;
                offset += 1;
            } else if(c == 'h') {
                *(int16_t *)ptr = (int16_t)val;
                offset += 2;
            } else if(c == 'H') {
                *(uint16_t *)ptr = (uint16_t)val;
                offset += 2;
            } else if(c == 'i') {
                *(int32_t *)ptr = (int32_t)val;
                offset += 4;
            } else if(c == 'I') {
                *(uint32_t *)ptr = (uint32_t)val;
                offset += 4;
            } else if(c == 'q') {
                *(int64_t *)ptr = (int64_t)val;
                offset += 8;
            } else if(c == 'Q') {
                *(uint64_t *)ptr = val;
                offset += 8;
            } else if(c == 'f') {
                float fval;
                uint32_t bits = (uint32_t)val;
                memcpy(&fval, &bits, 4);
                *(float *)ptr = fval;
                offset += 4;
            } else if(c == 'd') {
                double dval;
                memcpy(&dval, &val, 8);
                *(double *)ptr = dval;
                offset += 8;
            }
        }
    }
    va_end(args);

    return bytes_slice(data, offset, data.len - offset);
}


/* ============================================================================
 * Slicing
 * ============================================================================ */

Bytes bytes_from_buf(const uint8_t *buf, size_t len) { return (Bytes){(uint8_t *)buf, len}; }


Bytes bytes_slice(Bytes b, size_t offset, size_t len) {
    if(!b.data || offset + len > b.len) { return (Bytes){NULL, 0}; }
    return (Bytes){b.data + offset, len};
}


Bytes bytes_pad_even(Arena *a, Bytes b) {
    if(b.len % 2 == 0) { return b; }
    Bytes pad = bytes_alloc(a, b.len + 1);
    if(bytes_is_null(pad)) { return (Bytes){NULL, 0}; }
    memcpy(pad.data, b.data, b.len);
    pad.data[b.len] = 0x00;
    return pad;
}


/* ============================================================================
 * Debug
 * ============================================================================ */

void bytes_hexdump(Bytes b, const char *label) {
    fprintf(stderr, "\n%s (%zu bytes):\n", label, b.len);
    for(size_t i = 0; i < b.len; i += 16) {
        fprintf(stderr, "  ");
        size_t chunk_size = (b.len - i < 16) ? (b.len - i) : 16;
        for(size_t j = 0; j < chunk_size; j++) {
            fprintf(stderr, "%02X", b.data[i + j]);
            if(j < chunk_size - 1) { fprintf(stderr, " "); }
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}
