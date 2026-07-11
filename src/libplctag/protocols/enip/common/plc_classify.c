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

#include "plc_classify.h"

#define CIP_VENDOR_ROCKWELL ((uint16_t)0x0001)
#define CIP_VENDOR_OMRON    ((uint16_t)0x002F)

typedef struct {
    uint16_t vendor_id;
    const char *product_name_prefix;
    enip_plc_type_t plc_type;
} plc_classify_entry_t;

/*
 * Catalog-family table. One row per (vendor, family-prefix) -> PLC family.
 * The product name matched here may be either the CPU's own identity or
 * that of a separate Ethernet module/bridge in the same chassis -- both
 * share the family's catalog prefix, so device_type is deliberately not
 * part of the match (see plc_classify.h).
 *
 * "1761-" (MicroLogix 1000) is deliberately NOT in this table: the only
 * concrete real-world product name documented on that catalog prefix is
 * "1761-NET-ENNI", a standalone RS-232-to-Ethernet DF1 bridge that can sit
 * in front of *any* PCCC device (PLC-5, SLC, MicroLogix), not specifically
 * a MicroLogix 1000 CPU -- including "1761-" here would misclassify that
 * bridge as MicroLogix. Revisit if a real MicroLogix 1000 (1761-Lxx CPU)
 * capture becomes available and a way to distinguish it from the ENNI is
 * found.
 */
static const plc_classify_entry_t PLC_CLASSIFY_TABLE[] = {
    /* Rockwell -- verified against real captures (enip/DEVSIM_WIRE_REFERENCE.md
     * and live scans; see that file for the byte-exact identity dumps). */
    {CIP_VENDOR_ROCKWELL, "1756-", ENIP_PLC_LGX},  /* ControlLogix CPU (L5x/L6x/L7x/L8x) or Ethernet module (ENBT/EN2T/EN4TR) */
    {CIP_VENDOR_ROCKWELL, "PLC-5", ENIP_PLC_PLC5},
    {CIP_VENDOR_ROCKWELL, "1747-", ENIP_PLC_SLC},  /* SLC 5/05 CPU or 1747-AENTR remote I/O adapter */
    {CIP_VENDOR_ROCKWELL, "1763-", ENIP_PLC_MLGX}, /* MicroLogix 1100 */

    /* Rockwell -- standard catalog prefixes, not independently verified
     * against real hardware in this tree. */
    {CIP_VENDOR_ROCKWELL, "1769-", ENIP_PLC_LGX},  /* CompactLogix 5370 CPU or 1769-AENTR adapter */
    {CIP_VENDOR_ROCKWELL, "5069-", ENIP_PLC_LGX},  /* CompactLogix 5380 CPU */
    {CIP_VENDOR_ROCKWELL, "2080-", ENIP_PLC_MICRO800},
    {CIP_VENDOR_ROCKWELL, "1762-", ENIP_PLC_MLGX}, /* MicroLogix 1200 */
    {CIP_VENDOR_ROCKWELL, "1764-", ENIP_PLC_MLGX}, /* MicroLogix 1500 */
    {CIP_VENDOR_ROCKWELL, "1766-", ENIP_PLC_MLGX}, /* MicroLogix 1400 */

    /* OMRON -- not independently verified against real hardware in this tree. */
    {CIP_VENDOR_OMRON, "NX", ENIP_PLC_OMRON_NJNX},
    {CIP_VENDOR_OMRON, "NJ", ENIP_PLC_OMRON_NJNX},
};

#define PLC_CLASSIFY_TABLE_COUNT ((size_t)(sizeof(PLC_CLASSIFY_TABLE) / sizeof(PLC_CLASSIFY_TABLE[0])))

/* Product name (CIP SHORT_STRING: 1-byte length + ASCII, no terminator) sits
 * immediately after the fixed 14-byte identity prefix (vendor_id u16 +
 * device_type u16 + product_code u16 + rev_major u8 + rev_minor u8 +
 * status u16 + serial u32). */
#define CIP_IDENTITY_FIXED_PREFIX_LEN ((size_t)14)

/* Extract the product-name SHORT_STRING payload from a full
 * Get_Attributes_All reply. Returns bytes_null() on any parse failure. */
static Bytes extract_product_name(Bytes reply_data) {
    if(reply_data.len <= CIP_IDENTITY_FIXED_PREFIX_LEN) { return bytes_null(); }

    Bytes rest = bytes_slice(reply_data, CIP_IDENTITY_FIXED_PREFIX_LEN, reply_data.len - CIP_IDENTITY_FIXED_PREFIX_LEN);
    if(bytes_is_null(rest) || rest.len < 1) { return bytes_null(); }

    uint8_t name_len = rest.data[0];
    if((size_t)name_len > rest.len - 1) { return bytes_null(); }

    return bytes_slice(rest, 1, name_len);
}

extern enip_plc_type_t enip_classify_plc(uint16_t vendor_id, Bytes reply_data) {
    Bytes name = extract_product_name(reply_data);
    if(bytes_is_null(name)) { return ENIP_PLC_UNKNOWN; }

    for(size_t i = 0; i < PLC_CLASSIFY_TABLE_COUNT; i++) {
        if(PLC_CLASSIFY_TABLE[i].vendor_id == vendor_id && bytes_has_prefix(name, PLC_CLASSIFY_TABLE[i].product_name_prefix)) {
            return PLC_CLASSIFY_TABLE[i].plc_type;
        }
    }

    return ENIP_PLC_UNKNOWN;
}
