/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
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

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "stats.h"

/* Comparison function for qsort */
static int compare_ints(const void *a, const void *b) { return (*(const int *)a) - (*(const int *)b); }

int stats_calculate(const int *values, int count, stats_summary_t *summary) {
    if(!values || !summary || count <= 0) { return -1; }

    /* Allocate sorted copy for quartile calculations */
    int *sorted = malloc((size_t)count * sizeof(int));
    if(!sorted) { return -1; }
    memcpy(sorted, values, (size_t)count * sizeof(int));
    qsort(sorted, (size_t)count, sizeof(int), compare_ints);

    /* Initialize summary */
    memset(summary, 0, sizeof(*summary));
    summary->count = count;
    summary->min = sorted[0];
    summary->max = sorted[count - 1];

    /* Calculate mean */
    long long sum = 0;
    for(int i = 0; i < count; i++) { sum += values[i]; }
    summary->mean = (double)sum / count;

    /* Calculate variance and standard deviation */
    double variance_sum = 0.0;
    for(int i = 0; i < count; i++) {
        double diff = (double)values[i] - summary->mean;
        variance_sum += diff * diff;
    }
    summary->variance = variance_sum / count;
    summary->std_dev = sqrt(summary->variance);

    /* Coefficient of variation (as percentage) */
    if(summary->mean > 0.0) {
        summary->cv = (summary->std_dev / summary->mean) * 100.0;
    } else {
        summary->cv = 0.0;
    }

    /* Min/Max ratio */
    if(summary->max > 0) {
        summary->min_max_ratio = (double)summary->min / (double)summary->max;
    } else {
        summary->min_max_ratio = 0.0;
    }

    /* Quartiles (using sorted array) */
    /* Q1 = 25th percentile */
    int q1_idx = count / 4;
    summary->q1 = sorted[q1_idx];

    /* Median = 50th percentile */
    if(count % 2 == 0) {
        summary->median = (sorted[count / 2 - 1] + sorted[count / 2]) / 2;
    } else {
        summary->median = sorted[count / 2];
    }

    /* Q3 = 75th percentile */
    int q3_idx = (3 * count) / 4;
    summary->q3 = sorted[q3_idx];

    /* IQR */
    summary->iqr = summary->q3 - summary->q1;

    free(sorted);
    return 0;
}

void stats_print_summary(FILE *stream, const stats_summary_t *summary) {
    if(!stream || !summary) { return; }

    fprintf(stream, "\nStatistics Summary:\n");
    fprintf(stream, "-------------------\n");
    fprintf(stream, "Count:              %d\n", summary->count);
    fprintf(stream, "Min:                %d\n", summary->min);
    fprintf(stream, "Max:                %d\n", summary->max);
    fprintf(stream, "Mean:               %.2f\n", summary->mean);
    fprintf(stream, "Std Dev:            %.2f\n", summary->std_dev);
    fprintf(stream, "Variance:           %.2f\n", summary->variance);
    fprintf(stream, "CV:                 %.2f%%\n", summary->cv);
    fprintf(stream, "Min/Max Ratio:      %.3f\n", summary->min_max_ratio);
    fprintf(stream, "Q1 (25%%):           %d\n", summary->q1);
    fprintf(stream, "Median (50%%):       %d\n", summary->median);
    fprintf(stream, "Q3 (75%%):           %d\n", summary->q3);
    fprintf(stream, "IQR (Q3-Q1):        %d\n", summary->iqr);
}

