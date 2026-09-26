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
 * Verify auto_sync_write_ms takes effect when set at runtime, not just in the tag string.
 *
 * A callback timestamps the write completion, so the delay between dirtying the data and
 * the write landing is the measurement.
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 6, 0

#define DATA_TIMEOUT (5000)
#define DEFAULT_TAG "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]"

#define DELAY_MS (800)

/* The write must not go out immediately, nor later than the delay plus the tickler scan
 * interval and one network round trip. */
#define MIN_DELAY_MS (DELAY_MS / 2)
#define MAX_DELAY_MS (DELAY_MS * 3)

#define WAIT_TIMEOUT_MS (DELAY_MS * 5)
#define QUIET_MS (2000)

static compat_atomic_int64_t write_completed_at;
static compat_atomic_int32_t write_completions;
static int32_t failures = 0;


static void tag_callback(int32_t tag_id, int event, int status, void *userdata) {
    (void)tag_id;
    (void)status;
    (void)userdata;

    if(event == PLCTAG_EVENT_WRITE_COMPLETED) {
        compat_atomic_store_int64(&write_completed_at, compat_time_ms());
        compat_atomic_inc_int32(&write_completions);
    }
}


/* Returns the milliseconds from dirtying the tag to the write completing, or -1 on timeout. */
static int64_t time_one_auto_write(int32_t tag, int32_t value) {
    int64_t started_at = 0;
    int64_t deadline = 0;

    compat_atomic_store_int32(&write_completions, 0);
    compat_atomic_store_int64(&write_completed_at, 0);

    started_at = compat_time_ms();
    plc_tag_set_int32(tag, 0, value);

    deadline = started_at + WAIT_TIMEOUT_MS;

    while(compat_time_ms() < deadline) {
        if(compat_atomic_load_int32(&write_completions) > 0) {
            return compat_atomic_load_int64(&write_completed_at) - started_at;
        }

        compat_sleep_ms(10, NULL);
    }

    return -1;
}


int main(void) {
    int32_t tag = 0;
    int32_t rc = 0;
    int64_t elapsed = 0;

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    compat_atomic_store_int32(&write_completions, 0);
    compat_atomic_store_int64(&write_completed_at, 0);

    tag = plc_tag_create_ex(DEFAULT_TAG, tag_callback, NULL, DATA_TIMEOUT);
    if(tag < 0) {
        printf("Unable to create tag: %s!\n", plc_tag_decode_error(tag));
        return 1;
    }

    /* the tag string did not ask for automatic writes. */
    rc = plc_tag_get_int_attribute(tag, "auto_sync_write_ms", INT_MIN);
    if(rc != 0) {
        printf("FAIL: auto_sync_write_ms is %d before it is set, expected 0.\n", (int)rc);
        failures++;
    }

    printf("With automatic writes off, changing the data writes nothing.\n");

    plc_tag_set_int32(tag, 0, 1);
    compat_sleep_ms(QUIET_MS, NULL);

    if(compat_atomic_load_int32(&write_completions) != 0) {
        printf("FAIL: a write went out with auto_sync_write_ms at zero.\n");
        failures++;
    }

    printf("Rejecting a negative delay.\n");

    rc = plc_tag_set_int_attribute(tag, "auto_sync_write_ms", -1);
    if(rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
        printf("FAIL: setting auto_sync_write_ms to -1 returned %s, expected %s.\n", plc_tag_decode_error(rc),
               plc_tag_decode_error(PLCTAG_ERR_OUT_OF_BOUNDS));
        failures++;
    }

    rc = plc_tag_get_int_attribute(tag, "auto_sync_write_ms", INT_MIN);
    if(rc != 0) {
        printf("FAIL: a rejected write changed auto_sync_write_ms to %d.\n", (int)rc);
        failures++;
    }

    printf("Setting auto_sync_write_ms to %d at runtime.\n", DELAY_MS);

    rc = plc_tag_set_int_attribute(tag, "auto_sync_write_ms", DELAY_MS);
    if(rc != PLCTAG_STATUS_OK) {
        printf("FAIL: setting auto_sync_write_ms returned %s.\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    rc = plc_tag_get_int_attribute(tag, "auto_sync_write_ms", INT_MIN);
    if(rc != DELAY_MS) {
        printf("FAIL: auto_sync_write_ms reads back as %d, expected %d.\n", (int)rc, DELAY_MS);
        failures++;
    }

    elapsed = time_one_auto_write(tag, 42);

    if(elapsed < 0) {
        printf("FAIL: no automatic write completed within %d ms.\n", WAIT_TIMEOUT_MS);
        failures++;
    } else {
        printf("Automatic write completed %" PRId64 " ms after the data changed, expected about %d ms.\n", elapsed, DELAY_MS);

        if(elapsed < MIN_DELAY_MS || elapsed > MAX_DELAY_MS) {
            printf("FAIL: write delay %" PRId64 " ms is outside the range %d to %d.\n", elapsed, MIN_DELAY_MS, MAX_DELAY_MS);
            failures++;
        }
    }

    printf("Setting auto_sync_write_ms back to zero stops automatic writes.\n");

    rc = plc_tag_set_int_attribute(tag, "auto_sync_write_ms", 0);
    if(rc != PLCTAG_STATUS_OK) {
        printf("FAIL: clearing auto_sync_write_ms returned %s.\n", plc_tag_decode_error(rc));
        failures++;
    }

    compat_atomic_store_int32(&write_completions, 0);
    plc_tag_set_int32(tag, 0, 99);
    compat_sleep_ms(QUIET_MS, NULL);

    if(compat_atomic_load_int32(&write_completions) != 0) {
        printf("FAIL: automatic writes continued after auto_sync_write_ms was cleared.\n");
        failures++;
    }

    plc_tag_destroy(tag);

    if(failures) {
        printf("FAILED with %d failing checks.\n", (int)failures);
        return 1;
    }

    printf("PASSED\n");

    return 0;
}
