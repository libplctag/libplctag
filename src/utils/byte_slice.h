#pragma once

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
 * Bounds-checked views into a byte buffer, for encoding and decoding wire data.
 *
 * Every reader, writer and skip takes a slice and returns the remainder, so calls chain
 * and the error check happens once at the end:
 *
 *     src = byte_slice_read_u16_le(src, &my_uint16);
 *     src = byte_slice_read_u32_be(src, &my_uint32);
 *     if(byte_slice_has_error(src)) { ... }
 *
 * Contract:
 *
 *   - data is the parent buffer and never changes.  Child slices differ only in start and
 *     len, so slices from the same buffer can be checked against each other.
 *   - An error slice keeps data and start; len holds the PLCTAG_ERR_* code.  start is the
 *     offset of the operation that failed.
 *   - Every function passes an error slice through unchanged and writes nothing.
 *   - Readers zero *out before anything else, so a value read in a failed chain is 0.  Check
 *     the slice before using a read value for anything other than another slice call.
 *   - Errors:
 *       PLCTAG_ERR_TOO_SMALL  the slice ran out: a read, write, skip, take or cut needed more
 *                             bytes than were left.  The caller knows whether the chain was
 *                             reading input or writing output.
 *       PLCTAG_ERR_TOO_LARGE  bytes left over where byte_slice_end() expected none.
 *       PLCTAG_ERR_BAD_PARAM  a negative length, or a remainder from another slice.
 *   - Slices are function-local.  Never store one in a long-lived struct: the parent buffer
 *     may be reallocated.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <libplctag/api/libplctag.h>


typedef struct {
    uint8_t *data; /* parent buffer, never changed by any operation */
    int32_t start; /* offset into data; on error, where the failure happened */
    int32_t len;   /* bytes in the slice; negative is a PLCTAG_ERR_* code */
} byte_slice_t;


inline static byte_slice_t byte_slice_fail(byte_slice_t s, int32_t err) {
    s.len = err;
    return s;
}

inline static byte_slice_t byte_slice_make(uint8_t *data, int32_t len) {
    byte_slice_t s = {.data = data, .start = 0, .len = len};

    if(len < 0) { return byte_slice_fail(s, PLCTAG_ERR_BAD_PARAM); }

    if(data == NULL && len > 0) { return byte_slice_fail(s, PLCTAG_ERR_NULL_PTR); }

    return s;
}

inline static bool byte_slice_has_error(byte_slice_t s) { return (s.len < 0); }

inline static int32_t byte_slice_get_error(byte_slice_t s) { return (s.len < 0) ? s.len : PLCTAG_STATUS_OK; }

inline static int32_t byte_slice_len(byte_slice_t s) { return (s.len < 0) ? 0 : s.len; }

/* pointer to the first byte of the slice, NULL on error */
inline static uint8_t *byte_slice_get_data(byte_slice_t s) { return (s.len < 0) ? NULL : s.data + s.start; }


/* Cutting */

inline static byte_slice_t byte_slice_skip(byte_slice_t s, int32_t n) {
    if(byte_slice_has_error(s)) { return s; }

    if(n < 0) { return byte_slice_fail(s, PLCTAG_ERR_BAD_PARAM); }

    if(n > s.len) { return byte_slice_fail(s, PLCTAG_ERR_TOO_SMALL); }

    s.start += n;
    s.len -= n;

    return s;
}

/*
 * The next n bytes of s as *field; returns the remainder after them.  For a length-prefixed
 * field in a read chain, or for reserving a header in a write chain and filling it in later.
 * On error *field gets the error slice too.
 */
inline static byte_slice_t byte_slice_take(byte_slice_t s, int32_t n, byte_slice_t *field) {
    byte_slice_t rest = byte_slice_skip(s, n);

    *field = rest;

    if(!byte_slice_has_error(rest)) {
        field->start = s.start;
        field->len = n;
    }

    return rest;
}

/*
 * The end of a read chain: s unchanged if it is empty, PLCTAG_ERR_TOO_LARGE if bytes are left
 * over, or the chain's error.  So one check covers both input too short and input too long.
 */
