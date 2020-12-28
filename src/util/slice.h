/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include <util/debug.h>
#include <util/string.h>

typedef struct {
    int len;
    uint8_t *data;
} slice_t;


inline static slice_t slice_make(uint8_t *data, int len) {
    slice_t s;

    s.len = len;
    s.data = data;

    return s;
}

inline static slice_t slice_make_err(int err)
{
    return slice_make(NULL, err);
}

inline static int slice_len(slice_t s)
{
    return s.len;
}

inline uint8_t *slice_data(slice_t s)
{
    return s.data;
}

inline static int slice_in_bounds(slice_t s, int index)
{
    return (index < s.len) && (index >= 0) && (s.len >= 0);
}

inline static int slice_has_err(slice_t s)
{
    return (s.data == NULL);
}

inline static int slice_get_err(slice_t s)
{
    return s.len;
}

/* check whether in bounds or not before calling! */
inline static uint8_t slice_get_byte(slice_t s, int index)
{
    if(slice_in_bounds(s, index)) {
        return s.data[index];
    } else {
        return 0;
    }
}


inline static int slice_set_byte(slice_t s, int index, uint8_t val) {
    if(slice_in_bounds(s, index)) {
        s.data[index] = val;
        return PLCTAG_STATUS_OK;
    } else {
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }
}



inline static int slice_match_bytes(slice_t s, const uint8_t *data, int data_len)
{
    if(data_len != slice_len(s)) {
        return 0;
    }

    for(int i=0; i < data_len; i++) {
        if(slice_get_byte(s, i) != data[i]) {
            return 0;
        }
    }

    return 1;
}

inline static int slice_match_string(slice_t s, const char *data)
{
    return slice_match_bytes(s, (const uint8_t*)data, str_length(data));
}

inline static slice_t slice_from_slice(slice_t src, int start, int len) {
    int actual_start;
    int actual_len;
    slice_t res;

    if(slice_has_err(src)) {
        return src;
    }

    if(start < 0) {
        pdebug(DEBUG_WARN, "Start index less than zero!");
        return slice_make_err(PLCTAG_ERR_OUT_OF_BOUNDS);
    }

    if(len < 0) {
        pdebug(DEBUG_WARN, "Length less than zero!");
        return slice_make_err(PLCTAG_ERR_OUT_OF_BOUNDS);
    }

    if(start > src.len) {
        actual_start = src.len;
    } else {
        actual_start = (int)start;
    }

    if((int)len > (src.len - actual_start)) {
        /* truncate the slice to fit. */
        actual_len = (src.len - actual_start);
    } else {
        actual_len = (int)len;
    }

    res.data = &(src.data[actual_start]);
    res.len = actual_len;

    return res;
}


/* helper functions to get and set data in a slice. */

// inline static uint16_t slice_get_uint16_le(slice_t input_buf, int offset, int *rc)
// {
//     uint16_t res = 0;

//     *rc = PLCTAG_STATUS_OK;
//     if(slice_in_bounds(input_buf, offset + 1)) {
//         res = (uint16_t)(slice_get_uint8(input_buf, offset, rc) + (slice_get_uint8(input_buf, offset + 1, rc) << 8));
//         return res;
//     } else {
//         *rc = PLCTAG_ERR_OUT_OF_BOUNDS;
//         return 0;
//     }
// }

// inline static int slice_set_uint16_le(slice_t input_buf, int offset, uint16_t val)
// {
//     if(slice_in_bounds(input_buf, offset + 1)) {
//         slice_set_uint8(input_buf, offset, (uint8_t)(val & 0xFF));
//         slice_set_uint8(input_buf, offset + 1, (uint8_t)((val >> 8) & 0xFF));

//         return PLCTAG_STATUS_OK;
//     } else {
//         return PLCTAG_ERR_OUT_OF_BOUNDS;
//     }
// }


