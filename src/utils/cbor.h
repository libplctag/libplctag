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
 * Minimal CBOR (RFC 8949) encoder: the subset the format/schema subsystem
 * needs (see ENIP-METADATA-AND-DISCOVERY-DESIGN.md §0) -- unsigned/negative
 * integers, byte strings, text strings, arrays, and maps. No decoder: every
 * caller today (built-in ENIP metadata schemas) only renders CBOR from raw
 * bytes; nothing yet needs to parse CBOR back into raw. Add cbor_read_*
 * alongside these when a writable structured tag needs set_formatted_data
 * round-tripping.
 *
 * Two matched families per item type:
 *   cbor_size_*()  - bytes the encoding would take, no buffer needed. Used to
 *                    answer plc_tag_get_formatted_data_size() without writing.
 *   cbor_write_*() - writes into dest starting at *pos, bounds-checked against
 *                    dest.len, advances *pos on success. Used to answer
 *                    plc_tag_get_formatted_data() into the caller's buffer.
 * A codec calls the size functions once to size the caller's buffer request,
 * then the matching write functions in the same order to fill it -- the two
 * families always agree because both derive from the same RFC 8949 header
 * width rules (0/1/2/4/8-byte length encodings by magnitude).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <utils/bytes.h>

/* ============================================================================
 * Size (no buffer, no bounds check -- pure function of the value/length)
 * ============================================================================ */

extern size_t cbor_size_uint(uint64_t val);
extern size_t cbor_size_int(int64_t val);
extern size_t cbor_size_text(size_t len);
extern size_t cbor_size_bytes(size_t len);
extern size_t cbor_size_array_header(size_t count);
extern size_t cbor_size_map_header(size_t count);

/* ============================================================================
 * Write (bounds-checked against dest.len; advances *pos on success)
 * ============================================================================ */

/* Major type 0 (unsigned int, 0 <= val). */
extern bool cbor_write_uint(Bytes dest, size_t *pos, uint64_t val);

/* Major type 0 or 1, selected by sign. */
extern bool cbor_write_int(Bytes dest, size_t *pos, int64_t val);

/* Major type 3 (UTF-8 text string): header + len bytes of str copied verbatim
 * (caller is responsible for str actually being valid UTF-8; CIP SHORT_STRING
 * product names are ASCII, a UTF-8 subset). */
extern bool cbor_write_text(Bytes dest, size_t *pos, const char *str, size_t len);

/* Major type 2 (byte string): header + len raw bytes. */
extern bool cbor_write_bytes(Bytes dest, size_t *pos, const uint8_t *data, size_t len);

/* Major type 4 header (definite-length array of count items); caller writes
 * the count items with subsequent cbor_write_* calls. */
extern bool cbor_write_array_header(Bytes dest, size_t *pos, size_t count);

/* Major type 5 header (definite-length map of count key/value pairs); caller
 * writes 2*count items (key, value, key, value, ...) with subsequent calls. */
extern bool cbor_write_map_header(Bytes dest, size_t *pos, size_t count);
