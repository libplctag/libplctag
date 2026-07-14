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

#include <string.h>
#include <utils/cbor.h>

/* Major types (RFC 8949 §3), shifted into the initial byte's top 3 bits. */
#define CBOR_MAJOR_UINT  (0u << 5)
#define CBOR_MAJOR_NEGINT (1u << 5)
#define CBOR_MAJOR_BYTES (2u << 5)
#define CBOR_MAJOR_TEXT  (3u << 5)
#define CBOR_MAJOR_ARRAY (4u << 5)
#define CBOR_MAJOR_MAP   (5u << 5)

/* Additional-info thresholds: values 0-23 encode directly in the initial
 * byte; 24/25/26/27 select a following 1/2/4/8-byte big-endian length. */
#define CBOR_AI_1BYTE ((uint8_t)24)
#define CBOR_AI_2BYTE ((uint8_t)25)
#define CBOR_AI_4BYTE ((uint8_t)26)
#define CBOR_AI_8BYTE ((uint8_t)27)

/* Bytes needed for major-type-tagged val: 1 (direct), 2, 3, 5, or 9. */
static size_t cbor_size_header(uint64_t val) {
    if(val <= 23) { return 1; }
    if(val <= 0xFFu) { return 2; }
    if(val <= 0xFFFFu) { return 3; }
    if(val <= 0xFFFFFFFFu) { return 5; }
    return 9;
}

/* Write one major-type-tagged header (initial byte + optional length bytes,
 * big-endian) at *pos.  Bounds-checked; advances *pos on success. */
static bool cbor_write_header(Bytes dest, size_t *pos, uint8_t major, uint64_t val) {
    size_t need = cbor_size_header(val);
    if(*pos + need > dest.len) { return false; }

    uint8_t *p = dest.data + *pos;

    if(val <= 23) {
        p[0] = (uint8_t)(major | (uint8_t)val);
    } else if(val <= 0xFFu) {
        p[0] = (uint8_t)(major | CBOR_AI_1BYTE);
        p[1] = (uint8_t)val;
    } else if(val <= 0xFFFFu) {
        p[0] = (uint8_t)(major | CBOR_AI_2BYTE);
        p[1] = (uint8_t)(val >> 8);
        p[2] = (uint8_t)val;
    } else if(val <= 0xFFFFFFFFu) {
        p[0] = (uint8_t)(major | CBOR_AI_4BYTE);
        p[1] = (uint8_t)(val >> 24);
        p[2] = (uint8_t)(val >> 16);
        p[3] = (uint8_t)(val >> 8);
        p[4] = (uint8_t)val;
    } else {
        p[0] = (uint8_t)(major | CBOR_AI_8BYTE);
        p[1] = (uint8_t)(val >> 56);
        p[2] = (uint8_t)(val >> 48);
        p[3] = (uint8_t)(val >> 40);
        p[4] = (uint8_t)(val >> 32);
        p[5] = (uint8_t)(val >> 24);
        p[6] = (uint8_t)(val >> 16);
        p[7] = (uint8_t)(val >> 8);
        p[8] = (uint8_t)val;
    }

    *pos += need;
    return true;
}

size_t cbor_size_uint(uint64_t val) { return cbor_size_header(val); }

size_t cbor_size_int(int64_t val) {
    /* Negative int major type encodes (-1 - val); magnitude for header
     * sizing is that transformed value, not abs(val). */
    if(val >= 0) { return cbor_size_header((uint64_t)val); }
    return cbor_size_header((uint64_t)(-1 - val));
}

size_t cbor_size_text(size_t len) { return cbor_size_header((uint64_t)len) + len; }

size_t cbor_size_bytes(size_t len) { return cbor_size_header((uint64_t)len) + len; }

size_t cbor_size_array_header(size_t count) { return cbor_size_header((uint64_t)count); }

size_t cbor_size_map_header(size_t count) { return cbor_size_header((uint64_t)count); }

bool cbor_write_uint(Bytes dest, size_t *pos, uint64_t val) { return cbor_write_header(dest, pos, CBOR_MAJOR_UINT, val); }

bool cbor_write_int(Bytes dest, size_t *pos, int64_t val) {
    if(val >= 0) { return cbor_write_header(dest, pos, CBOR_MAJOR_UINT, (uint64_t)val); }
    return cbor_write_header(dest, pos, CBOR_MAJOR_NEGINT, (uint64_t)(-1 - val));
}

bool cbor_write_text(Bytes dest, size_t *pos, const char *str, size_t len) {
    size_t start = *pos;
    if(!cbor_write_header(dest, pos, CBOR_MAJOR_TEXT, (uint64_t)len)) { return false; }
    if(*pos + len > dest.len) {
        *pos = start;
        return false;
    }
    memcpy(dest.data + *pos, str, len);
    *pos += len;
    return true;
}

bool cbor_write_bytes(Bytes dest, size_t *pos, const uint8_t *data, size_t len) {
    size_t start = *pos;
    if(!cbor_write_header(dest, pos, CBOR_MAJOR_BYTES, (uint64_t)len)) { return false; }
    if(*pos + len > dest.len) {
        *pos = start;
        return false;
    }
    memcpy(dest.data + *pos, data, len);
    *pos += len;
    return true;
}

bool cbor_write_array_header(Bytes dest, size_t *pos, size_t count) {
    return cbor_write_header(dest, pos, CBOR_MAJOR_ARRAY, (uint64_t)count);
}

bool cbor_write_map_header(Bytes dest, size_t *pos, size_t count) {
    return cbor_write_header(dest, pos, CBOR_MAJOR_MAP, (uint64_t)count);
}
