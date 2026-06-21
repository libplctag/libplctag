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

#include <stddef.h>
#include <stdint.h>

#include "platform.h"
#include "utils/bytes.h"
#include "device.h"
#include "identity.h"

/* ============================================================================
 * CIP Identity attribute numbers (class 0x01)
 * ============================================================================ */

#define ATTR_VENDOR_ID    ((uint16_t)1)
#define ATTR_DEVICE_TYPE  ((uint16_t)2)
#define ATTR_PRODUCT_CODE ((uint16_t)3)
#define ATTR_REVISION     ((uint16_t)4)
#define ATTR_STATUS       ((uint16_t)5)
#define ATTR_SERIAL       ((uint16_t)6)
#define ATTR_PRODUCT_NAME ((uint16_t)7)
#define ATTR_STATE        ((uint16_t)8)

/* AF_INET = 2 on all POSIX/Windows platforms */
#define SOCKADDR_AF_INET  ((uint16_t)2)

/* ============================================================================
 * Built-in identity table, one entry per plc_type_t value.
 *
 * Captured values (byte-exact):
 *   PLC_CONTROL_LOGIX → 1756-L81E/B  rev 31.11
 *   PLC_PLC5          → PLC-5/30 C/K - 1785-ENET 2.17  rev 3.11
 *   PLC_MICROLOGIX    → 1763-L16BWA B/12.00  rev 2.12
 *
 * Reasonable defaults (not from captures):
 *   PLC_MICRO800, PLC_OMRON, PLC_SLC
 * ============================================================================ */

static const identity_t IDENTITIES[] = {
    /* [PLC_CONTROL_LOGIX] — 1756-L81E/B (capture exact) */
    [PLC_CONTROL_LOGIX] = {
        .vendor_id      = 0x0001,
        .device_type    = 0x000E,
        .product_code   = 0x00A4,
        .revision_major = 31,
        .revision_minor = 11,
        .status         = 0x3060,
        .serial         = 0x00F5D982,
        .product_name   = "1756-L81E/B",
        .state          = 0x03,
    },
    /* [PLC_MICRO800] — 2080-LC50-48QBB (reasonable default) */
    [PLC_MICRO800] = {
        .vendor_id      = 0x0001,
        .device_type    = 0x000E,
        .product_code   = 0x014E,
        .revision_major = 11,
        .revision_minor = 0,
        .status         = 0x3060,
        .serial         = 0x12345678,
        .product_name   = "2080-LC50-48QBB",
        .state          = 0x03,
    },
    /* [PLC_OMRON] — NJ501-1400 (reasonable default) */
    [PLC_OMRON] = {
        .vendor_id      = 0x02D4,
        .device_type    = 0x000C,
        .product_code   = 0x0069,
        .revision_major = 1,
        .revision_minor = 1,
        .status         = 0x0064,
        .serial         = 0xDEADBEEF,
        .product_name   = "NJ501-1400",
        .state          = 0x03,
    },
    /* [PLC_PLC5] — PLC-5/30 C/K (capture exact) */
    [PLC_PLC5] = {
        .vendor_id      = 0x0001,
        .device_type    = 0x000E,
        .product_code   = 0x0012,
        .revision_major = 3,
        .revision_minor = 11,
        .status         = 0x0060,
        .serial         = 0xBC033CDF,
        .product_name   = "PLC-5/30 C/K - 1785-ENET 2.17 ",
        .state          = 0x03,
    },
    /* [PLC_SLC] — SLC 5/05 (reasonable default) */
    [PLC_SLC] = {
        .vendor_id      = 0x0001,
        .device_type    = 0x000E,
        .product_code   = 0x001C,
        .revision_major = 5,
        .revision_minor = 3,
        .status         = 0x3160,
        .serial         = 0x00112233,
        .product_name   = "1747-L553 B/5.03",
        .state          = 0x03,
    },
    /* [PLC_MICROLOGIX] — 1763-L16BWA (capture exact) */
    [PLC_MICROLOGIX] = {
        .vendor_id      = 0x0001,
        .device_type    = 0x000C,
        .product_code   = 0x00B9,
        .revision_major = 2,
        .revision_minor = 12,
        .status         = 0x0064,
        .serial         = 0x9CA054FF,
        .product_name   = "1763-L16BWA B/12.00",
        .state          = 0x03,
    },
};

