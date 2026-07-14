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
 * enip-udp protocol: UDP unicast/broadcast CIP List Identity discovery
 * (design doc ENIP-METADATA-AND-DISCOVERY-DESIGN.md). Owns its own tag type
 * (enip_discover_tag_t, this file) and worker thread -- no enip_connection_t,
 * no TCP session; List Identity is connectionless. Only "@identity" is
 * supported for now ("@listidentity" alias deferred).
 */

#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <utils/attr.h>
#include <utils/bytes.h>

extern plc_tag_p enip_udp_tag_create_impl(attr attribs,
                                          void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                          void *userdata, plc_tag_p src_tag);

/*
 * Shared raw record layout (design doc §8): one length-prefixed record per
 * device, used identically by enip-udp discovery (this module) and the TCP
 * `enip` @identity tag (client/enip_tag.c, see design doc §9) -- so a caller
 * parses the same shape regardless of which transport produced it. All
 * multi-byte fields little-endian except ip (dotted octets, endian-neutral).
 * ip/port/state are 0 for the TCP case: no "reply source address" exists
 * over an already-established session (the caller already knows the
 * target), and `state` is absent from the Get_Attributes_All reply TCP
 * @identity reads (present only in a UDP List Identity reply).
 *
 *   +0   u16  record_len   (bytes after this field; next record at +2+record_len)
 *   +2   u8   ip[4]
 *   +6   u16  port
 *   +8   u16  vendor_id
 *   +10  u16  device_type
 *   +12  u16  product_code
 *   +14  u8   revision_major
 *   +15  u8   revision_minor
 *   +16  u16  status
 *   +18  u32  serial
 *   +22  u8   state
 *   +23  u8   name_len
 *   +24  ..   product_name[name_len]
 */
extern size_t enip_identity_raw_record_size(uint8_t name_len);

/* Bounds-checked write into dest starting at *pos (mirrors utils/cbor.h's
 * size/write convention); advances *pos on success. */
extern bool enip_identity_write_raw_record(Bytes dest, size_t *pos, uint32_t ip_host, uint16_t port_host, uint16_t vendor_id,
                                           uint16_t device_type, uint16_t product_code, uint8_t revision_major,
                                           uint8_t revision_minor, uint16_t status, uint32_t serial, uint8_t state,
                                           const uint8_t *product_name, uint8_t name_len);
