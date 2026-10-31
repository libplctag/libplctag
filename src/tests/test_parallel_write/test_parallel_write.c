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
 * test_parallel_write
 *
 * Each --tag gets its own thread.  The threads start together and, for --rounds rounds, write
 * a new 16-bit value to element 0 of their tag, read it back and compare.  A PLC that
 * acknowledges a write and then reads back something else is reported per round.
 *
 * Whether the tags share a session is up to the tag strings: give them different
 * connection_group_id values for separate sessions.  Only the low 16 bits of element 0 are
 * written and checked, so a 32-bit tag works too.
 *
 * Usage: test_parallel_write --tag="<tag string>" --tag="<tag string>" [--rounds=N]
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 6, 0
#define DATA_TIMEOUT (5000)
#define MAX_TAGS (8)
#define MAX_REPORTED (5)

typedef struct {
    const char *tag_string;
    int32_t index;
    int32_t rounds;
    int32_t mismatches;
    int32_t errors;
} worker_t;

static void *worker(void *arg);

static compat_atomic_int32_t go;


int main(int argc, char **argv) {
    worker_t workers[MAX_TAGS];
    compat_thread_t threads[MAX_TAGS];
    int32_t count = 0;
    int32_t rounds = 200;
    int32_t failures = 0;

    for(int32_t i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0 && count < MAX_TAGS) {
            workers[count] = (worker_t){.tag_string = argv[i] + 6, .index = count};
            count++;
        } else if(strncmp(argv[i], "--rounds=", 9) == 0) {
            rounds = (int32_t)atoi(argv[i] + 9);
        }
    }

    if(count < 1 || rounds < 1) {
        printf("Usage: test_parallel_write --tag=\"<tag string>\" [--tag=...] [--rounds=N]\n");
        return 1;
    }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    for(int32_t i = 0; i < count; i++) {
        workers[i].rounds = rounds;
        compat_thread_create(&threads[i], worker, &workers[i]);
    }

    compat_atomic_store_int32(&go, 1);

    for(int32_t i = 0; i < count; i++) {
        compat_thread_join(threads[i], NULL);
        printf("tag %" PRId32 ": %" PRId32 " lost writes, %" PRId32 " errors in %" PRId32 " rounds: %s\n", i,
               workers[i].mismatches, workers[i].errors, rounds, workers[i].tag_string);
        failures += workers[i].mismatches + workers[i].errors;
    }

    plc_tag_shutdown();

    printf("test_parallel_write: %s with %" PRId32 " failure(s).\n", failures ? "FAILED" : "PASSED", failures);

    return failures ? 1 : 0;
}


static void *worker(void *arg) {
    worker_t *w = (worker_t *)arg;
    int32_t tag = plc_tag_create(w->tag_string, DATA_TIMEOUT);

    if(tag < 0) {
        printf("tag %" PRId32 ": create failed, %s.\n", w->index, plc_tag_decode_error(tag));
        w->errors++;
        return NULL;
    }

    while(!compat_atomic_load_int32(&go)) { compat_sleep_ms(1, NULL); }

    for(int32_t round = 0; round < w->rounds; round++) {
        /* differs per round and per tag. */
        int16_t value = (int16_t)((round * MAX_TAGS + w->index + 1) & 0x7FFF);
        int16_t got = 0;
        int32_t rc = plc_tag_set_int16(tag, 0, value);

        if(rc == PLCTAG_STATUS_OK) { rc = plc_tag_write(tag, DATA_TIMEOUT); }
        if(rc == PLCTAG_STATUS_OK) { rc = plc_tag_read(tag, DATA_TIMEOUT); }

        if(rc != PLCTAG_STATUS_OK) {
            printf("tag %" PRId32 " round %" PRId32 ": %s.\n", w->index, round, plc_tag_decode_error(rc));
            w->errors++;
            continue;
        }

        got = plc_tag_get_int16(tag, 0);
        if(got != value) {
            if(w->mismatches < MAX_REPORTED) {
                printf("tag %" PRId32 " round %" PRId32 ": wrote %d, read back %d at %" PRId64 "ms.\n", w->index, round,
                       (int)value, (int)got, compat_time_ms());
            }
            w->mismatches++;
        }
    }

    plc_tag_destroy(tag);

    return NULL;
}
