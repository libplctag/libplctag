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
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 6, 12

/* Read tags: 8 tags total (11-14 on server 1, 15-18 on server 2) */
#define TAG_READ_11 "protocol=modbus_tcp&gateway=127.0.0.1:1502&path=0&name=co0&elem_count=86"
#define TAG_READ_12 "protocol=modbus_tcp&gateway=127.0.0.1:1502&path=0&name=di0&elem_count=92"
#define TAG_READ_13 "protocol=modbus_tcp&gateway=127.0.0.1:1502&path=0&name=ir0&elem_count=21"
#define TAG_READ_14 "protocol=modbus_tcp&gateway=127.0.0.1:1502&path=0&name=hr0&elem_count=23"

#define TAG_READ_15 "protocol=modbus_tcp&gateway=127.0.0.1:2502&path=0&name=co0&elem_count=86"
#define TAG_READ_16 "protocol=modbus_tcp&gateway=127.0.0.1:2502&path=0&name=di0&elem_count=92"
#define TAG_READ_17 "protocol=modbus_tcp&gateway=127.0.0.1:2502&path=0&name=ir0&elem_count=21"
#define TAG_READ_18 "protocol=modbus_tcp&gateway=127.0.0.1:2502&path=0&name=hr0&elem_count=23"

/* Write tags: 2 tags (19-20) on server 1 */
#define TAG_WRITE_19 "protocol=modbus_tcp&gateway=127.0.0.1:1502&path=0&name=hr0&elem_count=1"
#define TAG_WRITE_20 "protocol=modbus_tcp&gateway=127.0.0.1:1502&path=0&name=co0&elem_count=1"

#define DATA_TIMEOUT 500
#define READ_PHASE_TIME_MS 5000  /* ~5 seconds for read phase */
#define READ_PERIOD_MS 250  /* Read every 250 ms */
#define WRITE_PHASE_DELAY_MS 12000  /* ~12 seconds before switching to write phase */
#define WRITE_PHASE_TIME_MS 5000    /* ~5 seconds for write phase */
#define WRITE_PERIOD_MS 500 /* Write every 500 ms */

static void wait_for_ok(int32_t tags[], size_t num_tags, int32_t timeout_ms);


