/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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
 * Auto-Read Performance and Fairness Test
 *
 * Creates N tags using the provided tag attribute string (which must include
 * an auto_sync_read_ms parameter).  Uses event callbacks to measure per-tag
 * read statistics.  Statistics are only recorded after all tags are created
 * and ready to avoid skewing counts in favour of early-created tags.
 *
 * After the requested duration the program reports:
 *   - Per-tag read counts and average/min/max inter-read gap
 *   - Fairness across tags (CV and min/max ratio)
 *   - Global min and max inter-read gap across all tags
 *   - CPU load percentage
 *
 * Usage:
 *   test_auto_read --tag=<attr_string> --num-tags=N --duration=S
 *
 * The tag attribute string must already contain auto_sync_read_ms=<period>.
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat_utils.h"
#include "stats.h"
#include <libplctag/lib/libplctag.h>

#ifdef POSIX_PLATFORM
#    include <sys/resource.h>
#endif

#ifdef WINDOWS_PLATFORM
#    include <psapi.h>
#endif


#define TAG_CREATE_TIMEOUT_MS (10000)


/*
 * Set to 1 after all tags are created and ready.
 * Callbacks suppress statistics accumulation until this flag is set so that
 * early-created tags do not accumulate more reads than late-created ones.
 */
static volatile int recording = 0;


/*--- Per-tag statistics ---*/

typedef struct {
    compat_atomic_int32_t tag_id;
    compat_atomic_int64_t ready_time;   /* non-zero once PLCTAG_EVENT_CREATED fires */
    compat_atomic_int32_t read_count;   /* successful reads while recording */
    compat_atomic_int64_t last_read_ms; /* wall-clock timestamp of most recent recorded read */
    compat_atomic_int64_t total_gap_ms; /* sum of inter-read gaps */
    compat_atomic_int64_t min_gap_ms;   /* smallest inter-read gap seen (0 = not yet set) */
    compat_atomic_int64_t max_gap_ms;   /* largest inter-read gap seen */
} tag_stats_t;


/*--- CPU time helper (cross-platform) ---*/

static double get_cpu_time_ms(void) {
#ifdef POSIX_PLATFORM
    struct rusage usage;
    if(getrusage(RUSAGE_SELF, &usage) != 0) { return 0.0; }
    return (double)usage.ru_utime.tv_sec * 1000.0 + (double)usage.ru_utime.tv_usec / 1000.0
           + (double)usage.ru_stime.tv_sec * 1000.0 + (double)usage.ru_stime.tv_usec / 1000.0;
#elif defined(WINDOWS_PLATFORM)
    FILETIME creation, exitt, kernel, user;
    ULARGE_INTEGER k, u;
    if(!GetProcessTimes(GetCurrentProcess(), &creation, &exitt, &kernel, &user)) { return 0.0; }
    k.LowPart  = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart  = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return (double)(k.QuadPart + u.QuadPart) / 10000.0;
#else
    return 0.0;
#endif
}


/*--- Tag event callback ---*/

static void tag_callback(int32_t tag_id, int event, int status, void *userdata) {
    (void)tag_id;
    tag_stats_t *s = (tag_stats_t *)userdata;

    switch(event) {
        case PLCTAG_EVENT_CREATED:
            compat_atomic_store_int64(&s->ready_time, compat_time_ms());
            break;

        case PLCTAG_EVENT_READ_COMPLETED:
            if(status == PLCTAG_STATUS_OK && recording) {
                int64_t now  = compat_time_ms();
                int64_t last = compat_atomic_load_int64(&s->last_read_ms);

                if(last > 0) {
                    int64_t gap     = now - last;
                    int64_t cur_max = compat_atomic_load_int64(&s->max_gap_ms);
                    int64_t cur_min = compat_atomic_load_int64(&s->min_gap_ms);

                    compat_atomic_add_int64(&s->total_gap_ms, gap);

                    if(gap > cur_max) { compat_atomic_store_int64(&s->max_gap_ms, gap); }
                    if(cur_min == 0 || gap < cur_min) { compat_atomic_store_int64(&s->min_gap_ms, gap); }
                }

                compat_atomic_store_int64(&s->last_read_ms, now);
                compat_atomic_inc_int32(&s->read_count);
            }
            break;

        default: break;
    }
}


