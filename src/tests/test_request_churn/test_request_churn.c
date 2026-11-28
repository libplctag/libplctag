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
 * test_request_churn
 *
 * Random reads, writes, aborts, status calls and destroy/recreate on many tags from several
 * threads, with timeouts mostly of 0-4 ms against a server that delays its replies.  That releases CIP
 * requests every way there is (reply, abort while queued, abort on the wire, destroy) and reuses
 * each tag's spare request.  Run it under ASan, where a spare request is poisoned.
 *
 * Tag i is set to value_for(i) before the churn starts and only ever writes that value, so a
 * read that succeeds returns value_for(i).  Anything else is a reply that landed in the wrong
 * request.  The setup write matters on a real PLC, where earlier tests leave their own values.
 *
 * Usage: test_request_churn "<base tag string>" <name> <tags> <threads> <seconds>
 *   Tag i is <name>[i], one DINT.  A PCCC data file name ending in ':' (N7:) gives N7:<i>, one INT.
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 6, 0
#define CREATE_TIMEOUT (5000)
#define MAX_TAGS (2000)
#define MAX_THREADS (32)
#define MAX_REPORTED (10)

typedef struct {
    int32_t first;
    uint32_t seed;
} worker_t;

static const char *base_tag = NULL;
static const char *array_name = NULL;
static bool pccc = false;
static int32_t num_tags = 0;
static int32_t num_threads = 0;
static int64_t end_time = 0;
static int32_t tags[MAX_TAGS];

static compat_atomic_int32_t ops = {0};
static compat_atomic_int32_t completed_reads = {0};
static compat_atomic_int32_t timeouts = {0};
static compat_atomic_int32_t aborts = {0};
static compat_atomic_int32_t recreates = {0};
static compat_atomic_int32_t mismatches = {0};
static compat_atomic_int32_t unexpected = {0};

static int32_t value_for(int32_t i) { return pccc ? (int32_t)(0x5A00 | (i & 0xFF)) : (int32_t)(0x5A000000 | (uint32_t)i); }

