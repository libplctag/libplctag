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

#pragma once

#include <stdio.h>

/*
 * Statistics utility functions for analyzing distributions.
 *
 * These functions help measure fairness and detect clustering in
 * distributions of values (e.g., read counts per tag).
 */

/* Summary statistics structure */
typedef struct {
    int count;          /* Number of values */
    int min;            /* Minimum value */
    int max;            /* Maximum value */
    double mean;        /* Arithmetic mean */
    double variance;    /* Population variance */
    double std_dev;     /* Population standard deviation */
    double cv;          /* Coefficient of variation (std_dev/mean * 100) */
    double min_max_ratio; /* min/max ratio (1.0 = perfect equality) */
    int q1;             /* First quartile (25th percentile) */
    int median;         /* Median (50th percentile) */
    int q3;             /* Third quartile (75th percentile) */
    int iqr;            /* Interquartile range (Q3 - Q1) */
} stats_summary_t;

/*
 * Calculate summary statistics for an array of integer values.
 *
 * @param values    Array of integer values to analyze
 * @param count     Number of values in the array
 * @param summary   Output structure to fill with statistics
 *
 * @return 0 on success, -1 on error (null pointer or count <= 0)
 */
int stats_calculate(const int *values, int count, stats_summary_t *summary);

/*
 * Print summary statistics to a file stream.
 *
 * @param stream    Output stream (e.g., stderr)
 * @param summary   Statistics to print
 */
void stats_print_summary(FILE *stream, const stats_summary_t *summary);

/*
 * Print a histogram of values to a file stream.
 *
 * @param stream        Output stream (e.g., stderr)
 * @param values        Array of integer values
 * @param count         Number of values
 * @param num_buckets   Number of histogram buckets (0 for auto)
 * @param max_bar_width Maximum width of histogram bars in characters
 */
void stats_print_histogram(FILE *stream, const int *values, int count,
                           int num_buckets, int max_bar_width);

/*
 * Assess fairness based on statistics.
 *
 * @param summary   Statistics to assess
 * @param stream    Output stream for assessment (can be NULL for no output)
 *
 * @return 0 if fair (CV < 20% AND min/max > 0.7), -1 if unfair
 */
int stats_assess_fairness(const stats_summary_t *summary, FILE *stream);
