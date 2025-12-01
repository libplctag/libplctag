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

#include "plc.h"
#include "compat.h"
#include "fairness.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>


/*
 * Fairness Statistics Dump
 * 
 * Dumps per-request latency statistics to help diagnose fairness issues.
 */
void dump_fairness_stats(plc_s *plc) {
    tag_def_s *tag = NULL;
    int tag_count = 0;
    int total_requests = 0;
    int min_requests = INT32_MAX;
    int max_requests = 0;
    double request_mean = 0.0;
    double request_variance = 0.0;
    double request_std_dev = 0.0;
    double request_cv = 0.0;
    
    double latency_mean = 0.0;
    double latency_variance = 0.0;
    double latency_std_dev = 0.0;
    double latency_cv = 0.0;
    int64_t global_min_latency = INT64_MAX;
    int64_t global_max_latency = 0;
    
    if (!plc || !plc->tags) {
        return;
    }
    
    /* Count tags first */
    tag = plc->tags;
    while (tag) {
        tag_count++;
        tag = tag->next_tag;
    }
    
    if (tag_count == 0) {
        return;
    }
    
    fprintf(stderr, "\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "AB Server Fairness Statistics\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Total tags: %d\n\n", tag_count);
    
    fprintf(stderr, "Per-Tag Latency Statistics:\n");
    fprintf(stderr, "---------------------------\n");
    fprintf(stderr, "%-30s %8s %10s %10s %10s\n", "Tag Name", "Requests", "Avg (us)", "Min (us)", "Max (us)");
    fprintf(stderr, "%-30s %8s %10s %10s %10s\n", "--------", "--------", "--------", "--------", "--------");
    
    /* First pass: dump individual tag stats and collect totals */
    tag = plc->tags;
    while (tag) {
        int requests = atomic_load_int32(&tag->request_count);
        int64_t total_latency = atomic_load_int64(&tag->total_latency_us);
        int64_t min_latency = atomic_load_int64(&tag->min_latency_us);
        int64_t max_latency = atomic_load_int64(&tag->max_latency_us);
        int64_t avg_latency = (requests > 0) ? (total_latency / requests) : 0;
        
        fprintf(stderr, "%-30s %8d %10lld %10lld %10lld\n",
                tag->name, requests, 
                (long long)avg_latency,
                (long long)min_latency, 
                (long long)max_latency);
        
        total_requests += requests;
        if (requests < min_requests) min_requests = requests;
        if (requests > max_requests) max_requests = requests;
        if (min_latency > 0 && min_latency < global_min_latency) global_min_latency = min_latency;
        if (max_latency > global_max_latency) global_max_latency = max_latency;
        
        tag = tag->next_tag;
    }
    
    /* Calculate request count mean */
    request_mean = (double)total_requests / tag_count;
    
    /* Second pass: calculate request count variance and latency stats */
    tag = plc->tags;
    double total_avg_latency = 0.0;
    int tags_with_requests = 0;
    
    while (tag) {
        int requests = atomic_load_int32(&tag->request_count);
        double diff = requests - request_mean;
        request_variance += diff * diff;
        
        if (requests > 0) {
            int64_t total_latency = atomic_load_int64(&tag->total_latency_us);
            double avg_latency = (double)total_latency / requests;
            total_avg_latency += avg_latency;
            tags_with_requests++;
        }
        
        tag = tag->next_tag;
    }
    
    request_variance /= tag_count;
    request_std_dev = sqrt(request_variance);
    request_cv = (request_mean > 0) ? (request_std_dev / request_mean * 100.0) : 0.0;
    
    /* Calculate mean of average latencies */
    if (tags_with_requests > 0) {
        latency_mean = total_avg_latency / tags_with_requests;
        
        /* Third pass: calculate latency variance across tags */
        tag = plc->tags;
        while (tag) {
            int requests = atomic_load_int32(&tag->request_count);
            if (requests > 0) {
                int64_t total_latency = atomic_load_int64(&tag->total_latency_us);
                double avg_latency = (double)total_latency / requests;
                double diff = avg_latency - latency_mean;
                latency_variance += diff * diff;
            }
            tag = tag->next_tag;
        }
        
        latency_variance /= tags_with_requests;
        latency_std_dev = sqrt(latency_variance);
        latency_cv = (latency_mean > 0) ? (latency_std_dev / latency_mean * 100.0) : 0.0;
    }
    
    fprintf(stderr, "\nRequest Count Fairness:\n");
    fprintf(stderr, "----------------------\n");
    fprintf(stderr, "  Total requests:              %d\n", total_requests);
    fprintf(stderr, "  Mean requests per tag:       %.2f\n", request_mean);
    fprintf(stderr, "  Min requests:                %d\n", min_requests);
    fprintf(stderr, "  Max requests:                %d\n", max_requests);
    fprintf(stderr, "  Standard deviation:          %.2f\n", request_std_dev);
    fprintf(stderr, "  Coefficient of variation:    %.2f%%\n", request_cv);
    if (max_requests > 0) {
        fprintf(stderr, "  Min/Max ratio:               %.3f\n", (double)min_requests / max_requests);
    }
    
    fprintf(stderr, "\nLatency Fairness:\n");
    fprintf(stderr, "-----------------\n");
    fprintf(stderr, "  Mean avg latency per tag:    %.2f us\n", latency_mean);
    fprintf(stderr, "  Global min latency:          %lld us\n", (long long)global_min_latency);
    fprintf(stderr, "  Global max latency:          %lld us\n", (long long)global_max_latency);
    fprintf(stderr, "  Latency std deviation:       %.2f us\n", latency_std_dev);
    fprintf(stderr, "  Latency CV:                  %.2f%%\n", latency_cv);
    if (global_max_latency > 0 && global_min_latency < INT64_MAX) {
        fprintf(stderr, "  Min/Max latency ratio:       %.3f\n", (double)global_min_latency / (double)global_max_latency);
    }
    
    fprintf(stderr, "\nFairness Assessment:\n");
    fprintf(stderr, "-------------------\n");
    if (request_cv < 5.0) {
        fprintf(stderr, "  Request distribution: EXCELLENT (CV < 5%%)\n");
    } else if (request_cv < 10.0) {
        fprintf(stderr, "  Request distribution: GOOD (CV < 10%%)\n");
    } else if (request_cv < 20.0) {
        fprintf(stderr, "  Request distribution: ACCEPTABLE (CV < 20%%)\n");
    } else {
        fprintf(stderr, "  Request distribution: POOR (CV >= 20%%)\n");
    }
    
    if (latency_cv < 5.0) {
        fprintf(stderr, "  Latency fairness: EXCELLENT (CV < 5%%)\n");
    } else if (latency_cv < 10.0) {
        fprintf(stderr, "  Latency fairness: GOOD (CV < 10%%)\n");
    } else if (latency_cv < 20.0) {
        fprintf(stderr, "  Latency fairness: ACCEPTABLE (CV < 20%%)\n");
    } else {
        fprintf(stderr, "  Latency fairness: POOR (CV >= 20%%)\n");
    }
    
    fprintf(stderr, "========================================\n\n");
}
