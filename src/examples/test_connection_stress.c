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
 * This example creates many threads.  Each thread reads one tag as fast as it can.  Each thread uses its own
 * thread ID as the connection_group_id to force the library to create a separate PLC connection for each thread.
 *
 * This is more a test of the server than the client.
 *
 * Use ^C to terminate.
 */


#define CLOG_MAIN
#include "clog.h"

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define REQUIRED_VERSION 2, 4, 1

#define DATA_TIMEOUT (5000)
#define TAG_CREATE_TIMEOUT (5000)
#define RETRY_TIMEOUT (10000)

#define DEFAULT_TAG_PATH "protocol=modbus-tcp&gateway=10.206.1.59:1502&path=0&elem_count=2&name=hr10"
#define DEFAULT_THREAD_COUNT (10)


void usage(void) {
    printf(
        "Usage:\n"
        "  test_connection_stress [--num-threads=N] [--tag=TAG_STRING]\n"
        "  test_connection_stress <num_threads> <tag_string>  (legacy format)\n"
        "\n"
        "  --num-threads=N - The number of threads (default: %d)\n"
        "  --tag=TAG_STRING - The tag attribute string to use\n"
        "\n"
        "Example:\n"
        "  test_connection_stress --num-threads=10 --tag='protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=2&name=hr10'\n"
        "  test_connection_stress 10 'protocol=modbus-tcp&gateway=127.0.0.1:1502&path=0&elem_count=2&name=hr10'\n",
        DEFAULT_THREAD_COUNT);

    exit(PLCTAG_ERR_BAD_PARAM);
}


volatile int go = 0;

static void interrupt_handler(void) { go = 1; }

/*
 * This test program creates a lot of threads that read the same tag in
 * the plc.  They all hit the exact same underlying tag data structure.
 * This tests whether the library can handle multi-threaded
 * access and what the performance is.
 */

