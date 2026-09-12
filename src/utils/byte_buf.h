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
 * A length-carrying view over a byte buffer, passed by value.
 *
 * The length is signed.  A negative length means the buffer carries an error
 * instead of data: the length is the PLCTAG_ERR_* code and the data pointer is
 * a static message describing where it happened.  Every operation here checks
 * for that first and passes it through untouched, so a chain of encodes or
 * decodes needs one check at the end rather than one per field.
 *
 *     byte_buf rem = byte_buf_encode_uint16_le(out, command);
 *     rem = byte_buf_encode_uint16_le(rem, length);
 *     rem = byte_buf_encode_uint32_le(rem, session);
 *     if(byte_buf_has_err(rem)) { return (int32_t)byte_buf_get_err(rem); }
 *
 * Decoding returns the data that is left.  Encoding returns the space that is
 * left.  Either way the returned buffer is what the next call operates on.
 *
 * CAUTION: byte_buf_len() on an error buffer returns the error code, and
 * byte_buf_data() returns the message rather than bytes.  Check
 * byte_buf_has_err() before doing arithmetic on a length that might be one.
 *
 * Splitting exists for the encode direction, where an outer header cannot be
 * written until the payload it describes has been built:
 *
 *     byte_buf header = byte_buf_split_front(out, EIP_HEADER_SIZE);
 *     byte_buf body = byte_buf_split_back(out, EIP_HEADER_SIZE);
 *     byte_buf rem = encode_payload(body, ...);
 *     intptr_t payload_len = byte_buf_written(body, rem);
 *     ... now fill header, which knows payload_len ...
 *
 * Decoding needs no such thing; it reads in order.
 */

#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>


/*
 * NOTE: intptr_t is used for the length because it is signed and the same width
 * as a pointer, so it can hold any real buffer size and still leave the whole
 * negative range for error codes.
 */
typedef struct {
    intptr_t len;
    uint8_t *data;
} byte_buf;


/*********************************************************************
 ** Construction and inspection
 *********************************************************************/

inline static byte_buf byte_buf_make(const uint8_t *data, intptr_t len) {
    return (byte_buf){.len = len, .data = (uint8_t *)(intptr_t)data};
}


/* err must be negative; PLCTAG_ERR_* all are. */
inline static byte_buf byte_buf_make_err(const char *msg, intptr_t err) { return byte_buf_make((const uint8_t *)msg, err); }


inline static bool byte_buf_has_err(byte_buf bb) { return bb.len < 0; }


inline static intptr_t byte_buf_len(byte_buf bb) { return bb.len; }


inline static uint8_t *byte_buf_data(byte_buf bb) { return bb.data; }


inline static intptr_t byte_buf_get_err(byte_buf bb) { return bb.len; }


inline static const char *byte_buf_get_err_msg(byte_buf bb) { return (const char *)bb.data; }


inline static bool byte_buf_in_bounds(byte_buf bb, intptr_t index) {
    return !byte_buf_has_err(bb) && index >= 0 && index < bb.len;
}


/*
 * Is there room for count bytes starting at offset?  Written as a subtraction
 * so that offset + count cannot overflow.
 */
inline static bool byte_buf_has_room(byte_buf bb, intptr_t offset, intptr_t count) {
    if(byte_buf_has_err(bb) || offset < 0 || count < 0) { return false; }

    return (bb.len - offset) >= count;
}


/*********************************************************************
 ** Sub-buffers
 *********************************************************************/

/*
 * The sub-buffer of len bytes starting at start, clamped to what is actually
 * there.  An error buffer passes straight through.
 */
inline static byte_buf byte_buf_slice(byte_buf src, intptr_t start, intptr_t len) {
    intptr_t actual_start = 0;
    intptr_t actual_len = 0;

    if(byte_buf_has_err(src)) { return src; }

    if(start < 0) { return byte_buf_make_err("byte_buf_slice: negative start", PLCTAG_ERR_OUT_OF_BOUNDS); }

    actual_start = (start > src.len) ? src.len : start;
    actual_len = (len > (src.len - actual_start)) ? (src.len - actual_start) : len;

    if(actual_len < 0) { actual_len = 0; }

    return (byte_buf){.len = actual_len, .data = &(src.data[actual_start])};
}


/*
 * The two halves of a split, as separate calls so that neither needs a struct
 * to carry them.  split_front is everything before split_index; split_back is
 * split_index to the end.
 *
 * This is how an outer header gets written last: reserve its space with
 * split_front, build the payload into split_back, then fill the header once the
 * payload length is known.
 *
 * Unlike byte_buf_slice() these do not clamp.  A split_index past the end is an
 * error, because a reservation that silently came back short would then be
 * filled in past its end.
 */