void stats_print_histogram(FILE *stream, const int *values, int count, int num_buckets, int max_bar_width) {
    if(!stream || !values || count <= 0) { return; }

    /* Find min/max */
    int min_val = values[0];
    int max_val = values[0];
    for(int i = 1; i < count; i++) {
        if(values[i] < min_val) { min_val = values[i]; }
        if(values[i] > max_val) { max_val = values[i]; }
    }

    /* Auto-determine buckets if not specified */
    if(num_buckets <= 0) {
        int range = max_val - min_val + 1;
        if(range <= 10) {
            num_buckets = range; /* One bucket per value */
        } else if(range <= 20) {
            num_buckets = 10;
        } else {
            num_buckets = 15;
        }
    }

    if(max_bar_width <= 0) { max_bar_width = 40; }

    /* Allocate bucket counts */
    int *buckets = calloc((size_t)num_buckets, sizeof(int));
    if(!buckets) { return; }

    /* Calculate bucket size */
    int range = max_val - min_val;
    double bucket_size = (range > 0) ? ((double)range / num_buckets) : 1.0;
    if(bucket_size < 1.0) { bucket_size = 1.0; }

    /* Count values in each bucket */
    int max_bucket_count = 0;
    for(int i = 0; i < count; i++) {
        int bucket = (int)((values[i] - min_val) / bucket_size);
        if(bucket >= num_buckets) { bucket = num_buckets - 1; }
        buckets[bucket]++;
        if(buckets[bucket] > max_bucket_count) { max_bucket_count = buckets[bucket]; }
    }

    /* Print histogram */
    fprintf(stream, "\nHistogram:\n");
    fprintf(stream, "----------\n");

    for(int b = 0; b < num_buckets; b++) {
        int bucket_start = min_val + (int)(b * bucket_size);
        int bucket_end = min_val + (int)((b + 1) * bucket_size) - 1;
        if(b == num_buckets - 1) { bucket_end = max_val; }

        /* Calculate bar length */
        int bar_len = 0;
        if(max_bucket_count > 0) { bar_len = (buckets[b] * max_bar_width) / max_bucket_count; }

        /* Print bucket range and bar */
        if(bucket_start == bucket_end) {
            fprintf(stream, "%4d:     ", bucket_start);
        } else {
            fprintf(stream, "%4d-%4d: ", bucket_start, bucket_end);
        }

        for(int i = 0; i < bar_len; i++) { fprintf(stream, "#"); }
        fprintf(stream, " (%d)\n", buckets[b]);
    }

    free(buckets);
}

int stats_assess_fairness(const stats_summary_t *summary, FILE *stream) {
    if(!summary) { return -1; }

    int is_fair = 1;

    if(stream) {
        fprintf(stream, "\nFairness Assessment:\n");
        fprintf(stream, "--------------------\n");

        /* CV assessment */
        if(summary->cv < 5.0) {
            fprintf(stream, "CV:        EXCELLENT (%.2f%% < 5%%)\n", summary->cv);
        } else if(summary->cv < 10.0) {
            fprintf(stream, "CV:        GOOD (%.2f%% < 10%%)\n", summary->cv);
        } else if(summary->cv < 20.0) {
            fprintf(stream, "CV:        ACCEPTABLE (%.2f%% < 20%%)\n", summary->cv);
        } else {
            fprintf(stream, "CV:        POOR (%.2f%% >= 20%%)\n", summary->cv);
            is_fair = 0;
        }

        /* Min/Max ratio assessment */
        if(summary->min_max_ratio > 0.9) {
            fprintf(stream, "Min/Max:   EXCELLENT (%.3f > 0.9)\n", summary->min_max_ratio);
        } else if(summary->min_max_ratio > 0.8) {
            fprintf(stream, "Min/Max:   GOOD (%.3f > 0.8)\n", summary->min_max_ratio);
        } else if(summary->min_max_ratio > 0.7) {
            fprintf(stream, "Min/Max:   ACCEPTABLE (%.3f > 0.7)\n", summary->min_max_ratio);
        } else {
            fprintf(stream, "Min/Max:   POOR (%.3f <= 0.7)\n", summary->min_max_ratio);
            is_fair = 0;
        }

        /* Overall result */
        fprintf(stream, "\nResult:    %s\n", is_fair ? "PASS" : "FAIL");
    } else {
        /* Silent mode - just check thresholds */
        if(summary->cv >= 20.0 || summary->min_max_ratio <= 0.7) { is_fair = 0; }
    }

    return is_fair ? 0 : -1;
}
