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
 * Create many tags, write each one, read each one back and check it, then destroy them all.
 * Step 1.1 of docs/scale_and_performance_plan.md.
 *
 * Tag i is <name><a>[<e>] with a = (i mod K) + 1 and e = i / K, on connection group
 * (i mod G) + 1.  K is --arrays, default the smallest multiple of G that is at least
 * ceil(N / MAX_ARRAY_ELEMS), so every server array belongs to one connection and holds at most
 * MAX_ARRAY_ELEMS elements.  The server needs <name>1 .. <name>K, each with at least ceil(N / K)
 * elements.
 *
 * Each phase starts every tag's operation with timeout 0 and then waits for all of them, so a
 * phase of N tags costs about N / (requests per packet) round trips, not N.  Thread j handles
 * tags j, j + T, j + 2T, ...
 *
 * Values are written to the first 4 bytes of each element, so an element of any size works.
 *
 * Usage:
 *   test_many_tags --tag=BASE --name=PREFIX --tags=N [--groups=G] [--threads=T] [--arrays=K]
 *                  [--max-create-ms=M] [--passes=R] [--read-s=S] [--idle-s=S] [--max-idle-cpu-pct=P]
 * BASE is a tag string without name and connection_group_id.  --max-create-ms fails the test
 * if creating all the tags takes longer.
 *
 * --passes (default 1) repeats the read phase R times, and --read-s (default 0) keeps repeating
 * it until S seconds have passed too, so short passes add up to a window long enough to sample CPU.
 * Every pass is logged, then the fastest, median and slowest pass, the read rates and the client
 * CPU over all passes.  "# reads start"
 * and "# reads end" lines bracket the passes so a runner can sample the server over them.
 * Peak RSS is logged before create, after create (with bytes per tag) and after the reads.
 *
 * --idle-s (default 0, off) leaves every tag created with no operations for S seconds after the
 * read phase and logs the process CPU over that time.  It catches work that grows with the tag
 * count instead of with activity.  --max-idle-cpu-pct fails the test if that CPU, as percent of
 * one core, is higher.
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef POSIX_PLATFORM
#    include <sys/resource.h>
#endif

#define REQUIRED_VERSION 2, 6, 0

#define MAX_ARRAY_ELEMS (65535) /* the most elements libplctag can address in one array */
#define MAX_TAGS (1000000)
#define MAX_THREADS (64)
#define MAX_GROUPS (32767)
#define TAG_STR_SIZE (1024)
#define PHASE_TIMEOUT_MS (60000)
#define MAX_REPORTED_FAILURES (10)

typedef enum { PHASE_CREATE, PHASE_WRITE, PHASE_READ, PHASE_DESTROY } phase_t;

typedef struct {
    int32_t first;
    phase_t phase;
    int32_t failures;
} worker_t;

static const char *phase_names[] = {"create", "write", "read", "destroy"};

static const char *base_tag = NULL;
static const char *name_prefix = NULL;
static int32_t num_tags = 0;
static int32_t num_groups = 1;
static int32_t num_threads = 1;
static int32_t num_arrays = 0;
static int32_t *tags = NULL;
static uint32_t seed = 0;
static compat_atomic_int32_t reported = {0};

static int64_t now_us(void);
static int32_t run_phase(phase_t phase, int64_t *elapsed_us);


static int64_t now_ms(void) { return compat_time_ms(); }


static void report(int32_t i, const char *what, int32_t rc) {
    if(compat_atomic_inc_int32(&reported) <= MAX_REPORTED_FAILURES) {
        printf("FAIL tag %" PRId32 ": %s: %s\n", i, what, plc_tag_decode_error(rc));
    }
}


/* process CPU time, user + system */
static double cpu_time_ms(void) {
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


static int cmp_int64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}


/* Read every tag at least passes times and for at least read_s seconds.  Logs each pass, then
   the summary.  Returns the failure count. */
