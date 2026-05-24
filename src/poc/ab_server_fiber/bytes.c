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

#if 0

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


#endif /* helpers only needed by _fmt API */

/* ============================================================================
 * Type-safe pack implementation
 * ============================================================================ */

static size_t write_typed_args(uint8_t *dst, size_t cap, int endian, va_list args);

Bytes bytes_pack_impl(Arena *a, int endian, ...) {
    uint8_t *dst = arena_current(a);
    size_t   cap = arena_remaining(a);

    va_list args;
    va_start(args, endian);
    size_t written = write_typed_args(dst, cap, endian, args);
    va_end(args);

    if(written == SIZE_MAX) { return (Bytes){NULL, 0}; }
    arena_commit(a, written);
    return (Bytes){dst, written};
}


Bytes bytes_pack_into_impl(Bytes buf, int endian, ...) {
    if(!buf.data) { return (Bytes){NULL, 0}; }

    va_list args;
    va_start(args, endian);
    size_t written = write_typed_args(buf.data, buf.len, endian, args);
    va_end(args);

    if(written == SIZE_MAX) { return (Bytes){NULL, 0}; }
    return bytes_slice(buf, written, buf.len - written);
}


#if 0
/* ============================================================================
 * bytes_pack_fmt (old format-string API)
 * ============================================================================ */

Bytes bytes_pack_fmt(Arena *a, const char *fmt, ...) {
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
 * bytes_pack_into_fmt (old format-string API)
 * ============================================================================ */

Bytes bytes_pack_into_fmt(Bytes buf, const char *fmt, ...) {
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
 * bytes_unpack_fmt (old format-string API)
 * ============================================================================ */

Bytes bytes_unpack_fmt(Bytes data, const char *fmt, ...) {
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
#endif


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
 * Type-safe unpack implementation
 * ============================================================================ */

static size_t read_typed_args(const uint8_t *src, size_t len, int endian, va_list args);

Bytes bytes_unpack_impl(Bytes data, int endian, ...) {
    if(!data.data) { return (Bytes){NULL, 0}; }

    va_list args;
    va_start(args, endian);
    size_t consumed = read_typed_args(data.data, data.len, endian, args);
    va_end(args);

    if(consumed == SIZE_MAX) { return (Bytes){NULL, 0}; }
    return bytes_slice(data, consumed, data.len - consumed);
}


/* ============================================================================
 * write_typed_args — single-pass typed writer for bytes_pack_impl
 * ============================================================================ */

static size_t pack_type_elem_size(BytesPackType t) {
    switch(t) {
        case BYTES_TYPE_U8:
        case BYTES_TYPE_I8:  return 1;
        case BYTES_TYPE_U16:
        case BYTES_TYPE_I16: return 2;
        case BYTES_TYPE_U32:
        case BYTES_TYPE_I32:
        case BYTES_TYPE_F32: return 4;
        case BYTES_TYPE_U64:
        case BYTES_TYPE_I64:
        case BYTES_TYPE_F64: return 8;
        default:             return 0;
    }
}

/* Host-value → wire-byte-order helpers.  Use memcpy to transfer to/from buffers. */

static inline uint16_t u16h_to_le(uint16_t v) {
    uint16_t r; uint8_t *b = (uint8_t *)&r;
    b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8);
    return r;
}
static inline uint16_t u16h_to_be(uint16_t v) {
    uint16_t r; uint8_t *b = (uint8_t *)&r;
    b[0]=(uint8_t)(v>>8); b[1]=(uint8_t)v;
    return r;
}
static inline uint32_t u32h_to_le(uint32_t v) {
    uint32_t r; uint8_t *b = (uint8_t *)&r;
    b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8); b[2]=(uint8_t)(v>>16); b[3]=(uint8_t)(v>>24);
    return r;
}
static inline uint32_t u32h_to_be(uint32_t v) {
    uint32_t r; uint8_t *b = (uint8_t *)&r;
    b[0]=(uint8_t)(v>>24); b[1]=(uint8_t)(v>>16); b[2]=(uint8_t)(v>>8); b[3]=(uint8_t)v;
    return r;
}
static inline uint64_t u64h_to_le(uint64_t v) {
    uint64_t r; uint8_t *b = (uint8_t *)&r;
    b[0]=(uint8_t)v;      b[1]=(uint8_t)(v>>8);  b[2]=(uint8_t)(v>>16); b[3]=(uint8_t)(v>>24);
    b[4]=(uint8_t)(v>>32); b[5]=(uint8_t)(v>>40); b[6]=(uint8_t)(v>>48); b[7]=(uint8_t)(v>>56);
    return r;
}
static inline uint64_t u64h_to_be(uint64_t v) {
    uint64_t r; uint8_t *b = (uint8_t *)&r;
    b[0]=(uint8_t)(v>>56); b[1]=(uint8_t)(v>>48); b[2]=(uint8_t)(v>>40); b[3]=(uint8_t)(v>>32);
    b[4]=(uint8_t)(v>>24); b[5]=(uint8_t)(v>>16); b[6]=(uint8_t)(v>>8);  b[7]=(uint8_t)v;
    return r;
}

static inline uint16_t u16le_to_host(uint16_t v) {
    const uint8_t *b = (const uint8_t *)&v;
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1]<<8));
}
static inline uint16_t u16be_to_host(uint16_t v) {
    const uint8_t *b = (const uint8_t *)&v;
    return (uint16_t)(((uint16_t)b[0]<<8) | (uint16_t)b[1]);
}
static inline uint32_t u32le_to_host(uint32_t v) {
    const uint8_t *b = (const uint8_t *)&v;
    return (uint32_t)b[0] | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
}
static inline uint32_t u32be_to_host(uint32_t v) {
    const uint8_t *b = (const uint8_t *)&v;
    return ((uint32_t)b[0]<<24) | ((uint32_t)b[1]<<16) | ((uint32_t)b[2]<<8) | (uint32_t)b[3];
}
static inline uint64_t u64le_to_host(uint64_t v) {
    const uint8_t *b = (const uint8_t *)&v;
    return (uint64_t)b[0]       | ((uint64_t)b[1]<<8)  | ((uint64_t)b[2]<<16) | ((uint64_t)b[3]<<24)
         | ((uint64_t)b[4]<<32) | ((uint64_t)b[5]<<40) | ((uint64_t)b[6]<<48) | ((uint64_t)b[7]<<56);
}
static inline uint64_t u64be_to_host(uint64_t v) {
    const uint8_t *b = (const uint8_t *)&v;
    return ((uint64_t)b[0]<<56) | ((uint64_t)b[1]<<48) | ((uint64_t)b[2]<<40) | ((uint64_t)b[3]<<32)
         | ((uint64_t)b[4]<<24) | ((uint64_t)b[5]<<16) | ((uint64_t)b[6]<<8)  | (uint64_t)b[7];
}


