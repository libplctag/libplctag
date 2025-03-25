/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes  & @Joylei                           *
 *   Author Kyle Hayes  kyle.hayes@gmail.com, @Joylei                      *
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

#include "../lib/libplctag.h"
#include "utils.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*

Show how to have a callback wake up another
thread when a tag is ready to read.

*/

#define REQUIRED_VERSION 2, 6, 4

#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.40&path=1,4&cpu=ControlLogix&elem_count=1&name=TestBigArray[%d]"
#define ELEM_COUNT 1
#define ELEM_SIZE 4
#define DATA_TIMEOUT 500

#define MAX_THREADS (20)

typedef struct {
    int tid;
    int32_t tag;
    cnd_t read_event;
    mtx_t mutex;
} tag_state;


volatile int done = 0;

void interrupt_handler(void) { done = 1; }

static int num_threads = 0;
static tag_state states[MAX_THREADS] = {0};

void tag_callback(int32_t tag_id, int event, int status, void *arg) {
    int tid = (int)(intptr_t)arg;

    if(event != PLCTAG_EVENT_READ_COMPLETED) { return; }

    fprintf(stderr, "callback tag(%d), tag id(%d), event(%d), status(%d)\n", tid, tag_id, event, status);

    mtx_lock(&states[tid].mutex);
    cnd_signal(&states[tid].read_event);
    mtx_unlock(&states[tid].mutex);
}


/*
 * Thread function.  Just read until killed.
 */

int thread_func(void *data) {
    int rc = PLCTAG_STATUS_OK;
    int tid = (int)(intptr_t)data;
    int value;
    char buf[250] = {
        0,
    };
    snprintf(buf, sizeof(buf), TAG_PATH, tid);
    /* create the tag */
    int tag = plc_tag_create(buf, 0);
    states[tid].tag = tag;
    states[tid].tid = tid;

    /* should check result! */
    mtx_init((mtx_t *)&states[tid].mutex, mtx_plain);
    cnd_init(&states[tid].read_event);

    /* everything OK? */
    if(tag < 0) {
        fprintf(stderr, "ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return 0;
    }

    while((rc = plc_tag_status(tag)) == PLCTAG_STATUS_PENDING) {
        if(done) { break; }
        thrd_yield();
    }

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error setting up tag internal state. %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        mtx_destroy(&states[tid].mutex);
        cnd_destroy(&states[tid].read_event);
        return 0;
    }

    /* use extended callback to pass the thread index/id */
    plc_tag_register_callback_ex(tag, tag_callback, (void *)(intptr_t)tid);

    while(!done) {
        int64_t start;
        int64_t end;

        /* capture the starting time */
        start = util_time_ms();

        do {
            rc = plc_tag_read(tag, 0);
            if(rc < 0) { fprintf(stderr, "Error setting up tag internal state. %s\n", plc_tag_decode_error(rc)); }
            if(rc == PLCTAG_STATUS_PENDING) {
                mtx_lock(&states[tid].mutex);
                cnd_wait(&states[tid].read_event, &states[tid].mutex);
                mtx_unlock(&states[tid].mutex);

                if((rc = plc_tag_status(tag)) != PLCTAG_STATUS_OK) {
                    fprintf(stderr, "something is wrong for tag(%d), status(%s)\n", tag, plc_tag_decode_error(rc));
                    plc_tag_destroy(tag);
                    mtx_destroy(&states[tid].mutex);
                    cnd_destroy(&states[tid].read_event);
                    return 0;
                }
            }
            value = plc_tag_get_int32(tag, 0);
        } while(0);

        end = util_time_ms();

        fprintf(stderr, "Thread %d got result %d with return code %s in %" PRId64 "ms\n", tid, value, plc_tag_decode_error(rc),
                (end - start));

        thrd_yield();
    }

    plc_tag_destroy(tag);
    mtx_destroy(&states[tid].mutex);
    cnd_destroy(&states[tid].read_event);

    return 0;
}

int main(int argc, char **argv) {

    thrd_t thread[MAX_THREADS];

    int thread_id = 0;

    /* set up handler for ^C etc. */
    set_interrupt_handler(interrupt_handler);

    fprintf(stderr, "Hit ^C to terminate the test.\n");

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    if(argc != 2) {
        fprintf(stderr, "ERROR: Must provide number of threads to run (between 1 and 300) argc=%d!\n", argc);
        return 0;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    num_threads = (int)strtol(argv[1], NULL, 10);

    if(num_threads < 1 || num_threads > MAX_THREADS) {
        fprintf(stderr, "ERROR: %d (%s) is not a valid number. Must provide number of threads to run (between 1 and 300)!\n",
                num_threads, argv[1]);
        return 0;
    }

    /* create the read threads */
    fprintf(stderr, "Creating %d threads.\n", num_threads);

    for(thread_id = 0; thread_id < num_threads; thread_id++) {
        thrd_create(&thread[thread_id], thread_func, (void *)(intptr_t)thread_id);
    }

    /* wait until ^C */
    while(!done) { thrd_sleep_ms(100, NULL); }

    for(thread_id = 0; thread_id < num_threads; thread_id++) { thrd_join(thread[thread_id], NULL); }

    return 0;
}
