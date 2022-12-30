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
    int index = source_slice.start;
    
    if(source_slice.length <= 0) {
        *result_slice = source_slice;
        return PLCTAG_ERR_NO_DATA;
    }

    *val = source_slice.buffer[index];
    *result_slice = slice_init(source_slice.buffer, source_slice.start + 1, source_slice.length - 1);

    return PLCTAG_STATUS_OK;
}


static inline int slice_put_u8(uint8_t val, slice_t source_slice, slice_t *result_slice)
{
    int index = source_slice.start;
    
    if(source_slice.length <= 0) {
        *result_slice = source_slice;
        return PLCTAG_ERR_NO_DATA;
    }

    source_slice.buffer[index] = val;
    *result_slice = slice_init(source_slice.buffer, source_slice.start + 1, source_slice.length - 1);

    return PLCTAG_STATUS_OK;
}





typedef struct {
    uint16_t command;
    uint16_t length;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;
    slice_t payload;
} eip_encap_t;

extern int eip_encap_reserve(slice_t src, slice_t *result);
extern int eip_encap_deserialize(eip_encap_t *eip_encap, slice_t src, slice_t *result);
extern int eip_encap_serialize(eip_encap_t *eip_encap, slice_t dest, slice_t *result);

/* Session Registration Request and Response */
typedef struct {
    uint16_t eip_version;
    uint16_t option_flags;
} eip_session_registration_t;

extern int eip_session_registration_reserve(slice_t source, slice_t *result);
extern int eip_session_registration_deserialize(eip_session_registration_t *eip_reg, slice_t source, slice_t *result);
extern int eip_session_registration_serialize(eip_session_registration_t *eip_reg, slice_t dest, slice_t *result);

