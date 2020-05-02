/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h> /* for ssize_t */

#include <stdio.h>

typedef struct {
    ssize_t len;
    uint8_t *data;
} slice_s;

inline static slice_s slice_make(uint8_t *data, ssize_t len) { return (slice_s){ .len = len, .data = data }; }
inline static slice_s slice_make_err(ssize_t err) { return slice_make(NULL, err); }
inline static size_t slice_len(slice_s s) { return (size_t)s.len; }
inline static bool slice_in_bounds(slice_s s, size_t index) { if(index < (size_t)s.len) { return true; } else { return false; } }
inline static uint8_t slice_get_uint8(slice_s s, size_t index) { if(slice_in_bounds(s, index)) { return s.data[index]; } else { return UINT8_MAX; } }
inline static bool slice_set_uint8(slice_s s, size_t index, uint8_t val) { if(slice_in_bounds(s, index)) { s.data[index] = val; return true; } else { return false; } }
inline static uint8_t *slice_get_bytes(slice_s s, size_t index) {  if(slice_in_bounds(s, index)) { return &s.data[index];} else { return NULL;} }
inline static bool slice_has_err(slice_s s) { if(s.data == NULL) { return true; } else { return false; } }
inline static int slice_get_err(slice_s s) { return (int)(ssize_t)slice_len(s); }
inline static bool slice_match_bytes(slice_s s, const uint8_t *data, size_t data_len) {
    if(data_len > slice_len(s)) {
        /* fprintf(stderr, "lengths do not match! Slice has length %d and bytes have length %d!\n", (int)slice_len(s), (int)data_len); */
        return false;
    }
    for(size_t i=0; i < data_len; i++) {
        /* fprintf(stderr,"Comparing element %d, %x and %x\n", (int)i, (int)slice_get_uint8(s, (ssize_t)i), data[i]); */
        if(slice_get_uint8(s, i) != data[i]) {
            return false;
        }
    }
    return true;
}
inline static bool slice_match_string(slice_s s, const char *data) { return slice_match_bytes(s, (const uint8_t*)data, strlen(data)); }

inline static slice_s slice_from_slice(slice_s src, size_t start, size_t len) {
    ssize_t actual_start;
    ssize_t actual_len;

    if(slice_has_err(src)) {
        return src;
    }

    /* from here on we know that the src length is zero or positive. */

    if(start > (size_t)src.len) {
        actual_start = src.len;
    } else {
        actual_start = (ssize_t)start;
    }

    if((ssize_t)len > (src.len - actual_start)) {
        /* truncate the slice to fit. */
        actual_len = (src.len - actual_start);
    } else {
        actual_len = (ssize_t)len;
    }

    return (slice_s){ .len = actual_len, .data = &(src.data[actual_start]) };
}


/* helper functions to get and set data in a slice. */

inline static uint16_t slice_get_uint16_le(slice_s input_buf, size_t offset) {
    uint16_t res = 0;

    if(slice_in_bounds(input_buf, offset + 1)) {
        res = ((uint16_t)slice_get_uint8(input_buf, offset) + (uint16_t)(slice_get_uint8(input_buf, offset + 1) << 8));
    }

    return res;
}


inline static uint32_t slice_get_uint32_le(slice_s input_buf, size_t offset) {
    uint32_t res = 0;

    if(slice_in_bounds(input_buf, offset + 3)) {
        res =  (uint32_t)(slice_get_uint8(input_buf, offset))
             + (uint32_t)(slice_get_uint8(input_buf, offset + 1) << 8)
             + (uint32_t)(slice_get_uint8(input_buf, offset + 2) << 16)
             + (uint32_t)(slice_get_uint8(input_buf, offset + 3) << 24);
    }

    return res;
}


inline static uint64_t slice_get_uint64_le(slice_s input_buf, size_t offset) {
    uint64_t res = 0;

    if(slice_in_bounds(input_buf, offset + 7)) {
        res =  ((uint64_t)slice_get_uint8(input_buf, offset))
             + ((uint64_t)slice_get_uint8(input_buf, offset + 1) << 8)
             + ((uint64_t)slice_get_uint8(input_buf, offset + 2) << 16)
             + ((uint64_t)slice_get_uint8(input_buf, offset + 3) << 24)
             + ((uint64_t)slice_get_uint8(input_buf, offset + 4) << 32)
             + ((uint64_t)slice_get_uint8(input_buf, offset + 5) << 40)
             + ((uint64_t)slice_get_uint8(input_buf, offset + 6) << 48)
             + ((uint64_t)slice_get_uint8(input_buf, offset + 7) << 56);
    }

    return res;
}


/* FIXME - these probably should not just fail silently.  They are safe though. */

inline static void slice_set_uint16_le(slice_s output_buf, size_t offset, uint16_t val) {
    if(slice_in_bounds(output_buf, offset + 1)) {
        slice_set_uint8(output_buf, offset + 0, (uint8_t)(val & 0xFF));
        slice_set_uint8(output_buf, offset + 1, (uint8_t)((val >> 8) & 0xFF));
    }
}


inline static void slice_set_uint32_le(slice_s output_buf, size_t offset, uint32_t val) {
    if(slice_in_bounds(output_buf, offset + 3)) {
        slice_set_uint8(output_buf, offset + 0, (uint8_t)(val & 0xFF));
        slice_set_uint8(output_buf, offset + 1, (uint8_t)((val >> 8) & 0xFF));
        slice_set_uint8(output_buf, offset + 2, (uint8_t)((val >> 16) & 0xFF));
        slice_set_uint8(output_buf, offset + 3, (uint8_t)((val >> 24) & 0xFF));
    }
}


inline static void slice_set_uin64_le(slice_s output_buf, size_t offset, uint64_t val) {
    if(slice_in_bounds(output_buf, offset + 7)) {
        slice_set_uint8(output_buf, offset + 0, (uint8_t)(val & 0xFF));
        slice_set_uint8(output_buf, offset + 1, (uint8_t)((val >> 8) & 0xFF));
        slice_set_uint8(output_buf, offset + 2, (uint8_t)((val >> 16) & 0xFF));
        slice_set_uint8(output_buf, offset + 3, (uint8_t)((val >> 24) & 0xFF));
        slice_set_uint8(output_buf, offset + 4, (uint8_t)((val >> 32) & 0xFF));
        slice_set_uint8(output_buf, offset + 5, (uint8_t)((val >> 40) & 0xFF));
        slice_set_uint8(output_buf, offset + 6, (uint8_t)((val >> 48) & 0xFF));
        slice_set_uint8(output_buf, offset + 7, (uint8_t)((val >> 56) & 0xFF));
    }
}

