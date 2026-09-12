/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes  & @Joylei                           *
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

#include "compat_utils.h"
#include <utils/nap.h>
#include <utils/thread.h>
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*

Show how to have a callback wake up another
thread when a tag is ready to read.

*/

#define REQUIRED_VERSION 2, 6, 4

/*
 * The "%d" is filled in per thread, so each thread gets its own array element.
 * The default points at the local simulator; the hardware suite overrides it
 * with --tag=.
 */
#define DEFAULT_TAG_ATTRIBS "protocol=ab_eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[%d]"
#define ELEM_COUNT 1
#define ELEM_SIZE 4
#define DATA_TIMEOUT 500

/*
 * The wait below used to be an untimed condition variable wait.  A nap needs a
 * deadline, and a read that has not completed by this point has failed anyway:
 * the status check that follows the wait reports it.
 */
#define READ_EVENT_TIMEOUT_MS (5000)

#define MAX_THREADS (20)

typedef struct {
    int tid;
    int32_t tag;
    nap_p read_event;
} tag_state;


static compat_atomic_int32_t done = {0};

/*
 * The test used to run until ^C and always exit 0, which made it useless to a
 * runner.  These two make it report: any thread that gives up early bumps
 * thread_failures, every completed read bumps reads_completed, and main turns
 * the pair into an exit code.
 */
static compat_atomic_int32_t thread_failures = {0};
static compat_atomic_int32_t reads_completed = {0};

void interrupt_handler(void) { compat_atomic_store_int32(&done, 1); }

static int num_threads = 0;
static const char *tag_attribs = DEFAULT_TAG_ATTRIBS;
static tag_state states[MAX_THREADS] = {0};

void tag_callback(int32_t tag_id, int event, int status, void *arg) {
    int tid = (int)(intptr_t)arg;

    if(event != PLCTAG_EVENT_READ_COMPLETED) { return; }

    // NOLINTNEXTLINE
    fprintf(stderr, "callback tag(%d), tag id(%d), event(%d), status(%d)\n", tid, tag_id, event, status);

    nap_interrupt(states[tid].read_event);
}


/*
 * Thread function.  Just read until killed.
 */

THREAD_FUNC(thread_func) {
    int rc = PLCTAG_STATUS_OK;
    int tid = (int)(intptr_t)arg;
    int value;
    char buf[250] = {
        0,
    };

    // NOLINTNEXTLINE
    snprintf(buf, sizeof(buf), tag_attribs, tid);

    /* create the tag */
    int tag = plc_tag_create(buf, 0);
    states[tid].tag = tag;
    states[tid].tid = tid;

    /* should check result! */
    nap_create(&states[tid].read_event);

    /* everything OK? */
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        compat_atomic_add_int32(&thread_failures, 1);
        THREAD_RETURN(0);
    }

    while((rc = plc_tag_status(tag)) == PLCTAG_STATUS_PENDING) {
        if(compat_atomic_load_int32(&done)) { break; }
        thread_yield();
    }

    if(rc != PLCTAG_STATUS_OK) {
        /* a shutdown while the tag was still setting up is not a failure. */
        if(!compat_atomic_load_int32(&done)) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Error setting up tag internal state. %s\n", plc_tag_decode_error(rc));
            compat_atomic_add_int32(&thread_failures, 1);
        }
        plc_tag_destroy(tag);
        nap_destroy(&states[tid].read_event);
        THREAD_RETURN(0);
    }

    /* use extended callback to pass the thread index/id */
    plc_tag_register_callback_ex(tag, tag_callback, (void *)(intptr_t)tid);

    while(!compat_atomic_load_int32(&done)) {
        int64_t start;
        int64_t end;

        /* capture the starting time */
        start = compat_time_ms();

        do {
            /*
             * The callback fires on every completed read, including one that
             * completes before plc_tag_read() returns.  Discard any interrupt
             * left over from the previous pass so the wait below is real.
             */
            nap_clear(states[tid].read_event);

            rc = plc_tag_read(tag, 0);
            // NOLINTNEXTLINE
            if(rc < 0) {
                fprintf(stderr, "Error starting tag read. %s\n", plc_tag_decode_error(rc));
                compat_atomic_add_int32(&thread_failures, 1);
                break;
            }
            if(rc == PLCTAG_STATUS_PENDING) {
                nap_wait(states[tid].read_event, READ_EVENT_TIMEOUT_MS);

                if((rc = plc_tag_status(tag)) != PLCTAG_STATUS_OK) {
                    // NOLINTNEXTLINE
                    fprintf(stderr, "something is wrong for tag(%d), status(%s)\n", tag, plc_tag_decode_error(rc));
                    compat_atomic_add_int32(&thread_failures, 1);
                    plc_tag_destroy(tag);
                    nap_destroy(&states[tid].read_event);
                    THREAD_RETURN(0);
                }
            }
            value = plc_tag_get_int32(tag, 0);
            compat_atomic_add_int32(&reads_completed, 1);
        } while(0);

        end = compat_time_ms();

        // NOLINTNEXTLINE
        fprintf(stderr, "Thread %d got result %d with return code %s in %" PRId64 "ms\n", tid, value, plc_tag_decode_error(rc),
                (end - start));

        thread_yield();
    }

    plc_tag_destroy(tag);

    nap_destroy(&states[tid].read_event);

    THREAD_RETURN(0);
}

