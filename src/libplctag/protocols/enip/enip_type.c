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

#include <libplctag/protocols/enip/enip_type.h>
#include <inttypes.h>
#include <utils/debug.h>

/* CIP atomic types are always little-endian on the wire. */
static const tag_byte_order_t ENIP_ATOMIC_BYTE_ORDER = {
    .is_allocated = 0,

    .int16_order = {0, 1},
    .int32_order = {0, 1, 2, 3},
    .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
    .float32_order = {0, 1, 2, 3},
    .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},
};

typedef struct {
    uint16_t type_code;
    uint32_t elem_size;
} enip_atomic_type_entry_t;

/* clang-format off */
static const enip_atomic_type_entry_t ENIP_ATOMIC_TYPES[] = {
    { CIP_TYPE_BOOL,  1 },
    { CIP_TYPE_SINT,  1 },
    { CIP_TYPE_INT,   2 },
    { CIP_TYPE_DINT,  4 },
    { CIP_TYPE_LINT,  8 },
    { CIP_TYPE_USINT, 1 },
    { CIP_TYPE_UINT,  2 },
    { CIP_TYPE_UDINT, 4 },
    { CIP_TYPE_ULINT, 8 },
    { CIP_TYPE_REAL,  4 },
    { CIP_TYPE_LREAL, 8 },
};
/* clang-format on */

#define ENIP_NUM_ATOMIC_TYPES ((size_t)(sizeof(ENIP_ATOMIC_TYPES) / sizeof(ENIP_ATOMIC_TYPES[0])))

bool enip_type_decode(Bytes reply_data, uint8_t *header_len_out, uint32_t *elem_size_hint_out, tag_byte_order_t *order_out) {
    if(bytes_is_null(reply_data) || reply_data.len < 2 || !header_len_out || !elem_size_hint_out || !order_out) {
        return false;
    }

    uint16_t type_code = (uint16_t)((uint16_t)reply_data.data[0] | (uint16_t)((uint16_t)reply_data.data[1] << 8));

    if(type_code == CIP_TYPE_STRUCT_HEADER) {
        if(reply_data.len < ENIP_TYPE_HEADER_LEN_STRUCT) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "reply too short for structure header.");
            return false;
        }

        *header_len_out = ENIP_TYPE_HEADER_LEN_STRUCT;
        *elem_size_hint_out = 0;
        return true;
    }

    for(size_t i = 0; i < ENIP_NUM_ATOMIC_TYPES; i++) {
        if(ENIP_ATOMIC_TYPES[i].type_code == type_code) {
            *header_len_out = ENIP_TYPE_HEADER_LEN_ATOMIC;
            *elem_size_hint_out = ENIP_ATOMIC_TYPES[i].elem_size;
            *order_out = ENIP_ATOMIC_BYTE_ORDER;
            return true;
        }
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "unknown CIP type code 0x%04" PRIX16 ".", type_code);

    return false;
}