static size_t write_typed_args(uint8_t *dst, size_t cap, int endian, va_list args) {
    size_t off = 0;

    for(;;) {
        int tag = va_arg(args, int);
        if(tag == (int)BYTES_TYPE_END) { break; }

        switch((BytesPackType)tag) {
            case BYTES_TYPE_U8: {
                if(off + 1 > cap) { return SIZE_MAX; }
                dst[off++] = (uint8_t)va_arg(args, unsigned int);
                break;
            }
            case BYTES_TYPE_I8: {
                if(off + 1 > cap) { return SIZE_MAX; }
                dst[off++] = (uint8_t)(int8_t)va_arg(args, int);
                break;
            }
            case BYTES_TYPE_U16:
            case BYTES_TYPE_I16: {
                if(off + 2 > cap) { return SIZE_MAX; }
                uint16_t v = (uint16_t)va_arg(args, unsigned int);
                if(endian == (int)BYTES_LE) {
                    dst[off]   = (uint8_t) v;
                    dst[off+1] = (uint8_t)(v >> 8);
                } else {
                    dst[off]   = (uint8_t)(v >> 8);
                    dst[off+1] = (uint8_t) v;
                }
                off += 2;
                break;
            }
            case BYTES_TYPE_U32:
            case BYTES_TYPE_I32: {
                if(off + 4 > cap) { return SIZE_MAX; }
                uint32_t v = va_arg(args, uint32_t);
                if(endian == (int)BYTES_LE) {
                    dst[off]   = (uint8_t) v;
                    dst[off+1] = (uint8_t)(v >>  8);
                    dst[off+2] = (uint8_t)(v >> 16);
                    dst[off+3] = (uint8_t)(v >> 24);
                } else {
                    dst[off]   = (uint8_t)(v >> 24);
                    dst[off+1] = (uint8_t)(v >> 16);
                    dst[off+2] = (uint8_t)(v >>  8);
                    dst[off+3] = (uint8_t) v;
                }
                off += 4;
                break;
            }
            case BYTES_TYPE_U64:
            case BYTES_TYPE_I64: {
                if(off + 8 > cap) { return SIZE_MAX; }
                uint64_t v = va_arg(args, uint64_t);
                if(endian == (int)BYTES_LE) {
                    for(size_t i = 0; i < 8; i++) { dst[off + i] = (uint8_t)(v >> (i * 8)); }
                } else {
                    for(size_t i = 0; i < 8; i++) { dst[off + i] = (uint8_t)(v >> ((7 - i) * 8)); }
                }
                off += 8;
                break;
            }
            case BYTES_TYPE_F32: {
                if(off + 4 > cap) { return SIZE_MAX; }
                double   d = va_arg(args, double);
                float    f = (float)d;
                uint32_t bits;
                memcpy(&bits, &f, 4);
                if(endian == (int)BYTES_LE) {
                    dst[off]   = (uint8_t) bits;
                    dst[off+1] = (uint8_t)(bits >>  8);
                    dst[off+2] = (uint8_t)(bits >> 16);
                    dst[off+3] = (uint8_t)(bits >> 24);
                } else {
                    dst[off]   = (uint8_t)(bits >> 24);
                    dst[off+1] = (uint8_t)(bits >> 16);
                    dst[off+2] = (uint8_t)(bits >>  8);
                    dst[off+3] = (uint8_t) bits;
                }
                off += 4;
                break;
            }
            case BYTES_TYPE_F64: {
                if(off + 8 > cap) { return SIZE_MAX; }
                double   d = va_arg(args, double);
                uint64_t bits;
                memcpy(&bits, &d, 8);
                if(endian == (int)BYTES_LE) {
                    for(size_t i = 0; i < 8; i++) { dst[off + i] = (uint8_t)(bits >> (i * 8)); }
                } else {
                    for(size_t i = 0; i < 8; i++) { dst[off + i] = (uint8_t)(bits >> ((7 - i) * 8)); }
                }
                off += 8;
                break;
            }
            case BYTES_TYPE_BYTES: {
                Bytes b = va_arg(args, Bytes);
                if(b.data && b.len > 0) {
                    if(off + b.len > cap) { return SIZE_MAX; }
                    memcpy(dst + off, b.data, b.len);
                    off += b.len;
                }
                break;
            }
            case BYTES_TYPE_SKIP: {
                BytesSkip *s = va_arg(args, BytesSkip *);
                if(off + s->count > cap) { return SIZE_MAX; }
                memset(dst + off, 0, s->count);
                off += s->count;
                break;
            }
            case BYTES_TYPE_ARRAY: {
                BytesArray *ba = va_arg(args, BytesArray *);
                if(!ba->data || ba->count == 0) { break; }
                size_t esz = pack_type_elem_size(ba->elem_type);
                if(esz == 0 || off + ba->count * esz > cap) { return SIZE_MAX; }
                int le = (endian == (int)BYTES_LE);
                for(size_t i = 0; i < ba->count; i++) {
                    const uint8_t *elem = (const uint8_t *)ba->data + i * esz;
                    switch(ba->elem_type) {
                        case BYTES_TYPE_U8:
                        case BYTES_TYPE_I8:
                            dst[off] = *elem;
                            break;
                        case BYTES_TYPE_U16:
                        case BYTES_TYPE_I16: {
                            uint16_t t; memcpy(&t, elem, 2);
                            uint16_t w = le ? u16h_to_le(t) : u16h_to_be(t);
                            memcpy(dst + off, &w, 2);
                            break;
                        }
                        case BYTES_TYPE_U32:
                        case BYTES_TYPE_I32:
                        case BYTES_TYPE_F32: {
                            uint32_t t; memcpy(&t, elem, 4);
                            uint32_t w = le ? u32h_to_le(t) : u32h_to_be(t);
                            memcpy(dst + off, &w, 4);
                            break;
                        }
                        case BYTES_TYPE_U64:
                        case BYTES_TYPE_I64:
                        case BYTES_TYPE_F64: {
                            uint64_t t; memcpy(&t, elem, 8);
                            uint64_t w = le ? u64h_to_le(t) : u64h_to_be(t);
                            memcpy(dst + off, &w, 8);
                            break;
                        }
                        default: break;
                    }
                    off += esz;
                }
                break;
            }
            default: break;
        }
    }

    return off;
}