static uint32_t next_rand(uint32_t *s) {
    /* xorshift32: rand() is not thread safe. */
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static int32_t create_tag(int32_t i) {
    char str[512];

    compat_snprintf(str, sizeof(str), pccc ? "%s&elem_count=1&name=%s%" PRId32 : "%s&elem_count=1&name=%s[%" PRId32 "]",
                    base_tag, array_name, i);

    return plc_tag_create(str, CREATE_TIMEOUT);
}

static void check_rc(int32_t i, const char *what, int32_t rc) {
    if(rc == PLCTAG_STATUS_OK || rc == PLCTAG_STATUS_PENDING || rc == PLCTAG_ERR_BUSY) { return; }

    if(rc == PLCTAG_ERR_TIMEOUT) {
        compat_atomic_inc_int32(&timeouts);
        return;
    }

    if(rc == PLCTAG_ERR_ABORT) {
        compat_atomic_inc_int32(&aborts);
        return;
    }

    if(compat_atomic_inc_int32(&unexpected) <= MAX_REPORTED) {
        printf("FAIL: tag %" PRId32 " %s returned %s.\n", i, what, plc_tag_decode_error(rc));
    }
}

static void one_op(worker_t *w) {
    int32_t i = w->first + (int32_t)(next_rand(&w->seed) % (uint32_t)((num_tags - w->first + num_threads - 1) / num_threads)) * num_threads;
    uint32_t pick = next_rand(&w->seed) % 100;
    /* mostly 0-4 ms, so ops time out and abort; 1 in 8 waits long enough to finish on a real PLC. */
    int32_t timeout = (next_rand(&w->seed) % 8 == 0) ? 1000 : (int32_t)(next_rand(&w->seed) % 5);
    int32_t rc = PLCTAG_STATUS_OK;

    if(pick < 45) {
        rc = plc_tag_read(tags[i], timeout);
        check_rc(i, "read", rc);

        if(rc == PLCTAG_STATUS_OK) {
            int32_t val = pccc ? plc_tag_get_int16(tags[i], 0) : plc_tag_get_int32(tags[i], 0);

            compat_atomic_inc_int32(&completed_reads);

            if(val != value_for(i) && compat_atomic_inc_int32(&mismatches) <= MAX_REPORTED) {
                printf("FAIL: tag %" PRId32 " read 0x%08" PRIx32 ", expected 0x%08" PRIx32 ".\n", i, (uint32_t)val,
                       (uint32_t)value_for(i));
            }
        }
    } else if(pick < 65) {
        rc = pccc ? plc_tag_set_int16(tags[i], 0, (int16_t)value_for(i)) : plc_tag_set_int32(tags[i], 0, value_for(i));
        check_rc(i, "set", rc);
        rc = plc_tag_write(tags[i], timeout);
        check_rc(i, "write", rc);
    } else if(pick < 80) {
        check_rc(i, "abort", plc_tag_abort(tags[i]));
    } else if(pick < 95) {
        check_rc(i, "status", plc_tag_status(tags[i]));
    } else {
        /* destroy with whatever is in flight, queued, or kept as the spare. */
        plc_tag_destroy(tags[i]);
        tags[i] = create_tag(i);
        compat_atomic_inc_int32(&recreates);

        /* a failed create is a failure, but the tag must exist for the next op. */
        while(tags[i] < 0 && compat_time_ms() < end_time) {
            check_rc(i, "create", tags[i]);
            tags[i] = create_tag(i);
        }
    }

    compat_atomic_inc_int32(&ops);
}

static void *worker(void *arg) {
    worker_t *w = (worker_t *)arg;

    while(compat_time_ms() < end_time) { one_op(w); }

    return NULL;
}

int main(int argc, char **argv) {
    compat_thread_t threads[MAX_THREADS];
    worker_t workers[MAX_THREADS];
    int32_t seconds = 0;
    int32_t failures = 0;

    if(argc != 6) {
        printf("Usage: test_request_churn \"<base tag string>\" <name> <tags> <threads> <seconds>\n");
        return 1;
    }

    base_tag = argv[1];
    array_name = argv[2];
    pccc = (array_name[0] && array_name[strlen(array_name) - 1] == ':');
    num_tags = (int32_t)atoi(argv[3]);
    num_threads = (int32_t)atoi(argv[4]);
    seconds = (int32_t)atoi(argv[5]);

    if(num_tags < 1 || num_tags > MAX_TAGS || num_threads < 1 || num_threads > MAX_THREADS || num_threads > num_tags
       || seconds < 1) {
        printf("FAIL: 1 <= threads <= tags <= %d, threads <= %d, seconds >= 1.\n", MAX_TAGS, MAX_THREADS);
        return 1;
    }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("FAIL: library version too old.\n");
        return 1;
    }

    for(int32_t i = 0; i < num_tags; i++) {
        tags[i] = create_tag(i);

        if(tags[i] < 0) {
            printf("FAIL: unable to create tag %" PRId32 ": %s.\n", i, plc_tag_decode_error(tags[i]));
            return 1;
        }

        /* whatever an earlier test left there is replaced. */
        if(pccc) {
            plc_tag_set_int16(tags[i], 0, (int16_t)value_for(i));
        } else {
            plc_tag_set_int32(tags[i], 0, value_for(i));
        }

        if(plc_tag_write(tags[i], CREATE_TIMEOUT) != PLCTAG_STATUS_OK) {
            printf("FAIL: unable to set tag %" PRId32 " to 0x%08" PRIx32 ".\n", i, (uint32_t)value_for(i));
            return 1;
        }
    }

    end_time = compat_time_ms() + (int64_t)seconds * 1000;

    for(int32_t t = 0; t < num_threads; t++) {
        workers[t].first = t;
        workers[t].seed = 0x9E3779B9u ^ (uint32_t)(t + 1) * 2654435761u;
        compat_thread_create(&threads[t], worker, &workers[t]);
    }

    for(int32_t t = 0; t < num_threads; t++) { compat_thread_join(threads[t], NULL); }

    /* the last ops may still be in flight: destroy releases them. */
    for(int32_t i = 0; i < num_tags; i++) {
        if(tags[i] >= 0) { plc_tag_destroy(tags[i]); }
    }

    failures = compat_atomic_load_int32(&mismatches) + compat_atomic_load_int32(&unexpected);

    printf("%" PRId32 " ops: %" PRId32 " completed reads, %" PRId32 " timeouts, %" PRId32 " aborts, %" PRId32
           " recreates, %" PRId32 " wrong values, %" PRId32 " unexpected errors.\n",
           compat_atomic_load_int32(&ops), compat_atomic_load_int32(&completed_reads), compat_atomic_load_int32(&timeouts),
           compat_atomic_load_int32(&aborts), compat_atomic_load_int32(&recreates), compat_atomic_load_int32(&mismatches),
           compat_atomic_load_int32(&unexpected));

    if(compat_atomic_load_int32(&completed_reads) == 0) {
        printf("FAIL: no read completed, so the reply path was not exercised.\n");
        failures++;
    }

    printf("%s\n", failures ? "FAIL" : "PASS");

    return failures ? 1 : 0;
}
