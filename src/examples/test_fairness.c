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
 * Fairness Test for Tag Scheduling
 *
 * Creates N identical tags with auto-sync read and measures:
 * 1. Total reads per tag over test duration
 * 2. Standard deviation of read counts (lower = more fair)
 * 3. Min/Max read ratio (closer to 1.0 = more fair)
 * 4. Time between consecutive reads per tag
 *
 * Expected: All tags should get similar read counts.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>

#define DEFAULT_NUM_TAGS 200           /* Number of tags to test */
#define DEFAULT_TEST_DURATION_SECS 10  /* Test duration in seconds */
#define TAG_CREATE_TIMEOUT_MS 10000    /* Timeout for tags to become ready */

typedef struct {
    compat_atomic_int32_t tag_id;
    compat_atomic_int32_t read_started_count;
    compat_atomic_int32_t read_completed_count;
    compat_atomic_int32_t read_failed_count;
    compat_atomic_int64_t ready_time;
    compat_atomic_int64_t last_read_time;
    compat_atomic_int64_t total_wait_time;
    compat_atomic_int64_t max_wait_time;
    compat_atomic_int64_t min_wait_time;
} tag_stats_t;

void usage(const char *prog_name) {
    printf(
        "Usage:\n"
        " %s --tag=<tag_string> [--num-tags=N] [--test-duration-secs=N]\n"
        "  --tag=<tag_string> - The complete tag string (required)\n"
        "  --num-tags=N - Number of identical tags to create (default: %d)\n"
        "  --test-duration-secs=N - Test duration in seconds (default: %d)\n"
        "\n"
        "Arguments can be in any order.\n"
        "\n"
        "Example:\n"
        "  test_fairness --tag='protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=TestBigArray[0]&auto_sync_read_ms=200' --num-tags=200 --test-duration-secs=10\n",
        prog_name, DEFAULT_NUM_TAGS, DEFAULT_TEST_DURATION_SECS);

    exit(1);
}

/* Calculate standard deviation of read counts */
double calculate_std_dev(tag_stats_t *stats, int count) {
    double mean = 0.0;
    double variance = 0.0;

    for (int i = 0; i < count; i++) {
        mean += compat_atomic_load_int32(&stats[i].read_completed_count);
    }
    mean /= count;

    for (int i = 0; i < count; i++) {
        double diff = compat_atomic_load_int32(&stats[i].read_completed_count) - mean;
        variance += diff * diff;
    }

    variance /= count;
    return sqrt(variance);
}

/* Event callback to track reads */
void tag_callback(int32_t tag_id, int event, int status, void *userdata) {
    (void)tag_id; /* tag_id is not used but kept for callback signature compatibility */
    tag_stats_t *stats = (tag_stats_t *)userdata;
    int64_t now = compat_time_ms();

    switch (event) {
        case PLCTAG_EVENT_CREATED:
            compat_atomic_store_int64(&stats->ready_time, now);
            break;
        case PLCTAG_EVENT_READ_STARTED:
            compat_atomic_inc_int32(&stats->read_started_count);
            break;

        case PLCTAG_EVENT_READ_COMPLETED:
            if (status == PLCTAG_STATUS_OK) {
                compat_atomic_inc_int32(&stats->read_completed_count);

                /* Calculate wait time since last read */
                int64_t last_time = compat_atomic_load_int64(&stats->last_read_time);
                if (last_time > 0) {
                    int64_t wait = now - last_time;
                    compat_atomic_add_int64(&stats->total_wait_time, wait);

                    /* Update max wait time if needed (simple store, race is benign) */
                    int64_t current_max = compat_atomic_load_int64(&stats->max_wait_time);
                    if (wait > current_max) {
                        compat_atomic_store_int64(&stats->max_wait_time, wait);
                    }

                    /* Update min wait time if needed (simple store, race is benign) */
                    int64_t current_min = compat_atomic_load_int64(&stats->min_wait_time);
                    if (current_min == 0 || wait < current_min) {
                        compat_atomic_store_int64(&stats->min_wait_time, wait);
                    }
                }

                compat_atomic_store_int64(&stats->last_read_time, now);
            } else {
                compat_atomic_inc_int32(&stats->read_failed_count);
            }
            break;

        default:
            break;
    }
}

