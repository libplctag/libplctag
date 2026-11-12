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
 * test_create_fail_callback
 *
 * After plc_tag_create_ex() returns an error, its callback must never run again, so the app
 * can free userdata.  The failed tag's events can still be queued on the connection's I/O
 * thread when create returns, behind another tag's callback.
 *
 * The test makes that ordering happen with an auto-sync tag X, whose callbacks run on the
 * I/O thread:
 *
 * 1. X's READ_COMPLETED holds the I/O thread for HOLD_MS.  Meanwhile main creates the missing
 *    tag B synchronously, so B's create read is queued, and X's next auto-sync read comes due.
 * 2. The next pass tickles X, which queues X's READ_STARTED for dispatch, then sends B's create
 *    read on its own (create reads never pack) and decodes the error.  B's events are queued
 *    behind X's.
 * 3. X's READ_STARTED callback holds the I/O thread until B's create has returned.
 * 4. Main stamps B's userdata as freed.  When X's callback returns, B's events reach dispatch,
 *    and any call to B's callback is the bug.
 *
 * If X's callback never sees B's create return, the ordering did not happen and the run
 * proves nothing, so it fails as inconclusive.
 *
 * Usage: test_create_fail_callback --tag=<existing tag with auto_sync_read_ms> --missing-tag=<missing tag>
 *        Both tags must use the same connection.  X's auto-sync period must be well under HOLD_MS.
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 6, 0
#define DATA_TIMEOUT (5000)
#define HOLD_MS (300)
#define WAIT_CAP_MS (3000)
#define USERDATA_LIVE (0x1234)
#define USERDATA_FREED (0xDEAD)

enum { STAGE_IDLE = 0, STAGE_HOLD_FIRST, STAGE_QUEUE_B, STAGE_HOLD_SECOND, STAGE_DONE };

typedef struct {
    int32_t magic;
} user_data_t;

static compat_atomic_int32_t stage = {0};
static compat_atomic_int32_t create_returned = {0};
static compat_atomic_int32_t x_saw_return = {0};
static compat_atomic_int32_t late_callbacks = {0};

static void tag_x_callback(int32_t id, int event, int status, void *userdata);
static void tag_b_callback(int32_t id, int event, int status, void *userdata);
static const char *arg_value(int argc, char **argv, const char *prefix);


static void tag_x_callback(int32_t id, int event, int status, void *userdata) {
    int64_t deadline = 0;

    (void)id;
    (void)status;
    (void)userdata;

    /* 1: let main queue B behind the held thread. */
    if(event == PLCTAG_EVENT_READ_COMPLETED && compat_atomic_load_int32(&stage) == STAGE_HOLD_FIRST) {
        compat_atomic_store_int32(&stage, STAGE_QUEUE_B);
        compat_sleep_ms(HOLD_MS, NULL);
        compat_atomic_store_int32(&stage, STAGE_HOLD_SECOND);
        return;
    }

    /* 3: B's events are queued behind this one. */
    if(event == PLCTAG_EVENT_READ_STARTED && compat_atomic_load_int32(&stage) == STAGE_HOLD_SECOND) {
        compat_atomic_store_int32(&stage, STAGE_DONE);

        deadline = compat_time_ms() + WAIT_CAP_MS;
        while(!compat_atomic_load_int32(&create_returned) && compat_time_ms() < deadline) { compat_sleep_ms(1, NULL); }

        compat_atomic_store_int32(&x_saw_return, compat_atomic_load_int32(&create_returned));
    }
}


static void tag_b_callback(int32_t id, int event, int status, void *userdata) {
    user_data_t *ud = (user_data_t *)userdata;

    printf("  B callback: tag %" PRId32 ", event %d, status %s, create returned %d, magic 0x%x.\n", id, event,
           plc_tag_decode_error(status), (int)compat_atomic_load_int32(&create_returned), (unsigned)ud->magic);

    if(compat_atomic_load_int32(&create_returned) || ud->magic != USERDATA_LIVE) {
        compat_atomic_inc_int32(&late_callbacks);
    }
}


static const char *arg_value(int argc, char **argv, const char *prefix) {
    size_t len = strlen(prefix);

    for(int32_t i = 1; i < argc; i++) {
        if(strncmp(argv[i], prefix, len) == 0) { return argv[i] + len; }
    }

    return NULL;
}


int main(int argc, char **argv) {
    const char *tag_str = arg_value(argc, argv, "--tag=");
    const char *missing_str = arg_value(argc, argv, "--missing-tag=");
    user_data_t *ud = NULL;
    int32_t tag_x = 0;
    int32_t tag_b = 0;
    int64_t deadline = 0;
    int32_t result = 1;

    if(!tag_str || !missing_str) {
        printf("Usage: test_create_fail_callback --tag=<existing tag with auto_sync_read_ms> --missing-tag=<missing tag>\n");
        return 1;
    }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    tag_x = plc_tag_create_ex(tag_str, tag_x_callback, NULL, DATA_TIMEOUT);
    if(tag_x < 0) {
        printf("Unable to create tag X: %s!\n", plc_tag_decode_error(tag_x));
        goto done;
    }

    compat_atomic_store_int32(&stage, STAGE_HOLD_FIRST);

    deadline = compat_time_ms() + DATA_TIMEOUT;
    while(compat_atomic_load_int32(&stage) != STAGE_QUEUE_B && compat_time_ms() < deadline) { compat_sleep_ms(1, NULL); }
    if(compat_atomic_load_int32(&stage) != STAGE_QUEUE_B) {
        printf("Tag X never completed an auto-sync read!\n");
        goto done;
    }

    ud = malloc(sizeof(*ud));
    if(!ud) { goto done; }
    ud->magic = USERDATA_LIVE;

    tag_b = plc_tag_create_ex(missing_str, tag_b_callback, ud, DATA_TIMEOUT);
    printf("plc_tag_create_ex() for the missing tag returned %s.\n", plc_tag_decode_error(tag_b));

    if(tag_b >= 0) {
        printf("FAIL: the missing tag was created.\n");
        plc_tag_destroy(tag_b);
        goto done;
    }

    /* 4: what the app would do: free userdata.  The stamp stands in for free() and stays defined. */
    ud->magic = USERDATA_FREED;
    compat_atomic_store_int32(&create_returned, 1);

    /* let X's callback return and anything queued behind it dispatch. */
    compat_sleep_ms(HOLD_MS + 500, NULL);

    if(!compat_atomic_load_int32(&x_saw_return)) {
        printf("INCONCLUSIVE: B's create did not return while X's callback held the I/O thread.\n");
    } else if(compat_atomic_load_int32(&late_callbacks) > 0) {
        printf("FAIL: B's callback ran %d time(s) after plc_tag_create_ex() returned an error.\n",
               (int)compat_atomic_load_int32(&late_callbacks));
    } else {
        printf("PASS: no callback after the failed create returned.\n");
        result = 0;
    }

done:
    if(tag_x > 0) { plc_tag_destroy(tag_x); }
    plc_tag_shutdown();
    free(ud);

    return (int)result;
}