int main(void) {
    int32_t read_tags[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int32_t write_tags[2] = {0, 0};
    int i;
    const char *read_tag_paths[8] = {
        TAG_READ_11, TAG_READ_12, TAG_READ_13, TAG_READ_14,
        TAG_READ_15, TAG_READ_16, TAG_READ_17, TAG_READ_18
    };
    int64_t start_time, current_time, read_phase_end_time, write_phase_start_time, write_phase_end_time, last_write_time;
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

    start_time = compat_time_ms();

    /* ===== PHASE 1: Create all 8 read tags and wait for OK ===== */
    printf("PHASE 1: Creating read tags (11-18).\n");
    for(i = 0; i < 8; i++) {
        read_tags[i] = plc_tag_create(read_tag_paths[i], 0);
        if(read_tags[i] < 0) {
            printf("ERROR %s: Could not create read tag %d!\n", plc_tag_decode_error(read_tags[i]), i + 11);
            for(int j = 0; j < i; j++) {
                plc_tag_destroy(read_tags[j]);
            }
            return 1;
        }
        printf("  Created read tag %d (index %d).\n", i + 11, i);
    }

    printf("PHASE 1: Waiting for all read tags to complete creation.\n");
    wait_for_ok(read_tags, 8, DATA_TIMEOUT);
    printf("PHASE 1: All read tags created.\n");

    /* ===== PHASE 2: Read tags 11-18 every DATA_TIMEOUT ms until READ_PHASE_TIME_MS ===== */
    printf("PHASE 2: Starting read loop for %"PRId64" ms.\n", (long long)READ_PHASE_TIME_MS);

    read_phase_end_time = compat_time_ms() + READ_PHASE_TIME_MS;
    write_phase_start_time = read_phase_end_time + WRITE_PHASE_DELAY_MS;
    write_phase_end_time = write_phase_start_time + WRITE_PHASE_TIME_MS;

    while(compat_time_ms() < read_phase_end_time) {
        /* Issue reads on all 8 read tags */
        for(i = 0; i < 8; i++) {
            if(plc_tag_read(read_tags[i], 0) != PLCTAG_STATUS_PENDING) {
                printf("ERROR: Could not start read on tag %d (time=%lld)!\n", i + 11, compat_time_ms() - start_time);
                goto cleanup;
            }
        }

        /* Wait for all reads to complete */
        wait_for_ok(read_tags, 8, DATA_TIMEOUT);

        current_time = compat_time_ms();
        if((current_time - start_time) % 2000 < DATA_TIMEOUT) {  /* Print roughly every 2 seconds */
            printf("PHASE 2: Read phase running... elapsed %"PRId64" ms\n", current_time - start_time);
        }

        compat_sleep_ms(READ_PERIOD_MS, NULL);
    }

    printf("PHASE 2: Read phase complete at %"PRId64" ms.\n", compat_time_ms() - start_time);

    /* ===== PHASE 3: Wait for write_phase_start_time ===== */
    printf("PHASE 3: Waiting %"PRId64" ms before creating write tags...\n", (int64_t)WRITE_PHASE_DELAY_MS);
    while(compat_time_ms() < write_phase_start_time) {
        compat_sleep_ms(100, NULL);
    }
    printf("PHASE 3: Wait complete at %"PRId64" ms.\n", compat_time_ms() - start_time);

    /* ===== PHASE 4: Create both write tags and wait for OK ===== */
    printf("PHASE 4: Creating write tags (19-20).\n");
    write_tags[0] = plc_tag_create(TAG_WRITE_19, 0);
    if(write_tags[0] < 0) {
        printf("ERROR %s: Could not create write tag 19!\n", plc_tag_decode_error(write_tags[0]));
        goto cleanup;
    }
    printf("  Created write tag 19.\n");

    write_tags[1] = plc_tag_create(TAG_WRITE_20, 0);
    if(write_tags[1] < 0) {
        printf("ERROR %s: Could not create write tag 20!\n", plc_tag_decode_error(write_tags[1]));
        goto cleanup;
    }
    printf("  Created write tag 20.\n");

    printf("PHASE 4: Waiting for all write tags to complete creation.\n");
    wait_for_ok(write_tags, 2, DATA_TIMEOUT);
    printf("PHASE 4: All write tags created.\n");

    /* ===== PHASE 5: Read/write loop for WRITE_PHASE_TIME_MS in batch mode ===== */
    printf("PHASE 5: Starting read/write loop for %"PRId64" ms.\n", (long long)WRITE_PHASE_TIME_MS);
    last_write_time = compat_time_ms();

    uint16_t write_value = 0x01;
    int write_toggle = 0;

    while(compat_time_ms() < write_phase_end_time) {
        int all_tags_count = 10;
        int32_t all_tags[10];
        
        /* Build array of all tags for batch operations */
        for(i = 0; i < 8; i++) {
            all_tags[i] = read_tags[i];
        }
        all_tags[8] = write_tags[0];
        all_tags[9] = write_tags[1];

        /* Issue reads on all 8 read tags */
        for(i = 0; i < 8; i++) {
            if(plc_tag_read(read_tags[i], 0) != PLCTAG_STATUS_PENDING) {
                printf("ERROR: Could not start read on tag %d!\n", i + 11);
                goto cleanup;
            }
        }

        /* Issue writes on both write tags */
        plc_tag_set_uint16(write_tags[0], 0, write_value++);
        if(plc_tag_write(write_tags[0], 0) != PLCTAG_STATUS_PENDING) {
            printf("ERROR: Could not start write on tag 19!\n");
            goto cleanup;
        }

        write_toggle ^= 1;
        plc_tag_set_bit(write_tags[1], 0, write_toggle);
        if(plc_tag_write(write_tags[1], 0) != PLCTAG_STATUS_PENDING) {
            printf("ERROR: Could not start write on tag 20!\n");
            goto cleanup;
        }

        /* Wait for all tags (reads + writes) to complete in batch */
        wait_for_ok(all_tags, all_tags_count, DATA_TIMEOUT);

        current_time = compat_time_ms();
        if((current_time - start_time) % 2000 < DATA_TIMEOUT) {  /* Print roughly every 2 seconds */
            printf("PHASE 5: Read/write loop... elapsed %lld ms\n", current_time - start_time);
        }

        compat_sleep_ms(DATA_TIMEOUT, NULL);
    }

    printf("PHASE 5: Read/write phase complete at %"PRId64" ms.\n", compat_time_ms() - start_time);

cleanup:
    /* ===== PHASE 6: Destroy all tags ===== */
    printf("PHASE 6: Destroying all tags.\n");
    for(i = 0; i < 8; i++) {
        if(read_tags[i] > 0) {
            plc_tag_destroy(read_tags[i]);
        }
    }
    for(i = 0; i < 2; i++) {
        if(write_tags[i] > 0) {
            plc_tag_destroy(write_tags[i]);
        }
    }

    printf("PHASE 6: All tags destroyed.\n");
    printf("SUCCESS!\n");

    return 0;
}



void wait_for_ok(int32_t tags[], size_t num_tags, int32_t timeout_ms) {
    int rc = PLCTAG_STATUS_OK;
    int64_t timeout_time = timeout_ms + compat_time_ms();
    size_t i;
    bool all_ok = false;

    do {
        all_ok = true;
        
        /* Check all tags */
        for(i = 0; i < num_tags; i++) {
            rc = plc_tag_status(tags[i]);

            if(rc == PLCTAG_STATUS_PENDING) {
                all_ok = false;
            } else if(rc != PLCTAG_STATUS_OK) {
                fprintf(stderr, "wait_for_ok(): Error %s returned on tag %zu operation!\n", plc_tag_decode_error(rc), i);
                exit(1);
            }
        }

        /* If any are still pending, sleep a bit and retry */
        if(!all_ok) {
            compat_sleep_ms(5, NULL);
        }

        /* Check timeout */
        if(timeout_time < compat_time_ms()) {
            fprintf(stderr, "wait_for_ok(): Timeout waiting for tags.\n");
            exit(1);
        }
    } while(!all_ok);
}


