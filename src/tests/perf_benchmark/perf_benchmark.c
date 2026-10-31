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
 * Uses only the public API so the same source builds against older
 * library releases for A/B comparison.
 *
 * Usage:
 *   perf_benchmark --mode=sync|async|auto --groups=N --threads=M --tags=T
 *                  --tag=BASE_TAG_PATH [--op=read|write] [--auto-ms=P] [--poll-ms=M]
 *                  [--duration=S] [--interleave]
 *
 * BASE_TAG_PATH is the full tag attribute string without connection_group_id,
 * which the benchmark appends automatically per tag.  Example:
 *   protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=TestBigArray[0]
 *
 * Tags is the primary axis.  Threads and groups must be <= tags (groups
 * also capped at 100).  Each thread owns an exclusive slice of the tags
 * array -- no tag is shared across threads, so no locking is needed.
 * --interleave creates the tags round-robin across the threads' slices
 * (thread 0's first, thread 1's first, ...), so tags used by different
 * threads get adjacent IDs, as when threads create their tags concurrently.
 * Sync/async: each thread's tags share one connection group, and threads are
 * assigned to groups round-robin, so --groups=1 shares one connection and
 * --groups=threads gives each thread its own.  Auto: tags are assigned to
 * groups round-robin.
 *
 * Sync:  thread calls plc_tag_read/write(tag, timeout) in a blocking loop.
 *        Latency is the duration of each call.
 * Async: thread starts an operation on all its tags, then polls
 *        plc_tag_status().  Latency is start to observed completion.
 * Auto:  tags are created with auto_sync_read_ms=P and no threads run.
 *        Completed reads are counted from PLCTAG_EVENT_READ_COMPLETED.
 *        "Latency" is the interval between completions on a tag, i.e.
 *        the achieved period.  --op=write is not supported.
 *
 * Output (stdout): one CSV line:
 *   mode,connection_groups,threads,tags,tags_per_thread,duration_ms,
 *   total_ops,ops_per_sec,cpu_load_pct,fairness_cv,fairness_min_max_ratio,
 *   op,errors,p50_us,p95_us,p99_us,max_us,cpu_us_per_op,rss_growth_kb,
 *   create_ms,prime_ms
 *
 * Fairness is across threads for sync/async and across tags for auto.
 * rss_growth_kb is peak RSS at the end minus peak RSS before tag creation.
 * Latency percentiles come from a log-linear histogram with 16 buckets per
 * power of two, so they are accurate to about 3%.
 *
 * Diagnostics go to stderr.
 */

#include "compat_utils.h"
#include "stats.h"
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef POSIX_PLATFORM
#    include <sys/resource.h>
#endif

#ifdef WINDOWS_PLATFORM
#    include <psapi.h>
#endif


#define REQUIRED_VERSION 2, 6, 0

#define TAG_CREATE_TIMEOUT_MS (10000)
#define TAG_OP_TIMEOUT_MS (5000)
#define DEFAULT_DURATION_S (10)
#define DEFAULT_AUTO_MS (1)

#define MAX_TAGS (2000)
#define MAX_THREADS (1100)

/* Tag path template.  connection_group_id (and auto_sync_read_ms) are appended to the base path. */
#define TAG_PATH_FMT "%s&connection_group_id=%" PRId32
#define TAG_PATH_AUTO_FMT "%s&connection_group_id=%" PRId32 "&auto_sync_read_ms=%" PRId32

/* Log-linear histogram: values below HIST_SUB are exact, then HIST_SUB buckets per power of two. */
#define HIST_SUB_BITS (4)
#define HIST_SUB (1 << HIST_SUB_BITS)
#define HIST_BUCKETS (40 * HIST_SUB)

typedef enum { MODE_SYNC, MODE_ASYNC, MODE_AUTO } bench_mode_t;

/* Per-tag results.  Each tag is touched by one thread (or, in auto mode, by its callback) only. */
typedef struct {
    int64_t ops;
    int64_t errors;
    int64_t last_us; /* auto mode: time of the previous completion */
    int64_t max_us;
    uint32_t hist[HIST_BUCKETS];
} tag_stats_t;

typedef struct {
    int32_t thread_id;
    int32_t *tags;      /* this thread's exclusive slice of tags */
    tag_stats_t *stats; /* matching slice of per-tag stats */
    int32_t num_tags;
    bench_mode_t mode;
    bool is_write;
} thread_data_t;