inline static byte_slice_t byte_slice_end(byte_slice_t s) {
    if(byte_slice_has_error(s) || s.len == 0) { return s; }

    return byte_slice_fail(s, PLCTAG_ERR_TOO_LARGE);
}

/*
 * Split src at offset at, for starting chains on each half.  Either out may be NULL.  On
 * error both outs get the error slice.  Returns PLCTAG_STATUS_OK or the error code.
 */
inline static int32_t byte_slice_cut(byte_slice_t src, int32_t at, byte_slice_t *out_left, byte_slice_t *out_right) {
    byte_slice_t right = byte_slice_skip(src, at);
    byte_slice_t left = right;

    if(!byte_slice_has_error(right)) {
        left = src;
        left.len = at;
    }

    if(out_left) { *out_left = left; }

    if(out_right) { *out_right = right; }

    return byte_slice_get_error(right);
}

/*
 * The part of src before remainder: what a chain of writes produced or a chain of reads
 * consumed.  remainder must come from src, so the same data and a start inside src.
 */
inline static byte_slice_t byte_slice_used(byte_slice_t src, byte_slice_t remainder) {
    if(byte_slice_has_error(src)) { return src; }

    if(byte_slice_has_error(remainder)) { return remainder; }

    if(remainder.data != src.data || remainder.start < src.start || remainder.start > src.start + src.len) {
        return byte_slice_fail(src, PLCTAG_ERR_BAD_PARAM);
    }

    src.len = remainder.start - src.start;

    return src;
}

/*
 * memcmp order over at most max_len bytes; within that, a shorter slice sorts first.
 * Exact match: max_len = INT32_MAX.  Prefix match: max_len = byte_slice_len(prefix).
 * An error slice never compares equal.
 */
inline static int32_t byte_slice_compare(byte_slice_t a, byte_slice_t b, int32_t max_len) {
    int32_t a_len = a.len;
    int32_t b_len = b.len;
    int32_t n = 0;
    int32_t rc = 0;

    if(byte_slice_has_error(a)) { return -1; }

    if(byte_slice_has_error(b)) { return 1; }

    if(a_len > max_len) { a_len = max_len; }

    if(b_len > max_len) { b_len = max_len; }

    n = (a_len < b_len) ? a_len : b_len;

    if(n > 0) { rc = (int32_t)memcmp(a.data + a.start, b.data + b.start, (size_t)n); }

    if(rc != 0) { return (rc < 0) ? -1 : 1; }

    if(a_len == b_len) { return 0; }

    return (a_len < b_len) ? -1 : 1;
}


/* Readers.  Each returns the remainder after the value. */

inline static byte_slice_t byte_slice_read_uint(byte_slice_t s, int32_t n, bool big_endian, uint64_t *out) {
    *out = 0;

    if(byte_slice_has_error(s)) { return s; }

    if(n > s.len) { return byte_slice_fail(s, PLCTAG_ERR_TOO_SMALL); }

    for(int32_t i = 0; i < n; i++) {
        int32_t index = big_endian ? i : (n - 1 - i);

        *out = (*out << 8) | (uint64_t)s.data[s.start + index];
    }

    return byte_slice_skip(s, n);
}

inline static byte_slice_t byte_slice_read_u8(byte_slice_t s, uint8_t *out) {
    uint64_t val = 0;
    s = byte_slice_read_uint(s, 1, false, &val);
    *out = (uint8_t)val;
    return s;
}

inline static byte_slice_t byte_slice_read_u16_le(byte_slice_t s, uint16_t *out) {
    uint64_t val = 0;
    s = byte_slice_read_uint(s, 2, false, &val);
    *out = (uint16_t)val;
    return s;
}

inline static byte_slice_t byte_slice_read_u16_be(byte_slice_t s, uint16_t *out) {
    uint64_t val = 0;
    s = byte_slice_read_uint(s, 2, true, &val);
    *out = (uint16_t)val;
    return s;
}

