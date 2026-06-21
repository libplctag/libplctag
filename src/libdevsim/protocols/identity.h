/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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

#include "utils/arena.h"
#include "utils/bytes.h"
#include "device.h"   /* provides identity_t, plc_type_t */

/* Return the built-in default identity for a given PLC type (used at create time). */
extern const identity_t *identity_for_plc_type(plc_type_t pt);

/*
 * Encode GetAttributesAll body (no CIP response header).
 * Layout: vendor_id(u16LE) device_type(u16LE) product_code(u16LE)
 *         revision_major(u8) revision_minor(u8) status(u16LE)
 *         serial(u32LE) name_len(u8) name(bytes)
 */
extern Bytes identity_encode_get_attrs_all(Arena *a, const identity_t *id);

/*
 * Encode a single attribute value (no CIP response header).
 * Returns null Bytes for unknown attribute numbers.
 */
extern Bytes identity_encode_get_attr_single(Arena *a, uint16_t attr, const identity_t *id);

/*
 * Encode a CPF List Identity item body (type 0x000C).
 * Layout is the exact inverse of scan_eip_network.c:parse_list_identity_item.
 * ipv4_host and port_host are host-byte-order; the function writes them BE.
 */
extern Bytes identity_encode_listid_item(Arena *a, const identity_t *id,
                                         uint32_t ipv4_host, uint16_t port_host);
