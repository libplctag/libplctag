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
 * Step 1.1 of ephemeral_docs/scale_and_performance_plan.md.
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
 *                  [--max-create-ms=M]
 * BASE is a tag string without name and connection_group_id.  --max-create-ms fails the test
 * if creating all the tags takes longer.
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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


static int64_t now_ms(void) { return compat_time_ms(); }


static void report(int32_t i, const char *what, int32_t rc) {
    if(compat_atomic_inc_int32(&reported) <= MAX_REPORTED_FAILURES) {
        printf("FAIL tag %" PRId32 ": %s: %s\n", i, what, plc_tag_decode_error(rc));
    }
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


/* Run one phase on every thread.  Returns the failure count and sets *elapsed_ms. */
static int32_t run_phase(phase_t phase, int64_t *elapsed_ms) {
    static compat_thread_t threads[MAX_THREADS];
    static bool started[MAX_THREADS];
    static worker_t workers[MAX_THREADS];
    int32_t failures = 0;
    int64_t start_ms = now_ms();

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

    *elapsed_ms = now_ms() - start_ms;

    printf("%-8s %" PRId32 " tags in %" PRId64 " ms, %" PRId32 " failures\n", phase_names[phase], num_tags, *elapsed_ms, failures);
    fflush(stdout);

    return failures;
}


static void usage(void) {
    printf("Usage: test_many_tags --tag=BASE --name=PREFIX --tags=N [--groups=G] [--threads=T] [--arrays=K]\n"
           "                      [--max-create-ms=M]\n"
           "  BASE is a tag string without name and connection_group_id.  Tag i is PREFIX<a>[<e>],\n"
           "  a = (i mod K) + 1, e = i / K, on connection group (i mod G) + 1.\n");
    exit(1);
}


int main(int argc, char **argv) {
    int64_t max_create_ms = 0;
    int64_t elapsed_ms = 0;
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
        } else {
            printf("Unknown argument: %s\n", argv[i]);
            usage();
        }
    }

    if(!base_tag || !name_prefix || num_tags <= 0 || num_tags > MAX_TAGS || num_groups <= 0 || num_groups > MAX_GROUPS
       || num_threads <= 0 || num_threads > MAX_THREADS || num_arrays < 0) {
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

    failures += run_phase(PHASE_CREATE, &elapsed_ms);
    if(max_create_ms > 0 && elapsed_ms > max_create_ms) {
        printf("FAIL: create took %" PRId64 " ms, more than %" PRId64 " ms\n", elapsed_ms, max_create_ms);
        failures++;
    }

    if(failures == 0) { failures += run_phase(PHASE_WRITE, &elapsed_ms); }
    if(failures == 0) { failures += run_phase(PHASE_READ, &elapsed_ms); }

    failures += run_phase(PHASE_DESTROY, &elapsed_ms);

    free(tags);

    printf("%s\n", failures == 0 ? "PASS" : "FAIL");

    return failures == 0 ? 0 : 1;
}