inline static byte_slice_t byte_slice_read_u32_le(byte_slice_t s, uint32_t *out) {
    uint64_t val = 0;
    s = byte_slice_read_uint(s, 4, false, &val);
    *out = (uint32_t)val;
    return s;
}

inline static byte_slice_t byte_slice_read_u32_be(byte_slice_t s, uint32_t *out) {
    uint64_t val = 0;
    s = byte_slice_read_uint(s, 4, true, &val);
    *out = (uint32_t)val;
    return s;
}

inline static byte_slice_t byte_slice_read_u64_le(byte_slice_t s, uint64_t *out) {
    return byte_slice_read_uint(s, 8, false, out);
}

inline static byte_slice_t byte_slice_read_u64_be(byte_slice_t s, uint64_t *out) { return byte_slice_read_uint(s, 8, true, out); }

/* copies exactly n bytes out */
inline static byte_slice_t byte_slice_read_bytes(byte_slice_t s, uint8_t *out, int32_t n) {
    if(byte_slice_has_error(s)) { return s; }

    if(n < 0) { return byte_slice_fail(s, PLCTAG_ERR_BAD_PARAM); }

    if(n > s.len) { return byte_slice_fail(s, PLCTAG_ERR_TOO_SMALL); }

    if(n > 0) { memcpy(out, s.data + s.start, (size_t)n); }

    return byte_slice_skip(s, n);
}


/* Writers.  Each returns the remaining space after the value. */

inline static byte_slice_t byte_slice_write_uint(byte_slice_t s, int32_t n, bool big_endian, uint64_t val) {
    if(byte_slice_has_error(s)) { return s; }

    if(n > s.len) { return byte_slice_fail(s, PLCTAG_ERR_TOO_SMALL); }

    for(int32_t i = 0; i < n; i++) {
        int32_t index = big_endian ? (n - 1 - i) : i;

        s.data[s.start + index] = (uint8_t)(val & 0xFF);
        val >>= 8;
    }

    return byte_slice_skip(s, n);
}

inline static byte_slice_t byte_slice_write_u8(byte_slice_t s, uint8_t val) { return byte_slice_write_uint(s, 1, false, val); }

inline static byte_slice_t byte_slice_write_u16_le(byte_slice_t s, uint16_t val) {
    return byte_slice_write_uint(s, 2, false, val);
}

inline static byte_slice_t byte_slice_write_u16_be(byte_slice_t s, uint16_t val) {
    return byte_slice_write_uint(s, 2, true, val);
}

inline static byte_slice_t byte_slice_write_u32_le(byte_slice_t s, uint32_t val) {
    return byte_slice_write_uint(s, 4, false, val);
}

inline static byte_slice_t byte_slice_write_u32_be(byte_slice_t s, uint32_t val) {
    return byte_slice_write_uint(s, 4, true, val);
}

inline static byte_slice_t byte_slice_write_u64_le(byte_slice_t s, uint64_t val) {
    return byte_slice_write_uint(s, 8, false, val);
}

inline static byte_slice_t byte_slice_write_u64_be(byte_slice_t s, uint64_t val) {
    return byte_slice_write_uint(s, 8, true, val);
}

inline static byte_slice_t byte_slice_write_bytes(byte_slice_t s, const uint8_t *src, int32_t n) {
    if(byte_slice_has_error(s)) { return s; }

    if(n < 0) { return byte_slice_fail(s, PLCTAG_ERR_BAD_PARAM); }

    if(n > s.len) { return byte_slice_fail(s, PLCTAG_ERR_TOO_SMALL); }

    if(n > 0) { memcpy(s.data + s.start, src, (size_t)n); }

    return byte_slice_skip(s, n);
}

/*
 * Move s to offset 0 of its parent buffer, for keeping leftover input before the next read.
 * Overwrites data[0 .. s.start), so every other slice of the buffer is stale afterward.
 */
inline static byte_slice_t byte_slice_compress(byte_slice_t s) {
    if(byte_slice_has_error(s)) { return s; }

    if(s.start > 0) { memmove(s.data, s.data + s.start, (size_t)s.len); }

    s.start = 0;

    return s;
}
