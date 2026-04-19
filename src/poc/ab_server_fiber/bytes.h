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
 * Python-like bytes object: (pointer, length) pair backed by arena allocation.
 * All operations that need memory take an Arena* and allocate from it.
 *
 * Copied from ~/Projects/data_table and modified:
 *   - Added bytes_alloc(), bytes_zero(), bytes_is_null().
 *   - bytes_pack() returns {NULL,0} if arena is full; callers must check.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"

/* ============================================================================
 * Core type
 * ============================================================================ */

typedef struct {
    uint8_t *data;
    size_t len;
} Bytes;

/* ============================================================================
 * New helpers added for modbus_server3
 * ============================================================================ */

/* Allocate len uninitialized bytes from arena.  Returns {NULL,0} on OOM. */
extern Bytes bytes_alloc(Arena *a, size_t len);

/* Zero all bytes spanned by b.  No-op if b.data is NULL. */
extern void bytes_zero(Bytes b);

/* True when b.data is NULL. */
static inline bool bytes_is_null(Bytes b) { return b.data == NULL; }

/* Return the filled portion of original given the unfilled remainder.
 * Typical use: Bytes packet = bytes_filled(buf, rest_after_pack_into); */
static inline Bytes bytes_filled(Bytes original, Bytes rest) { return (Bytes){original.data, original.len - rest.len}; }

/* ============================================================================
 * Concatenation
 * ============================================================================ */

/* Concatenate count Bytes values (variadic).  All args must be Bytes by value.
 * Use the bytes_concat() macro instead of calling bytes_concat_impl() directly. */
extern Bytes bytes_concat_impl(Arena *a, int count, ...);

/* Count helper — do not call directly. */
#define BYTES_NARGS_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N
#define BYTES_NARGS(...) BYTES_NARGS_(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)

/* bytes_concat(arena, b1, b2, ...) — count is generated automatically. */
#define bytes_concat(a_, ...) bytes_concat_impl((a_), (int)BYTES_NARGS(__VA_ARGS__), __VA_ARGS__)

/* ============================================================================
 * Type-safe pack (C11 _Generic dispatch)
 *
 * BytesEndian selects byte order.  Each value is tagged at compile time via
 * BYTES_TYPE_OF/_Generic; the implementation does a single pass with no
 * format-string parsing.
 *
 * Scalar values are passed directly.  Typed arrays use BYTES_ARRAY() for
 * per-element endian conversion.  Raw byte blobs use a Bytes struct (no
 * endian conversion).
 *
 * Usage:
 *   bytes_pack(arena, BYTES_LE, (uint8_t)cmd, (uint16_t)len, someBytes)
 *   bytes_pack(arena, BYTES_LE, (uint8_t)cmd, BYTES_ARRAY(my_u32_arr, 5))
 *   bytes_pack_into(buf, BYTES_BE, (uint32_t)val)
 * ============================================================================ */

typedef enum {
    BYTES_LE =  1,
    BYTES_BE = -1,
} BytesEndian;

typedef enum {
    BYTES_TYPE_END   = 0,
    BYTES_TYPE_U8,
    BYTES_TYPE_U16,
    BYTES_TYPE_U32,
    BYTES_TYPE_U64,
    BYTES_TYPE_I8,
    BYTES_TYPE_I16,
    BYTES_TYPE_I32,
    BYTES_TYPE_I64,
    BYTES_TYPE_F32,       /* float  — passed as double via vararg promotion */
    BYTES_TYPE_F64,       /* double */
    BYTES_TYPE_BYTES,     /* Bytes struct — raw memcpy, no endian conversion */
    BYTES_TYPE_ARRAY,     /* BytesArray struct — per-element endian conversion */
    BYTES_TYPE_SKIP,      /* BytesSkip* — zero-fill (pack) or advance (unpack) N bytes */
} BytesPackType;

/*
 * Typed array descriptor for BYTES_TYPE_ARRAY.
 * elem_type must be one of the scalar tags (U8..F64).
 */