// inline static uint32_t slice_get_uint32_le(slice_t input_buf, int offset, int *rc)
// {
//     uint32_t res = 0;

//     *rc = PLCTAG_STATUS_OK;

//     if(slice_in_bounds(input_buf, offset + 3)) {
//         res =  (uint32_t)(slice_get_uint8(input_buf, offset))
//              + (uint32_t)(slice_get_uint8(input_buf, offset + 1) << 8)
//              + (uint32_t)(slice_get_uint8(input_buf, offset + 2) << 16)
//              + (uint32_t)(slice_get_uint8(input_buf, offset + 3) << 24);
//     } else {
//         *rc = PLCTAG_ERR_OUT_OF_BOUNDS
//         res = 0;
//     }

//     return res;
// }

// inline static int slice_set_uint32_t_le(slice_t output_buf, int offset, uint32_t val)
// {
//     if(slice_in_bounds(input_buf, offset + 3)) {
//         slice_set_uint8(input_buf, offset,     (uint8_t)(val & 0xFF));
//         slice_set_uint8(input_buf, offset + 1, (uint8_t)((val >> 8)  & 0xFF));
//         slice_set_uint8(input_buf, offset + 2, (uint8_t)((val >> 16) & 0xFF));
//         slice_set_uint8(input_buf, offset + 3, (uint8_t)((val >> 24) & 0xFF));

//         return PLCTAG_STATUS_OK;
//     } else {
//         return PLCTAG_ERR_OUT_OF_BOUNDS;
//     }
// }


// inline static uint64_t slice_get_uint64_le(slice_t input_buf, size_t offset, int *rc) {
//     uint64_t res = 0;

//     *rc = PLCTAG_STATUS_OK;

//     if(slice_in_bounds(input_buf, offset + 7)) {
//         res =  ((uint64_t)slice_get_uint8(input_buf, offset,     rc))
//              + ((uint64_t)slice_get_uint8(input_buf, offset + 1, rc) << 8)
//              + ((uint64_t)slice_get_uint8(input_buf, offset + 2, rc) << 16)
//              + ((uint64_t)slice_get_uint8(input_buf, offset + 3, rc) << 24)
//              + ((uint64_t)slice_get_uint8(input_buf, offset + 4, rc) << 32)
//              + ((uint64_t)slice_get_uint8(input_buf, offset + 5, rc) << 40)
//              + ((uint64_t)slice_get_uint8(input_buf, offset + 6, rc) << 48)
//              + ((uint64_t)slice_get_uint8(input_buf, offset + 7, rc) << 56);
//     } else {
//         *rc = PLCTAG_ERR_OUT_OF_BOUNDS;
//         res = 0;
//     }

//     return res;
// }

// inline static int slice_set_uint64_t_le(slice_t output_buf, int offset, uint64_t val)
// {
//     if(slice_in_bounds(input_buf, offset + 7)) {
//         slice_set_uint8(input_buf, offset,     (uint8_t)(val & 0xFF));
//         slice_set_uint8(input_buf, offset + 1, (uint8_t)((val >> 8)  & 0xFF));
//         slice_set_uint8(input_buf, offset + 2, (uint8_t)((val >> 16) & 0xFF));
//         slice_set_uint8(input_buf, offset + 3, (uint8_t)((val >> 24) & 0xFF));
//         slice_set_uint8(input_buf, offset + 4, (uint8_t)((val >> 32) & 0xFF));
//         slice_set_uint8(input_buf, offset + 5, (uint8_t)((val >> 40) & 0xFF));
//         slice_set_uint8(input_buf, offset + 6, (uint8_t)((val >> 48) & 0xFF));
//         slice_set_uint8(input_buf, offset + 7, (uint8_t)((val >> 56) & 0xFF));

//         return PLCTAG_STATUS_OK;
//     } else {
//         return PLCTAG_ERR_OUT_OF_BOUNDS;
//     }
// }

