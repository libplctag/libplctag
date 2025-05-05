/***************************************************************************
 *   Copyright (C) 2025 by Simon Labrecque                                 *
 *   Author Simon Labrecque  simon@wegel.ca                                *
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

#define REQUIRED_VERSION 2, 1, 0
#define TAG_STRING_TEMPLATE "protocol=ab_eip&gateway=%s&path=%s&cpu=controllogix&elem_type=DINT&elem_count=1&name=%s[%d]"
#define DEFAULT_TIMEOUT 5000
#define DEFAULT_NUM_INDEXES 3
#define MAX_NUM_INDEXES 100

static void usage(void) {
    // NOLINTNEXTLINE
    fprintf(stderr,
            "Usage: test_array_notation <PLC IP> <PLC path> <tag name> <count> [timeout]\n"
            "  <PLC IP>   - IP address or hostname of the PLC (e.g., '127.0.0.1')\n"
            "  <PLC path> - Path to the PLC (e.g. '1,0')\n"
            "  <tag name> - Base name of the array tag to test\n"
            "  <count>    - Number of array indexes to test (1-%d)\n"
            "  [timeout]  - Timeout in milliseconds (default %d)\n"
            "\nExample: test_array_notation 127.0.0.1 1,0 DINT_ARRAY 3 2000\n",
            MAX_NUM_INDEXES, DEFAULT_TIMEOUT);
    exit(1);
}

static char *setup_tag_string(const char *gateway, const char *path, const char *tag_name, int index) {
    char *tag_string = (char *)calloc(1, 256);
    if(!tag_string) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Memory allocation failed!\n");
        exit(1);
    }

    if(strlen(gateway) == 0 || strlen(path) == 0 || strlen(tag_name) == 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Gateway IP, PLC path, and tag name must not be empty!\n");
        free(tag_string);
        usage();
    }

    // NOLINTNEXTLINE
    snprintf(tag_string, 256, TAG_STRING_TEMPLATE, gateway, path, tag_name, index);
    return tag_string;
}

static int write_value(const char *tag_string, int32_t value, int timeout) {
    int32_t tag = plc_tag_create(tag_string, timeout);
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR creating tag %s: %s\n", tag_string, plc_tag_decode_error(tag));
        return tag;
    }

    int rc = plc_tag_status(tag);
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR setting up tag %s: %s\n", tag_string, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    plc_tag_set_int32(tag, 0, value);
    rc = plc_tag_write(tag, timeout);

    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR writing value %d to %s: %s\n", value, tag_string, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Wrote value %d to %s\n", value, tag_string);
    plc_tag_destroy(tag);
    return PLCTAG_STATUS_OK;
}

static int32_t read_value(const char *tag_string, int timeout, int *status) {
    int32_t tag = plc_tag_create(tag_string, timeout);
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR creating tag %s: %s\n", tag_string, plc_tag_decode_error(tag));
        *status = tag;
        return 0;
    }

    int rc = plc_tag_read(tag, timeout);
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR reading from %s: %s\n", tag_string, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        *status = rc;
        return 0;
    }

    int32_t value = plc_tag_get_int32(tag, 0);
    // NOLINTNEXTLINE
    fprintf(stderr, "Read value %d from %s\n", value, tag_string);

    plc_tag_destroy(tag);
    *status = PLCTAG_STATUS_OK;
    return value;
}

int main(int argc, char **argv) {
    int timeout = DEFAULT_TIMEOUT;
    int num_indexes;
    int rc = PLCTAG_STATUS_OK;
    int32_t *test_values;
    int all_passed = 1;
    int i;

    if(argc < 5) { usage(); }

    /* check the library version */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Required library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    const char *gateway = argv[1];
    const char *path = argv[2];
    const char *tag_name = argv[3];

    /* get number of indexes to test */
    num_indexes = atoi(argv[4]);
    if(num_indexes <= 0 || num_indexes > MAX_NUM_INDEXES) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Number of indexes must be between 1 and %d!\n", MAX_NUM_INDEXES);
        usage();
    }

    /* get optional timeout parameter */
    if(argc > 5) {
        timeout = atoi(argv[5]);
        if(timeout <= 0) {
            // NOLINTNEXTLINE
            fprintf(stderr, "ERROR: Timeout must be a positive integer!\n");
            usage();
        }
    }

    /* allocate test values array */
    test_values = (int32_t *)calloc((size_t)num_indexes, sizeof(int32_t));
    if(!test_values) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Failed to allocate memory for test values!\n");
        return 1;
    }

    /* initialize test values - use distinct values for each index */
    for(i = 0; i < num_indexes; i++) {
        test_values[i] = (i + 1) * 1111;  // 1111, 2222, 3333, etc.
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Testing array notation for tag %s (%d indexes)...\n", tag_name, num_indexes);

    /* write values to each index */
    for(i = 0; i < num_indexes; i++) {
        char *tag_string = setup_tag_string(gateway, path, tag_name, i);
        rc = write_value(tag_string, test_values[i], timeout);
        free(tag_string);

        if(rc != PLCTAG_STATUS_OK) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Failed to write to index %d\n", i);
            all_passed = 0;
            goto cleanup;
        }
    }

    /* small delay to ensure writes complete */
    compat_sleep_ms(100, NULL);

    /* read back and verify all values */
    // NOLINTNEXTLINE
    fprintf(stderr, "\nReading back all values to verify array indexing behavior...\n");
    for(i = 0; i < num_indexes; i++) {
        char *tag_string = setup_tag_string(gateway, path, tag_name, i);
        int read_status;
        int32_t read_result = read_value(tag_string, timeout, &read_status);
        free(tag_string);

        if(read_status != PLCTAG_STATUS_OK) {
            all_passed = 0;
            continue;
        }

        if(read_result != test_values[i]) {
            // NOLINTNEXTLINE
            fprintf(stderr, "ERROR: Array index mismatch at [%d]: wrote %d, read %d\n", i, test_values[i], read_result);
            all_passed = 0;
        } else {
            // NOLINTNEXTLINE
            fprintf(stderr, "Array index [%d] matches expected value\n", i);
        }
    }

cleanup:
    free(test_values);

    // NOLINTNEXTLINE
    fprintf(stderr, "\nArray notation test %s\n", all_passed ? "PASSED" : "FAILED");

    return all_passed ? 0 : 1;
}