typedef struct {
    void         *data;
    size_t        count;
    BytesPackType elem_type;
} BytesArray;

/*
 * Skip descriptor for BYTES_TYPE_SKIP.  Always passed as BytesSkip* so that
 * BYTES_SKIP(n) works identically in bytes_pack and bytes_unpack.
 * Pack: writes n zero bytes.  Unpack: advances past n bytes.
 */
typedef struct { size_t count; } BytesSkip;

/*
 * Resolve a C expression to its BytesPackType tag at compile time.
 * Unrecognised types fall through to BYTES_TYPE_BYTES (raw copy).
 */
#define BYTES_TYPE_OF(x) _Generic((x),    \
    uint8_t:     BYTES_TYPE_U8,           \
    uint16_t:    BYTES_TYPE_U16,          \
    uint32_t:    BYTES_TYPE_U32,          \
    uint64_t:    BYTES_TYPE_U64,          \
    int8_t:      BYTES_TYPE_I8,           \
    int16_t:     BYTES_TYPE_I16,          \
    int32_t:     BYTES_TYPE_I32,          \
    int64_t:     BYTES_TYPE_I64,          \
    float:       BYTES_TYPE_F32,          \
    double:      BYTES_TYPE_F64,          \
    Bytes:       BYTES_TYPE_BYTES,        \
    BytesArray*: BYTES_TYPE_ARRAY,        \
    BytesSkip*:  BYTES_TYPE_SKIP,         \
    default:     BYTES_TYPE_BYTES         \
)

/* Expand one user argument to a (type-tag, value) pair. */
#define BYTES_WRAP(x)  (int)BYTES_TYPE_OF(x), (x)

/*
 * Zero-fill (pack) or skip (unpack) n bytes inline.
 * Yields a BytesSkip* (pointer to a compound literal); lifetime spans the call.
 * Works identically in bytes_pack and bytes_unpack.
 *
 * Example: bytes_pack(a, BYTES_LE, (uint8_t)cmd, BYTES_SKIP(2), (uint16_t)len)
 *          bytes_unpack(data, BYTES_LE, &cmd, BYTES_SKIP(2), &len)
 */
#define BYTES_SKIP(n_)  (&(BytesSkip){(n_)})

/*
 * Wrap any typed array pointer + element count into a BytesArray.
 * Element type is derived from the pointer type at compile time.
 * Endian conversion is applied per-element in write_typed_args.
 */
#define BYTES_ARRAY(ptr_, count_) \
    (&(BytesArray){ \
        .data      = (void *)(ptr_), \
        .count     = (count_), \
        .elem_type = BYTES_TYPE_OF(*(ptr_)) \
    })

/* Count up to 32 arguments. */
#define BYTES_NARGS32_(_1,_2,_3,_4,_5,_6,_7,_8,          \
                       _9,_10,_11,_12,_13,_14,_15,_16,    \
                       _17,_18,_19,_20,_21,_22,_23,_24,   \
                       _25,_26,_27,_28,_29,_30,_31,_32,N,...) N
#define BYTES_NARGS32(...) \
    BYTES_NARGS32_(__VA_ARGS__,                            \
        32,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,  \
        16,15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)