typedef struct {
    int tid;
    int32_t tag;
    int status;
    int32_t iteration;
    int64_t total_io_time;
    int64_t min_io_time;
    int64_t max_io_time;
    int64_t sum_sq_deviation;
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
    // NOLINTNEXTLINE
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    /* cat ^C etc. */
    compat_set_interrupt_handler(interrupt_handler);

    /* Initialize logger to write to stderr */
    if (clog_init_fd(0, fileno(stderr)) != 0) {
        fprintf(stderr, "Failed to initialize logger.\n");
        return 1;
    }
    /* Set format to match modbus_server output: YYYY-MM-DD HH:MM:SS.MICROSECONDS LEVEL message */
    clog_set_fmt(0, "%d %t.%u %l: %m\n");

    clog_info(CLOG(0), "Hit ^C to terminate the test.");

    /* Parse command-line arguments */
    num_threads = DEFAULT_THREAD_COUNT;
    tag_string = NULL;

    for(int i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--num-threads=", 14) == 0) {
            num_threads = atoi(argv[i] + 14);
        } else if(strncmp(argv[i], "--tag=", 6) == 0) {
            tag_string = argv[i] + 6;
        } else if(i == 1 && argc == 3) {
            /* Legacy format: num_threads tag_string */
            num_threads = atoi(argv[1]);
            tag_string = argv[2];
            break;
        } else if(i == 2 && argc == 3) {
            tag_string = argv[2];
            break;
        }
    }

    /* Use defaults if not specified */
    if(!tag_string) {
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
        char full_tag_string[1024];

        // NOLINTNEXTLINE
        fprintf(stderr, "--- Creating test tag %d.\n", tid);

        /* Append connection_group_id to force separate connection per thread */
        // NOLINTNEXTLINE
        snprintf(full_tag_string, sizeof(full_tag_string), "%s&connection_group_id=%d", tag_string, tid);

        tag = plc_tag_create(full_tag_string, TAG_CREATE_TIMEOUT);

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

    /* FIXME - wait for the threads to stop. */
    compat_sleep_ms(100, NULL);

    for(int tid = 0; tid < num_threads && tid < MAX_THREADS; tid++) { compat_thread_join(thread[tid], NULL); }

    /* close the tags. */
    for(int tid = 0; tid < num_threads && tid < MAX_THREADS; tid++) { plc_tag_destroy(args[tid].tag); }

    /* check the status */
    for(int tid = 0; tid < num_threads && tid < MAX_THREADS; tid++) {
        if(args[tid].status != PLCTAG_STATUS_OK) { success = 0; }
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "--- All test threads terminated after running %" PRId64 "ms.\n", total_run_time);

    /* Calculate total requests and statistics */
    int32_t total_requests = 0;
    int32_t min_requests = INT32_MAX;
    int32_t max_requests = 0;
    int64_t avg_request_time = 0;
    int64_t total_request_time = 0;

    for(int tid = 0; tid < num_threads; tid++) {
        total_requests += args[tid].iteration;
        if(args[tid].iteration < min_requests) { min_requests = args[tid].iteration; }
        if(args[tid].iteration > max_requests) { max_requests = args[tid].iteration; }
        total_request_time += args[tid].total_io_time;
    }

    if(total_requests > 0) {
        avg_request_time = total_request_time / total_requests;
    }

    /* Print detailed statistics */
    // NOLINTNEXTLINE
    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════════╗\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "║        CONNECTION STRESS TEST (MULTIPLE CONNECTIONS)           ║\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "╠════════════════════════════════════════════════════════════════╣\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "║ Runtime: %" PRId64 " seconds                                      \n", total_run_time / 1000);
    // NOLINTNEXTLINE
    fprintf(stderr, "║ Total requests: %d                                              \n", total_requests);
    // NOLINTNEXTLINE
    fprintf(stderr, "║ Throughput: %.2f requests/sec                                    \n", (total_requests * 1000.0) / (total_run_time > 0 ? total_run_time : 1));
    // NOLINTNEXTLINE
    fprintf(stderr, "╠════════════════════════════════════════════════════════════════╣\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "║                   FAIRNESS ANALYSIS                           ║\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "╠════════════════════════════════════════════════════════════════╣\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "║ Thread    Requests      Min    Avg    Max    (ms per request)  ║\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "║ ─────────────────────────────────────────────────────────────  ║\n");

    for(int tid = 0; tid < num_threads; tid++) {
        int64_t avg = args[tid].iteration > 0 ? args[tid].total_io_time / args[tid].iteration : 0;
        // NOLINTNEXTLINE
        fprintf(stderr, "║  %3d     %6d        %4" PRId64 "   %4" PRId64 "   %4" PRId64 "                      ║\n",
                tid, args[tid].iteration, args[tid].min_io_time, avg, args[tid].max_io_time);
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "╠════════════════════════════════════════════════════════════════╣\n");

    /* Calculate fairness metrics */
    double fairness_ratio = max_requests > 0 ? (double)min_requests / (double)max_requests : 0.0;
    const char *fairness_assessment = "POOR";
    if(fairness_ratio >= 0.9) {
        fairness_assessment = "EXCELLENT";
    } else if(fairness_ratio >= 0.8) {
        fairness_assessment = "GOOD";
    } else if(fairness_ratio >= 0.7) {
        fairness_assessment = "FAIR";
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "║ Min Requests:   %d                                             \n", min_requests);
    // NOLINTNEXTLINE
    fprintf(stderr, "║ Max Requests:   %d                                             \n", max_requests);
    // NOLINTNEXTLINE
    fprintf(stderr, "║ Min/Max Ratio:  %.3f                                            \n", fairness_ratio);
    // NOLINTNEXTLINE
    fprintf(stderr, "║ Assessment:     %s                                             \n", fairness_assessment);
    // NOLINTNEXTLINE
    fprintf(stderr, "║ Average Req Time: %" PRId64 " ms                                  \n", avg_request_time);
    // NOLINTNEXTLINE
    fprintf(stderr, "╚════════════════════════════════════════════════════════════════╝\n\n");

    if(success) {
        clog_info(CLOG(0), "*** Test SUCCEEDED!");
        clog_free(0);
        return 0;
    } else {
        clog_info(CLOG(0), "*** Test FAILED!");
        clog_free(0);
        return -1;
    }
}
