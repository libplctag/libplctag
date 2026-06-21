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
 * CIP type code -> element layout (design doc §14.6).
 *
 * Pure, no I/O.  enip_type_decode() inspects the leading bytes of a ReadTag
 * reply payload, distinguishes the 2-byte atomic header from the 4-byte
 * abbreviated-structure header (0x02A0), and for atomic types reports the
 * element size and CIP (little-endian) byte order.
 */

#include <stdbool.h>
#include <stdint.h>
#include <libplctag/lib/tag.h>
#include <utils/bytes.h>

/* CIP atomic type codes (Volume 1, Appendix C) */
#define CIP_TYPE_BOOL  ((uint16_t)0x00C1)
#define CIP_TYPE_SINT  ((uint16_t)0x00C2)
#define CIP_TYPE_INT   ((uint16_t)0x00C3)
#define CIP_TYPE_DINT  ((uint16_t)0x00C4)
#define CIP_TYPE_LINT  ((uint16_t)0x00C5)
#define CIP_TYPE_USINT ((uint16_t)0x00C6)
#define CIP_TYPE_UINT  ((uint16_t)0x00C7)
#define CIP_TYPE_UDINT ((uint16_t)0x00C8)
#define CIP_TYPE_ULINT ((uint16_t)0x00C9)
#define CIP_TYPE_REAL  ((uint16_t)0x00CA)
#define CIP_TYPE_LREAL ((uint16_t)0x00CB)

/* Abbreviated-structure header, as a 16-bit little-endian word (0xA0 0x02) */
#define CIP_TYPE_STRUCT_HEADER ((uint16_t)0x02A0)

/* Atomic header is 2 bytes; structure header is 4 bytes */
#define ENIP_TYPE_HEADER_LEN_ATOMIC ((uint8_t)2)
#define ENIP_TYPE_HEADER_LEN_STRUCT ((uint8_t)4)

/*
 * Inspect the leading type-code bytes of a ReadTag reply payload.
 *
 * If the first 16-bit little-endian word is CIP_TYPE_STRUCT_HEADER, this is an
 * abbreviated structure: *header_len_out = 4, *elem_size_hint_out = 0 (the
 * caller derives elem_size from reply_data.len - header_len, §11.2), and
 * *order_out is left as opaque/native (no per-element conversion applies).
 *
 * Otherwise, the first 16-bit little-endian word must be one of the
 * CIP_TYPE_* atomic codes: *header_len_out = 2, *elem_size_hint_out is the
 * element width in bytes, and *order_out is filled with the CIP (always
 * little-endian) int/float orders.
 *
 * Returns false if reply_data is too short or the type code is unrecognized.
 */
extern bool enip_type_decode(Bytes reply_data, uint8_t *header_len_out, uint32_t *elem_size_hint_out,
                              tag_byte_order_t *order_out);