int main(int argc, char **argv) {

    thread_p thread[MAX_THREADS];

    int thread_id = 0;
    int run_seconds = 0;
    int64_t deadline = 0;
    int failures = 0;
    int reads = 0;

    /* set up handler for ^C etc. */
    compat_set_interrupt_handler(interrupt_handler);

    // NOLINTNEXTLINE
    fprintf(stderr, "Hit ^C to terminate the test.\n");

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    /*
     * Positional arguments are the thread count and, optionally, the run
     * duration in seconds.  --tag= may appear anywhere and overrides the
     * default attribute string.
     */
    for(int arg = 1; arg < argc; arg++) {
        if(strncmp(argv[arg], "--tag=", 6) == 0) {
            tag_attribs = &argv[arg][6];
        } else if(num_threads == 0) {
            num_threads = (int)strtol(argv[arg], NULL, 10);
        } else if(run_seconds == 0) {
            run_seconds = (int)strtol(argv[arg], NULL, 10);
        } else {
            // NOLINTNEXTLINE
            fprintf(stderr, "ERROR: unexpected argument \"%s\"!\n", argv[arg]);
            return 1;
        }
    }

    if(num_threads == 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Usage: %s <num_threads> [run_seconds] [--tag=<attribute string>]\n", argv[0]);
        return 1;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    if(num_threads < 1 || num_threads > MAX_THREADS) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: %d is not a valid thread count.  Must be between 1 and %d!\n", num_threads, MAX_THREADS);
        return 1;
    }

    /*
     * Without a duration the test runs until ^C, which is how it has always
     * been driven by hand.  A test runner needs it to stop on its own.
     */
    if(run_seconds < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: %d is not a valid run duration in seconds!\n", run_seconds);
        return 1;
    }

    /* create the read threads */
    // NOLINTNEXTLINE
    fprintf(stderr, "Creating %d threads.\n", num_threads);

    for(thread_id = 0; thread_id < num_threads; thread_id++) {
        thread_create(&thread[thread_id], thread_func, 0, (void *)(intptr_t)thread_id);
    }

    /* wait until ^C, or until the deadline if one was given */
    deadline = (run_seconds > 0) ? (compat_time_ms() + ((int64_t)run_seconds * 1000)) : 0;

    while(!compat_atomic_load_int32(&done)) {
        if(deadline > 0 && compat_time_ms() >= deadline) {
            compat_atomic_store_int32(&done, 1);
            break;
        }

        compat_sleep_ms(100, NULL);
    }

    for(thread_id = 0; thread_id < num_threads; thread_id++) { thread_join(&thread[thread_id]); }

    failures = compat_atomic_load_int32(&thread_failures);
    reads = compat_atomic_load_int32(&reads_completed);

    // NOLINTNEXTLINE
    fprintf(stderr, "%d reads completed, %d thread failures.\n", reads, failures);

    if(failures > 0 || reads == 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "FAILURE\n");
        return 1;
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "SUCCESS\n");
    return 0;
}