int main(int argc, char **argv) {
    tag_stats_t *stats = NULL;
    int rc = PLCTAG_STATUS_OK;
    int64_t start_time, end_time;
    int num_tags = DEFAULT_NUM_TAGS;
    int test_duration_secs = DEFAULT_TEST_DURATION_SECS;
    const char *tag_string = NULL;

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /* Parse command line arguments in any order */
    if (argc < 2) {
        usage(argv[0]);
    }

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--tag=", 6) == 0) {
            tag_string = argv[i] + 6;
        } else if (strncmp(argv[i], "--num-tags=", 11) == 0) {
            num_tags = atoi(argv[i] + 11);
            if (num_tags <= 0) {
                fprintf(stderr, "Number of tags must be greater than zero!\n");
                usage(argv[0]);
            }
        } else if (strncmp(argv[i], "--test-duration-secs=", 21) == 0) {
            test_duration_secs = atoi(argv[i] + 21);
            if (test_duration_secs <= 0) {
                fprintf(stderr, "Test duration must be greater than zero!\n");
                usage(argv[0]);
            }
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
        }
    }

    /* Verify required arguments */
    if (!tag_string) {
        fprintf(stderr, "Error: --tag argument is required!\n");
        usage(argv[0]);
    }

    int test_duration_ms = test_duration_secs * 1000;

    /* Allocate dynamic arrays */
    stats = calloc((size_t)num_tags, sizeof(*stats));
    if (!stats) {
        fprintf(stderr, "Error allocating stats array!\n");
        return 1;
    }

    fprintf(stderr, "Fairness Test\n");
    fprintf(stderr, "=============\n");
    fprintf(stderr, "Tags: %d\n", num_tags);
    fprintf(stderr, "Tag string: %s\n", tag_string);
    fprintf(stderr, "Test duration: %d seconds\n\n", test_duration_secs);

    /* Create all tags with the same tag string */
    fprintf(stderr, "Creating tags...\n");
    for (int i = 0; i < num_tags; i++) {
        /* create the tags async */
        int32_t tag_id = plc_tag_create_ex(tag_string, tag_callback, &stats[i], 0);
        if (tag_id < 0) {
            fprintf(stderr, "Failed to create tag %d: %s\n", i, plc_tag_decode_error(tag_id));
            free(stats);
            return 1;
        }

        /* initialize stats with tag ID - must be set AFTER tag creation */
        compat_atomic_store_int32(&stats[i].tag_id, tag_id);

        if ((i + 1) % 10 == 0) {
            fprintf(stderr, "  Created %d tags...\n", i + 1);
        }
    }

    /* Wait for all tags to be ready */
    fprintf(stderr, "\nWaiting for all tags to become ready...\n");
    int64_t tag_create_timeout = compat_time_ms() + TAG_CREATE_TIMEOUT_MS;
    int all_ready = 0;
    while (compat_time_ms() < tag_create_timeout) {
        all_ready = 1;
        for (int i = 0; i < num_tags; i++) {
            if(compat_atomic_load_int64(&stats[i].ready_time) == 0) {
                all_ready = 0;
                break;
            }
        }

        if (all_ready) {
            break;
        }

        compat_sleep_ms(100, NULL);
    }

    if (!all_ready) {
        fprintf(stderr, "Warning: Not all tags became ready before timeout!\n");
        for (int i = 0; i < num_tags; i++) {
            int32_t tag_id = compat_atomic_load_int32(&stats[i].tag_id);
            int status = plc_tag_status(tag_id);
            if (status != PLCTAG_STATUS_OK) {
                fprintf(stderr, "  Tag %d: %s\n", i, plc_tag_decode_error(status));
            }
        }
    } else {
        fprintf(stderr, "All tags ready.\n");
    }

    fprintf(stderr, "\nRunning test for %d seconds...\n", test_duration_secs);
    start_time = compat_time_ms();

    /* Wait for test duration */
    compat_sleep_ms((uint32_t)test_duration_ms, NULL);

    end_time = compat_time_ms();
    int64_t actual_duration = end_time - start_time;

    fprintf(stderr, "Test complete. Actual duration: %lld ms\n\n", (long long)actual_duration);

    /* make sure we flush the output*/
    fflush(stderr);

    /* Collect final statistics */
    fprintf(stderr, "Per-Tag Results:\n");
    fprintf(stderr, "----------------\n");

    int total_reads = 0;
    int total_started = 0;
    int min_reads = 999999;
    int max_reads = 0;

    for (int i = 0; i < num_tags; i++) {
        int32_t tag_id = compat_atomic_load_int32(&stats[i].tag_id);
        int32_t started = compat_atomic_load_int32(&stats[i].read_started_count);
        int32_t completed = compat_atomic_load_int32(&stats[i].read_completed_count);
        int32_t failed = compat_atomic_load_int32(&stats[i].read_failed_count);
        int64_t total_wait = compat_atomic_load_int64(&stats[i].total_wait_time);
        int64_t min_wait = compat_atomic_load_int64(&stats[i].min_wait_time);
        int64_t max_wait = compat_atomic_load_int64(&stats[i].max_wait_time);

        fprintf(stderr, "Tag %2d (ID=%d): started=%d, completed=%d, failed=%d, avg_wait=%" PRId64 "ms, min_wait=%" PRId64 "ms, max_wait=%" PRId64 "ms\n",
               i, tag_id, started, completed, failed,
               completed > 1 ? (long long)(total_wait / (completed - 1)) : 0,
               (long long)min_wait,
               (long long)max_wait);

        total_reads += completed;
        total_started += started;
        if (completed < min_reads) min_reads = completed;
        if (completed > max_reads) max_reads = completed;
    }

    /* Calculate fairness metrics */
    fprintf(stderr, "\nFairness Metrics:\n");
    fprintf(stderr, "-----------------\n");

    double mean = (double)total_reads / num_tags;
    double std_dev = calculate_std_dev(stats, num_tags);
    double coefficient_variation = (std_dev / mean) * 100.0;
    double min_max_ratio = (min_reads > 0) ? ((double)min_reads / max_reads) : 0.0;

    fprintf(stderr, "Total started: %d\n", total_started);
    fprintf(stderr, "Total completed: %d\n", total_reads);
    fprintf(stderr, "Mean reads per tag: %.2f\n", mean);
    fprintf(stderr, "Min reads: %d\n", min_reads);
    fprintf(stderr, "Max reads: %d\n", max_reads);
    fprintf(stderr, "Standard deviation: %.2f\n", std_dev);
    fprintf(stderr, "Coefficient of variation: %.2f%%\n", coefficient_variation);
    fprintf(stderr, "Min/Max ratio: %.3f\n", min_max_ratio);

    /* Fairness assessment */
    fprintf(stderr, "\nFairness Assessment:\n");
    fprintf(stderr, "--------------------\n");
    if (coefficient_variation < 5.0) {
        fprintf(stderr, "EXCELLENT: Very fair distribution (CV < 5%%)\n");
    } else if (coefficient_variation < 10.0) {
        fprintf(stderr, "GOOD: Fair distribution (CV < 10%%)\n");
    } else if (coefficient_variation < 20.0) {
        fprintf(stderr, "ACCEPTABLE: Moderate fairness (CV < 20%%)\n");
    } else {
        fprintf(stderr, "POOR: Unfair distribution (CV >= 20%%)\n");
        rc = PLCTAG_ERR_BAD_STATUS;
    }

    if (min_max_ratio > 0.9) {
        fprintf(stderr, "EXCELLENT: Min/Max ratio > 0.9\n");
    } else if (min_max_ratio > 0.8) {
        fprintf(stderr, "GOOD: Min/Max ratio > 0.8\n");
    } else if (min_max_ratio > 0.7) {
        fprintf(stderr, "ACCEPTABLE: Min/Max ratio > 0.7\n");
    } else {
        fprintf(stderr, "POOR: Min/Max ratio <= 0.7\n");
        rc = PLCTAG_ERR_BAD_STATUS;
    }

    /* Cleanup */
    fprintf(stderr, "\nCleaning up...\n");
    for (int i = 0; i < num_tags; i++) {
        int32_t tag_id = compat_atomic_load_int32(&stats[i].tag_id);
        plc_tag_destroy(tag_id);
    }

    free(stats);

    fprintf(stderr, "Done.\n");

    fflush(stderr);

    return (rc == PLCTAG_STATUS_OK) ? 0 : 1;
}
