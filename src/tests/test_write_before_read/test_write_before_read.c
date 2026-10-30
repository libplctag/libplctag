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
 * Write to a tag the caller has never read.
 *
 * A write has to carry the tag's encoded type, which only a read supplies.  Creating
 * the tag asynchronously and writing straight away is the case where the caller has
 * asked for neither, and it has to end up with a correct element size and a value on
 * the wire either way.
 *
 * The write is refused with PLCTAG_ERR_BUSY until the create's own read has finished,
 * so the caller retries past BUSY.
 *
 * --abort-create-read and --missing-tag check the other side: a create read that ends in
 * an abort or a PLC error fails the create for good.  Status, read and write must all
 * report that error from then on.
 */

#include "compat_utils.h"
#include <libplctag/api/libplctag.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 4, 0

#define DATA_TIMEOUT (5000)


static int write_before_read(const char *tag_string, int32_t value) {
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;
    int elem_size = 0;
    int64_t deadline = 0;

    printf("--- %s\n", tag_string);

    /* create asynchronously so that nothing has been read when the write starts. */
    tag = plc_tag_create(tag_string, 0);
    if(tag < 0) {
        printf("ERROR %s: could not create tag!\n", plc_tag_decode_error(tag));
        return 1;
    }

    /* the create's read fetches the type; until it lands a write is refused with BUSY. */
    deadline = compat_time_ms() + DATA_TIMEOUT;
    do {
        rc = plc_tag_write(tag, DATA_TIMEOUT);
        if(rc == PLCTAG_ERR_BUSY) { compat_sleep_ms(10, NULL); }
    } while(rc == PLCTAG_ERR_BUSY && compat_time_ms() < deadline);

    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: write before any read failed, %s!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    /* the create read must have established a usable element size. */
    elem_size = plc_tag_get_int_attribute(tag, "elem_size", -1);
    if(elem_size <= 0) {
        printf("ERROR: element size is %d after the create read!\n", elem_size);
        plc_tag_destroy(tag);
        return 1;
    }

    printf("Write before read succeeded, element size %d bytes.\n", elem_size);

    /* now put a known value in and read it back, to prove the write actually landed. */
    rc = plc_tag_set_int32(tag, 0, value);
    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: unable to set the value, %s!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    rc = plc_tag_write(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: second write failed, %s!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    deadline = compat_time_ms() + DATA_TIMEOUT;
    do {
        rc = plc_tag_read(tag, DATA_TIMEOUT);
    } while(rc == PLCTAG_STATUS_PENDING && compat_time_ms() < deadline);

    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: read back failed, %s!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    if(plc_tag_get_int32(tag, 0) != value) {
        printf("ERROR: read back %d but wrote %d!\n", plc_tag_get_int32(tag, 0), value);
        plc_tag_destroy(tag);
        return 1;
    }

    printf("Round trip of %d verified.\n", value);

    plc_tag_destroy(tag);

    return 0;
}


/*
 * Create asynchronously and make the create read fail, by aborting it or by naming a tag
 * the PLC does not have.  The failure must be final: status, read and write all report
 * the same error, and waiting does not change it.
 */
static int create_fails_for_good(const char *tag_string, bool abort_create_read) {
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;
    int create_rc = PLCTAG_STATUS_OK;
    int64_t deadline = 0;

    printf("--- %s\n", tag_string);

    tag = plc_tag_create(tag_string, 0);

    /* a PLC that answers before create returns gives the error here, with no handle to check. */
    if(tag < 0 && !abort_create_read) {
        printf("Create failed with %s.\n", plc_tag_decode_error(tag));
        return 0;
    }

    if(tag < 0) {
        printf("ERROR %s: could not create tag!\n", plc_tag_decode_error(tag));
        return 1;
    }

    if(abort_create_read) {
        rc = plc_tag_abort(tag);
        if(rc != PLCTAG_STATUS_OK) {
            printf("ERROR %s: could not abort the create's read!\n", plc_tag_decode_error(rc));
            plc_tag_destroy(tag);
            return 1;
        }
    }

    deadline = compat_time_ms() + DATA_TIMEOUT;
    while((create_rc = plc_tag_status(tag)) == PLCTAG_STATUS_PENDING && compat_time_ms() < deadline) {
        compat_sleep_ms(10, NULL);
    }

    if(create_rc == PLCTAG_STATUS_OK || create_rc == PLCTAG_STATUS_PENDING) {
        printf("ERROR: the create ended with %s, not an error!\n", plc_tag_decode_error(create_rc));
        plc_tag_destroy(tag);
        return 1;
    }

    if(abort_create_read && create_rc != PLCTAG_ERR_ABORT) {
        printf("ERROR: an aborted create ended with %s, not ABORT!\n", plc_tag_decode_error(create_rc));
        plc_tag_destroy(tag);
        return 1;
    }

    printf("Create failed with %s.\n", plc_tag_decode_error(create_rc));

    /* give a retry or a reconnect time to undo it, if anything would. */
    compat_sleep_ms(500, NULL);

    if((rc = plc_tag_read(tag, DATA_TIMEOUT)) != create_rc) {
        printf("ERROR: read returned %s after the create failed!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    /* a read-only tag, such as @udt or @tags, has no write at all; the library says so first. */
    rc = plc_tag_write(tag, DATA_TIMEOUT);
    if(rc != create_rc && rc != PLCTAG_ERR_NOT_IMPLEMENTED) {
        printf("ERROR: write returned %s after the create failed!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    if((rc = plc_tag_status(tag)) != create_rc) {
        printf("ERROR: status changed to %s after the create failed!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    printf("The create error stuck on status, read and write.\n");

    plc_tag_destroy(tag);

    return 0;
}


static void usage(void) {
    printf("Usage: test_write_before_read [--abort-create-read | --missing-tag] --tag=<attribute string> [--tag=...]\n");
    printf("  Each tag is created asynchronously and written before it has ever been read.\n");
    printf("  --abort-create-read aborts the create's read; the create must fail with ABORT for good.\n");
    printf("  --missing-tag expects tags the PLC does not have; the create must fail with its error for good.\n");
}


int main(int argc, char **argv) {
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);
    int failures = 0;
    int tags_run = 0;
    bool abort_create_read = false;
    bool missing_tag = false;

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available, found %d.%d.%d!\n", REQUIRED_VERSION, version_major,
               version_minor, version_patch);
        return 1;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    printf("Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--abort-create-read") == 0) {
            abort_create_read = true;
        } else if(strcmp(argv[i], "--missing-tag") == 0) {
            missing_tag = true;
        } else if(strncmp(argv[i], "--tag=", 6) == 0 && (abort_create_read || missing_tag)) {
            failures += create_fails_for_good(argv[i] + 6, abort_create_read);
            tags_run++;
        } else if(strncmp(argv[i], "--tag=", 6) == 0) {
            failures += write_before_read(argv[i] + 6, 0x01020304 + i);
            tags_run++;
        } else {
            printf("ERROR: unknown argument \"%s\"!\n", argv[i]);
            usage();
            return 1;
        }
    }

    if(tags_run == 0) {
        usage();
        return 1;
    }

    if(failures) {
        printf("FAILED on %d of %d tag(s).\n", failures, tags_run);
        return 1;
    }

    printf("SUCCESS!\n");

    return 0;
}
