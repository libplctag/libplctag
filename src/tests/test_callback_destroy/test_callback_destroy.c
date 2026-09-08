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
 * event callback.  The callback runs on the tickler thread while the library still holds a
 * reference to the tag, so the object cannot actually be freed underneath the caller:
 * plc_tag_destroy() removes it from the ID lookup table and the last reference release does
 * the freeing once the callback has returned.
 *
 * This test pins that behaviour down.  Every API call made with the tag ID after the
 * in-callback destroy must report PLCTAG_ERR_NOT_FOUND rather than touching freed memory,
 * and the process must survive a subsequent plc_tag_shutdown().  Run it under ASan to get
 * any use-after-free.
 *
 * Usage: test_callback_destroy "<tag string>"
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>

#define REQUIRED_VERSION 2, 6, 0
#define DATA_TIMEOUT (5000)


static int32_t tag_id = 0;
static int destroy_called = 0;
static int destroy_rc = PLCTAG_ERR_NO_DATA;


static void tag_callback(int32_t id, int event, int status, void *userdata) {
    (void)status;
    (void)userdata;

    /* only fire once, and only from the tag's own completion event. */
    if(event == PLCTAG_EVENT_READ_COMPLETED && !destroy_called) {
        destroy_called = 1;

        printf("  callback: READ_COMPLETED for tag %" PRId32 ", destroying it from inside the callback.\n", id);

        destroy_rc = plc_tag_destroy(tag_id);

        printf("  callback: plc_tag_destroy() returned %s.\n", plc_tag_decode_error(destroy_rc));
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


int main(int argc, char **argv) {
    int rc = PLCTAG_STATUS_OK;
    int failures = 0;

    if(argc != 2) {
        printf("Usage: test_callback_destroy \"<tag string>\"\n");
        return 1;
    }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    tag_id = plc_tag_create_ex(argv[1], tag_callback, NULL, DATA_TIMEOUT);
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

    /* this read completes, fires the callback, and the callback destroys the tag. */
    rc = plc_tag_read(tag_id, DATA_TIMEOUT);
    printf("plc_tag_read() returned %s.\n", plc_tag_decode_error(rc));

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

    /* the ID must be dead for every entry point that takes one. */
    if(!check_gone("plc_tag_status()", plc_tag_status(tag_id))) { failures++; }
    if(!check_gone("plc_tag_read()", plc_tag_read(tag_id, 1000))) { failures++; }
    if(!check_gone("plc_tag_write()", plc_tag_write(tag_id, 1000))) { failures++; }
    if(!check_gone("plc_tag_get_size()", plc_tag_get_size(tag_id))) { failures++; }
    if(!check_gone("plc_tag_abort()", plc_tag_abort(tag_id))) { failures++; }
    if(!check_gone("second plc_tag_destroy()", plc_tag_destroy(tag_id))) { failures++; }

    /* the freeing itself happens after the callback returns, so make it happen here. */
    plc_tag_shutdown();

    if(failures) {
        printf("RESULT: FAIL, %d checks failed.\n", failures);
        return 1;
    }

    printf("RESULT: PASS\n");

    return 0;
}