#define BYTES_FOREACH_1(_1)       BYTES_WRAP(_1)
#define BYTES_FOREACH_2(_1,...)   BYTES_WRAP(_1), BYTES_FOREACH_1(__VA_ARGS__)
#define BYTES_FOREACH_3(_1,...)   BYTES_WRAP(_1), BYTES_FOREACH_2(__VA_ARGS__)
#define BYTES_FOREACH_4(_1,...)   BYTES_WRAP(_1), BYTES_FOREACH_3(__VA_ARGS__)
#define BYTES_FOREACH_5(_1,...)   BYTES_WRAP(_1), BYTES_FOREACH_4(__VA_ARGS__)
#define BYTES_FOREACH_6(_1,...)   BYTES_WRAP(_1), BYTES_FOREACH_5(__VA_ARGS__)
#define BYTES_FOREACH_7(_1,...)   BYTES_WRAP(_1), BYTES_FOREACH_6(__VA_ARGS__)
#define BYTES_FOREACH_8(_1,...)   BYTES_WRAP(_1), BYTES_FOREACH_7(__VA_ARGS__)
#define BYTES_FOREACH_9(_1,...)   BYTES_WRAP(_1), BYTES_FOREACH_8(__VA_ARGS__)
#define BYTES_FOREACH_10(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_9(__VA_ARGS__)
#define BYTES_FOREACH_11(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_10(__VA_ARGS__)
#define BYTES_FOREACH_12(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_11(__VA_ARGS__)
#define BYTES_FOREACH_13(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_12(__VA_ARGS__)
#define BYTES_FOREACH_14(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_13(__VA_ARGS__)
#define BYTES_FOREACH_15(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_14(__VA_ARGS__)
#define BYTES_FOREACH_16(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_15(__VA_ARGS__)
#define BYTES_FOREACH_17(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_16(__VA_ARGS__)
#define BYTES_FOREACH_18(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_17(__VA_ARGS__)
#define BYTES_FOREACH_19(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_18(__VA_ARGS__)
#define BYTES_FOREACH_20(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_19(__VA_ARGS__)
#define BYTES_FOREACH_21(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_20(__VA_ARGS__)
#define BYTES_FOREACH_22(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_21(__VA_ARGS__)
#define BYTES_FOREACH_23(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_22(__VA_ARGS__)
#define BYTES_FOREACH_24(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_23(__VA_ARGS__)
#define BYTES_FOREACH_25(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_24(__VA_ARGS__)
#define BYTES_FOREACH_26(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_25(__VA_ARGS__)
#define BYTES_FOREACH_27(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_26(__VA_ARGS__)
#define BYTES_FOREACH_28(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_27(__VA_ARGS__)
#define BYTES_FOREACH_29(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_28(__VA_ARGS__)
#define BYTES_FOREACH_30(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_29(__VA_ARGS__)
#define BYTES_FOREACH_31(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_30(__VA_ARGS__)
#define BYTES_FOREACH_32(_1,...)  BYTES_WRAP(_1), BYTES_FOREACH_31(__VA_ARGS__)

#define BYTES_FOREACH_CAT_(a,b)  a##b
#define BYTES_FOREACH_CAT(a,b)   BYTES_FOREACH_CAT_(a,b)
#define BYTES_FOREACH(...)  BYTES_FOREACH_CAT(BYTES_FOREACH_, BYTES_NARGS32(__VA_ARGS__))(__VA_ARGS__)

/* Implementation functions — call via macros below, not directly. */
extern Bytes bytes_pack_impl(Arena *a, int endian, ...);
extern Bytes bytes_pack_into_impl(Bytes buf, int endian, ...);

/*
 * Type-safe pack into a fresh arena allocation.  Returns {NULL,0} on OOM.
 */
#define bytes_pack(a_, endian_, ...) \
    bytes_pack_impl((a_), (int)(endian_), BYTES_FOREACH(__VA_ARGS__), (int)BYTES_TYPE_END)

/*
 * Type-safe pack into an existing Bytes buffer.
 * Returns the remaining unfilled slice; use bytes_filled(buf, rest) to recover what was written.
 */
#define bytes_pack_into(buf_, endian_, ...) \
    bytes_pack_into_impl((buf_), (int)(endian_), BYTES_FOREACH(__VA_ARGS__), (int)BYTES_TYPE_END)