inline static byte_buf byte_buf_split_front(byte_buf src, intptr_t split_index) {
    if(byte_buf_has_err(src)) { return src; }

    if(!byte_buf_has_room(src, 0, split_index)) {
        return byte_buf_make_err("byte_buf_split_front: split index past the end", PLCTAG_ERR_TOO_SMALL);
    }

    return (byte_buf){.len = split_index, .data = src.data};
}


inline static byte_buf byte_buf_split_back(byte_buf src, intptr_t split_index) {
    if(byte_buf_has_err(src)) { return src; }

    if(!byte_buf_has_room(src, 0, split_index)) {
        return byte_buf_make_err("byte_buf_split_back: split index past the end", PLCTAG_ERR_TOO_SMALL);
    }

    return (byte_buf){.len = src.len - split_index, .data = &(src.data[split_index])};
}


/*
 * How many bytes an encode chain consumed, given where it started and what it
 * returned.  Returns the error code if either end carries one.
 */
inline static intptr_t byte_buf_written(byte_buf start, byte_buf remaining) {
    if(byte_buf_has_err(start)) { return byte_buf_get_err(start); }
    if(byte_buf_has_err(remaining)) { return byte_buf_get_err(remaining); }

    return start.len - remaining.len;
}


/* The first len bytes, for handing a filled region on to whoever sends it. */
inline static byte_buf byte_buf_truncate(byte_buf src, intptr_t len) { return byte_buf_slice(src, 0, len); }


/*********************************************************************
 ** Peek: read without consuming
 *********************************************************************/

inline static bool byte_buf_peek_uint8(byte_buf bb, intptr_t offset, uint8_t *val) {
    if(!byte_buf_has_room(bb, offset, 1) || val == NULL) { return false; }

    *val = bb.data[offset];

    return true;
}


/*
 * The multi-byte peeks and pokes all funnel through these two so that the
 * bounds check and the byte order live in one place each.  count is 2, 4 or 8.
 */
inline static bool byte_buf_peek_uint_le(byte_buf bb, intptr_t offset, intptr_t count, uint64_t *val) {
    uint64_t acc = 0;

    if(!byte_buf_has_room(bb, offset, count) || val == NULL) { return false; }

    for(intptr_t i = count - 1; i >= 0; i--) { acc = (acc << 8) + (uint64_t)bb.data[offset + i]; }

    *val = acc;

    return true;
}


inline static bool byte_buf_peek_uint_be(byte_buf bb, intptr_t offset, intptr_t count, uint64_t *val) {
    uint64_t acc = 0;

    if(!byte_buf_has_room(bb, offset, count) || val == NULL) { return false; }

    for(intptr_t i = 0; i < count; i++) { acc = (acc << 8) + (uint64_t)bb.data[offset + i]; }

    *val = acc;

    return true;
}


