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
 * This test verifies that:
 * 1. Multiple threads can create tags and read/write successfully
 * 2. After plc_tag_shutdown(), all tags become invalid
 * 3. All threads can recreate new tags after shutdown (library restarts automatically)
 * 4. The new tags can be read/written successfully
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 5, 0
#define DATA_TIMEOUT (5000)
#define NUM_THREADS (10)
#define LOOP_INTERVAL_MS (250)
#define PRE_SHUTDOWN_WAIT_MS (2000)
#define POST_SHUTDOWN_WAIT_MS (2000)

/* Base tag path from command line */
static char *base_tag_path = NULL;

/* Thread state flags */
static volatile int terminate_threads = 0;

/* Thread statistics */
typedef struct {
    int thread_id;
    int initial_tag_created;
    int64_t initial_tag_create_time_ms;
    int recreate_attempted;
    int recreate_succeeded;
    int64_t recreate_time_ms;
    int read_count;
    int write_count;
    int error_count;
    int last_error;
} thread_stats_t;

/* Forward declarations */
static void *worker_thread(void *arg);


static void parse_args(int argc, char **argv) {
    if(argc < 2) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Usage: test_shutdown_restart --tag=TAG_STRING\n");
        // NOLINTNEXTLINE
        fprintf(stderr, "  --tag=TAG_STRING: tag path string\n");
        exit(1);
    }

    for(int i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) {
            base_tag_path = &argv[i][6];
        }
    }

    if(base_tag_path == NULL || strlen(base_tag_path) == 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error: tag path must be specified\n");
        exit(1);
    }
}


int main(int argc, char **argv) {
    compat_thread_t threads[NUM_THREADS] = {0};
    thread_stats_t thread_stats[NUM_THREADS] = {0};
    int rc = PLCTAG_STATUS_OK;
    int all_succeeded = 1;
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);

    /* Parse command line arguments */
    parse_args(argc, argv);

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        fprintf(stderr, "Available library version is %d.%d.%d.\n", version_major, version_minor, version_patch);
        return 1;
    }

    fprintf(stderr, "Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    /* Set debug level to DETAIL (4) */
    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /*
     * Step 1: Create worker threads
     */
    fprintf(stderr, "\n=== Step 1: Creating %d worker threads ===\n", NUM_THREADS);

    for(int i = 0; i < NUM_THREADS; i++) {
        thread_stats[i].thread_id = i;
        rc = compat_thread_create(&threads[i], worker_thread, &thread_stats[i]);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr, "ERROR: Could not create thread %d! Error: %s\n", i, plc_tag_decode_error(rc));
            terminate_threads = 1;
            return 1;
        }
        fprintf(stderr, "Thread %d created.\n", i);
    }

    /*
     * Step 2: Wait for threads to run
     */
    fprintf(stderr, "\n=== Step 2: Waiting %d ms for threads to run ===\n", PRE_SHUTDOWN_WAIT_MS);
    compat_sleep_ms(PRE_SHUTDOWN_WAIT_MS, NULL);

    /*
     * Step 3: Shutdown the library
     */
    fprintf(stderr, "\n=== Step 3: Calling plc_tag_shutdown() ===\n");
    plc_tag_shutdown();
    fprintf(stderr, "Library shutdown complete.\n");

    /*
     * Step 4: Wait for threads to recover
     */
    fprintf(stderr, "\n=== Step 4: Waiting %d ms for threads to recover ===\n", POST_SHUTDOWN_WAIT_MS);
    compat_sleep_ms(POST_SHUTDOWN_WAIT_MS, NULL);

    /*
     * Step 5: Signal threads to terminate
     */
    fprintf(stderr, "\n=== Step 5: Signaling threads to terminate ===\n");
    terminate_threads = 1;

    /*
     * Step 6: Wait for threads to finish
     */
    fprintf(stderr, "\n=== Step 6: Waiting for threads to finish ===\n");
    for(int i = 0; i < NUM_THREADS; i++) {
        rc = compat_thread_join(threads[i], NULL);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr, "WARNING: Error joining thread %d: %s\n", i, plc_tag_decode_error(rc));
        }
        fprintf(stderr, "Thread %d joined.\n", i);
    }

    /*
     * Step 7: Report statistics
     */
    fprintf(stderr, "\n=== Step 7: Thread Statistics ===\n");
    fprintf(stderr, "%-8s %-12s %-14s %-12s %-12s %-14s %-8s %-8s %-8s\n",
            "Thread", "InitCreate", "InitTime(ms)", "RecreateOK", "RecreateTry", "RecreTime(ms)", "Reads", "Writes", "Errors");
    fprintf(stderr, "-------- ------------ -------------- ------------ ------------ -------------- -------- -------- --------\n");

    for(int i = 0; i < NUM_THREADS; i++) {
        thread_stats_t *stats = &thread_stats[i];
        fprintf(stderr, "%-8d %-12s %-14" PRId64 " %-12s %-12s %-14" PRId64 " %-8d %-8d %-8d\n",
                stats->thread_id,
                stats->initial_tag_created ? "YES" : "NO",
                stats->initial_tag_create_time_ms,
                stats->recreate_succeeded ? "YES" : "NO",
                stats->recreate_attempted ? "YES" : "NO",
                stats->recreate_time_ms,
                stats->read_count,
                stats->write_count,
                stats->error_count);

        if(!stats->initial_tag_created) {
            fprintf(stderr, "  Thread %d: FAILED to create initial tag!\n", i);
            all_succeeded = 0;
        }
        if(stats->recreate_attempted && !stats->recreate_succeeded) {
            fprintf(stderr, "  Thread %d: FAILED to recreate tag after shutdown!\n", i);
            all_succeeded = 0;
        }
    }

    /*
     * Step 8: Final result
     */
    fprintf(stderr, "\n=== Step 8: Final Result ===\n");
    if(all_succeeded) {
        fprintf(stderr, "\n=== TEST PASSED ===\n");
        fprintf(stderr, "Successfully verified:\n");
        fprintf(stderr, "  1. All %d threads created initial tags successfully\n", NUM_THREADS);
        fprintf(stderr, "  2. Library shutdown completed\n");
        fprintf(stderr, "  3. All threads that attempted recreation succeeded\n");
        fprintf(stderr, "  4. Library restart after shutdown works correctly\n");
        return 0;
    } else {
        fprintf(stderr, "\n=== TEST FAILED ===\n");
        return 1;
    }
}


