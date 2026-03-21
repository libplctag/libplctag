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

/*
 * Performance Benchmark for libplctag
 *
 * Runs a single test configuration and outputs one CSV line to stdout.
 * Designed to be driven by run_perf_benchmark.sh which iterates over the
 * full parameter matrix.
 *
 * Usage:
 *   perf_benchmark --mode=sync|async --groups=N --threads=M --tags=T
 *                  [--duration=S] [--gateway=IP] [--port=PORT]
 *
 * Tags is the primary axis.  Threads and groups must be <= tags (groups
 * also capped at 100).  Each thread owns an exclusive slice of the tags
 * array -- no tag is shared across threads, so no locking is needed.
 * Tags are distributed across connection groups round-robin.
 *
 * Sync:  thread calls plc_tag_read(tag, timeout) in a blocking loop.
 * Async: thread calls plc_tag_read(tag, 0) then polls plc_tag_status().
 *
 * Output (stdout): one CSV line:
 *   mode,connection_groups,threads,tags,tags_per_thread,duration_ms,
 *   total_reads,reads_per_sec,cpu_load_pct,fairness_cv,fairness_min_max_ratio
 *
 * Diagnostics go to stderr.
 */

#include "compat_utils.h"
#include "stats.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef POSIX_PLATFORM
#    include <sys/resource.h>
#endif

#ifdef WINDOWS_PLATFORM
#    include <psapi.h>
#endif


#define REQUIRED_VERSION 2, 6, 0

#define TAG_CREATE_TIMEOUT_MS (10000)
#define TAG_READ_TIMEOUT_MS (5000)
#define DEFAULT_DURATION_S (10)
#define DEFAULT_GATEWAY "127.0.0.1"
#define DEFAULT_PORT (44818)
#define TAG_NAME "TestBigArray"

#define MAX_TAGS (1100)
#define MAX_THREADS (1100)

/* Tag path template.  connection_group_id is filled per tag. */
#define TAG_PATH_FMT                                          \
    "protocol=ab-eip&gateway=%s:%d&path=1,0&plc=ControlLogix" \
    "&elem_count=1&name=%s[0]&connection_group_id=%d"


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
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return (double)(k.QuadPart + u.QuadPart) / 10000.0;
#else
    return 0.0;
#endif
}


/*--- Global synchronisation flags ---*/

static volatile int go = 0;   /* set to 1 to start all threads */
static volatile int done = 0; /* set to 1 to stop all threads  */
static volatile int terminate = 0;

static void handle_interrupt(void) {
    terminate = 1;
    done = 1;
}


/*--- Per-thread data ---*/

typedef struct {
    int thread_id;
    int32_t *tags; /* pointer to this thread's exclusive slice of tags */
    int num_tags;  /* number of tags in this thread's slice */
    int is_async;
    int64_t read_count;  /* successful reads */
    int64_t error_count; /* failed reads */
} thread_data_t;


/*--- Thread function ---*/

static void *thread_func(void *arg) {
    thread_data_t *td = (thread_data_t *)arg;
    /* Each thread starts at the first tag in its own exclusive slice. */
    int tag_idx = 0;

    td->read_count = 0;
    td->error_count = 0;

    /* Spin until signalled to start. */
    while(!go && !done) { compat_thread_yield(); }

    if(td->is_async) {
        /* Async: fire reads on all tags, then poll all for completion.
         * Batching all reads before polling allows the library to have all
         * requests in-flight simultaneously rather than serializing them.
         * No locking needed -- this thread exclusively owns its tags. */
        int read_rc[td->num_tags];

        while(!done) {
            /* Phase 1: Start reads on all tags. */
            for(int i = 0; i < td->num_tags; i++) {
                read_rc[i] = plc_tag_read(td->tags[i], 0);
                if(read_rc[i] != PLCTAG_STATUS_OK && read_rc[i] != PLCTAG_STATUS_PENDING) {
                    td->error_count++;
                }
            }

            /* Phase 2: Poll until all successfully-started reads are done. */
            int pending;
            do {
                pending = 0;
                for(int i = 0; i < td->num_tags; i++) {
                    if(read_rc[i] == PLCTAG_STATUS_OK || read_rc[i] == PLCTAG_STATUS_PENDING) {
                        if(plc_tag_status(td->tags[i]) == PLCTAG_STATUS_PENDING) {
                            pending = 1;
                        }
                    }
                }
                if(pending) {
                    compat_thread_yield();
                }
            } while(pending && !done);

            /* Phase 3: Tally completed reads. */
            for(int i = 0; i < td->num_tags; i++) {
                if(read_rc[i] == PLCTAG_STATUS_OK || read_rc[i] == PLCTAG_STATUS_PENDING) {
                    int status = plc_tag_status(td->tags[i]);
                    if(status == PLCTAG_STATUS_OK) {
                        td->read_count++;
                    } else if(!done) {
                        td->error_count++;
                    }
                }
            }
        }
    } else {
        /* Sync: blocking read.
         * No locking needed -- this thread exclusively owns its tags. */
        while(!done) {
            int32_t tag = td->tags[tag_idx];
            int rc = plc_tag_read(tag, TAG_READ_TIMEOUT_MS);

            if(rc == PLCTAG_STATUS_OK) {
                td->read_count++;
            } else {
                td->error_count++;
                if(!done) {
                    fprintf(stderr, "Thread %d: read error %s on tag idx %d\n", td->thread_id, plc_tag_decode_error(rc), tag_idx);
                }
            }

            tag_idx = (tag_idx + 1) % td->num_tags;
        }
    }

    return NULL;
}