static int64_t now_us(void);
static double get_cpu_time_ms(void);
static int64_t get_peak_rss_kb(void);
static size_t hist_index(int64_t v);
static int64_t hist_value(size_t idx);
static void record(tag_stats_t *s, int64_t us);
static int64_t percentile(const uint64_t *hist, uint64_t total, double p);
static int32_t start_op(int32_t tag, bool is_write, int32_t timeout_ms);
static void auto_callback(int32_t tag_id, int event, int status, void *userdata);
static void *thread_func(void *arg);
static void handle_interrupt(void);
static void usage(const char *prog);


static compat_atomic_int32_t go = {0};   /* set to 1 to start measuring */
static compat_atomic_int32_t done = {0}; /* set to 1 to stop */
static int32_t poll_ms = 0;              /* async: sleep between status passes; 0 just yields */
static bool interleave = false;          /* create tags round-robin across thread slices */
static compat_atomic_int32_t terminate = {0};


/*--- Clocks and process statistics (cross-platform) ---*/

static int64_t now_us(void) {
#ifdef WINDOWS_PLATFORM
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (int64_t)((double)count.QuadPart * 1000000.0 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)ts.tv_nsec / 1000;
#endif
}


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


static int64_t get_peak_rss_kb(void) {
#ifdef POSIX_PLATFORM
    struct rusage usage;
    if(getrusage(RUSAGE_SELF, &usage) != 0) { return 0; }
#    ifdef __APPLE__
    return (int64_t)usage.ru_maxrss / 1024; /* bytes on macOS */
#    else
    return (int64_t)usage.ru_maxrss; /* KB on Linux and BSD */
#    endif
#elif defined(WINDOWS_PLATFORM)
    PROCESS_MEMORY_COUNTERS pmc;
    if(!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) { return 0; }
    return (int64_t)(pmc.PeakWorkingSetSize / 1024);
#else
    return 0;
#endif
}


/*--- Histogram ---*/

static size_t hist_index(int64_t v) {
    int32_t e = 0;
    size_t idx;

    if(v < HIST_SUB) { return v < 0 ? 0 : (size_t)v; }

    while((v >> (e + 1)) != 0) { e++; }

    idx = (size_t)(e - HIST_SUB_BITS + 1) * HIST_SUB + (size_t)((v >> (e - HIST_SUB_BITS)) & (HIST_SUB - 1));

    return idx < HIST_BUCKETS ? idx : HIST_BUCKETS - 1;
}


/* Midpoint of a bucket. */
static int64_t hist_value(size_t idx) {
    int32_t shift;

    if(idx < HIST_SUB) { return (int64_t)idx; }

    shift = (int32_t)(idx / HIST_SUB) - 1;

    return ((int64_t)(HIST_SUB + (idx % HIST_SUB)) << shift) + ((int64_t)1 << shift) / 2;
}


static void record(tag_stats_t *s, int64_t us) {
    s->ops++;
    s->hist[hist_index(us)]++;
    if(us > s->max_us) { s->max_us = us; }
}


static int64_t percentile(const uint64_t *hist, uint64_t total, double p) {
    uint64_t rank = (uint64_t)((double)total * p + 0.999999);
    uint64_t seen = 0;

    if(total == 0) { return 0; }
    if(rank < 1) { rank = 1; }

    for(size_t i = 0; i < HIST_BUCKETS; i++) {
        seen += hist[i];
        if(seen >= rank) { return hist_value(i); }
    }

    return hist_value(HIST_BUCKETS - 1);
}


/*--- Operations ---*/

static int32_t start_op(int32_t tag, bool is_write, int32_t timeout_ms) {
    return (int32_t)(is_write ? plc_tag_write(tag, timeout_ms) : plc_tag_read(tag, timeout_ms));
}


static void auto_callback(int32_t tag_id, int event, int status, void *userdata) {
    tag_stats_t *s = (tag_stats_t *)userdata;
    int64_t t;

    (void)tag_id;

    if(event != PLCTAG_EVENT_READ_COMPLETED) { return; }

    t = now_us();

    if(!compat_atomic_load_int32(&go) || compat_atomic_load_int32(&done)) {
        s->last_us = t;
        return;
    }

    if(status != PLCTAG_STATUS_OK) {
        s->errors++;
    } else if(s->last_us > 0) {
        record(s, t - s->last_us);
    } else {
        s->ops++; /* first completion has no interval */
    }

    s->last_us = t;
}