static int32_t run_reads(int32_t passes, int32_t read_s) {
    int32_t cap = passes;
    int64_t *pass_us = (int64_t *)calloc((size_t)cap, sizeof(int64_t));
    int32_t failures = 0;
    int32_t done = 0;

    if(!pass_us) {
        printf("FAIL: out of memory\n");
        return 1;
    }

    printf("# reads start\n");
    fflush(stdout);
    double cpu_start = cpu_time_ms();
    int64_t wall_start = now_us();

    for(done = 0; failures == 0 && (done < passes || now_us() - wall_start < (int64_t)read_s * 1000000); done++) {
        if(done == cap) {
            int64_t *more = (int64_t *)realloc(pass_us, (size_t)cap * 2 * sizeof(int64_t));
            if(!more) {
                printf("FAIL: out of memory\n");
                failures++;
                break;
            }
            pass_us = more;
            cap *= 2;
        }
        failures += run_phase(PHASE_READ, &pass_us[done]);
        printf("read     pass %" PRId32 ": %" PRId32 " tags in %.3f ms, %.0f tags/s, %" PRId32 " failures\n", done + 1, num_tags,
               (double)pass_us[done] / 1000.0, pass_us[done] > 0 ? (double)num_tags * 1e6 / (double)pass_us[done] : 0.0,
               failures);
    }

    int64_t wall_us = now_us() - wall_start;
    double cpu_ms = cpu_time_ms() - cpu_start;
    printf("# reads end\n");
    fflush(stdout);

    qsort(pass_us, (size_t)done, sizeof(int64_t), cmp_int64);
    double sum_us = 0.0;
    for(int32_t p = 0; p < done; p++) { sum_us += (double)pass_us[p]; }
    double median_us = (done % 2) ? (double)pass_us[done / 2] : ((double)pass_us[done / 2 - 1] + (double)pass_us[done / 2]) / 2.0;
    double rate = (double)num_tags * 1e6;

    printf("reads    %" PRId32 " passes: fastest %.3f ms, median %.3f ms, slowest %.3f ms, mean %.3f ms\n", done,
           (double)pass_us[0] / 1000.0, median_us / 1000.0, (double)pass_us[done - 1] / 1000.0, sum_us / done / 1000.0);
    printf("rate     fastest pass %.0f tags/s, median pass %.0f tags/s, overall %.0f tags/s\n",
           pass_us[0] > 0 ? rate / (double)pass_us[0] : 0.0, median_us > 0 ? rate / median_us : 0.0,
           wall_us > 0 ? rate * done / (double)wall_us : 0.0);
    printf("CPU      client %.1f ms over %.1f ms of reads, %.1f%% of one core\n", cpu_ms, (double)wall_us / 1000.0,
           wall_us > 0 ? cpu_ms * 100000.0 / (double)wall_us : 0.0);

    free(pass_us);
    return failures;
}


/* Leave every tag alone for idle_s seconds.  Returns 1 if CPU is over the limit, else 0. */
static int32_t run_idle(int32_t idle_s, double max_idle_cpu_pct) {
    double cpu_start = cpu_time_ms();
    int64_t wall_start = now_ms();
    compat_sleep_ms((uint32_t)idle_s * 1000, NULL);
    int64_t wall_ms = now_ms() - wall_start;
    double cpu_ms = cpu_time_ms() - cpu_start;
    double pct = wall_ms > 0 ? cpu_ms * 100.0 / (double)wall_ms : 0.0;

    printf("idle     %" PRId32 " tags for %" PRId64 " ms, CPU %.1f ms, %.2f%% of one core\n", num_tags, wall_ms, cpu_ms, pct);
    if(max_idle_cpu_pct > 0.0 && pct > max_idle_cpu_pct) {
        printf("FAIL: idle CPU %.2f%% is more than %g%%\n", pct, max_idle_cpu_pct);
        return 1;
    }
    return 0;
}


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


static int64_t peak_rss_kb(void) {
#ifdef __linux__
    /* VmHWM is this image only; ru_maxrss can carry the parent's high-water mark over exec. */
    char line[128];
    int64_t kb = 0;
    FILE *f = fopen("/proc/self/status", "r");

    if(f) {
        while(fgets(line, sizeof(line), f)) {
            if(sscanf(line, "VmHWM: %" SCNd64, &kb) == 1) { break; }
        }
        fclose(f);
    }
    return kb;
#elif defined(POSIX_PLATFORM)
    struct rusage usage;
    if(getrusage(RUSAGE_SELF, &usage) != 0) { return 0; }
#    ifdef __APPLE__
    return (int64_t)usage.ru_maxrss / 1024; /* bytes on macOS */
#    else
    return (int64_t)usage.ru_maxrss;
#    endif
#else
    return 0; /* shortcut: no Windows peak RSS (needs psapi), add it if the ramps run on Windows */
#endif
}


static uint32_t value_for(int32_t i) { return (uint32_t)i ^ seed; }


