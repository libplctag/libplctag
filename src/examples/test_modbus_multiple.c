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
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define REQUIRED_VERSION 2, 6, 12

#define TAG_PATH1 "protocol=modbus-tcp&gateway=127.0.0.1:1502&path=1&elem_count=10&name=hr1"
#define TAG_PATH2 "protocol=modbus-tcp&gateway=127.0.0.1:2502&path=1&elem_count=10&name=hr1"
#define DATA_TIMEOUT 500
#define TEST_TIMEOUT 30000


static void wait_for_ok(int32_t tags[], size_t num_tags, int32_t timeout_ms);
static void test_reads(int32_t tags[], size_t num_tags);


int main(void) {
    int32_t tags[2] = {0, 0};
    int i;
    const char *tag_paths[2] = {TAG_PATH1, TAG_PATH2};
    size_t num_tags = sizeof(tags) / sizeof(tags[0]);
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available, found %d.%d.%d!\n", REQUIRED_VERSION, version_major,
               version_minor, version_patch);
        return 1;
    }

    printf("Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /* create the tags */
    printf("Creating test tags.\n");
    for(i = 0; i < (int)num_tags; i++) {
        tags[i] = plc_tag_create(tag_paths[i], DATA_TIMEOUT);
        if(tags[i] < 0) {
            printf("ERROR %s: Could not create tag %d!\n", plc_tag_decode_error(tags[i]), i);
            for(int j = 0; j < i; j++) {
                plc_tag_destroy(tags[j]);
            }
            return 1;
        }
    }

    test_reads(tags, num_tags);

    /* cleanup */
    for(i = 0; i < (int)num_tags; i++) {
        plc_tag_destroy(tags[i]);
    }

    printf("SUCCESS!\n");

    return 0;
}



void wait_for_ok(int32_t tags[], size_t num_tags, int32_t timeout_ms) {
    int rc = PLCTAG_STATUS_OK;
    int64_t timeout_time = timeout_ms + compat_time_ms();
    size_t i;
    bool all_ok = false;

    // NOLINTNEXTLINE
    fprintf(stderr, "wait_for_ok() starting.\n");

    do {
        all_ok = true;
        for(i = 0; i < num_tags; i++) {
            rc = plc_tag_status(tags[i]);

            if(rc == PLCTAG_STATUS_PENDING) {
                all_ok = false;
                break;
            } else if(rc != PLCTAG_STATUS_OK) {
                // NOLINTNEXTLINE
                fprintf(stderr, "wait_for_ok(): Error %s returned on tag %zu operation.!\n", plc_tag_decode_error(rc), i);
                for(size_t j = 0; j < num_tags; j++) {
                    plc_tag_destroy(tags[j]);
                }
                exit(1);
            }
        }

        if(!all_ok) {
            compat_sleep_ms(20, NULL);

            if(timeout_time < compat_time_ms()) {
                // NOLINTNEXTLINE
                fprintf(stderr, "wait_for_ok(): Timeout waiting for tags.\n");
                for(size_t j = 0; j < num_tags; j++) {
                    plc_tag_destroy(tags[j]);
                }
                exit(1);
            }
        }
    } while(!all_ok);

    // NOLINTNEXTLINE
    fprintf(stderr, "wait_for_ok() done.\n");
}


void test_reads(int32_t tags[], size_t num_tags) {
    int rc;
    int64_t end_time = compat_time_ms() + TEST_TIMEOUT;
    size_t i;

    while(compat_time_ms() < end_time) {
        for(i = 0; i < num_tags; i++) {
            printf("Reading tag %zu data.\n", i);
            rc = plc_tag_read(tags[i], 0);
            if(rc != PLCTAG_STATUS_PENDING) {
                printf("ERROR: Unable to read tag %zu data! Got error code %d: %s\n", i, rc, plc_tag_decode_error(rc));
                for(size_t j = 0; j < num_tags; j++) {
                    plc_tag_destroy(tags[j]);
                }
                exit(1);
            }
        }

        printf("Waiting for tag read operations to complete.\n");
        wait_for_ok(tags, num_tags, DATA_TIMEOUT);

        /* wait for next round */
        compat_sleep_ms(100, NULL);
    }

    printf("Read tags complete.\n");
}