#define IDENTITIES_COUNT ((size_t)(sizeof(IDENTITIES) / sizeof(IDENTITIES[0])))

/* ============================================================================
 * Public functions
 * ============================================================================ */

extern const identity_t *identity_for_plc_type(plc_type_t pt) {
    if((size_t)pt < IDENTITIES_COUNT) { return &IDENTITIES[(size_t)pt]; }
    return &IDENTITIES[PLC_CONTROL_LOGIX];
}


extern Bytes identity_encode_get_attrs_all(Arena *a, const identity_t *id) {
    int32_t name_len = str_length(id->product_name);

    Bytes fixed = bytes_pack(a, BYTES_LE,
                              id->vendor_id,
                              id->device_type,
                              id->product_code,
                              id->revision_major,
                              id->revision_minor,
                              id->status,
                              id->serial,
                              (uint8_t)name_len);
    if(bytes_is_null(fixed)) { return (Bytes){NULL, 0}; }

    Bytes name_data = bytes_from_buf((const uint8_t *)id->product_name, (size_t)name_len);
    return bytes_concat(a, fixed, name_data);
}


extern Bytes identity_encode_get_attr_single(Arena *a, uint16_t attr, const identity_t *id) {
    switch(attr) {
        case ATTR_VENDOR_ID:    return bytes_pack(a, BYTES_LE, id->vendor_id);
        case ATTR_DEVICE_TYPE:  return bytes_pack(a, BYTES_LE, id->device_type);
        case ATTR_PRODUCT_CODE: return bytes_pack(a, BYTES_LE, id->product_code);
        case ATTR_REVISION:     return bytes_pack(a, BYTES_LE, id->revision_major, id->revision_minor);
        case ATTR_STATUS:       return bytes_pack(a, BYTES_LE, id->status);
        case ATTR_SERIAL:       return bytes_pack(a, BYTES_LE, id->serial);
        case ATTR_PRODUCT_NAME: {
            int32_t nlen = str_length(id->product_name);
            Bytes hdr = bytes_pack(a, BYTES_LE, (uint8_t)nlen);
            if(bytes_is_null(hdr)) { return (Bytes){NULL, 0}; }
            Bytes data = bytes_from_buf((const uint8_t *)id->product_name, (size_t)nlen);
            return bytes_concat(a, hdr, data);
        }
        case ATTR_STATE: return bytes_pack(a, BYTES_LE, id->state);
        default: return (Bytes){NULL, 0};
    }
}


extern Bytes identity_encode_listid_item(Arena *a, const identity_t *id,
                                          uint32_t ipv4_host, uint16_t port_host) {
    /* protocol_version = 1 (LE) */
    Bytes proto = bytes_pack(a, BYTES_LE, (uint16_t)1);

    /* sockaddr portion is big-endian as consumed by scan_eip_network.c:
     *   sin_family(u16BE=2), sin_port(u16BE), sin_addr(u32BE) */
    Bytes saddr = bytes_pack(a, BYTES_BE, SOCKADDR_AF_INET, port_host, ipv4_host);

    /* 8 reserved zero bytes */
    Bytes rsv = bytes_alloc(a, 8);
    if(!bytes_is_null(rsv)) { bytes_zero(rsv); }

    /* identity fields — same layout as GetAttributesAll body */
    Bytes obj = identity_encode_get_attrs_all(a, id);

    /* device state */
    Bytes st = bytes_pack(a, BYTES_LE, id->state);

    if(bytes_is_null(proto) || bytes_is_null(saddr) || bytes_is_null(rsv)
       || bytes_is_null(obj) || bytes_is_null(st)) {
        return (Bytes){NULL, 0};
    }

    return bytes_concat(a, proto, saddr, rsv, obj, st);
}
