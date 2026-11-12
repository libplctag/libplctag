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
 * test_callback_destroy
 *
 * An application is allowed to call plc_tag_destroy() on a tag from inside that tag's own
 * event callback.  The callback runs on a library thread while the library still holds a
 * reference to the tag, so the object cannot actually be freed underneath the caller:
 * plc_tag_destroy() removes it from the ID lookup table and the last reference release does
 * the freeing once the callback has returned.
 *
 * This test pins that behaviour down.  Every API call made with the tag ID after the
 * in-callback destroy must report PLCTAG_ERR_NOT_FOUND rather than touching freed memory,
 * and the process must survive a subsequent plc_tag_shutdown().  Run it under ASan to get
 * any use-after-free.
 *
 * The userdata follows the documented pattern: freed in the DESTROYED callback.  The
 * READ_COMPLETED callback writes to it after its plc_tag_destroy() returns, as any callback
 * still running may.  If DESTROYED is delivered nested inside that callback, the write lands
 * on freed memory and ASan reports a heap-use-after-free.
 *
 * Destroying the tag in READ_STARTED or WRITE_STARTED must stop that operation.  Those cases
 * call with timeout 0, so a waiter woken by the abort cannot hide a request still queued: the
 * call must return an error, not PENDING or OK.
 *
 * Usage: test_callback_destroy "<tag string>"
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <stdio.h>
#include <stdlib.h>

#define REQUIRED_VERSION 2, 6, 0
#define DATA_TIMEOUT (5000)


static int32_t tag_id = 0;
static int destroy_on = PLCTAG_EVENT_READ_COMPLETED;
static int destroy_called = 0;
static int destroy_rc = PLCTAG_ERR_NO_DATA;


static void tag_callback(int32_t id, int event, int status, void *userdata) {
    int32_t *events_seen = (int32_t *)userdata;

    (void)status;

    if(event == PLCTAG_EVENT_DESTROYED) {
        printf("  callback: DESTROYED for tag %" PRId32 ", freeing the userdata.\n", id);
        free(events_seen);
        return;
    }

    /* only fire once, and only from the chosen event. */
    if(event == destroy_on && !destroy_called) {
        destroy_called = 1;

        printf("  callback: event %d for tag %" PRId32 ", destroying it from inside the callback.\n", event, id);

        destroy_rc = plc_tag_destroy(tag_id);

        printf("  callback: plc_tag_destroy() returned %s.\n", plc_tag_decode_error(destroy_rc));

        /* this callback has not returned, so its userdata must still be live. */
        (*events_seen)++;
    }
}


/* every one of these must report the tag as gone rather than dereference it. */
static int check_gone(const char *what, int rc) {
    if(rc != PLCTAG_ERR_NOT_FOUND) {
        printf("  FAIL: %s returned %s, expected PLCTAG_ERR_NOT_FOUND.\n", what, plc_tag_decode_error(rc));
        return 0;
    }

    printf("  PASS: %s returned PLCTAG_ERR_NOT_FOUND.\n", what);

    return 1;
}


/* destroy the tag from the callback for event, during a read or a write. */
static int run_case(const char *tag_str, int event, int is_write) {
    int rc = PLCTAG_STATUS_OK;
    int failures = 0;
    int32_t *events_seen = NULL;
    int timeout = (event == PLCTAG_EVENT_READ_COMPLETED) ? DATA_TIMEOUT : 0;

    destroy_on = event;
    destroy_called = 0;
    destroy_rc = PLCTAG_ERR_NO_DATA;

    printf("Case: destroy on event %d during a %s.\n", event, is_write ? "write" : "read");

    events_seen = calloc(1, sizeof(*events_seen));
    if(!events_seen) {
        printf("Unable to allocate the userdata!\n");
        return 1;
    }

    /* the DESTROYED callback owns the userdata from here on. */
    tag_id = plc_tag_create_ex(tag_str, tag_callback, events_seen, DATA_TIMEOUT);
    if(tag_id < 0) {
        printf("Unable to create tag, error %s!\n", plc_tag_decode_error(tag_id));
        return 1;
    }

    if((rc = plc_tag_status(tag_id)) != PLCTAG_STATUS_OK) {
        printf("Tag creation failed, error %s!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag_id);
        return 1;
    }

    printf("Created tag %" PRId32 ".\n", tag_id);

    /* the operation fires the callback, and the callback destroys the tag. */
    rc = is_write ? plc_tag_write(tag_id, timeout) : plc_tag_read(tag_id, timeout);
    printf("operation returned %s.\n", plc_tag_decode_error(rc));

    if(!destroy_called) {
        printf("FAIL: the callback never fired, so nothing was tested.\n");
        plc_tag_destroy(tag_id);
        return 1;
    }

    if(destroy_rc != PLCTAG_STATUS_OK) {
        printf("  FAIL: in-callback plc_tag_destroy() returned %s, expected PLCTAG_STATUS_OK.\n",
               plc_tag_decode_error(destroy_rc));
        failures++;
    } else {
        printf("  PASS: in-callback plc_tag_destroy() returned PLCTAG_STATUS_OK.\n");
    }

    /* a STARTED callback destroyed the tag, so the operation must not have gone ahead. */
    if(event != PLCTAG_EVENT_READ_COMPLETED) {
        if(rc == PLCTAG_STATUS_OK || rc == PLCTAG_STATUS_PENDING) {
            printf("  FAIL: the operation on the destroyed tag returned %s.\n", plc_tag_decode_error(rc));
            failures++;
        } else {
            printf("  PASS: the operation on the destroyed tag returned %s.\n", plc_tag_decode_error(rc));
        }
    }

    /* the ID must be dead for every entry point that takes one. */
    if(!check_gone("plc_tag_status()", plc_tag_status(tag_id))) { failures++; }
    if(!check_gone("plc_tag_read()", plc_tag_read(tag_id, 1000))) { failures++; }
    if(!check_gone("plc_tag_write()", plc_tag_write(tag_id, 1000))) { failures++; }
    if(!check_gone("plc_tag_get_size()", plc_tag_get_size(tag_id))) { failures++; }
    if(!check_gone("plc_tag_abort()", plc_tag_abort(tag_id))) { failures++; }
    if(!check_gone("second plc_tag_destroy()", plc_tag_destroy(tag_id))) { failures++; }

    return failures;
}


int main(int argc, char **argv) {
    int failures = 0;

    if(argc != 2) {
        printf("Usage: test_callback_destroy \"<tag string>\"\n");
        return 1;
    }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    failures += run_case(argv[1], PLCTAG_EVENT_READ_COMPLETED, 0);
    failures += run_case(argv[1], PLCTAG_EVENT_READ_STARTED, 0);
    failures += run_case(argv[1], PLCTAG_EVENT_WRITE_STARTED, 1);

    /* the freeing itself happens after the callback returns, so make it happen here. */
    plc_tag_shutdown();

    if(failures) {
        printf("RESULT: FAIL, %d checks failed.\n", failures);
        return 1;
    }

    printf("RESULT: PASS\n");

    return 0;
}
