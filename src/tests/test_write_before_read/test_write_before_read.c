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
 * Note what this does NOT reach: tag_write_start()'s own pre-write read.  The create
 * kicks off a read of its own, a write refuses outright while that is in flight, and
 * by the time it clears the type is already known.  That branch is only reachable
 * when the create's read fails outright -- an unreachable PLC, say -- and there is no
 * deterministic way to arrange that from here.
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

    /*
     * Write as soon as the tag is not busy.  The create kicks off a read of its own to
     * fetch the type; while that is in flight a write is refused outright, and only once
     * it has finished -- unsuccessfully, or the type would be known -- does the write
     * reach the pre-write read.  Retrying past BUSY is what the caller would do anyway.
     */
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

    /* the pre-write read must have established a usable element size. */
    elem_size = plc_tag_get_int_attribute(tag, "elem_size", -1);
    if(elem_size <= 0) {
        printf("ERROR: element size is %d after the pre-write read!\n", elem_size);
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


static void usage(void) {
    printf("Usage: test_write_before_read --tag=<attribute string> [--tag=<attribute string> ...]\n");
    printf("  Each tag is created asynchronously and written before it has ever been read.\n");
    printf("  Pass an array tag as well as a scalar: only the array reaches the case where\n");
    printf("  the pre-write read used to come back with an unusable element size.\n");
}


int main(int argc, char **argv) {
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);
    int failures = 0;
    int tags_run = 0;

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available, found %d.%d.%d!\n", REQUIRED_VERSION, version_major,
               version_minor, version_patch);
        return 1;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    printf("Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    for(int i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) {
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