/* Start one tag's part of a phase.  Returns false if it failed outright. */
static bool start(worker_t *w, int32_t i) {
    char str[TAG_STR_SIZE];
    int32_t rc = PLCTAG_STATUS_OK;
    int32_t array = (i % num_arrays) + 1;
    int32_t elem = i / num_arrays;

    switch(w->phase) {
        case PHASE_CREATE:
            compat_snprintf(str, sizeof(str), "%s&name=%s%" PRId32 "[%" PRId32 "]&connection_group_id=%" PRId32, base_tag,
                            name_prefix, array, elem, (i % num_groups) + 1);
            tags[i] = plc_tag_create(str, 0);
            rc = (tags[i] < 0) ? tags[i] : PLCTAG_STATUS_OK;
            break;

        case PHASE_WRITE:
            rc = plc_tag_set_uint32(tags[i], 0, value_for(i));
            if(rc == PLCTAG_STATUS_OK) { rc = plc_tag_write(tags[i], 0); }
            break;

        case PHASE_READ: rc = plc_tag_read(tags[i], 0); break;

        case PHASE_DESTROY: rc = plc_tag_destroy(tags[i]); break;
    }

    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        report(i, phase_names[w->phase], rc);
        return false;
    }

    return true;
}


/* Wait for one tag's part of a phase to finish, and check a read. */
static bool finish(worker_t *w, int32_t i, int64_t deadline) {
    int32_t rc = PLCTAG_STATUS_PENDING;

    if(w->phase == PHASE_DESTROY) { return true; }

    while((rc = plc_tag_status(tags[i])) == PLCTAG_STATUS_PENDING && now_ms() < deadline) { compat_sleep_ms(1, NULL); }

    if(rc != PLCTAG_STATUS_OK) {
        report(i, phase_names[w->phase], rc);
        return false;
    }

    if(w->phase == PHASE_READ && plc_tag_get_uint32(tags[i], 0) != value_for(i)) {
        report(i, "read back a different value", PLCTAG_ERR_BAD_DATA);
        return false;
    }

    return true;
}


static void *worker_func(void *arg) {
    worker_t *w = (worker_t *)arg;
    int64_t deadline = 0;

    for(int32_t i = w->first; i < num_tags; i += num_threads) {
        /* a tag whose create failed has nothing to do in the later phases. */
        if(w->phase != PHASE_CREATE && tags[i] <= 0) { continue; }

        if(!start(w, i)) {
            w->failures++;
            if(w->phase == PHASE_CREATE) { tags[i] = 0; }
        }
    }

    deadline = now_ms() + PHASE_TIMEOUT_MS;

    for(int32_t i = w->first; i < num_tags; i += num_threads) {
        if(tags[i] > 0 && !finish(w, i, deadline)) { w->failures++; }
    }

    return NULL;
}


/* Run one phase on every thread.  Returns the failure count and sets *elapsed_us. */
static int32_t run_phase(phase_t phase, int64_t *elapsed_us) {
    static compat_thread_t threads[MAX_THREADS];
    static bool started[MAX_THREADS];
    static worker_t workers[MAX_THREADS];
    int32_t failures = 0;
    int64_t start_us = now_us();

    for(int32_t j = 0; j < num_threads; j++) {
        workers[j].first = j;
        workers[j].phase = phase;
        workers[j].failures = 0;

        started[j] = (compat_thread_create(&threads[j], worker_func, &workers[j]) == 0);
        if(!started[j]) {
            printf("FAIL: cannot start thread %" PRId32 "\n", j);
            workers[j].failures = 1;
        }
    }

    for(int32_t j = 0; j < num_threads; j++) {
        if(started[j]) { compat_thread_join(threads[j], NULL); }
        failures += workers[j].failures;
    }

    *elapsed_us = now_us() - start_us;

    if(phase != PHASE_READ) {
        printf("%-8s %" PRId32 " tags in %" PRId64 " ms, %" PRId32 " failures\n", phase_names[phase], num_tags,
               *elapsed_us / 1000, failures);
    }
    fflush(stdout);

    return failures;
}


static void usage(void) {
    printf("Usage: test_many_tags --tag=BASE --name=PREFIX --tags=N [--groups=G] [--threads=T] [--arrays=K]\n"
           "                      [--max-create-ms=M] [--passes=R] [--read-s=S]\n"
           "                      [--idle-s=S] [--max-idle-cpu-pct=P]\n"
           "  BASE is a tag string without name and connection_group_id.  Tag i is PREFIX<a>[<e>],\n"
           "  a = (i mod K) + 1, e = i / K, on connection group (i mod G) + 1.\n"
           "  --passes repeats the read phase R times and logs each pass and the summary (default 1).\n"
           "  --read-s keeps repeating it until S seconds have passed as well (default 0).\n"
           "  --idle-s leaves the tags idle S seconds after the read phase and logs process CPU (default 0, off).\n"
           "  --max-idle-cpu-pct fails the test if idle CPU is more than P percent of one core.\n");
    exit(1);
}


