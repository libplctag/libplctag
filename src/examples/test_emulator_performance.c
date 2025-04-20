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


/*
 * This example reads a small set of tags repeatedly as fast as possible.  It does not destroy the tags on errors, but simply
 * calls plc_tag_abort() and retries.
 *
 * Use ^C to terminate.
 */


#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define REQUIRED_VERSION 2, 6, 4

#define MAX_THREADS (70)
#define THREAD_INC (10)


#define TAG_CREATE_TIMEOUT_MS (5000)
#define TAG_OP_TIMEOUT_MS (1000)
#define TEST_TIME_MS (5000)


#define TEST_TAG_PATH_TEMPLATE \
    "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray&connection_group_id=%d"

static volatile int terminate = 0;

void handle_interrupt(void) { terminate = 1; }

static void run_test(size_t num_threads);
static void *test_func(void *arg);

int main(void) {
    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);

    compat_set_interrupt_handler(handle_interrupt);

    // NOLINTNEXTLINE
    fprintf(stderr, "Starting tests...\n\n");

    /* increase the connection group count each time */
    for(size_t thread_count = 0; thread_count <= MAX_THREADS && !terminate; thread_count += THREAD_INC) {
        run_test((thread_count == 0) ? 1 : thread_count);
    }

    if(terminate) {
        // NOLINTNEXTLINE
        fprintf(stderr, "\nTests aborted by user!\n");
    } else {
        // NOLINTNEXTLINE
        fprintf(stderr, "\nTests complete.\n");
    }

    return 0;
}


static volatile int end_test_run = 0;


void run_test(size_t thread_count) {
    size_t total_iterations = 0;
    compat_thread_t threads[MAX_THREADS] = {0};
    int64_t start_time_ms = compat_time_ms();
    int64_t end_time_ms = start_time_ms + TEST_TIME_MS;
    int64_t total_test_run_time = 0;

    end_test_run = 0;

    // NOLINTNEXTLINE
    fprintf(stderr, "Test %zu threads for %dms... \n", thread_count, TEST_TIME_MS);

    for(size_t thread_id = 0; thread_id < thread_count; thread_id++) {
        /* create the threads that run the test. */
        compat_thread_create(&threads[thread_id], test_func, (void *)(uintptr_t)thread_id);
    }

    /* wait for the test to end. */
    while(end_time_ms > compat_time_ms() && !terminate) { compat_sleep_ms(100, NULL); }

    end_test_run = 1;

    /* join with the threads and add up the iterations. */
    for(size_t thread_index = 0; thread_index < thread_count; thread_index++) {
        void *result_ptr;
        int result = 0;

        compat_thread_join(threads[thread_index], &result_ptr);

        total_iterations += (size_t)(uintptr_t)result;
    }

    total_test_run_time = compat_time_ms() - start_time_ms;

    // NOLINTNEXTLINE
    fprintf(stderr, "Test %zu threads ran for %" PRId64 "ms and completed with %zu total iterations per millisecond.\n",
            thread_count, total_test_run_time, (size_t)(total_iterations / (size_t)(uint64_t)total_test_run_time));
}


void *test_func(void *arg) {
    int thread_id = (int)(intptr_t)arg;
    int iteration_count = 0;
    int64_t longest_read = 0;

    while(!end_test_run) {
        char tag_str[250] = {0};
        int32_t tag = 0;
        int rc = PLCTAG_STATUS_OK;
        int64_t start_ms = 0;
        int64_t end_ms = 0;

        /* make the tag string */
        // NOLINTNEXTLINE
        snprintf(tag_str, sizeof(tag_str), TEST_TAG_PATH_TEMPLATE, (int)(size_t)thread_id);

        /* create the tag */
        start_ms = compat_time_ms();
        tag = plc_tag_create(tag_str, TAG_CREATE_TIMEOUT_MS);
        end_ms = compat_time_ms();

        // NOLINTNEXTLINE
        fprintf(stderr, "Thread %d: tag creation took %" PRId64 "ms\n", thread_id, end_ms - start_ms);
        fflush(stderr);

        if(tag < 0) {
            // NOLINTNEXTLINE
            fprintf(stderr, "ERROR %s: Thread %d could not create tag!\n", plc_tag_decode_error(tag), thread_id);
            continue;
        }

        /* read as fast as we can */
        while(!end_test_run) {
            start_ms = compat_time_ms();
            rc = plc_tag_read(tag, TAG_OP_TIMEOUT_MS);
            end_ms = compat_time_ms();

            if(end_ms - start_ms > longest_read) { longest_read = end_ms - start_ms; }

            iteration_count++;

            // fprintf(stderr, "Thread %d: interation %d tag read took %" PRId64 "ms\n", thread_id, iteration_count, end_ms -
            // start_ms); fflush(stderr);

            if(rc != PLCTAG_STATUS_OK) {
                // NOLINTNEXTLINE
                fprintf(stderr, "ERROR %s: Thread %d unable to read the data!\n", plc_tag_decode_error(rc), thread_id);
                break;
            }
        }

        /* destroy the tag */
        plc_tag_destroy(tag);
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Thread %d: longest read was %" PRId64 "ms\n", thread_id, longest_read);
    fflush(stderr);

    return (void *)(intptr_t)iteration_count;
}
