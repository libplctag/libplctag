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
 * identity_cbor -- reads an "@identity" tag (protocol=enip-tcp, or
 * enip-udp unicast) and checks its PLCTAG_FORMAT_CBOR rendering
 * (plc_tag_get_formatted_data_size/plc_tag_get_formatted_data), which only
 * exists when LIBPLCTAG_FEATURE_ENIP is on -- plain ab-eip/ab_eip identity
 * tags have no formatted-data support, so this tool is enip-tcp/enip-udp
 * only, unlike examples/get_identity.c which works against any protocol.
 *
 * USAGE:
 *   identity_cbor --tag='protocol=enip-tcp&gateway=10.206.1.40&path=1,4&name=@identity'
 */

#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 4, 0
#define TAG_CREATE_TIMEOUT 5000

static int contains(const unsigned char *buf, int len, const char *needle) {
    int needle_len = (int)strlen(needle);
    for(int i = 0; i + needle_len <= len; i++) {
        if(memcmp(buf + i, needle, (size_t)needle_len) == 0) { return 1; }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;
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
        fprintf(stderr, "   %s --tag='protocol=enip-tcp&gateway=10.206.1.40&path=1,4&name=@identity'\n", argv[0]);
        return 1;
    }

    const char *tag_arg = argv[1];
    const char *tag_prefix = "--tag=";

    if(strncmp(tag_arg, tag_prefix, strlen(tag_prefix)) != 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Expected --tag= argument\n");
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

    rc = plc_tag_read(tag, TAG_CREATE_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        printf("ERROR: Unable to read identity tag! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    int raw_size = plc_tag_get_size(tag);
    // NOLINTNEXTLINE
    printf("Raw identity size: %d bytes\n", raw_size);
    if(raw_size < 14) {
        // NOLINTNEXTLINE
        printf("ERROR: raw identity reply implausibly small (%d bytes)\n", raw_size);
        plc_tag_destroy(tag);
        return 1;
    }

    int cbor_size = plc_tag_get_formatted_data_size(tag, PLCTAG_FORMAT_CBOR);
    // NOLINTNEXTLINE
    printf("CBOR formatted size: %d bytes\n", cbor_size);
    if(cbor_size <= 0) {
        // NOLINTNEXTLINE
        printf("ERROR: plc_tag_get_formatted_data_size(CBOR) failed: %d (%s)\n", cbor_size, plc_tag_decode_error(cbor_size));
        plc_tag_destroy(tag);
        return 1;
    }

    unsigned char *buf = (unsigned char *)malloc((size_t)cbor_size);
    if(!buf) {
        // NOLINTNEXTLINE
        printf("ERROR: out of memory allocating %d bytes\n", cbor_size);
        plc_tag_destroy(tag);
        return 1;
    }

    rc = plc_tag_get_formatted_data(tag, PLCTAG_FORMAT_CBOR, buf, cbor_size);
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        printf("ERROR: plc_tag_get_formatted_data(CBOR) failed: %d (%s)\n", rc, plc_tag_decode_error(rc));
        free(buf);
        plc_tag_destroy(tag);
        return 1;
    }

    int failures = 0;
    static const char *const required_keys[] = {"schema",     "schema-version", "records",       "vendor_id",
                                                 "device_type", "product_code",   "revision_major", "revision_minor",
                                                 "status",     "serial",         "product_name"};
    for(size_t i = 0; i < sizeof(required_keys) / sizeof(required_keys[0]); i++) {
        int found = contains(buf, cbor_size, required_keys[i]);
        // NOLINTNEXTLINE
        printf("%s: CBOR contains key \"%s\"\n", found ? "OK  " : "FAIL", required_keys[i]);
        if(!found) { failures++; }
    }

    plc_tag_destroy(tag);
    free(buf);

    if(failures > 0) {
        // NOLINTNEXTLINE
        printf("\nFAILED: %d missing key(s)\n", failures);
        return 1;
    }

    // NOLINTNEXTLINE
    printf("\nSUCCESS!\n");
    return 0;
}