int main(int argc, char **argv) {
    int64_t max_create_ms = 0;
    int32_t idle_s = 0;
    double max_idle_cpu_pct = 0.0;
    int32_t passes = 1;
    int32_t read_s = 0;
    int64_t elapsed_us = 0;
    int32_t failures = 0;

    for(int32_t i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) {
            base_tag = argv[i] + 6;
        } else if(strncmp(argv[i], "--name=", 7) == 0) {
            name_prefix = argv[i] + 7;
        } else if(strncmp(argv[i], "--tags=", 7) == 0) {
            num_tags = (int32_t)atoi(argv[i] + 7);
        } else if(strncmp(argv[i], "--groups=", 9) == 0) {
            num_groups = (int32_t)atoi(argv[i] + 9);
        } else if(strncmp(argv[i], "--threads=", 10) == 0) {
            num_threads = (int32_t)atoi(argv[i] + 10);
        } else if(strncmp(argv[i], "--arrays=", 9) == 0) {
            num_arrays = (int32_t)atoi(argv[i] + 9);
        } else if(strncmp(argv[i], "--max-create-ms=", 16) == 0) {
            max_create_ms = (int64_t)atoll(argv[i] + 16);
        } else if(strncmp(argv[i], "--passes=", 9) == 0) {
            passes = (int32_t)atoi(argv[i] + 9);
        } else if(strncmp(argv[i], "--read-s=", 9) == 0) {
            read_s = (int32_t)atoi(argv[i] + 9);
        } else if(strncmp(argv[i], "--idle-s=", 9) == 0) {
            idle_s = (int32_t)atoi(argv[i] + 9);
        } else if(strncmp(argv[i], "--max-idle-cpu-pct=", 19) == 0) {
            max_idle_cpu_pct = atof(argv[i] + 19);
        } else {
            printf("Unknown argument: %s\n", argv[i]);
            usage();
        }
    }

    if(!base_tag || !name_prefix || num_tags <= 0 || num_tags > MAX_TAGS || num_groups <= 0 || num_groups > MAX_GROUPS
       || num_threads <= 0 || num_threads > MAX_THREADS || num_arrays < 0 || idle_s < 0 || passes <= 0 || read_s < 0) {
        usage();
    }

    if(num_arrays == 0) {
        int32_t needed = (num_tags + MAX_ARRAY_ELEMS - 1) / MAX_ARRAY_ELEMS;
        num_arrays = ((needed + num_groups - 1) / num_groups) * num_groups;
    }

    if((num_tags + num_arrays - 1) / num_arrays > MAX_ARRAY_ELEMS) {
        printf("FAIL: %" PRId32 " tags in %" PRId32 " arrays is more than %d elements per array.\n", num_tags, num_arrays,
               MAX_ARRAY_ELEMS);
        return 1;
    }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required library version not available!\n");
        return 1;
    }

    tags = (int32_t *)calloc((size_t)num_tags, sizeof(int32_t));
    if(!tags) {
        printf("FAIL: out of memory\n");
        return 1;
    }

    seed = (uint32_t)now_ms();

    printf("%" PRId32 " tags, %" PRId32 " groups, %" PRId32 " threads, %" PRId32 " arrays of %" PRId32 " elements\n", num_tags,
           num_groups, num_threads, num_arrays, (num_tags + num_arrays - 1) / num_arrays);

    int64_t rss_start_kb = peak_rss_kb();
    failures += run_phase(PHASE_CREATE, &elapsed_us);
    int64_t rss_created_kb = peak_rss_kb();
    printf("memory   peak RSS %" PRId64 " KB before create, %" PRId64 " KB after create, %.0f bytes per tag\n", rss_start_kb,
           rss_created_kb, (double)(rss_created_kb - rss_start_kb) * 1024.0 / num_tags);
    if(max_create_ms > 0 && elapsed_us / 1000 > max_create_ms) {
        printf("FAIL: create took %" PRId64 " ms, more than %" PRId64 " ms\n", elapsed_us / 1000, max_create_ms);
        failures++;
    }

    if(failures == 0) { failures += run_phase(PHASE_WRITE, &elapsed_us); }
    if(failures == 0) {
        failures += run_reads(passes, read_s);
        printf("memory   peak RSS %" PRId64 " KB after reads\n", peak_rss_kb());
    }
    if(failures == 0 && idle_s > 0) { failures += run_idle(idle_s, max_idle_cpu_pct); }

    failures += run_phase(PHASE_DESTROY, &elapsed_us);

    free(tags);

    printf("%s\n", failures == 0 ? "PASS" : "FAIL");

    return failures == 0 ? 0 : 1;
}
