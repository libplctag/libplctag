/***************************************************************************
 *   Copyright (C) 2022 by Kyle Hayes                                      *
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

#include <stdint.h>
#include <lib/libplctag.h>


typedef struct {
    int start;
    int length;
    uint8_t *buffer;
} slice_t;

static inline slice_t slice_init(uint8_t *buffer, int start, int length)
{
    slice_t result;

    result.start = start;
    result.length = length;
    result.buffer = buffer;

    return result;
}

static inline int slice_get_u8(uint8_t *val, slice_t source_slice, slice_t *result_slice)
{
    if(source_slice.length < 1) {
        return PLCTAG_ERR_PARTIAL;
    }

    *val = source_slice.buffer[source_slice.start];

    result_slice->buffer = source_slice.buffer;
    result_slice->start = source_slice.start + 1;
    result_slice->length = source_slice.length - 1;

    return PLCTAG_STATUS_OK;
}


static inline int slice_put_u8(uint8_t val, slice_t dest_slice, slice_t *result_slice)
{
    if(dest_slice.length < 1) {
        return PLCTAG_ERR_NO_DATA;
    }

    dest_slice.buffer[dest_slice.start] = val;

    result_slice->buffer = dest_slice.buffer;
    result_slice->start = dest_slice.start + 1;
    result_slice->length = dest_slice.length - 1;

    return PLCTAG_STATUS_OK;
}




int slice_get_u16le(uint16_t *val, slice_t source, slice_t *result)
{
    if(source.length < 2) {
        return PLCTAG_ERR_PARTIAL;
    }

    *val = 0;
    *val = (uint16_t)source.buffer[source.start] 
         | (uint16_t)((uint16_t)source.buffer[source.start+1] << (uint16_t)8);

    result->buffer = source.buffer;
    result->start = source.start + 2;
    result->length = source.length - 2;

    return PLCTAG_STATUS_OK;
}



int slice_put_u16le(uint16_t val, slice_t dest, slice_t *result)
{
    if(dest.length < 2) {
        return PLCTAG_ERR_PARTIAL;
    }

    dest.buffer[dest.start] = (uint8_t)(val & (uint16_t)0xFF);
    dest.buffer[dest.start+1] = (uint8_t)((uint16_t)(val >> 8) & (uint16_t)0xFF);
    
    result->buffer = dest.buffer;
    result->start = dest.start + 2;
    result->length = dest.length - 2;

    return PLCTAG_STATUS_OK;
}




int slice_get_u32le(uint32_t *val, slice_t source, slice_t *result)
{
    if(source.length < 4) {
        return PLCTAG_ERR_PARTIAL;
    }

    *val = 0;
    *val = (uint32_t)source.buffer[source.start] 
         | (uint32_t)((uint32_t)source.buffer[source.start+1] << (uint32_t)8)
         | (uint32_t)((uint32_t)source.buffer[source.start+2] << (uint32_t)8)
         | (uint32_t)((uint32_t)source.buffer[source.start+3] << (uint32_t)8);

    result->buffer = source.buffer;
    result->start = source.start + 4;
    result->length = source.length - 4;

    return PLCTAG_STATUS_OK;
}



int slice_put_u32le(uint32_t val, slice_t dest, slice_t *result)
{
    if(dest.length < 4) {
        return PLCTAG_ERR_PARTIAL;
    }

    dest.buffer[dest.start] = (uint8_t)(val & (uint32_t)0xFF);
    dest.buffer[dest.start+1] = (uint8_t)((uint32_t)(val >> 8) & (uint32_t)0xFF);
    dest.buffer[dest.start+2] = (uint8_t)((uint32_t)(val >> 16) & (uint32_t)0xFF);
    dest.buffer[dest.start+3] = (uint8_t)((uint32_t)(val >> 24) & (uint32_t)0xFF);
    
    result->buffer = dest.buffer;
    result->start = dest.start + 4;
    result->length = dest.length - 4;

    return PLCTAG_STATUS_OK;
}






int slice_get_u64le(uint64_t *val, slice_t source, slice_t *result)
{
    if(source.length < 8) {
        return PLCTAG_ERR_PARTIAL;
    }

    *val = 0;
    *val = (uint64_t)source.buffer[source.start] 
         | (uint64_t)((uint64_t)source.buffer[source.start+1] << (uint64_t)8)
         | (uint64_t)((uint64_t)source.buffer[source.start+2] << (uint64_t)16)
         | (uint64_t)((uint64_t)source.buffer[source.start+3] << (uint64_t)24)
         | (uint64_t)((uint64_t)source.buffer[source.start+4] << (uint64_t)32)
         | (uint64_t)((uint64_t)source.buffer[source.start+5] << (uint64_t)40)
         | (uint64_t)((uint64_t)source.buffer[source.start+6] << (uint64_t)48)
         | (uint64_t)((uint64_t)source.buffer[source.start+7] << (uint64_t)56);

    result->buffer = source.buffer;
    result->start = source.start + 8;
    result->length = source.length - 8;

    return PLCTAG_STATUS_OK;
}



int slice_put_u64le(uint64_t val, slice_t dest, slice_t *result)
{
    if(dest.length < 8) {
        return PLCTAG_ERR_PARTIAL;
    }

    dest.buffer[dest.start] = (uint8_t)(val & (uint32_t)0xFF);
    dest.buffer[dest.start+1] = (uint8_t)((uint32_t)(val >> 8) & (uint32_t)0xFF);
    dest.buffer[dest.start+2] = (uint8_t)((uint32_t)(val >> 16) & (uint32_t)0xFF);
    dest.buffer[dest.start+3] = (uint8_t)((uint32_t)(val >> 24) & (uint32_t)0xFF);
    dest.buffer[dest.start+4] = (uint8_t)((uint32_t)(val >> 32) & (uint32_t)0xFF);
    dest.buffer[dest.start+5] = (uint8_t)((uint32_t)(val >> 40) & (uint32_t)0xFF);
    dest.buffer[dest.start+6] = (uint8_t)((uint32_t)(val >> 48) & (uint32_t)0xFF);
    dest.buffer[dest.start+7] = (uint8_t)((uint32_t)(val >> 56) & (uint32_t)0xFF);
    
    result->buffer = dest.buffer;
    result->start = dest.start + 8;
    result->length = dest.length - 8;

    return PLCTAG_STATUS_OK;
}