/*--- Usage ---*/

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s --mode=sync|async --groups=N --threads=M --tags=T\n"
            "     [--duration=S] [--gateway=IP] [--port=PORT]\n"
            "\n"
            "  --mode        sync or async\n"
            "  --groups      number of connection groups (1-100)\n"
            "  --threads     number of reader threads (1-1000)\n"
            "  --tags        number of tags to create (1-1000)\n"
            "  --duration    test duration in seconds (default: %d)\n"
            "  --gateway     PLC/simulator IP (default: %s)\n"
            "  --port        PLC/simulator port (default: %d)\n",
            prog, DEFAULT_DURATION_S, DEFAULT_GATEWAY, DEFAULT_PORT);
    exit(1);
}


/*--- Main ---*/

int main(int argc, char **argv) {
    int is_async = -1;
    int num_groups = -1;
    int num_threads = -1;
    int num_tags = -1;
    int duration_s = DEFAULT_DURATION_S;
    const char *gateway = DEFAULT_GATEWAY;
    int port = DEFAULT_PORT;

    int32_t tag_handles[MAX_TAGS];
    compat_thread_t threads[MAX_THREADS];
    thread_data_t tdata[MAX_THREADS];

    /*--- Parse arguments ---*/
    for(int i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--mode=", 7) == 0) {
            const char *m = argv[i] + 7;
            if(strcmp(m, "sync") == 0) {
                is_async = 0;
            } else if(strcmp(m, "async") == 0) {
                is_async = 1;
            } else {
                fprintf(stderr, "Unknown mode: %s\n", m);
                usage(argv[0]);
            }
        } else if(strncmp(argv[i], "--groups=", 9) == 0) {
            num_groups = atoi(argv[i] + 9);
        } else if(strncmp(argv[i], "--threads=", 10) == 0) {
            num_threads = atoi(argv[i] + 10);
        } else if(strncmp(argv[i], "--tags=", 7) == 0) {
            num_tags = atoi(argv[i] + 7);
        } else if(strncmp(argv[i], "--duration=", 11) == 0) {
            duration_s = atoi(argv[i] + 11);
        } else if(strncmp(argv[i], "--gateway=", 10) == 0) {
            gateway = argv[i] + 10;
        } else if(strncmp(argv[i], "--port=", 7) == 0) {
            port = atoi(argv[i] + 7);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
        }
    }

    if(is_async < 0 || num_groups <= 0 || num_threads <= 0 || num_tags <= 0 || duration_s <= 0) {
        fprintf(stderr, "Error: --mode, --groups, --threads, and --tags are required and must be positive.\n");
        usage(argv[0]);
    }

    if(num_tags > MAX_TAGS || num_threads > MAX_THREADS) {
        fprintf(stderr, "Error: max tags=%d, max threads=%d\n", MAX_TAGS, MAX_THREADS);
        return 1;
    }

    /*--- Library version check ---*/
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);
    compat_set_interrupt_handler(handle_interrupt);

    const char *mode_str = is_async ? "async" : "sync";
    fprintf(stderr, "perf_benchmark: mode=%s groups=%d threads=%d tags=%d duration=%ds gateway=%s:%d\n", mode_str, num_groups,
            num_threads, num_tags, duration_s, gateway, port);

    /*--- Create tags ---*/
    fprintf(stderr, "Creating %d tags...\n", num_tags);
    for(int i = 0; i < num_tags; i++) {
        char path[512];
        int group_id = (i % num_groups) + 1;

        snprintf(path, sizeof(path), TAG_PATH_FMT, gateway, port, TAG_NAME, group_id);

        tag_handles[i] = plc_tag_create(path, TAG_CREATE_TIMEOUT_MS);
        if(tag_handles[i] < 0) {
            fprintf(stderr, "ERROR: Failed to create tag %d (group %d): %s\n", i, group_id, plc_tag_decode_error(tag_handles[i]));
            /* Clean up already-created tags. */
            for(int j = 0; j < i; j++) { plc_tag_destroy(tag_handles[j]); }
            return 1;
        }

        if((i + 1) % 100 == 0) { fprintf(stderr, "  created %d tags...\n", i + 1); }
    }
    fprintf(stderr, "All %d tags created.\n", num_tags);

    /* Do one sync read per tag to prime the connections. */
    fprintf(stderr, "Priming tags...\n");
    for(int i = 0; i < num_tags; i++) {
        int rc = plc_tag_read(tag_handles[i], TAG_READ_TIMEOUT_MS);
        if(rc != PLCTAG_STATUS_OK) { fprintf(stderr, "WARNING: prime read failed on tag %d: %s\n", i, plc_tag_decode_error(rc)); }
    }

    /*--- Assign an exclusive slice of tags to each thread ---*/
    {
        int base = num_tags / num_threads;
        int remainder = num_tags % num_threads;
        int offset = 0;
        for(int i = 0; i < num_threads; i++) {
            int count = base + (i < remainder ? 1 : 0);
            tdata[i].thread_id = i;
            tdata[i].tags = &tag_handles[offset];
            tdata[i].num_tags = count;
            tdata[i].is_async = is_async;
            tdata[i].read_count = 0;
            tdata[i].error_count = 0;
            offset += count;
        }
    }

    /*--- Create threads (they spin-wait on go flag) ---*/
    fprintf(stderr, "Creating %d threads...\n", num_threads);
    for(int i = 0; i < num_threads; i++) {
        if(compat_thread_create(&threads[i], thread_func, &tdata[i]) != 0) {
            fprintf(stderr, "ERROR: Failed to create thread %d\n", i);
            done = 1;
            for(int j = 0; j < i; j++) { compat_thread_join(threads[j], NULL); }
            for(int j = 0; j < num_tags; j++) { plc_tag_destroy(tag_handles[j]); }
            return 1;
        }
    }

    /* Brief pause for threads to reach their spin-wait. */
    compat_sleep_ms(50, NULL);

    /*--- Run the benchmark ---*/
    double cpu_start = get_cpu_time_ms();
    int64_t wall_start = compat_time_ms();

    go = 1; /* release all threads */

    /* Sleep for the test duration. */
    for(int s = 0; s < duration_s && !terminate; s++) { compat_sleep_ms(1000, NULL); }

    done = 1; /* signal threads to stop */

    /* Join all threads. */
    for(int i = 0; i < num_threads; i++) { compat_thread_join(threads[i], NULL); }

    int64_t wall_end = compat_time_ms();
    double cpu_end = get_cpu_time_ms();

    int64_t duration_ms = wall_end - wall_start;
    double cpu_time_ms = cpu_end - cpu_start;
    double cpu_load_pct = (duration_ms > 0) ? (cpu_time_ms / (double)duration_ms) * 100.0 : 0.0;

    /*--- Aggregate results ---*/
    int64_t total_reads = 0;
    int64_t total_errors = 0;

    int *per_thread_reads = (int *)calloc((size_t)num_threads, sizeof(int));
    if(!per_thread_reads) {
        fprintf(stderr, "ERROR: calloc failed\n");
        for(int j = 0; j < num_tags; j++) { plc_tag_destroy(tag_handles[j]); }
        return 1;
    }

    for(int i = 0; i < num_threads; i++) {
        total_reads += tdata[i].read_count;
        total_errors += tdata[i].error_count;
        per_thread_reads[i] = (int)tdata[i].read_count;
    }

    double reads_per_sec = (duration_ms > 0) ? (double)total_reads / ((double)duration_ms / 1000.0) : 0.0;
    double tags_per_thread = (double)num_tags / (double)num_threads;

    /*--- Fairness ---*/
    double fairness_cv = 0.0;
    double fairness_min_max = 0.0;

    if(num_threads > 1) {
        stats_summary_t summary;
        if(stats_calculate(per_thread_reads, num_threads, &summary) == 0) {
            fairness_cv = summary.cv;
            fairness_min_max = summary.min_max_ratio;
        }
    } else {
        fairness_cv = 0.0;
        fairness_min_max = 1.0;
    }

    /*--- Print CSV line to stdout ---*/
    printf("%s,%d,%d,%d,%.2f,%" PRId64 ",%" PRId64 ",%.1f,%.2f,%.2f,%.4f\n", mode_str, num_groups, num_threads, num_tags,
           tags_per_thread, duration_ms, total_reads, reads_per_sec, cpu_load_pct, fairness_cv, fairness_min_max);
    fflush(stdout);

    /*--- Summary to stderr ---*/
    fprintf(stderr, "\n--- Results ---\n");
    fprintf(stderr, "Duration:       %" PRId64 " ms\n", duration_ms);
    fprintf(stderr, "Total reads:    %" PRId64 "\n", total_reads);
    fprintf(stderr, "Total errors:   %" PRId64 "\n", total_errors);
    fprintf(stderr, "Reads/sec:      %.1f\n", reads_per_sec);
    fprintf(stderr, "CPU load:       %.2f%%\n", cpu_load_pct);
    fprintf(stderr, "Fairness CV:    %.2f%%\n", fairness_cv);
    fprintf(stderr, "Fairness ratio: %.4f\n", fairness_min_max);

    if(num_threads > 1) {
        stats_summary_t summary;
        if(stats_calculate(per_thread_reads, num_threads, &summary) == 0) { stats_print_summary(stderr, &summary); }
    }

    /*--- Cleanup ---*/
    free(per_thread_reads);

    fprintf(stderr, "Destroying tags...\n");
    for(int i = 0; i < num_tags; i++) { plc_tag_destroy(tag_handles[i]); }

    fprintf(stderr, "Done.\n");

    return (terminate) ? 1 : 0;
}
