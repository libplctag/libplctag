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
 * Struct pack / unpack  (Python struct module style)
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
 *
 * Example: bytes_pack(arena, ">HHHb", txn_id, proto_id, length, unit_id)
 * ============================================================================ */

/* Pack values into an arena-allocated Bytes.  Returns {NULL,0} on OOM. */
extern Bytes bytes_pack(Arena *a, const char *fmt, ...);

/*
 * Pack values into an existing Bytes buffer.
 * Returns the remaining (unfilled) slice, or {NULL,0} if buf is too small.
 * Same format string as bytes_pack(); no arena needed.
 */
extern Bytes bytes_pack_into(Bytes buf, const char *fmt, ...);

/*
 * Unpack values from data according to fmt.
 * Output pointers are passed as variadic args in format order.
 * Returns a Bytes slice of remaining unread data, or {NULL,0} on error.
 */
extern Bytes bytes_unpack(Bytes data, const char *fmt, ...);

/* ============================================================================
 * Slicing (no allocation)
 * ============================================================================ */

/* Wrap a raw pointer + length into a Bytes (no allocation). */
extern Bytes bytes_from_buf(const uint8_t *buf, size_t len);

/* Sub-slice starting at offset with given len.  Returns {NULL,0} if out of range. */
extern Bytes bytes_slice(Bytes b, size_t offset, size_t len);

/* Pad to even length by appending one zero byte if len is odd. */
extern Bytes bytes_pad_even(Arena *a, Bytes b);

/* ============================================================================
 * Debug
 * ============================================================================ */

/* Print hex dump to stdout with label. */
extern void bytes_hexdump(Bytes b, const char *label);

#ifdef __cplusplus
}
#endif