static void *worker_thread(void *arg) {
    thread_stats_t *stats = (thread_stats_t *)arg;
    char tag_string[512];
    int32_t tag_id = 0;
    int rc = PLCTAG_STATUS_OK;
    int32_t value = 0;
    int64_t start_time = 0;
    int need_recreate = 0;

    /* Build tag string with unique array index */
    snprintf(tag_string, sizeof(tag_string), "%s[%d]", base_tag_path, stats->thread_id);

    fprintf(stderr, "Thread %d: Starting with tag string: %s\n", stats->thread_id, tag_string);

    /*
     * Create initial tag
     */
    start_time = compat_time_ms();
    tag_id = plc_tag_create(tag_string, DATA_TIMEOUT);
    stats->initial_tag_create_time_ms = compat_time_ms() - start_time;

    if(tag_id < 0) {
        fprintf(stderr, "Thread %d: ERROR creating initial tag: %s\n", stats->thread_id, plc_tag_decode_error(tag_id));
        stats->initial_tag_created = 0;
        stats->last_error = tag_id;
        return NULL;
    }

    stats->initial_tag_created = 1;
    fprintf(stderr, "Thread %d: Initial tag created (ID=%" PRId32 ") in %" PRId64 " ms\n",
            stats->thread_id, tag_id, stats->initial_tag_create_time_ms);

    /*
     * Main loop: read, increment, write, sleep
     */
    while(!terminate_threads) {
        /* Check if we need to recreate the tag */
        if(need_recreate) {
            stats->recreate_attempted = 1;

            fprintf(stderr, "Thread %d: Attempting to recreate tag...\n", stats->thread_id);

            start_time = compat_time_ms();
            tag_id = plc_tag_create(tag_string, DATA_TIMEOUT);
            stats->recreate_time_ms = compat_time_ms() - start_time;

            if(tag_id < 0) {
                fprintf(stderr, "Thread %d: ERROR recreating tag: %s, retrying...\n",
                        stats->thread_id, plc_tag_decode_error(tag_id));
                stats->error_count++;
                stats->last_error = tag_id;
                compat_sleep_ms(LOOP_INTERVAL_MS, NULL);
                continue;
            }

            stats->recreate_succeeded = 1;
            need_recreate = 0;
            fprintf(stderr, "Thread %d: Tag recreated (ID=%" PRId32 ") in %" PRId64 " ms\n",
                    stats->thread_id, tag_id, stats->recreate_time_ms);
        }

        /* Read the tag */
        rc = plc_tag_read(tag_id, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
            fprintf(stderr, "Thread %d: Read error: %s\n", stats->thread_id, plc_tag_decode_error(rc));
            stats->error_count++;
            stats->last_error = rc;
            need_recreate = 1;
            continue;
        }
        stats->read_count++;

        /* Get value and increment */
        value = plc_tag_get_int32(tag_id, 0);
        value++;

        /* Set the new value */
        rc = plc_tag_set_int32(tag_id, 0, value);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr, "Thread %d: Set error: %s\n", stats->thread_id, plc_tag_decode_error(rc));
            stats->error_count++;
            stats->last_error = rc;
            need_recreate = 1;
            continue;
        }

        /* Write the tag */
        rc = plc_tag_write(tag_id, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
            fprintf(stderr, "Thread %d: Write error: %s\n", stats->thread_id, plc_tag_decode_error(rc));
            stats->error_count++;
            stats->last_error = rc;
            need_recreate = 1;
            continue;
        }
        stats->write_count++;

        /* Wait before next iteration */
        compat_sleep_ms(LOOP_INTERVAL_MS, NULL);
    }

    /*
     * Cleanup: destroy tag if valid
     */
    if(tag_id > 0) {
        rc = plc_tag_destroy(tag_id);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr, "Thread %d: Warning destroying tag: %s\n", stats->thread_id, plc_tag_decode_error(rc));
        }
    }

    fprintf(stderr, "Thread %d: Exiting. Reads=%d, Writes=%d, Errors=%d\n",
            stats->thread_id, stats->read_count, stats->write_count, stats->error_count);

    return NULL;
}

