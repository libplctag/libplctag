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
 * Fairness Test for AB/ControlLogix Tag Scheduling
 * 
 * Creates N tags with auto-sync read enabled and measures:
 * 1. Total reads per tag over test duration
 * 2. Standard deviation of read counts (lower = more fair)
 * 3. Min/Max read ratio (closer to 1.0 = more fair)
 * 4. Time between consecutive reads per tag
 * 
 * Expected: With vector sorting, all tags should get similar read counts.
 * Old ring rotation: Tags added later starve initially.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>

#define NUM_TAGS 50              /* Number of tags to test */
#define AUTO_SYNC_MS 200         /* Auto-sync interval */
#define TEST_DURATION_MS 10000   /* 10 second test */

typedef struct {
    compat_atomic_int32_t tag_id;
    compat_atomic_int32_t read_started_count;
    compat_atomic_int32_t read_completed_count;
    compat_atomic_int64_t last_read_time;
    compat_atomic_int64_t total_wait_time;
    compat_atomic_int64_t max_wait_time;
    compat_atomic_int64_t min_wait_time;
} tag_stats_t;

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
    tag_stats_t *stats = (tag_stats_t *)userdata;
    int64_t now = compat_time_ms();
    
    /* Find this tag's stats by matching tag_id */
    for (int i = 0; i < NUM_TAGS; i++) {
        if (compat_atomic_load_int32(&stats[i].tag_id) == tag_id) {
            if (event == PLCTAG_EVENT_READ_STARTED) {
                compat_atomic_inc_int32(&stats[i].read_started_count);
            } else if (event == PLCTAG_EVENT_READ_COMPLETED && status == PLCTAG_STATUS_OK) {
                compat_atomic_inc_int32(&stats[i].read_completed_count);
                
                /* Calculate wait time since last read */
                int64_t last_time = compat_atomic_load_int64(&stats[i].last_read_time);
                if (last_time > 0) {
                    int64_t wait = now - last_time;
                    compat_atomic_add_int64(&stats[i].total_wait_time, wait);
                    
                    /* Update max wait time if needed (simple store, race is benign) */
                    int64_t current_max = compat_atomic_load_int64(&stats[i].max_wait_time);
                    if (wait > current_max) {
                        compat_atomic_store_int64(&stats[i].max_wait_time, wait);
                    }
                    
                    /* Update min wait time if needed (simple store, race is benign) */
                    int64_t current_min = compat_atomic_load_int64(&stats[i].min_wait_time);
                    if (current_min == 0 || wait < current_min) {
                        compat_atomic_store_int64(&stats[i].min_wait_time, wait);
                    }
                }
                
                compat_atomic_store_int64(&stats[i].last_read_time, now);
            }
            break;
        }
    }
}

int main(void) {
    tag_stats_t stats[NUM_TAGS];
    int32_t tags[NUM_TAGS];
    char tag_string[256];
    int rc = PLCTAG_STATUS_OK;
    int64_t start_time, end_time;
    
    /* Clear entire stats structure first to ensure zeros */
    memset(stats, 0, sizeof(stats));
    memset(tags, 0, sizeof(tags));
    
    fprintf(stderr, "AB/ControlLogix Fairness Test\n");
    fprintf(stderr, "==============================\n");
    fprintf(stderr, "Tags: %d\n", NUM_TAGS);
    fprintf(stderr, "Auto-sync interval: %d ms\n", AUTO_SYNC_MS);
    fprintf(stderr, "Test duration: %d ms\n\n", TEST_DURATION_MS);
    
    /* Create all tags with auto-sync read */
    fprintf(stderr, "Creating tags...\n");
    for (int i = 0; i < NUM_TAGS; i++) {
        snprintf(tag_string, sizeof(tag_string),
                 "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix"
                 "&elem_count=1&name=TestBigArray[%d]&auto_sync_read_ms=%d",
                 i, AUTO_SYNC_MS);
        
        tags[i] = plc_tag_create_ex(tag_string, tag_callback, &stats, 5000);
        if (tags[i] < 0) {
            fprintf(stderr, "Failed to create tag %d: %s\n", i, plc_tag_decode_error(tags[i]));
            return 1;
        }
        
        /* Initialize stats atomically - tag_id set last so callback can safely check it */
        compat_atomic_store_int32(&stats[i].read_started_count, 0);
        compat_atomic_store_int32(&stats[i].read_completed_count, 0);
        compat_atomic_store_int64(&stats[i].last_read_time, 0);
        compat_atomic_store_int64(&stats[i].total_wait_time, 0);
        compat_atomic_store_int64(&stats[i].max_wait_time, 0);
        compat_atomic_store_int64(&stats[i].min_wait_time, 0);
        compat_atomic_store_int32(&stats[i].tag_id, tags[i]);  /* Set tag_id last */
        
        if ((i + 1) % 10 == 0) {
            fprintf(stderr, "  Created %d tags...\n", i + 1);
        }
    }
    
    fprintf(stderr, "\nRunning test for %d ms...\n", TEST_DURATION_MS);
    start_time = compat_time_ms();
    
    /* Wait for test duration */
    compat_sleep_ms(TEST_DURATION_MS, NULL);
    
    end_time = compat_time_ms();
    int64_t actual_duration = end_time - start_time;
    
    fprintf(stderr, "Test complete. Actual duration: %lld ms\n\n", (long long)actual_duration);
    
    /* Collect final statistics */
    fprintf(stderr, "Per-Tag Results:\n");
    fprintf(stderr, "----------------\n");
    int total_reads = 0;
    int total_started = 0;
    int min_reads = 999999;
    int max_reads = 0;
    
    for (int i = 0; i < NUM_TAGS; i++) {
        int32_t tag_id = compat_atomic_load_int32(&stats[i].tag_id);
        int32_t started = compat_atomic_load_int32(&stats[i].read_started_count);
        int32_t completed = compat_atomic_load_int32(&stats[i].read_completed_count);
        int64_t total_wait = compat_atomic_load_int64(&stats[i].total_wait_time);
        int64_t min_wait = compat_atomic_load_int64(&stats[i].min_wait_time);
        int64_t max_wait = compat_atomic_load_int64(&stats[i].max_wait_time);
        
        fprintf(stderr, "Tag %2d (ID=%d): started=%d, completed=%d, avg_wait=%lld ms, min_wait=%lld ms, max_wait=%lld ms\n",
               i, tag_id, started, completed,
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
    
    double mean = (double)total_reads / NUM_TAGS;
    double std_dev = calculate_std_dev(stats, NUM_TAGS);
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
    for (int i = 0; i < NUM_TAGS; i++) {
        plc_tag_destroy(tags[i]);
    }
    
    fprintf(stderr, "Done.\n");
    return (rc == PLCTAG_STATUS_OK) ? 0 : 1;
}