/* ============================================================================
 * Format-string pack / unpack  (Python struct module style, old API)
 *
 * Byte-order prefix: "<" little-endian, ">" big-endian, "=" / "@" native.
 * Format chars:
 *   b/B  int8/uint8     (1 byte)
 *   h/H  int16/uint16   (2 bytes)
 *   i/I  int32/uint32   (4 bytes)
 *   q/Q  int64/uint64   (8 bytes)
 *   f    float          (4 bytes)
 *   d    double         (8 bytes)
 *   z    C-string (const char*) — raw bytes, no length prefix, NUL not written
 *   x    padding zero byte (count prefix supported, e.g. "8x")
 *   *T   dynamic array — next two args are (size_t count, T *array);
 *        writes/reads count elements of type T (b/B/h/H/i/I/q/Q/f/d).
 *   *x   dynamic zero padding — next arg is (size_t count); writes count
 *        zero bytes with no array pointer argument.
 * ============================================================================ */

#if 0
/* Pack values into an arena-allocated Bytes.  Returns {NULL,0} on OOM. */
extern Bytes bytes_pack_fmt(Arena *a, const char *fmt, ...);

/*
 * Pack values into an existing Bytes buffer.
 * Returns the remaining (unfilled) slice, or {NULL,0} if buf is too small.
 */
extern Bytes bytes_pack_into_fmt(Bytes buf, const char *fmt, ...);
#endif

/* ============================================================================
 * Type-safe unpack (C11 _Generic dispatch)
 *
 * Each output argument must be a typed pointer: uint16_t*, uint32_t*, etc.
 * For Bytes*: pre-set ptr->len; a zero-copy slice is assigned to ptr->data.
 * For BytesArray*: pre-set count, elem_type, and data (pre-allocated storage).
 * For padding bytes: call bytes_skip() before the next bytes_unpack call.
 *
 * Usage:
 *   uint16_t cmd; uint32_t session; uint64_t ctx;
 *   Bytes rest = bytes_unpack(data, BYTES_LE, &cmd, &session, &ctx);
 * ============================================================================ */

/*
 * Resolve an output pointer to its BytesPackType tag at compile time.
 */
#define BYTES_OUT_TYPE_OF(ptr_) _Generic((ptr_),  \
    uint8_t*:    BYTES_TYPE_U8,                   \
    uint16_t*:   BYTES_TYPE_U16,                  \
    uint32_t*:   BYTES_TYPE_U32,                  \
    uint64_t*:   BYTES_TYPE_U64,                  \
    int8_t*:     BYTES_TYPE_I8,                   \
    int16_t*:    BYTES_TYPE_I16,                  \
    int32_t*:    BYTES_TYPE_I32,                  \
    int64_t*:    BYTES_TYPE_I64,                  \
    float*:      BYTES_TYPE_F32,                  \
    double*:     BYTES_TYPE_F64,                  \
    Bytes*:      BYTES_TYPE_BYTES,                \
    BytesArray*: BYTES_TYPE_ARRAY,                \
    BytesSkip*:  BYTES_TYPE_SKIP,                 \
    default:     BYTES_TYPE_BYTES                 \
)

/* Expand one output pointer to a (type-tag, void*) pair. */
#define BYTES_UNWRAP(ptr_)  (int)BYTES_OUT_TYPE_OF(ptr_), (void *)(ptr_)

