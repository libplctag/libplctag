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
 * Verify auto_sync_read_ms takes effect when set at runtime, not just in the tag string.
 *
 * Automatic reads start off; a callback counts completions over WINDOW_MS.  The bounds are
 * wide: the window includes network round trips and the tickler's scan interval.
 */

#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 6, 0

#define DATA_TIMEOUT (5000)
#define DEFAULT_TAG "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]"

#define PERIOD_MS (200)
#define WINDOW_MS (4000)
#define EXPECTED_READS (WINDOW_MS / PERIOD_MS)

/* Allow half to double the ideal count.  Anything outside that is a real failure, not jitter. */
#define MIN_READS (EXPECTED_READS / 2)
#define MAX_READS (EXPECTED_READS * 2)

/* After automatic reads are turned off, this many stragglers may still be in flight. */
#define MAX_STRAGGLERS (2)
#define QUIET_MS (2000)

static compat_atomic_int32_t read_completions;
static int32_t failures = 0;


static void tag_callback(int32_t tag_id, int event, int status, void *userdata) {
    (void)tag_id;
    (void)status;
    (void)userdata;

    if(event == PLCTAG_EVENT_READ_COMPLETED) { compat_atomic_inc_int32(&read_completions); }
}


int main(void) {
    int32_t tag = 0;
    int32_t rc = 0;
    int32_t during_window = 0;
    int32_t after_stop = 0;

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    compat_atomic_store_int32(&read_completions, 0);

    tag = plc_tag_create_ex(DEFAULT_TAG, tag_callback, NULL, DATA_TIMEOUT);
    if(tag < 0) {
        printf("Unable to create tag: %s!\n", plc_tag_decode_error(tag));
        return 1;
    }

    /* the tag string did not ask for automatic reads. */
    rc = plc_tag_get_int_attribute(tag, "auto_sync_read_ms", INT_MIN);
    if(rc != 0) {
        printf("FAIL: auto_sync_read_ms is %d before it is set, expected 0.\n", (int)rc);
        failures++;
    }

    printf("Rejecting a negative period.\n");

    rc = plc_tag_set_int_attribute(tag, "auto_sync_read_ms", -1);
    if(rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
        printf("FAIL: setting auto_sync_read_ms to -1 returned %s, expected %s.\n", plc_tag_decode_error(rc),
               plc_tag_decode_error(PLCTAG_ERR_OUT_OF_BOUNDS));
        failures++;
    }

    rc = plc_tag_get_int_attribute(tag, "auto_sync_read_ms", INT_MIN);
    if(rc != 0) {
        printf("FAIL: a rejected write changed auto_sync_read_ms to %d.\n", (int)rc);
        failures++;
    }

    printf("Setting auto_sync_read_ms to %d at runtime.\n", PERIOD_MS);

    rc = plc_tag_set_int_attribute(tag, "auto_sync_read_ms", PERIOD_MS);
    if(rc != PLCTAG_STATUS_OK) {
        printf("FAIL: setting auto_sync_read_ms returned %s.\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    rc = plc_tag_get_int_attribute(tag, "auto_sync_read_ms", INT_MIN);
    if(rc != PERIOD_MS) {
        printf("FAIL: auto_sync_read_ms reads back as %d, expected %d.\n", (int)rc, PERIOD_MS);
        failures++;
    }

    compat_atomic_store_int32(&read_completions, 0);
    compat_sleep_ms(WINDOW_MS, NULL);
    during_window = compat_atomic_load_int32(&read_completions);

    printf("Saw %d read completions in %d ms, expected about %d.\n", (int)during_window, WINDOW_MS, EXPECTED_READS);

    if(during_window < MIN_READS || during_window > MAX_READS) {
        printf("FAIL: read completion count %d is outside the range %d to %d.\n", (int)during_window, MIN_READS, MAX_READS);
        failures++;
    }

    printf("Setting auto_sync_read_ms back to zero stops automatic reads.\n");

    rc = plc_tag_set_int_attribute(tag, "auto_sync_read_ms", 0);
    if(rc != PLCTAG_STATUS_OK) {
        printf("FAIL: clearing auto_sync_read_ms returned %s.\n", plc_tag_decode_error(rc));
        failures++;
    }

    /* let anything already in flight land before the quiet window starts. */
    compat_sleep_ms(500, NULL);
    compat_atomic_store_int32(&read_completions, 0);
    compat_sleep_ms(QUIET_MS, NULL);
    after_stop = compat_atomic_load_int32(&read_completions);

    printf("Saw %d read completions in %d ms after stopping.\n", (int)after_stop, QUIET_MS);

    if(after_stop > MAX_STRAGGLERS) {
        printf("FAIL: automatic reads continued after auto_sync_read_ms was cleared.\n");
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
