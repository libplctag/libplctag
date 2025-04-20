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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define REQUIRED_VERSION 2, 1, 0

#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=LGX&elem_size=4&elem_count=1&name=TestDINTArray[4]&debug=4"

#define DATA_TIMEOUT 1500


/*
 * This test program creates a lot of threads that read the same tag in
 * the plc.  They all hit the exact same underlying tag data structure.
 * This tests, to some extent, whether the library can handle multi-threaded
 * access.
 */


/* global to cheat on passing it to threads. */
volatile int done = 0;
volatile int32_t tag = 0;


static int open_tag(const char *tag_str) {
    int rc = PLCTAG_STATUS_OK;
    int32_t tag = plc_tag_create(tag_str, DATA_TIMEOUT);

    /* everything OK? */
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return PLCTAG_ERR_CREATE;
    } else {
        // NOLINTNEXTLINE
        fprintf(stderr, "INFO: Tag created with status %s\n", plc_tag_decode_error(plc_tag_status(tag)));
    }

    if((rc = plc_tag_status(tag)) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error %s setting up tag internal state.\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    return tag;
}


void *test_tag(void *data) {
    int tid = (int)(intptr_t)data;
    int iteration = 1;

    while(!done) {
        int rc = PLCTAG_STATUS_OK;
        int32_t value = 0;
        int64_t start = 0;
        int64_t end = 0;

        /* capture the starting time */
        start = compat_time_ms();

        /* read the tag */
        rc = plc_tag_read(tag, DATA_TIMEOUT);

        end = compat_time_ms();

        if(rc != PLCTAG_STATUS_OK) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Test %d, terminating test, read resulted in error %s\n", tid, plc_tag_decode_error(rc));
            done = 1;
        } else {
            value = plc_tag_get_int32(tag, 0);

            /* increment the value, keep it in bounds of 0-499 */
            value = (value >= (int32_t)500 ? (int32_t)0 : value + (int32_t)1);

            /* yes, we should be checking this return value too... */
            plc_tag_set_int32(tag, 0, value);

            /* write the value */
            rc = plc_tag_write(tag, DATA_TIMEOUT);

            if(rc != PLCTAG_STATUS_OK) {
                // NOLINTNEXTLINE
                fprintf(stderr, "Test %d, terminating test, write resulted in error %s\n", tid, plc_tag_decode_error(rc));
                done = 1;
            } else {
                // NOLINTNEXTLINE
                fprintf(stderr, "Test %d, iteration %d, got result %d with return code %s in %dms\n", tid, iteration, value,
                        plc_tag_decode_error(rc), (int)(end - start));
            }
        }

        iteration++;
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Test %d terminating.\n", tid);

    return 0;
}


#define MAX_THREADS (100)

int main(int argc, char **argv) {
    compat_thread_t threads[MAX_THREADS];
    int64_t start_time;
    int64_t end_time;
    int64_t seconds = 30; /* default 30 seconds */
    int num_threads = 0;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    if(argc == 2) {
        num_threads = atoi(argv[1]);
    } else {
        // NOLINTNEXTLINE
        fprintf(stderr, "Usage: stress_api_lock <num threads>\n");
        return 0;
    }

    tag = open_tag(TAG_PATH);
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Unable to create tag! %s\n", plc_tag_decode_error(tag));
        return 1;
    }

    /* create the test threads */
    for(int tid = 0; tid < num_threads && tid < MAX_THREADS; tid++) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Creating serial test thread (Test #%d).\n", tid);
        compat_thread_create(&threads[tid], test_tag, (void *)(intptr_t)tid);
    }

    start_time = compat_time_ms();
    end_time = start_time + (seconds * 1000);

    while(!done && compat_time_ms() < end_time) { compat_sleep_ms(100, NULL); }

    if(done) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Test FAILED!\n");
    } else {
        // NOLINTNEXTLINE
        fprintf(stderr, "Test SUCCEEDED!\n");
    }

    done = 1;

    for(int tid = 0; tid < num_threads && tid < MAX_THREADS; tid++) { compat_thread_join(threads[tid], NULL); }

    // NOLINTNEXTLINE
    fprintf(stderr, "All test threads terminated.\n");

    plc_tag_destroy(tag);


    return 0;
}
