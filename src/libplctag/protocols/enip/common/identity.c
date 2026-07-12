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
#include "utils/debug.h"
#include <libplctag/protocols/enip/server/device.h>
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
 * Built-in identity table. Shared with the client's CIP-Identity
 * auto-classification (common/plc_classify.c): the vendor_id/device_type/
 * product_name prefix emitted here for a given family MUST match what that
 * family's classifier recognizes, so a client talking to this simulator
 * detects the family it was told to emulate.
 *
 * More than one catalog model can exist per family (e.g. OMRON NX1P2 vs
 * NX102 vs NX701 vs NJ501 all classify as ENIP_PLC_OMRON_NJNX). `model` is
 * the optional selector for device_sim's `--model=` CLI flag / a server
 * tag's `model=` attribute; the first row for a family is that family's
 * default when no model is given (device_sim_create's usual case). A row's
 * `model` only needs to be unique within its own plc_type, not globally.
 *
 * Captured values (byte-exact):
 *   ENIP_PLC_LGX  → 1756-L81E/B  rev 31.11
 *   ENIP_PLC_PLC5 → PLC-5/30 C/K - 1785-ENET 2.17  rev 3.11
 *   ENIP_PLC_MLGX → 1763-L16BWA B/12.00  rev 2.12
 *
 * Reasonable defaults (not from captures, not independently verified against
 * real hardware -- plausible catalog numbers with self-consistent encoding
 * only):
 *   ENIP_PLC_MICRO800   → 2080-LC50-24QBB (Micro850)  rev 12.1
 *   ENIP_PLC_OMRON_NJNX → NX1P2-1040DT1, NX102-1020, NX701-1600,
 *                          NJ501-1300, NJ501-1420
 *   ENIP_PLC_SLC        → 1747-L553 5/05 CPU  rev 16.5
 * ============================================================================ */

typedef struct {
    enip_plc_type_t plc_type;
    const char *model; /* NULL only for a family with exactly one entry */
    identity_t identity;
} identity_entry_t;

static const identity_entry_t IDENTITY_TABLE[] = {
    /* ENIP_PLC_LGX — 1756-L81E/B (capture exact) */
    {
        .plc_type = ENIP_PLC_LGX,
        .model    = NULL,
        .identity = {
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
    },
    /* ENIP_PLC_MICRO800 — 2080-LC50-24QBB / Micro850 (reasonable default,
     * not from a capture). Raw GetAttributesAll body this encodes to:
     *   01 00 0E 00 58 03 0C 01 20 30 A2 B3 11 04 0F
     *   32 30 38 30 2D 4C 43 35 30 2D 32 34 51 42 42
     * (name_len byte is 0x0F/15, matching the 15-character name below -- an
     * earlier draft of this row carried a mistyped 0x11/17 that didn't match
     * either the name length or the remaining byte count; fixed here.) */
    {
        .plc_type = ENIP_PLC_MICRO800,
        .model    = NULL,
        .identity = {
            .vendor_id      = 0x0001,
            .device_type    = 0x000E,
            .product_code   = 0x0358,
            .revision_major = 12,
            .revision_minor = 1,
            .status         = 0x3020,
            .serial         = 0x0411B3A2,
            .product_name   = "2080-LC50-24QBB",
            .state          = 0x03,
        },
    },
    /* ENIP_PLC_OMRON_NJNX — five catalog models (§ vendor_id 0x002F matches
     * common/plc_classify.c's CIP_VENDOR_OMRON -- keep the two in sync).
     * None of these are independently verified against real OMRON hardware;
     * device_type varies (0x000E here vs an earlier placeholder's 0x000C
     * guess) because neither value has been confirmed. NX1P2 is first and is
     * therefore the default when model= is not given. */
    {
        /* Raw: 2F 00 0E 00 BC 02 01 1A 00 00 F4 4C DE A1 0D
         *      4E 58 31 50 32 2D 31 30 34 30 44 54 31 */
        .plc_type = ENIP_PLC_OMRON_NJNX,
        .model    = "NX1P2",
        .identity = {
            .vendor_id      = 0x002F,
            .device_type    = 0x000E,
            .product_code   = 0x02BC,
            .revision_major = 1,
            .revision_minor = 26,
            .status         = 0x0000,
            .serial         = 0xA1DE4CF4,
            .product_name   = "NX1P2-1040DT1",
            .state          = 0x03,
        },
    },
    {
        /* Raw: 2F 00 0E 00 C1 02 01 22 00 00 E8 89 12 C4 0A
         *      4E 58 31 30 32 2D 31 30 32 30 */
        .plc_type = ENIP_PLC_OMRON_NJNX,
        .model    = "NX102",
        .identity = {
            .vendor_id      = 0x002F,
            .device_type    = 0x000E,
            .product_code   = 0x02C1,
            .revision_major = 1,
            .revision_minor = 34,
            .status         = 0x0000,
            .serial         = 0xC41289E8,
            .product_name   = "NX102-1020",
            .state          = 0x03,
        },
    },
    {
        /* Raw: 2F 00 0E 00 DE 02 01 1F 00 00 12 AB CD 55 0A
         *      4E 58 37 30 31 2D 31 36 30 30 */
        .plc_type = ENIP_PLC_OMRON_NJNX,
        .model    = "NX701",
        .identity = {
            .vendor_id      = 0x002F,
            .device_type    = 0x000E,
            .product_code   = 0x02DE,
            .revision_major = 1,
            .revision_minor = 31,
            .status         = 0x0000,
            .serial         = 0x55CDAB12,
            .product_name   = "NX701-1600",
            .state          = 0x03,
        },
    },
    {
        /* Raw: 2F 00 0E 00 1A 01 01 30 00 00 44 33 22 11 0A
         *      4E 4A 35 30 31 2D 31 33 30 30 */
        .plc_type = ENIP_PLC_OMRON_NJNX,
        .model    = "NJ501-1300",
        .identity = {
            .vendor_id      = 0x002F,
            .device_type    = 0x000E,
            .product_code   = 0x011A,
            .revision_major = 1,
            .revision_minor = 48,
            .status         = 0x0000,
            .serial         = 0x11223344,
            .product_name   = "NJ501-1300",
            .state          = 0x03,
        },
    },
    {
        /* Raw: 2F 00 0E 00 24 01 01 2D 00 00 AA BB CC DD 0A
         *      4E 4A 35 30 31 2D 31 34 32 30 */
        .plc_type = ENIP_PLC_OMRON_NJNX,
        .model    = "NJ501-1420",
        .identity = {
            .vendor_id      = 0x002F,
            .device_type    = 0x000E,
            .product_code   = 0x0124,
            .revision_major = 1,
            .revision_minor = 45,
            .status         = 0x0000,
            .serial         = 0xDDCCBBAA,
            .product_name   = "NJ501-1420",
            .state          = 0x03,
        },
    },
    /* ENIP_PLC_PLC5 — PLC-5/30 C/K (capture exact) */
    {
        .plc_type = ENIP_PLC_PLC5,
        .model    = NULL,
        .identity = {
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
    },
    /* ENIP_PLC_SLC — 1747-L553 5/05 CPU (reasonable default, not from a
     * capture). Raw GetAttributesAll body this encodes to:
     *   01 00 0E 00 4E 00 10 05 00 40 1A 2B 3C 4D 12
     *   31 37 34 37 2D 4C 35 35 33 20 35 2F 30 35 20 43 50 55
     * (name_len byte is 0x12/18, matching the 18-character name below -- an
     * earlier draft of this row carried a mistyped 0x13/19.) */
    {
        .plc_type = ENIP_PLC_SLC,
        .model    = NULL,
        .identity = {
            .vendor_id      = 0x0001,
            .device_type    = 0x000E,
            .product_code   = 0x004E,
            .revision_major = 16,
            .revision_minor = 5,
            .status         = 0x4000,
            .serial         = 0x4D3C2B1A,
            .product_name   = "1747-L553 5/05 CPU",
            .state          = 0x03,
        },
    },
    /* ENIP_PLC_MLGX — 1763-L16BWA (capture exact) */
    {
        .plc_type = ENIP_PLC_MLGX,
        .model    = NULL,
        .identity = {
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
    },
};

#define IDENTITY_TABLE_COUNT ((size_t)(sizeof(IDENTITY_TABLE) / sizeof(IDENTITY_TABLE[0])))

/* ============================================================================
 * Public functions
 * ============================================================================ */

extern const identity_t *identity_for_plc_type_model(enip_plc_type_t pt, const char *model) {
    /* pt == ENIP_PLC_UNKNOWN is a valid enum value but has no rows -- fall
     * through to IDENTITY_TABLE[0] (ENIP_PLC_LGX) below, same as an
     * out-of-range/unrecognized pt. */
    const identity_entry_t *family_default = NULL;

    for(size_t i = 0; i < IDENTITY_TABLE_COUNT; i++) {
        if(IDENTITY_TABLE[i].plc_type != pt) { continue; }
        if(!family_default) { family_default = &IDENTITY_TABLE[i]; }
        if(model && str_length(model) > 0 && IDENTITY_TABLE[i].model && str_cmp_i(IDENTITY_TABLE[i].model, model) == 0) {
            return &IDENTITY_TABLE[i].identity;
        }
    }

    if(family_default) {
        if(model && str_length(model) > 0) {
            pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0,
                   "identity_for_plc_type_model: no model \"%s\" for plc_type %d; using default \"%s\".", model, (int)pt,
                   family_default->identity.product_name);
        }
        return &family_default->identity;
    }

    return &IDENTITY_TABLE[0].identity; /* IDENTITY_TABLE[0] is the LGX row */
}

extern const identity_t *identity_for_plc_type(enip_plc_type_t pt) { return identity_for_plc_type_model(pt, NULL); }


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
