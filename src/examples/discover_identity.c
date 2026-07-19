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
 * discover_identity -- exercises protocol=enip-udp CIP List Identity
 * discovery (unicast against one host, or broadcast across a subnet with a
 * CIDR mask < 32) using only the public libplctag API.
 *
 * Every record in tag->data is a self-describing, length-prefixed entry
 * (client/enip_discover.h's documented "shared raw record layout" -- the
 * same shape the TCP @identity read uses minus ip/port/state):
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
 *
 * USAGE:
 *   discover_identity --tag='protocol=enip-udp&gateway=<ip>[/cidr]&name=@identity'
 *
 *   Unicast a known device:
 *     discover_identity --tag='protocol=enip-udp&gateway=10.206.1.40&name=@identity'
 *   Broadcast a /24 subnet:
 *     discover_identity --tag='protocol=enip-udp&gateway=10.206.1.255/24&name=@identity'
 */

#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <string.h>

#define REQUIRED_VERSION 2, 4, 0
#define TAG_CREATE_TIMEOUT 5000
#define TAG_READ_TIMEOUT 10000

int main(int argc, char *argv[]) {
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;
    int size = 0;
    char tag_string[256];
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available, found %d.%d.%d!\n", REQUIRED_VERSION,
                version_major, version_minor, version_patch);
        return 1;
    }

    if(argc < 2) {
        // NOLINTNEXTLINE
        fprintf(stderr, "USAGE: %s --tag=<tag_string>\n", argv[0]);
        // NOLINTNEXTLINE
        fprintf(stderr, "   Unicast a known device:\n");
        // NOLINTNEXTLINE
        fprintf(stderr, "   %s --tag='protocol=enip-udp&gateway=10.206.1.40&name=@identity'\n", argv[0]);
        // NOLINTNEXTLINE
        fprintf(stderr, "   Broadcast a /24 subnet:\n");
        // NOLINTNEXTLINE
        fprintf(stderr, "   %s --tag='protocol=enip-udp&gateway=10.206.1.255/24&name=@identity'\n", argv[0]);
        return 1;
    }

    const char *tag_arg = argv[1];
    const char *tag_prefix = "--tag=";

    if(strncmp(tag_arg, tag_prefix, strlen(tag_prefix)) != 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Expected --tag= argument\n");
        // NOLINTNEXTLINE
        fprintf(stderr, "USAGE: %s --tag=<tag_string>\n", argv[0]);
        return 1;
    }

    const char *tag_value = tag_arg + strlen(tag_prefix);
    strncpy(tag_string, tag_value, sizeof(tag_string) - 1);
    tag_string[sizeof(tag_string) - 1] = '\0';

    // NOLINTNEXTLINE
    printf("Using tag string: %s\n", tag_string);

    tag = plc_tag_create(tag_string, TAG_CREATE_TIMEOUT);
    if(tag < 0) {
        // NOLINTNEXTLINE
        printf("ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return 1;
    }

    // NOLINTNEXTLINE
    printf("Tag created successfully (id=%d)\n", tag);

    /* Broadcast discovery needs longer than the create timeout: it collects
     * replies from every device on the subnet, not just one. */
    rc = plc_tag_read(tag, TAG_READ_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        printf("ERROR: Discovery read failed! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    size = plc_tag_get_size(tag);
    if(size <= 0) {
        // NOLINTNEXTLINE
        printf("ERROR: No discovery records returned (size=%d)!\n", size);
        plc_tag_destroy(tag);
        return 1;
    }

    // NOLINTNEXTLINE
    printf("Discovery data size: %d bytes\n\n", size);

    int record_count = 0;
    int pos = 0;

    while(pos + 2 <= size) {
        uint16_t record_len = plc_tag_get_uint16(tag, pos);
        int record_start = pos;
        int record_total = 2 + (int)record_len;

        if(record_len < 22 || record_start + record_total > size) {
            // NOLINTNEXTLINE
            printf("ERROR: malformed record at offset %d (record_len=%u)\n", record_start, (unsigned)record_len);
            plc_tag_destroy(tag);
            return 1;
        }

        record_count++;

        uint8_t ip0 = plc_tag_get_uint8(tag, record_start + 2);
        uint8_t ip1 = plc_tag_get_uint8(tag, record_start + 3);
        uint8_t ip2 = plc_tag_get_uint8(tag, record_start + 4);
        uint8_t ip3 = plc_tag_get_uint8(tag, record_start + 5);
        uint16_t port = plc_tag_get_uint16(tag, record_start + 6);
        uint16_t vendor_id = plc_tag_get_uint16(tag, record_start + 8);
        uint16_t device_type = plc_tag_get_uint16(tag, record_start + 10);
        uint16_t product_code = plc_tag_get_uint16(tag, record_start + 12);
        uint8_t revision_major = plc_tag_get_uint8(tag, record_start + 14);
        uint8_t revision_minor = plc_tag_get_uint8(tag, record_start + 15);
        uint16_t status = plc_tag_get_uint16(tag, record_start + 16);
        uint32_t serial = plc_tag_get_uint32(tag, record_start + 18);
        uint8_t state = plc_tag_get_uint8(tag, record_start + 22);
        uint8_t name_len = plc_tag_get_uint8(tag, record_start + 23);

        char product_name[257] = {0};
        int copy_len = (name_len < 255) ? (int)name_len : 255;
        if(record_start + 24 + copy_len > size) { copy_len = 0; }
        for(int i = 0; i < copy_len; i++) { product_name[i] = (char)plc_tag_get_uint8(tag, record_start + 24 + i); }

        // NOLINTNEXTLINE
        printf("=== Record %d ===\n", record_count);
        // NOLINTNEXTLINE
        printf("  Source:       %u.%u.%u.%u:%u\n", ip0, ip1, ip2, ip3, port);
        // NOLINTNEXTLINE
        printf("  Vendor ID:    %u (0x%04X)\n", vendor_id, vendor_id);
        // NOLINTNEXTLINE
        printf("  Device Type:  %u (0x%04X)\n", device_type, device_type);
        // NOLINTNEXTLINE
        printf("  Product Code: %u (0x%04X)\n", product_code, product_code);
        // NOLINTNEXTLINE
        printf("  Revision:     %u.%u\n", revision_major, revision_minor);
        // NOLINTNEXTLINE
        printf("  Status:       0x%04X\n", status);
        // NOLINTNEXTLINE
        printf("  Serial:       %u (0x%08X)\n", serial, serial);
        // NOLINTNEXTLINE
        printf("  State:        %u (0x%02X)\n", state, state);
        // NOLINTNEXTLINE
        printf("  Product Name: %s\n\n", product_name);

        pos = record_start + record_total;
    }

    plc_tag_destroy(tag);

    if(record_count == 0) {
        // NOLINTNEXTLINE
        printf("ERROR: parsed 0 records from a non-empty reply!\n");
        return 1;
    }

    // NOLINTNEXTLINE
    printf("Found %d device(s).\n", record_count);
    // NOLINTNEXTLINE
    printf("\nSUCCESS!\n");

    return 0;
}
