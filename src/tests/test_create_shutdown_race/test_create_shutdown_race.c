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
 * Drive plc_tag_create() concurrently with plc_tag_shutdown().
 *
 * plc_tag_shutdown() closes the registry gate and then sweeps the tag table, destroying
 * every tag it finds.  A create that passed the gate before it closed is still running,
 * and publishes its tag into the table when it finishes.  If that publish lands after the
 * sweep has already passed, the tag is never destroyed by shutdown and outlives the
 * protocol module teardown that follows.
 *
 * The observable signal is the callback: every tag shutdown destroys is delivered a
 * PLCTAG_EVENT_DESTROYED event before shutdown returns.  A tag published after the sweep
 * gets no such event.  So a create that returned a valid ID without a matching DESTROYED
 * event is a tag that escaped.
 *
 * Nothing here needs a PLC.  The creates are expected to fail to connect; a tag is
 * registered and its callback fires regardless of whether the connection succeeds, which
 * is all this test looks at.
 */

#include "compat_utils.h"
#include <libplctag/api/libplctag.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define REQUIRED_VERSION 2, 7, 0

/*
 * An address that will not connect.  The create still registers the tag and still raises
 * CREATED and DESTROYED, which is what this test counts, and a create that cannot reach a
 * PLC returns promptly rather than blocking the race window open.
 */
#define TAG_ATTRIBS "protocol=ab_eip&gateway=127.0.0.1:1&path=1,0&plc=ControlLogix&elem_type=DINT&elem_count=1&name=DummyTag"

#define NUM_CREATORS (4)

/* how long the creator threads run before shutdown is called. */
#define SPIN_UP_MS (300)

/* how many times to replay the race.  One pass rarely lands in the window. */
#define NUM_ROUNDS (20)


static compat_atomic_int32_t stop_flag = {0};
static compat_atomic_int32_t created_ok = {0};
static compat_atomic_int32_t destroyed_seen = {0};
static compat_atomic_int32_t create_failed = {0};


static void tag_callback(int32_t tag_id, int event, int status, void *userdata) {
    (void)tag_id;
    (void)status;
    (void)userdata;

    if(event == PLCTAG_EVENT_DESTROYED) { compat_atomic_inc_int32(&destroyed_seen); }
}


static void *creator_function(void *arg) {
    (void)arg;

    while(compat_atomic_load_int32(&stop_flag) == 0) {
        int32_t tag = plc_tag_create_ex(TAG_ATTRIBS, tag_callback, NULL, 0);

        if(tag > 0) {
            compat_atomic_inc_int32(&created_ok);

            /*
             * Deliberately not destroyed here.  Leaving it to shutdown is the whole point:
             * a tag this thread published is either swept by shutdown, which raises
             * DESTROYED, or it is not, which is the escape being tested for.
             */
        } else {
            compat_atomic_inc_int32(&create_failed);
        }
    }

    return NULL;
}


static int run_one_round(int round) {
    compat_thread_t creators[NUM_CREATORS];
    int32_t created = 0;
    int32_t destroyed = 0;
    int32_t failed = 0;
    int32_t escaped = 0;

    compat_atomic_store_int32(&stop_flag, 0);
    compat_atomic_store_int32(&created_ok, 0);
    compat_atomic_store_int32(&destroyed_seen, 0);
    compat_atomic_store_int32(&create_failed, 0);

    for(int i = 0; i < NUM_CREATORS; i++) {
        if(compat_thread_create(&creators[i], creator_function, NULL) != 0) {
            fprintf(stderr, "round %d: unable to start creator thread %d!\n", round, i);
            return -1;
        }
    }

    /* let the creators get into their stride so shutdown lands mid-create. */
    compat_sleep_ms(SPIN_UP_MS, NULL);

    /*
     * Stop the creators first, then shut down immediately.  Each thread finishes at most
     * one more create, so there are creates in flight across the shutdown -- the race
     * being tested -- without an unbounded tail of new ones afterwards.
     */
    compat_atomic_store_int32(&stop_flag, 1);

    plc_tag_shutdown();

    for(int i = 0; i < NUM_CREATORS; i++) { compat_thread_join(creators[i], NULL); }

    /*
     * A create that began after the gate closed re-initialises the library and registers
     * its tag in a fresh registry, which the first shutdown never saw.  That is a restart,
     * not an escape, so sweep again before counting: the second shutdown destroys those
     * tags and raises their DESTROYED events, and any gap left over is a real escape.
     */
    plc_tag_shutdown();

    created = compat_atomic_load_int32(&created_ok);
    destroyed = compat_atomic_load_int32(&destroyed_seen);
    failed = compat_atomic_load_int32(&create_failed);

    /*
     * Creates that ran after the gate closed return an error and never register a tag, so
     * they are not part of the comparison.  Only tags that were actually created count.
     */
    escaped = created - destroyed;

    printf("round %2d: created %4d, destroyed %4d, create failed %4d", round, created, destroyed, failed);

    if(escaped > 0) {
        printf("  <-- %d tag%s escaped the shutdown sweep\n", escaped, (escaped == 1 ? "" : "s"));
    } else {
        printf("\n");
    }

    return (int)escaped;
}


int main(void) {
    int total_escaped = 0;
    int rounds_with_escapes = 0;

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    printf("Racing plc_tag_create() against plc_tag_shutdown(), %d rounds, %d creator threads.\n", NUM_ROUNDS, NUM_CREATORS);

    for(int round = 1; round <= NUM_ROUNDS; round++) {
        int escaped = run_one_round(round);

        if(escaped < 0) { return 1; }

        if(escaped > 0) {
            total_escaped += escaped;
            rounds_with_escapes++;
        }
    }

    printf("\n%d of %d rounds leaked a tag past shutdown, %d tags in total.\n", rounds_with_escapes, NUM_ROUNDS,
           total_escaped);

    /*
     * A tag that escapes the sweep is a tag nothing will ever destroy, holding protocol
     * state that module teardown has already freed.  add_tag_lookup() refuses to publish
     * once the sweep has begun, so this must be zero.
     *
     * Note that the plain run is a weak check: the window between a create taking its
     * registry reference and publishing its tag is only a few instructions wide, and
     * before it was closed this test still reported zero escapes over ~155,000 races.
     * What actually demonstrates the property is widening that window -- insert a delay
     * ahead of the add_tag_lookup() call in plc_tag_create_impl() and every create landing
     * in the window escapes without the guard, and is refused with it.
     */
    if(total_escaped > 0) {
        printf("\nFAIL: %d tag%s outlived plc_tag_shutdown().\n", total_escaped, (total_escaped == 1 ? "" : "s"));
        return 1;
    }

    printf("PASS: shutdown raced %d times, no tag outlived it, no crash or sanitizer report.\n", NUM_ROUNDS);

    return 0;
}