/*--- Usage ---*/

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s --tag=<attr_string> --num-tags=N --duration=S\n"
            "\n"
            "  --tag=<attr_string>  Full tag attribute string (required).\n"
            "                       Must include auto_sync_read_ms=<period>.\n"
            "  --num-tags=N         Number of tags to create (required, > 0).\n"
            "  --duration=S         Test duration in seconds after all tags are\n"
            "                       ready (required, > 0).\n"
            "\n"
            "Example:\n"
            "  %s --tag='protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix"
            "&name=TestBigArray[0]&auto_sync_read_ms=200' --num-tags=100 --duration=30\n",
            prog, prog);
    exit(1);
}


/*--- Main ---*/

int main(int argc, char **argv) {
    const char *tag_string = NULL;
    int num_tags   = 0;
    int duration_s = 0;

    /*--- Parse arguments ---*/
    for(int i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) {
            tag_string = argv[i] + 6;
        } else if(strncmp(argv[i], "--num-tags=", 11) == 0) {
            num_tags = atoi(argv[i] + 11);
        } else if(strncmp(argv[i], "--duration=", 11) == 0) {
            duration_s = atoi(argv[i] + 11);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
        }
    }

    if(!tag_string) {
        fprintf(stderr, "Error: --tag is required.\n");
        usage(argv[0]);
    }

    if(num_tags <= 0) {
        fprintf(stderr, "Error: --num-tags must be a positive integer.\n");
        usage(argv[0]);
    }

    if(duration_s <= 0) {
        fprintf(stderr, "Error: --duration must be a positive integer.\n");
        usage(argv[0]);
    }

    /*--- Allocate per-tag arrays ---*/
    tag_stats_t *stats = calloc((size_t)num_tags, sizeof(*stats));
    if(!stats) {
        fprintf(stderr, "Error: failed to allocate stats array.\n");
        return 1;
    }

    int32_t *tag_ids = calloc((size_t)num_tags, sizeof(*tag_ids));
    if(!tag_ids) {
        fprintf(stderr, "Error: failed to allocate tag_ids array.\n");
        free(stats);
        return 1;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);

    fprintf(stderr, "Auto-Read Test\n");
    fprintf(stderr, "==============\n");
    fprintf(stderr, "Tag string: %s\n", tag_string);
    fprintf(stderr, "Num tags:   %d\n", num_tags);
    fprintf(stderr, "Duration:   %d seconds\n\n", duration_s);

    /*--- Create all tags asynchronously ---*/
    fprintf(stderr, "Creating tags...\n");
    for(int i = 0; i < num_tags; i++) {
        int32_t id = plc_tag_create_ex(tag_string, tag_callback, &stats[i], 0);
        if(id < 0) {
            fprintf(stderr, "Failed to create tag %d: %s\n", i, plc_tag_decode_error(id));
            for(int j = 0; j < i; j++) { plc_tag_destroy(tag_ids[j]); }
            free(tag_ids);
            free(stats);
            return 1;
        }

        tag_ids[i] = id;
        compat_atomic_store_int32(&stats[i].tag_id, id);

        if((i + 1) % 50 == 0) { fprintf(stderr, "  Created %d tags...\n", i + 1); }
    }

    fprintf(stderr, "All %d tags submitted for creation.\n", num_tags);

    /*--- Wait for all tags to be ready ---*/
    fprintf(stderr, "Waiting for all tags to become ready...\n");
    int64_t create_deadline = compat_time_ms() + TAG_CREATE_TIMEOUT_MS;
    int all_ready = 0;

    while(compat_time_ms() < create_deadline) {
        all_ready = 1;
        for(int i = 0; i < num_tags; i++) {
            if(compat_atomic_load_int64(&stats[i].ready_time) == 0) {
                all_ready = 0;
                break;
            }
        }
        if(all_ready) { break; }
        compat_sleep_ms(100, NULL);
    }

    if(!all_ready) {
        fprintf(stderr, "Warning: not all tags became ready within %d ms.\n", TAG_CREATE_TIMEOUT_MS);
        for(int i = 0; i < num_tags; i++) {
            if(compat_atomic_load_int64(&stats[i].ready_time) == 0) {
                fprintf(stderr, "  Tag %d (id=%d): %s\n", i, tag_ids[i],
                        plc_tag_decode_error(plc_tag_status(tag_ids[i])));
            }
        }
    } else {
        fprintf(stderr, "All tags ready.\n");
    }

    /*--- Enable recording and start measurement ---*/
    fprintf(stderr, "\nRunning test for %d seconds...\n", duration_s);
    double  cpu_start  = get_cpu_time_ms();
    int64_t wall_start = compat_time_ms();

    recording = 1; /* callbacks begin accumulating stats */

    compat_sleep_ms((uint32_t)(duration_s * 1000), NULL);

    recording = 0; /* stop accumulating before destroying tags */

    int64_t wall_end = compat_time_ms();
    double  cpu_end  = get_cpu_time_ms();

    int64_t duration_ms  = wall_end - wall_start;
    double  cpu_time_ms  = cpu_end - cpu_start;
    double  cpu_load_pct = (duration_ms > 0) ? (cpu_time_ms / (double)duration_ms) * 100.0 : 0.0;

    fprintf(stderr, "Test complete. Actual duration: %" PRId64 " ms\n\n", duration_ms);
    fflush(stderr);

    /*--- Destroy tags ---*/
    fprintf(stderr, "Destroying tags...\n");
    for(int i = 0; i < num_tags; i++) { plc_tag_destroy(tag_ids[i]); }
    free(tag_ids);

    /*--- Collect per-tag results ---*/
    fprintf(stderr, "\nPer-Tag Results:\n");
    fprintf(stderr, "----------------\n");

    int *read_counts = calloc((size_t)num_tags, sizeof(*read_counts));
    if(!read_counts) {
        fprintf(stderr, "Error: failed to allocate read_counts array.\n");
        free(stats);
        return 1;
    }

    int64_t global_min_gap = 0;
    int64_t global_max_gap = 0;

    for(int i = 0; i < num_tags; i++) {
        int32_t count     = compat_atomic_load_int32(&stats[i].read_count);
        int64_t total_gap = compat_atomic_load_int64(&stats[i].total_gap_ms);
        int64_t min_gap   = compat_atomic_load_int64(&stats[i].min_gap_ms);
        int64_t max_gap   = compat_atomic_load_int64(&stats[i].max_gap_ms);
        int64_t avg_gap   = (count > 1) ? (total_gap / (count - 1)) : 0;

        fprintf(stderr,
                "Tag %3d (id=%d): reads=%d  avg_gap=%" PRId64 "ms"
                "  min_gap=%" PRId64 "ms  max_gap=%" PRId64 "ms\n",
                i, compat_atomic_load_int32(&stats[i].tag_id), count, avg_gap, min_gap, max_gap);

        read_counts[i] = (int)count;

        if(min_gap > 0 && (global_min_gap == 0 || min_gap < global_min_gap)) { global_min_gap = min_gap; }
        if(max_gap > global_max_gap) { global_max_gap = max_gap; }
    }

    /*--- Fairness ---*/
    stats_summary_t summary;
    int stats_ok = (stats_calculate(read_counts, num_tags, &summary) == 0);

    double fairness_cv      = stats_ok ? summary.cv            : 0.0;
    double fairness_min_max = stats_ok ? summary.min_max_ratio : 0.0;

    /*--- Print summary ---*/
    fprintf(stderr, "\n--- Summary ---\n");
    fprintf(stderr, "Duration:          %" PRId64 " ms\n", duration_ms);
    fprintf(stderr, "CPU load:          %.2f%%\n",          cpu_load_pct);
    fprintf(stderr, "Global min gap:    %" PRId64 " ms\n",  global_min_gap);
    fprintf(stderr, "Global max gap:    %" PRId64 " ms\n",  global_max_gap);
    fprintf(stderr, "Fairness CV:       %.2f%%\n",          fairness_cv);
    fprintf(stderr, "Fairness min/max:  %.4f\n",            fairness_min_max);

    if(stats_ok) {
        stats_print_summary(stderr, &summary);
        stats_print_histogram(stderr, read_counts, num_tags, 0, 40);
    }

    int fair = 1;
    if(stats_ok) { fair = (stats_assess_fairness(&summary, stderr) == 0); }

    free(read_counts);
    free(stats);

    fprintf(stderr, "\nDone.\n");
    fflush(stderr);

    return fair ? 0 : 1;
}