static void *thread_func(void *arg) {
    thread_data_t *td = (thread_data_t *)arg;

    /* Spin until signalled to start. */
    while(!compat_atomic_load_int32(&go) && !compat_atomic_load_int32(&done)) { compat_thread_yield(); }

    if(td->mode == MODE_ASYNC) {
        /* Start an operation on all tags, then poll all for completion, so
         * the library has all requests in flight at once. */
        int64_t *started = (int64_t *)calloc((size_t)td->num_tags, sizeof(int64_t));
        if(!started) { return NULL; }

        while(!compat_atomic_load_int32(&done)) {
            int32_t pending = 0;

            for(int32_t i = 0; i < td->num_tags; i++) {
                int32_t rc = start_op(td->tags[i], td->is_write, 0);
                if(rc == PLCTAG_STATUS_OK || rc == PLCTAG_STATUS_PENDING) {
                    started[i] = now_us();
                    pending++;
                } else {
                    started[i] = 0;
                    td->stats[i].errors++;
                }
            }

            while(pending > 0 && !compat_atomic_load_int32(&done)) {
                for(int32_t i = 0; i < td->num_tags; i++) {
                    int32_t status;

                    if(started[i] == 0) { continue; }

                    status = (int32_t)plc_tag_status(td->tags[i]);
                    if(status == PLCTAG_STATUS_PENDING) { continue; }

                    if(status == PLCTAG_STATUS_OK) {
                        record(&td->stats[i], now_us() - started[i]);
                    } else {
                        td->stats[i].errors++;
                    }

                    started[i] = 0;
                    pending--;
                }

                if(pending > 0) {
                    if(poll_ms > 0) {
                        compat_sleep_ms((uint32_t)poll_ms, NULL);
                    } else {
                        compat_thread_yield();
                    }
                }
            }
        }

        free(started);
    } else {
        int32_t idx = 0;

        while(!compat_atomic_load_int32(&done)) {
            int64_t t0 = now_us();
            int32_t rc = start_op(td->tags[idx], td->is_write, TAG_OP_TIMEOUT_MS);

            if(rc == PLCTAG_STATUS_OK) {
                record(&td->stats[idx], now_us() - t0);
            } else if(!compat_atomic_load_int32(&done)) {
                td->stats[idx].errors++;
                fprintf(stderr, "Thread %" PRId32 ": error %s on tag idx %" PRId32 "\n", td->thread_id,
                        plc_tag_decode_error(rc), idx);
            }

            idx = (idx + 1) % td->num_tags;
        }
    }

    return NULL;
}


static void handle_interrupt(void) {
    compat_atomic_store_int32(&terminate, 1);
    compat_atomic_store_int32(&done, 1);
}


static void usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s --mode=sync|async|auto --groups=N --threads=M --tags=T\n"
            "     --tag=BASE_TAG_PATH [--op=read|write] [--auto-ms=P] [--duration=S]\n"
            "\n"
            "  --mode        sync, async, or auto (auto_sync_read_ms, no threads)\n"
            "  --groups      number of connection groups (1-100)\n"
            "  --threads     number of threads (1-1000, ignored for auto)\n"
            "  --tags        number of tags to create (1-1000)\n"
            "  --tag         base tag attribute string (connection_group_id is appended)\n"
            "  --op          read or write (default: read; auto supports read only)\n"
            "  --auto-ms     auto_sync_read_ms for auto mode (default: %d)\n"
            "  --poll-ms     async: sleep this long between status passes (default: 0, yield only)\n"
            "  --duration    test duration in seconds (default: %d)\n"
            "  --interleave  sync/async: create tags round-robin across threads (tags must divide evenly)\n"
            "\n"
            "Example:\n"
            "  %s --mode=sync --groups=1 --threads=1 --tags=10 --duration=10\n"
            "     --tag=\"protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=TestBigArray[0]\"\n",
            prog, DEFAULT_AUTO_MS, DEFAULT_DURATION_S, prog);
    exit(1);
}


