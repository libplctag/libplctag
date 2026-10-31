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
 * test_timeout_next
 *
 * A read with a timeout completes, and its READ_COMPLETED callback starts the next read.
 * When the first call's timeout runs out while that next read is in flight, the first call
 * must return OK, and the next read must not be aborted.
 *
 * The timeout is set to 1.5 round trips, so the first read ends inside it and the next one
 * after it.  Round trips vary, so an attempt that did not land that way is retried.  This
 * needs a target with a round trip of several milliseconds: a slow PLC, or a simulator with
 * a reply delay.
 *
 * Usage: test_timeout_next "<tag string>"
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <stdio.h>

#define REQUIRED_VERSION 2, 6, 0
#define DATA_TIMEOUT (5000)
#define ATTEMPTS (20)

static void callback(int32_t tag_id, int event, int status, void *userdata);

static compat_atomic_int32_t restart_armed;
static compat_atomic_int32_t restart_rc;
static compat_atomic_int32_t reads_completed;
static compat_atomic_int32_t last_read_status;


int main(int argc, char **argv) {
    int32_t id = 0;
    int32_t rc = PLCTAG_STATUS_OK;
    int32_t failures = 0;
    int32_t tested = 0;

    if(argc != 2) {
        printf("Usage: test_timeout_next \"<tag string>\"\n");
        return 1;
    }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    id = plc_tag_create_ex(argv[1], callback, NULL, DATA_TIMEOUT);
    if(id < 0) {
        printf("FAIL: unable to create the tag, %s.\n", plc_tag_decode_error(id));
        return 1;
    }

    for(int32_t attempt = 0; attempt < ATTEMPTS && !tested && !failures; attempt++) {
        int64_t start = compat_time_ms();
        int32_t timeout = 0;
        int32_t done_at_return = 0;

        rc = plc_tag_read(id, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            printf("FAIL: the timing read returned %s.\n", plc_tag_decode_error(rc));
            failures++;
            break;
        }

        timeout = (int32_t)(compat_time_ms() - start) * 3 / 2;
        if(timeout < 1) { timeout = 1; }

        compat_atomic_store_int32(&reads_completed, 0);
        compat_atomic_store_int32(&restart_rc, PLCTAG_ERR_NO_DATA);
        compat_atomic_store_int32(&restart_armed, 1);

        rc = plc_tag_read(id, timeout);
        done_at_return = compat_atomic_load_int32(&reads_completed);
        compat_atomic_store_int32(&restart_armed, 0);

        /* the next read must finish on its own, whatever happened. */
        start = compat_time_ms();
        while(plc_tag_status(id) == PLCTAG_STATUS_PENDING && compat_time_ms() - start < DATA_TIMEOUT) {
            compat_sleep_ms(1, NULL);
        }

        if(compat_atomic_load_int32(&restart_rc) != PLCTAG_STATUS_PENDING) {
            /* the first read did not complete inside the timeout, so no next read started. */
            printf("Attempt %" PRId32 ": timeout %" PRId32 "ms, first read returned %s; retrying.\n", attempt, timeout,
                   plc_tag_decode_error(rc));
            continue;
        }

        if(rc != PLCTAG_STATUS_OK) {
            printf("FAIL: the read returned %s, but it completed inside its %" PRId32 "ms timeout.\n",
                   plc_tag_decode_error(rc), timeout);
            failures++;
        }

        if(compat_atomic_load_int32(&reads_completed) < 2) {
            printf("FAIL: the callback's read never completed.\n");
            failures++;
        } else if(compat_atomic_load_int32(&last_read_status) != PLCTAG_STATUS_OK) {
            printf("FAIL: the callback's read completed with %s.\n",
                   plc_tag_decode_error(compat_atomic_load_int32(&last_read_status)));
            failures++;
        }

        if(failures) { break; }

        if(done_at_return >= 2) {
            printf("Attempt %" PRId32 ": timeout %" PRId32 "ms, the next read ended before the timeout; retrying.\n",
                   attempt, timeout);
            continue;
        }

        printf("Attempt %" PRId32 ": timeout %" PRId32 "ms, the next read was in flight when the timeout ran out.\n",
               attempt, timeout);
        tested = 1;
    }

    if(!tested && !failures) {
        printf("FAIL: no attempt had the next read in flight when the timeout ran out, so nothing was tested.\n");
        failures++;
    }

    plc_tag_destroy(id);
    plc_tag_shutdown();

    printf("test_timeout_next: %s with %" PRId32 " failure(s).\n", failures ? "FAILED" : "PASSED", failures);

    return failures ? 1 : 0;
}


static void callback(int32_t tag_id, int event, int status, void *userdata) {
    (void)userdata;

    if(event != PLCTAG_EVENT_READ_COMPLETED) { return; }

    compat_atomic_store_int32(&last_read_status, status);
    compat_atomic_add_int32(&reads_completed, 1);

    /* an aborted first read must not start the next one. */
    if(status == PLCTAG_STATUS_OK && compat_atomic_load_int32(&restart_armed)) {
        compat_atomic_store_int32(&restart_armed, 0);
        compat_atomic_store_int32(&restart_rc, plc_tag_read(tag_id, 0));
    }
}