#define BYTES_FOREACH_OUT_1(_1)       BYTES_UNWRAP(_1)
#define BYTES_FOREACH_OUT_2(_1,...)   BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_1(__VA_ARGS__)
#define BYTES_FOREACH_OUT_3(_1,...)   BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_2(__VA_ARGS__)
#define BYTES_FOREACH_OUT_4(_1,...)   BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_3(__VA_ARGS__)
#define BYTES_FOREACH_OUT_5(_1,...)   BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_4(__VA_ARGS__)
#define BYTES_FOREACH_OUT_6(_1,...)   BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_5(__VA_ARGS__)
#define BYTES_FOREACH_OUT_7(_1,...)   BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_6(__VA_ARGS__)
#define BYTES_FOREACH_OUT_8(_1,...)   BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_7(__VA_ARGS__)
#define BYTES_FOREACH_OUT_9(_1,...)   BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_8(__VA_ARGS__)
#define BYTES_FOREACH_OUT_10(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_9(__VA_ARGS__)
#define BYTES_FOREACH_OUT_11(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_10(__VA_ARGS__)
#define BYTES_FOREACH_OUT_12(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_11(__VA_ARGS__)
#define BYTES_FOREACH_OUT_13(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_12(__VA_ARGS__)
#define BYTES_FOREACH_OUT_14(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_13(__VA_ARGS__)
#define BYTES_FOREACH_OUT_15(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_14(__VA_ARGS__)
#define BYTES_FOREACH_OUT_16(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_15(__VA_ARGS__)
#define BYTES_FOREACH_OUT_17(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_16(__VA_ARGS__)
#define BYTES_FOREACH_OUT_18(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_17(__VA_ARGS__)
#define BYTES_FOREACH_OUT_19(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_18(__VA_ARGS__)
#define BYTES_FOREACH_OUT_20(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_19(__VA_ARGS__)
#define BYTES_FOREACH_OUT_21(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_20(__VA_ARGS__)
#define BYTES_FOREACH_OUT_22(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_21(__VA_ARGS__)
#define BYTES_FOREACH_OUT_23(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_22(__VA_ARGS__)
#define BYTES_FOREACH_OUT_24(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_23(__VA_ARGS__)
#define BYTES_FOREACH_OUT_25(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_24(__VA_ARGS__)
#define BYTES_FOREACH_OUT_26(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_25(__VA_ARGS__)
#define BYTES_FOREACH_OUT_27(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_26(__VA_ARGS__)
#define BYTES_FOREACH_OUT_28(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_27(__VA_ARGS__)
#define BYTES_FOREACH_OUT_29(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_28(__VA_ARGS__)
#define BYTES_FOREACH_OUT_30(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_29(__VA_ARGS__)
#define BYTES_FOREACH_OUT_31(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_30(__VA_ARGS__)
#define BYTES_FOREACH_OUT_32(_1,...)  BYTES_UNWRAP(_1), BYTES_FOREACH_OUT_31(__VA_ARGS__)

#define BYTES_FOREACH_OUT(...)  BYTES_FOREACH_CAT(BYTES_FOREACH_OUT_, BYTES_NARGS32(__VA_ARGS__))(__VA_ARGS__)

/* Implementation function — call via macro below, not directly. */
extern Bytes bytes_unpack_impl(Bytes data, int endian, ...);

/*
 * Type-safe unpack from a Bytes source.
 * Returns the remaining unread slice, or {NULL,0} on underflow.
 */
#define bytes_unpack(data_, endian_, ...) \
    bytes_unpack_impl((data_), (int)(endian_), BYTES_FOREACH_OUT(__VA_ARGS__), (int)BYTES_TYPE_END)

/* Format-string variant (old API). */
#if 0
extern Bytes bytes_unpack_fmt(Bytes data, const char *fmt, ...);
#endif

/* ============================================================================
 * Slicing (no allocation)
 * ============================================================================ */

/* Wrap a raw pointer + length into a Bytes (no allocation). */
extern Bytes bytes_from_buf(const uint8_t *buf, size_t len);

/* Sub-slice starting at offset with given len.  Returns {NULL,0} if out of range. */
extern Bytes bytes_slice(Bytes b, size_t offset, size_t len);

/* Pad to even length by appending one zero byte if len is odd. */
extern Bytes bytes_pad_even(Arena *a, Bytes b);

/* Advance past n bytes; returns remaining slice or {NULL,0} if out of range. */
static inline Bytes bytes_skip(Bytes b, size_t n) {
    if(n > b.len) { return (Bytes){NULL, 0}; }
    return bytes_slice(b, n, b.len - n);
}

/* ============================================================================
 * Debug
 * ============================================================================ */

/* Print hex dump to stdout with label. */
extern void bytes_hexdump(Bytes b, const char *label);

#ifdef __cplusplus
}
#endif
