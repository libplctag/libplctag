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
 * This example tests connection grouping.
 *
 * Use ^C to terminate early.
 */


#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 4, 1

#define DATA_TIMEOUT (5000)
#define TAG_CREATE_TIMEOUT (5000)
#define RETRY_TIMEOUT (10000)

#define DEFAULT_TAG_PATH "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=TestDINTArray"
#define DEFAULT_THREAD_COUNT (10)


void usage(void) {
    // NOLINTNEXTLINE
    printf(
        "Usage:\n "
        "thread_stress <num tags> <path>\n"
        "  <num_tags> - The number of threads to use in the test.\n"
        "  <path> - The tag path to use.\n"
        "\n"
        "Example: thread_stress 14 'protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=LGX&elem_size=4&elem_count=1&name=test_tag'\n");

    exit(PLCTAG_ERR_BAD_PARAM);
}

volatile int go = 0;

void interrupt_handler(void) { go = 0; }


/*
 * This test program creates a lot of threads that read the same tag in
 * the plc.  They all hit the exact same underlying tag data structure.
 * This tests whether the library can handle multi-threaded
 * access and what the performance is.
 */

typedef struct {
    int64_t max_io_time;
    int64_t min_io_time;
    int64_t total_io_time;
    int32_t tag;
    int group;
    int iteration;
    int status;
    int tid;
} thread_args;


void *test_runner(void *data) {
    thread_args *args = (thread_args *)data;
    int tid = args->tid;
    int32_t tag = args->tag;
    int *status = &(args->status);
    int *iteration = &(args->iteration);
    int64_t *total_io_time = &(args->total_io_time);
    int64_t *max_io_time = &(args->max_io_time);
    int64_t *min_io_time = &(args->min_io_time);
    int rc = PLCTAG_STATUS_OK;

    *status = PLCTAG_STATUS_OK;
    *iteration = 0;
    *total_io_time = 0;
    *max_io_time = 0;
    *min_io_time = 1000000000L;

    /* wait until all threads ready. */
    while(!go) { compat_sleep_ms(10, NULL); }

    while(go) {
        int64_t start = 0;
        int64_t io_time = 0;

        (*iteration)++;

        /* capture the starting time */
        start = compat_time_ms();

        rc = plc_tag_read(tag, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            // NOLINTNEXTLINE
            fprintf(stderr, "!!! Thread %d, iteration %d, read failed after %" PRId64 "ms  with error %s\n", tid, *iteration,
                    (int64_t)(compat_time_ms() - start), plc_tag_decode_error(rc));
            break;
        }

        io_time = compat_time_ms() - start;

        *total_io_time += io_time;

        if(io_time > *max_io_time) { *max_io_time = io_time; }

        if(io_time < *min_io_time) { *min_io_time = io_time; }
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "*** Thread %d terminating after %d iterations and an average of %dms per iteration.\n", tid, *iteration,
            (int)(*total_io_time / (*iteration)));

    fflush(stderr);

    return 0;
}


#define MAX_THREADS (100)