int main(int argc, char **argv) {
    static const char *mode_names[] = {"sync", "async", "auto"};
    int32_t mode = -1;
    bool is_write = false;
    int32_t num_groups = -1;
    int32_t num_threads = -1;
    int32_t num_tags = -1;
    int32_t duration_s = DEFAULT_DURATION_S;
    int32_t auto_ms = DEFAULT_AUTO_MS;
    const char *base_tag_path = NULL;

    static int32_t tag_handles[MAX_TAGS];
    static compat_thread_t threads[MAX_THREADS];
    static thread_data_t tdata[MAX_THREADS];
    static uint64_t total_hist[HIST_BUCKETS];
    tag_stats_t *stats = NULL;
    int *per_unit_ops = NULL;
    int32_t num_units = 0;
    int32_t threads_started = 0;
    int32_t rc = 0;

    /*--- Parse arguments ---*/
    for(int32_t i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--mode=", 7) == 0) {
            const char *m = argv[i] + 7;
            for(int32_t k = 0; k < 3; k++) {
                if(strcmp(m, mode_names[k]) == 0) { mode = k; }
            }
            if(mode < 0) {
                fprintf(stderr, "Unknown mode: %s\n", m);
                usage(argv[0]);
            }
        } else if(strncmp(argv[i], "--op=", 5) == 0) {
            const char *o = argv[i] + 5;
            if(strcmp(o, "read") == 0) {
                is_write = false;
            } else if(strcmp(o, "write") == 0) {
                is_write = true;
            } else {
                fprintf(stderr, "Unknown op: %s\n", o);
                usage(argv[0]);
            }
        } else if(strncmp(argv[i], "--groups=", 9) == 0) {
            num_groups = (int32_t)atoi(argv[i] + 9);
        } else if(strncmp(argv[i], "--threads=", 10) == 0) {
            num_threads = (int32_t)atoi(argv[i] + 10);
        } else if(strncmp(argv[i], "--tags=", 7) == 0) {
            num_tags = (int32_t)atoi(argv[i] + 7);
        } else if(strncmp(argv[i], "--duration=", 11) == 0) {
            duration_s = (int32_t)atoi(argv[i] + 11);
        } else if(strncmp(argv[i], "--poll-ms=", 10) == 0) {
            poll_ms = (int32_t)atoi(argv[i] + 10);
        } else if(strcmp(argv[i], "--interleave") == 0) {
            interleave = true;
        } else if(strncmp(argv[i], "--auto-ms=", 10) == 0) {
            auto_ms = (int32_t)atoi(argv[i] + 10);
        } else if(strncmp(argv[i], "--tag=", 6) == 0) {
            base_tag_path = argv[i] + 6;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
        }
    }

    if(mode < 0 || num_groups <= 0 || num_threads <= 0 || num_tags <= 0 || duration_s <= 0 || auto_ms <= 0
       || base_tag_path == NULL || base_tag_path[0] == '\0') {
        fprintf(stderr, "Error: --mode, --groups, --threads, --tags, and --tag are required and must be valid.\n");
        usage(argv[0]);
    }

    if(mode == MODE_AUTO && is_write) {
        fprintf(stderr, "Error: --mode=auto supports --op=read only.\n");
        usage(argv[0]);
    }

    if(interleave && (mode == MODE_AUTO || num_tags % num_threads != 0)) {
        fprintf(stderr, "Error: --interleave needs sync/async and tags divisible by threads.\n");
        usage(argv[0]);
    }

    if(num_tags > MAX_TAGS || num_threads > MAX_THREADS) {
        fprintf(stderr, "Error: max tags=%d, max threads=%d\n", MAX_TAGS, MAX_THREADS);
        return 1;
    }

    if(mode == MODE_AUTO) { num_threads = 1; /* only used for the tags_per_thread column */ }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);
    compat_set_interrupt_handler(handle_interrupt);

    fprintf(stderr, "perf_benchmark: mode=%s op=%s groups=%" PRId32 " threads=%" PRId32 " tags=%" PRId32 " duration=%" PRId32
                    "s tag=%s\n",
            mode_names[mode], is_write ? "write" : "read", num_groups, num_threads, num_tags, duration_s, base_tag_path);

    /* Touch the stats now so their pages count in the RSS baseline, not the growth. */
    stats = (tag_stats_t *)malloc((size_t)num_tags * sizeof(tag_stats_t));
    num_units = (mode == MODE_AUTO) ? num_tags : num_threads;
    per_unit_ops = (int *)calloc((size_t)num_units, sizeof(int));
    if(!stats || !per_unit_ops) {
        fprintf(stderr, "ERROR: out of memory\n");
        free(stats);
        free(per_unit_ops);
        return 1;
    }
    memset(stats, 0, (size_t)num_tags * sizeof(tag_stats_t));

    int64_t rss_base_kb = get_peak_rss_kb();

    /*--- Create tags ---*/
    fprintf(stderr, "Creating %" PRId32 " tags...\n", num_tags);
    int64_t create_start = now_us();
    for(int32_t k = 0; k < num_tags; k++) {
        char path[512];
        /* k is the creation order, i the position in tag_handles. */
        int32_t i = interleave ? (k % num_threads) * (num_tags / num_threads) + k / num_threads : k;
        int32_t group_id = (i % num_groups) + 1;

        if(mode != MODE_AUTO) {
            /* Same contiguous slicing as the thread setup below. */
            int32_t base = num_tags / num_threads;
            int32_t remainder = num_tags % num_threads;
            int32_t split = remainder * (base + 1);
            int32_t owner = (i < split) ? i / (base + 1) : remainder + (i - split) / base;

            group_id = (owner % num_groups) + 1;
        }

        if(mode == MODE_AUTO) {
            snprintf(path, sizeof(path), TAG_PATH_AUTO_FMT, base_tag_path, group_id, auto_ms);
        } else {
            snprintf(path, sizeof(path), TAG_PATH_FMT, base_tag_path, group_id);
        }

        tag_handles[i] = plc_tag_create(path, TAG_CREATE_TIMEOUT_MS);
        if(tag_handles[i] < 0) {
            fprintf(stderr, "ERROR: Failed to create tag %" PRId32 " (group %" PRId32 "): %s\n", i, group_id,
                    plc_tag_decode_error(tag_handles[i]));
            for(int32_t j = 0; j < num_tags; j++) {
                if(tag_handles[j] > 0) { plc_tag_destroy(tag_handles[j]); }
            }
            free(stats);
            free(per_unit_ops);
            return 1;
        }
    }
    int64_t create_ms = (now_us() - create_start) / 1000;
    fprintf(stderr, "All %" PRId32 " tags created in %" PRId64 " ms.\n", num_tags, create_ms);

    /* One sync read per tag primes the connections and, for writes, the buffer contents. */
    int64_t prime_start = now_us();
    for(int32_t i = 0; i < num_tags; i++) {
        int32_t prc = (int32_t)plc_tag_read(tag_handles[i], TAG_OP_TIMEOUT_MS);
        if(prc != PLCTAG_STATUS_OK) {
            fprintf(stderr, "WARNING: prime read failed on tag %" PRId32 ": %s\n", i, plc_tag_decode_error(prc));
        }
    }
    int64_t prime_ms = (now_us() - prime_start) / 1000;

    if(mode == MODE_AUTO) {
        for(int32_t i = 0; i < num_tags; i++) {
            int32_t crc = (int32_t)plc_tag_register_callback_ex(tag_handles[i], auto_callback, &stats[i]);
            if(crc != PLCTAG_STATUS_OK) {
                fprintf(stderr, "ERROR: callback registration failed on tag %" PRId32 ": %s\n", i, plc_tag_decode_error(crc));
                rc = 1;
                goto cleanup;
            }
        }
    } else {
        int32_t base = num_tags / num_threads;
        int32_t remainder = num_tags % num_threads;
        int32_t offset = 0;

        for(int32_t i = 0; i < num_threads; i++) {
            int32_t count = base + (i < remainder ? 1 : 0);
            tdata[i].thread_id = i;
            tdata[i].tags = &tag_handles[offset];
            tdata[i].stats = &stats[offset];
            tdata[i].num_tags = count;
            tdata[i].mode = (bench_mode_t)mode;
            tdata[i].is_write = is_write;
            offset += count;
        }

        for(int32_t i = 0; i < num_threads; i++) {
            if(compat_thread_create(&threads[i], thread_func, &tdata[i]) != 0) {
                fprintf(stderr, "ERROR: Failed to create thread %" PRId32 "\n", i);
                compat_atomic_store_int32(&done, 1);
                rc = 1;
                goto cleanup;
            }
            threads_started++;
        }

        /* Brief pause for threads to reach their spin-wait. */
        compat_sleep_ms(50, NULL);
    }

    /*--- Run the benchmark ---*/
    double cpu_start = get_cpu_time_ms();
    int64_t wall_start = now_us();

    compat_atomic_store_int32(&go, 1);

    for(int32_t s = 0; s < duration_s && !compat_atomic_load_int32(&terminate); s++) { compat_sleep_ms(1000, NULL); }

    compat_atomic_store_int32(&done, 1);

    for(int32_t i = 0; i < threads_started; i++) { compat_thread_join(threads[i], NULL); }
    threads_started = 0;

    int64_t duration_ms = (now_us() - wall_start) / 1000;
    double cpu_time_ms = get_cpu_time_ms() - cpu_start;
    int64_t rss_growth_kb = get_peak_rss_kb() - rss_base_kb;

    /*--- Aggregate results ---*/
    int64_t total_ops = 0;
    int64_t total_errors = 0;
    int64_t max_us = 0;
    uint64_t hist_total = 0;

    for(int32_t i = 0; i < num_tags; i++) {
        total_ops += stats[i].ops;
        total_errors += stats[i].errors;
        if(stats[i].max_us > max_us) { max_us = stats[i].max_us; }
        for(size_t b = 0; b < HIST_BUCKETS; b++) {
            total_hist[b] += stats[i].hist[b];
            hist_total += stats[i].hist[b];
        }
    }

    if(mode == MODE_AUTO) {
        for(int32_t i = 0; i < num_tags; i++) { per_unit_ops[i] = (int)stats[i].ops; }
    } else {
        for(int32_t i = 0; i < num_threads; i++) {
            int64_t n = 0;
            for(int32_t j = 0; j < tdata[i].num_tags; j++) { n += tdata[i].stats[j].ops; }
            per_unit_ops[i] = (int)n;
        }
    }

    double ops_per_sec = (duration_ms > 0) ? (double)total_ops / ((double)duration_ms / 1000.0) : 0.0;
    double cpu_load_pct = (duration_ms > 0) ? (cpu_time_ms / (double)duration_ms) * 100.0 : 0.0;
    double cpu_us_per_op = (total_ops > 0) ? cpu_time_ms * 1000.0 / (double)total_ops : 0.0;
    double tags_per_thread = (double)num_tags / (double)num_threads;

    double fairness_cv = 0.0;
    double fairness_min_max = 1.0;
    stats_summary_t summary;
    bool have_summary = (num_units > 1 && stats_calculate(per_unit_ops, (int)num_units, &summary) == 0);

    if(have_summary) {
        fairness_cv = summary.cv;
        fairness_min_max = summary.min_max_ratio;
    }

    int64_t p50 = percentile(total_hist, hist_total, 0.50);
    int64_t p95 = percentile(total_hist, hist_total, 0.95);
    int64_t p99 = percentile(total_hist, hist_total, 0.99);

    printf("%s,%" PRId32 ",%" PRId32 ",%" PRId32 ",%.2f,%" PRId64 ",%" PRId64 ",%.1f,%.2f,%.2f,%.4f,%s,%" PRId64 ",%" PRId64
           ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%.2f,%" PRId64 ",%" PRId64 ",%" PRId64 "\n",
           mode_names[mode], num_groups, num_threads, num_tags, tags_per_thread, duration_ms, total_ops, ops_per_sec, cpu_load_pct,
           fairness_cv, fairness_min_max, is_write ? "write" : "read", total_errors, p50, p95, p99, max_us, cpu_us_per_op,
           rss_growth_kb, create_ms, prime_ms);
    fflush(stdout);

    fprintf(stderr, "\n--- Results ---\n");
    fprintf(stderr, "Duration:       %" PRId64 " ms\n", duration_ms);
    fprintf(stderr, "Total ops:      %" PRId64 "\n", total_ops);
    fprintf(stderr, "Total errors:   %" PRId64 "\n", total_errors);
    fprintf(stderr, "Ops/sec:        %.1f\n", ops_per_sec);
    fprintf(stderr, "Latency us:     p50 %" PRId64 "  p95 %" PRId64 "  p99 %" PRId64 "  max %" PRId64 "\n", p50, p95, p99, max_us);
    fprintf(stderr, "CPU load:       %.2f%%  (%.2f us/op)\n", cpu_load_pct, cpu_us_per_op);
    fprintf(stderr, "RSS growth:     %" PRId64 " KB\n", rss_growth_kb);
    fprintf(stderr, "Create/prime:   %" PRId64 " / %" PRId64 " ms\n", create_ms, prime_ms);
    fprintf(stderr, "Fairness CV:    %.2f%%\n", fairness_cv);
    fprintf(stderr, "Fairness ratio: %.4f\n", fairness_min_max);
    if(have_summary) { stats_print_summary(stderr, &summary); }

cleanup:
    for(int32_t i = 0; i < threads_started; i++) { compat_thread_join(threads[i], NULL); }

    fprintf(stderr, "Destroying tags...\n");
    for(int32_t i = 0; i < num_tags; i++) { plc_tag_destroy(tag_handles[i]); }

    /* Callbacks are gone once their tags are destroyed. */
    free(stats);
    free(per_unit_ops);

    fprintf(stderr, "Done.\n");

    return (rc != 0 || compat_atomic_load_int32(&terminate)) ? 1 : 0;
}