#define BYTE_BUF_DEFINE_PEEK(WIDTH, BYTES, ORDER)                                                                \
    inline static bool byte_buf_peek_uint##WIDTH##_##ORDER(byte_buf bb, intptr_t offset, uint##WIDTH##_t *val) { \
        uint64_t wide = 0;                                                                                       \
                                                                                                                 \
        if(!byte_buf_peek_uint_##ORDER(bb, offset, (BYTES), &wide) || val == NULL) { return false; }             \
                                                                                                                 \
        *val = (uint##WIDTH##_t)wide;                                                                            \
                                                                                                                 \
        return true;                                                                                             \
    }

BYTE_BUF_DEFINE_PEEK(16, 2, le)
BYTE_BUF_DEFINE_PEEK(32, 4, le)
BYTE_BUF_DEFINE_PEEK(64, 8, le)
BYTE_BUF_DEFINE_PEEK(16, 2, be)
BYTE_BUF_DEFINE_PEEK(32, 4, be)
BYTE_BUF_DEFINE_PEEK(64, 8, be)

#undef BYTE_BUF_DEFINE_PEEK


/*********************************************************************
 ** Decode: read and consume, returning the data that is left
 *********************************************************************/

inline static byte_buf byte_buf_decode_uint8(byte_buf src, uint8_t *val) {
    if(byte_buf_has_err(src)) { return src; }

    if(!byte_buf_peek_uint8(src, 0, val)) {
        return byte_buf_make_err("byte_buf_decode_uint8: out of bounds", PLCTAG_ERR_OUT_OF_BOUNDS);
    }

    return byte_buf_slice(src, 1, src.len - 1);
}


/* Copy count bytes out.  Used for addresses, names and other runs of bytes. */
inline static byte_buf byte_buf_decode_bytes(byte_buf src, uint8_t *dest, intptr_t count) {
    if(byte_buf_has_err(src)) { return src; }

    if(dest == NULL || !byte_buf_has_room(src, 0, count)) {
        return byte_buf_make_err("byte_buf_decode_bytes: out of bounds", PLCTAG_ERR_OUT_OF_BOUNDS);
    }

    memcpy(dest, src.data, (size_t)count);

    return byte_buf_slice(src, count, src.len - count);
}


#define BYTE_BUF_DEFINE_DECODE(WIDTH, BYTES, ORDER)                                                                         \
    inline static byte_buf byte_buf_decode_uint##WIDTH##_##ORDER(byte_buf src, uint##WIDTH##_t *val) {                      \
        if(byte_buf_has_err(src)) { return src; }                                                                           \
                                                                                                                            \
        if(!byte_buf_peek_uint##WIDTH##_##ORDER(src, 0, val)) {                                                             \
            return byte_buf_make_err("byte_buf_decode_uint" #WIDTH "_" #ORDER ": out of bounds", PLCTAG_ERR_OUT_OF_BOUNDS); \
        }                                                                                                                   \
                                                                                                                            \
        return byte_buf_slice(src, (BYTES), src.len - (BYTES));                                                             \
    }

BYTE_BUF_DEFINE_DECODE(16, 2, le)
BYTE_BUF_DEFINE_DECODE(32, 4, le)
BYTE_BUF_DEFINE_DECODE(64, 8, le)
BYTE_BUF_DEFINE_DECODE(16, 2, be)
BYTE_BUF_DEFINE_DECODE(32, 4, be)
BYTE_BUF_DEFINE_DECODE(64, 8, be)

#undef BYTE_BUF_DEFINE_DECODE


/*********************************************************************
 ** Encode: write and consume, returning the space that is left
 *********************************************************************/

inline static byte_buf byte_buf_encode_uint8(byte_buf dest, uint8_t val) {
    if(byte_buf_has_err(dest)) { return dest; }

    if(!byte_buf_has_room(dest, 0, 1)) {
        return byte_buf_make_err("byte_buf_encode_uint8: out of bounds", PLCTAG_ERR_OUT_OF_BOUNDS);
    }

    dest.data[0] = val;

    return byte_buf_slice(dest, 1, dest.len - 1);
}


inline static byte_buf byte_buf_encode_bytes(byte_buf dest, const uint8_t *src, intptr_t count) {
    if(byte_buf_has_err(dest)) { return dest; }

    if(src == NULL || !byte_buf_has_room(dest, 0, count)) {
        return byte_buf_make_err("byte_buf_encode_bytes: out of bounds", PLCTAG_ERR_OUT_OF_BOUNDS);
    }

    memcpy(dest.data, src, (size_t)count);

    return byte_buf_slice(dest, count, dest.len - count);
}


inline static byte_buf byte_buf_encode_uint_le(byte_buf dest, uint64_t val, intptr_t count) {
    if(byte_buf_has_err(dest)) { return dest; }

    if(!byte_buf_has_room(dest, 0, count)) {
        return byte_buf_make_err("byte_buf_encode_uint_le: out of bounds", PLCTAG_ERR_OUT_OF_BOUNDS);
    }

    for(intptr_t i = 0; i < count; i++) { dest.data[i] = (uint8_t)((val >> (8 * i)) & 0xFF); }

    return byte_buf_slice(dest, count, dest.len - count);
}


inline static byte_buf byte_buf_encode_uint_be(byte_buf dest, uint64_t val, intptr_t count) {
    if(byte_buf_has_err(dest)) { return dest; }

    if(!byte_buf_has_room(dest, 0, count)) {
        return byte_buf_make_err("byte_buf_encode_uint_be: out of bounds", PLCTAG_ERR_OUT_OF_BOUNDS);
    }

    for(intptr_t i = 0; i < count; i++) { dest.data[i] = (uint8_t)((val >> (8 * (count - 1 - i))) & 0xFF); }

    return byte_buf_slice(dest, count, dest.len - count);
}


#define BYTE_BUF_DEFINE_ENCODE(WIDTH, BYTES, ORDER)                                                    \
    inline static byte_buf byte_buf_encode_uint##WIDTH##_##ORDER(byte_buf dest, uint##WIDTH##_t val) { \
        return byte_buf_encode_uint_##ORDER(dest, (uint64_t)val, (BYTES));                             \
    }

BYTE_BUF_DEFINE_ENCODE(16, 2, le)
BYTE_BUF_DEFINE_ENCODE(32, 4, le)
BYTE_BUF_DEFINE_ENCODE(64, 8, le)
BYTE_BUF_DEFINE_ENCODE(16, 2, be)
BYTE_BUF_DEFINE_ENCODE(32, 4, be)
BYTE_BUF_DEFINE_ENCODE(64, 8, be)

#undef BYTE_BUF_DEFINE_ENCODE