int main(int argc, char **argv) {
    compat_thread_t thread[MAX_THREADS];
    int num_threads = 0;
    int success = 0;
    thread_args args[MAX_THREADS];
    char *tag_string = NULL;
    int64_t start = 0;
    int64_t total_run_time = 0;
    int count_down = 50;

    /* set up logging */
    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    /* set up handler for ^C etc. */
    compat_set_interrupt_handler(interrupt_handler);

    // NOLINTNEXTLINE
    fprintf(stderr, "Hit ^C to terminate the test.\n");

    if(argc == 3) {
        num_threads = atoi(argv[1]);
        tag_string = argv[2];
    } else {
        // usage();
        num_threads = DEFAULT_THREAD_COUNT;
        tag_string = DEFAULT_TAG_PATH;
    }

    if(num_threads > MAX_THREADS) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Too many threads.  A maximum of %d threads are supported.\n", MAX_THREADS);
        usage();
    }

    if(!tag_string || strlen(tag_string) < 10) {
        // NOLINTNEXTLINE
        fprintf(stderr, "You must provide a valid tag string.\n");
        usage();
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "--- starting run with %d threads using tag string \"%s\".\n", num_threads, tag_string);

    /* create the test tags */
    for(int tid = 0; tid < num_threads && tid < MAX_THREADS; tid++) {
        int32_t tag = 0;

        // NOLINTNEXTLINE
        fprintf(stderr, "--- Creating test tag %d.\n", tid);

        if(tid != 0) {
            tag = plc_tag_create(tag_string, TAG_CREATE_TIMEOUT);
        } else {
            /* make the first tag in a different connection group. */
            const char *conn_grp_string = "&connection_group_id=1";
            size_t new_string_size = strlen(tag_string) + strlen(conn_grp_string) + 1;
            char *new_tag_string = (char *)calloc(1, new_string_size);

            if(new_tag_string) {
                // NOLINTNEXTLINE
                int size = snprintf(new_tag_string, new_string_size, "%s%s", tag_string, conn_grp_string);

                if((size + 1) == (int)(unsigned int)new_string_size) {
                    tag = plc_tag_create(new_tag_string, TAG_CREATE_TIMEOUT);
                } else {
                    // NOLINTNEXTLINE
                    fprintf(stderr, "New string: \"%s\".\n", new_tag_string);
                    // NOLINTNEXTLINE
                    fprintf(stderr, "string size %d, printed size %d.\n", (int)(unsigned int)new_string_size, size);
                    // NOLINTNEXTLINE
                    fprintf(stderr, "Unable to copy new attribute to tag string!\n");
                    tag = PLCTAG_ERR_CREATE;
                }

                free(new_tag_string);
            } else {
                // NOLINTNEXTLINE
                fprintf(stderr, "Unable to allocate new tag string!\n");
                tag = PLCTAG_ERR_NO_MEM;
            }
        }

        if(tag < 0) {
            // NOLINTNEXTLINE
            fprintf(stderr, "!!! Failed to create tag for thread %d with error %s!\n", tid, plc_tag_decode_error(tag));
            usage();
        }

        args[tid].tid = tid;
        args[tid].tag = tag;
        args[tid].status = PLCTAG_STATUS_OK;
        args[tid].iteration = 0;
        args[tid].total_io_time = 0;
        args[tid].min_io_time = 0;
        args[tid].max_io_time = 0;
    }

    for(int tid = 0; tid < num_threads && tid < MAX_THREADS; tid++) {
        // NOLINTNEXTLINE
        fprintf(stderr, "--- Creating test thread %d.\n", args[tid].tid);
        compat_thread_create(&thread[tid], test_runner, (void *)&args[tid]);
    }

    /* wait for threads to create and start. */
    compat_sleep_ms(100, NULL);

    /* launch the threads */
    go = 1;

    start = compat_time_ms();

    while(go && (--count_down) > 0) { compat_sleep_ms(100, NULL); }

    go = 0;

    total_run_time = compat_time_ms() - start;

    success = 1;

    /* wait for the threads to stop. */
    compat_sleep_ms(100, NULL);

    for(int tid = 0; tid < num_threads && tid < MAX_THREADS; tid++) { compat_thread_join(thread[tid], NULL); }

    /* close the tags but get the group first. */
    for(int tid = 0; tid < num_threads && tid < MAX_THREADS; tid++) {
        args[tid].group = plc_tag_get_int_attribute(args[tid].tag, "connection_group_id", -1);
        plc_tag_destroy(args[tid].tag);
    }

    /* check the status */
    for(int tid = 0; tid < num_threads && tid < MAX_THREADS; tid++) {
        if(args[tid].status != PLCTAG_STATUS_OK) { success = 0; }
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "--- All test threads terminated after running %" PRId64 "ms.\n", total_run_time);

    /* print out statistics. */
    for(int tid = 0; tid < num_threads; tid++) {
        // NOLINTNEXTLINE
        fprintf(stderr,
                "--- Thread %d in group %d ran %d iterations with a total io time of %" PRId64 "ms and min/avg/max of %" PRId64
                "ms/%" PRId64 "ms/%" PRId64 "ms.\n",
                tid, args[tid].group, args[tid].iteration, args[tid].total_io_time, args[tid].min_io_time,
                args[tid].total_io_time / args[tid].iteration, args[tid].max_io_time);
    }

    if(!success) {
        // NOLINTNEXTLINE
        fprintf(stderr, "*** Test FAILED!\n");
        return 1;
    } else {
        // NOLINTNEXTLINE
        fprintf(stderr, "*** Test SUCCEEDED!\n");
        return 0;
    }
}