/* ============================================================================
 * read_typed_args — single-pass typed reader for bytes_unpack_impl
 * ============================================================================ */

static size_t read_typed_args(const uint8_t *src, size_t len, int endian, va_list args) {
    size_t off = 0;

    for(;;) {
        int   tag = va_arg(args, int);
        if(tag == (int)BYTES_TYPE_END) { break; }
        void *ptr = va_arg(args, void *);

        switch((BytesPackType)tag) {
            case BYTES_TYPE_U8: {
                if(off + 1 > len) { return SIZE_MAX; }
                *(uint8_t *)ptr = src[off++];
                break;
            }
            case BYTES_TYPE_I8: {
                if(off + 1 > len) { return SIZE_MAX; }
                *(int8_t *)ptr = (int8_t)src[off++];
                break;
            }
            case BYTES_TYPE_U16: {
                if(off + 2 > len) { return SIZE_MAX; }
                uint16_t v = (endian == (int)BYTES_LE)
                    ? (uint16_t)((uint16_t)src[off] | ((uint16_t)src[off+1] << 8))
                    : (uint16_t)(((uint16_t)src[off] << 8) | (uint16_t)src[off+1]);
                *(uint16_t *)ptr = v;
                off += 2;
                break;
            }
            case BYTES_TYPE_I16: {
                if(off + 2 > len) { return SIZE_MAX; }
                uint16_t v = (endian == (int)BYTES_LE)
                    ? (uint16_t)((uint16_t)src[off] | ((uint16_t)src[off+1] << 8))
                    : (uint16_t)(((uint16_t)src[off] << 8) | (uint16_t)src[off+1]);
                *(int16_t *)ptr = (int16_t)v;
                off += 2;
                break;
            }
            case BYTES_TYPE_U32: {
                if(off + 4 > len) { return SIZE_MAX; }
                uint32_t v = (endian == (int)BYTES_LE)
                    ? ((uint32_t)src[off] | ((uint32_t)src[off+1] << 8) | ((uint32_t)src[off+2] << 16) | ((uint32_t)src[off+3] << 24))
                    : (((uint32_t)src[off] << 24) | ((uint32_t)src[off+1] << 16) | ((uint32_t)src[off+2] << 8) | (uint32_t)src[off+3]);
                *(uint32_t *)ptr = v;
                off += 4;
                break;
            }
            case BYTES_TYPE_I32: {
                if(off + 4 > len) { return SIZE_MAX; }
                uint32_t v = (endian == (int)BYTES_LE)
                    ? ((uint32_t)src[off] | ((uint32_t)src[off+1] << 8) | ((uint32_t)src[off+2] << 16) | ((uint32_t)src[off+3] << 24))
                    : (((uint32_t)src[off] << 24) | ((uint32_t)src[off+1] << 16) | ((uint32_t)src[off+2] << 8) | (uint32_t)src[off+3]);
                *(int32_t *)ptr = (int32_t)v;
                off += 4;
                break;
            }
            case BYTES_TYPE_U64: {
                if(off + 8 > len) { return SIZE_MAX; }
                uint64_t v = 0;
                if(endian == (int)BYTES_LE) {
                    for(size_t i = 0; i < 8; i++) { v |= ((uint64_t)src[off + i] << (i * 8)); }
                } else {
                    for(size_t i = 0; i < 8; i++) { v |= ((uint64_t)src[off + i] << ((7 - i) * 8)); }
                }
                *(uint64_t *)ptr = v;
                off += 8;
                break;
            }
            case BYTES_TYPE_I64: {
                if(off + 8 > len) { return SIZE_MAX; }
                uint64_t v = 0;
                if(endian == (int)BYTES_LE) {
                    for(size_t i = 0; i < 8; i++) { v |= ((uint64_t)src[off + i] << (i * 8)); }
                } else {
                    for(size_t i = 0; i < 8; i++) { v |= ((uint64_t)src[off + i] << ((7 - i) * 8)); }
                }
                *(int64_t *)ptr = (int64_t)v;
                off += 8;
                break;
            }
            case BYTES_TYPE_F32: {
                if(off + 4 > len) { return SIZE_MAX; }
                uint32_t bits = (endian == (int)BYTES_LE)
                    ? ((uint32_t)src[off] | ((uint32_t)src[off+1] << 8) | ((uint32_t)src[off+2] << 16) | ((uint32_t)src[off+3] << 24))
                    : (((uint32_t)src[off] << 24) | ((uint32_t)src[off+1] << 16) | ((uint32_t)src[off+2] << 8) | (uint32_t)src[off+3]);
                memcpy(ptr, &bits, 4);
                off += 4;
                break;
            }
            case BYTES_TYPE_F64: {
                if(off + 8 > len) { return SIZE_MAX; }
                uint64_t bits = 0;
                if(endian == (int)BYTES_LE) {
                    for(size_t i = 0; i < 8; i++) { bits |= ((uint64_t)src[off + i] << (i * 8)); }
                } else {
                    for(size_t i = 0; i < 8; i++) { bits |= ((uint64_t)src[off + i] << ((7 - i) * 8)); }
                }
                memcpy(ptr, &bits, 8);
                off += 8;
                break;
            }
            case BYTES_TYPE_BYTES: {
                Bytes *bp = (Bytes *)ptr;
                if(off + bp->len > len) { return SIZE_MAX; }
                bp->data = (uint8_t *)src + off;
                off += bp->len;
                break;
            }
            case BYTES_TYPE_SKIP: {
                BytesSkip *s = (BytesSkip *)ptr;
                if(off + s->count > len) { return SIZE_MAX; }
                off += s->count;
                break;
            }
            case BYTES_TYPE_ARRAY: {
                BytesArray *bap = (BytesArray *)ptr;
                if(!bap->data || bap->count == 0) { break; }
                size_t esz = pack_type_elem_size(bap->elem_type);
                if(esz == 0 || off + bap->count * esz > len) { return SIZE_MAX; }
                int le = (endian == (int)BYTES_LE);
                for(size_t i = 0; i < bap->count; i++) {
                    const uint8_t *src_elem = src + off;
                    uint8_t *dst_elem = (uint8_t *)bap->data + i * esz;
                    switch(bap->elem_type) {
                        case BYTES_TYPE_U8:
                        case BYTES_TYPE_I8:
                            *dst_elem = *src_elem;
                            break;
                        case BYTES_TYPE_U16:
                        case BYTES_TYPE_I16: {
                            uint16_t t; memcpy(&t, src_elem, 2);
                            uint16_t h = le ? u16le_to_host(t) : u16be_to_host(t);
                            memcpy(dst_elem, &h, 2);
                            break;
                        }
                        case BYTES_TYPE_U32:
                        case BYTES_TYPE_I32:
                        case BYTES_TYPE_F32: {
                            uint32_t t; memcpy(&t, src_elem, 4);
                            uint32_t h = le ? u32le_to_host(t) : u32be_to_host(t);
                            memcpy(dst_elem, &h, 4);
                            break;
                        }
                        case BYTES_TYPE_U64:
                        case BYTES_TYPE_I64:
                        case BYTES_TYPE_F64: {
                            uint64_t t; memcpy(&t, src_elem, 8);
                            uint64_t h = le ? u64le_to_host(t) : u64be_to_host(t);
                            memcpy(dst_elem, &h, 8);
                            break;
                        }
                        default: break;
                    }
                    off += esz;
                }
                break;
            }
            default: break;
        }
    }

    return off;
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
