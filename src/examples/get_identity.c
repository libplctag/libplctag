/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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

#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 4, 0
#define TAG_CREATE_TIMEOUT 5000

int main(int argc, char *argv[]) {
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;
    int size = 0;
    int i = 0;
    char tag_string[256];
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available, found %d.%d.%d!\n", REQUIRED_VERSION,
                version_major, version_minor, version_patch);
        return 1;
    }

    /* check for argument */
    if(argc < 2) {
        // NOLINTNEXTLINE
        fprintf(stderr, "USAGE: %s --tag=<tag_string>\n", argv[0]);
        // NOLINTNEXTLINE
        fprintf(stderr, "   Generic CIP device (for discovery):\n");
        // NOLINTNEXTLINE
        fprintf(stderr, "   %s --tag='protocol=ab_eip&gateway=192.168.1.42&plc=generic&name=@identity'\n", argv[0]);
        // NOLINTNEXTLINE
        fprintf(stderr, "   With optional path to reach modules in ControlLogix chassis:\n");
        // NOLINTNEXTLINE
        fprintf(stderr, "   %s --tag='protocol=ab_eip&gateway=192.168.1.42&plc=generic&path=1,2&name=@identity'\n", argv[0]);
        return 1;
    }

    /* parse the --tag argument */
    const char *tag_arg = argv[1];
    const char *tag_prefix = "--tag=";

    if(strncmp(tag_arg, tag_prefix, strlen(tag_prefix)) != 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Expected --tag= argument\n");
        // NOLINTNEXTLINE
        fprintf(stderr, "USAGE: %s --tag=<tag_string>\n", argv[0]);
        return 1;
    }

    /* extract the tag string after --tag= */
    const char *tag_value = tag_arg + strlen(tag_prefix);
    strncpy(tag_string, tag_value, sizeof(tag_string) - 1);
    tag_string[sizeof(tag_string) - 1] = '\0';

    // NOLINTNEXTLINE
    printf("Using tag string: %s\n", tag_string);

    /* create the tag */
    tag = plc_tag_create(tag_string, TAG_CREATE_TIMEOUT);
    if(tag < 0) {
        // NOLINTNEXTLINE
        printf("ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return 1;
    }

    // NOLINTNEXTLINE
    printf("Tag created successfully (id=%d)\n", tag);

    /* perform a read operation */
    rc = plc_tag_read(tag, TAG_CREATE_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        printf("ERROR: Unable to read identity tag! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    // NOLINTNEXTLINE
    printf("Read completed successfully\n");

    /* get the size of the returned data. */
    size = plc_tag_get_size(tag);
    if(size <= 0) {
        // NOLINTNEXTLINE
        printf("ERROR: Unable to get the data size!\n");
        plc_tag_destroy(tag);
        return 1;
    }

    // NOLINTNEXTLINE
    printf("Identity data size: %d bytes\n\n", size);

    /*
     * Parse the CIP Identity Object response
     * The response includes a CIP reply header followed by the identity attributes
     */

    int offset = 0;

    if(size < offset + 14) {
        // NOLINTNEXTLINE
        printf("ERROR: Response too small to contain identity data (got %d bytes, need at least %d)\n", size, offset + 14);
        plc_tag_destroy(tag);
        return 1;
    }

    /* Parse Identity Object attributes */
    // NOLINTNEXTLINE
    printf("=== CIP Identity Object ===\n\n");

    /* Vendor ID (UINT, 2 bytes) */
    uint16_t vendor_id = plc_tag_get_uint16(tag, offset);
    offset += 2;
    // NOLINTNEXTLINE
    printf("Vendor ID: %u (0x%04X)\n", vendor_id, vendor_id);

    /* Device Type (UINT, 2 bytes) */
    uint16_t device_type = plc_tag_get_uint16(tag, offset);
    offset += 2;
    // NOLINTNEXTLINE
    printf("Device Type: %u (0x%04X)\n", device_type, device_type);

    /* Product Code (UINT, 2 bytes) */
    uint16_t product_code = plc_tag_get_uint16(tag, offset);
    offset += 2;
    // NOLINTNEXTLINE
    printf("Product Code: %u (0x%04X)\n", product_code, product_code);

    /* Revision (2 bytes: major.minor) */
    uint8_t revision_major = plc_tag_get_uint8(tag, offset);
    offset += 1;
    uint8_t revision_minor = plc_tag_get_uint8(tag, offset);
    offset += 1;
    // NOLINTNEXTLINE
    printf("Revision: %u.%u\n", revision_major, revision_minor);

    /* Status Word (WORD, 2 bytes) */
    uint16_t status_word = plc_tag_get_uint16(tag, offset);
    offset += 2;
    // NOLINTNEXTLINE
    printf("Status: 0x%04X\n", status_word);

    /* Serial Number (UDINT, 4 bytes) */
    uint32_t serial_number = plc_tag_get_uint32(tag, offset);
    offset += 4;
    // NOLINTNEXTLINE
    printf("Serial Number: %u (0x%08X)\n", serial_number, serial_number);

    /* Product Name (SHORT_STRING: 1 byte length + N bytes string) */
    if(offset < size) {
        uint8_t name_length = plc_tag_get_uint8(tag, offset);
        offset += 1;

        if(offset + (int)name_length <= size) {
            char product_name[257] = {0}; /* zero it out first */
            int name_idx;

            /* Copy the product name */
            for(name_idx = 0; name_idx < (int)name_length && name_idx < 255; name_idx++) {
                product_name[name_idx] = (char)plc_tag_get_uint8(tag, offset + name_idx);
            }
            product_name[name_idx] = '\0';
            offset += (int)name_length;

            // NOLINTNEXTLINE
            printf("Product Name: %s\n", product_name);
        } else {
            // NOLINTNEXTLINE
            printf("Product Name: <truncated or invalid>\n");
        }
    }

    /* State (USINT, 1 byte) - optional, may not be present in all devices */
    if(offset < size) {
        uint8_t state = plc_tag_get_uint8(tag, offset);
        offset += 1;
        // NOLINTNEXTLINE
        printf("State: %u (0x%02X)\n", state, state);
    }

    /* Print raw data for debugging */
    // NOLINTNEXTLINE
    printf("\n=== Raw Data ===\n");
    for(i = 0; i < size; i++) {
        uint8_t data = plc_tag_get_uint8(tag, i);
        // NOLINTNEXTLINE
        printf("%02X ", (unsigned int)data);
        if((i + 1) % 16 == 0) {
            // NOLINTNEXTLINE
            printf("\n");
        }
    }

    if(size % 16 != 0) {
        // NOLINTNEXTLINE
        printf("\n");
    }

    plc_tag_destroy(tag);

    // NOLINTNEXTLINE
    printf("\nSUCCESS!\n");

    return 0;
}
