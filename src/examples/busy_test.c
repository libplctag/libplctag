/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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
 * This example reads from a large DINT array.  It creates many tags that each read from one element of the
 * array. It fires off all the tags at once and waits for them to complete the reads. In this case, it waits
 * a fixed amount of time and then tries to read the tags.
 */


#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 1, 0

#define TAG_ATTRIBS "protocol=ab_eip&gateway=10.206.1.40&path=1,4&cpu=LGX&elem_type=DINT&elem_count=%d&name=TestBigArray[%d]"
#define NUM_TAGS (5)
#define NUM_ELEMS (10)
#define DATA_TIMEOUT (1000)

int main(void) {
    int32_t tag[NUM_TAGS];
    int rc;
    int i;
    int64_t timeout = DATA_TIMEOUT + compat_time_ms();
    int done = 0;
    int64_t start = 0;
    int64_t end = 0;
    int num_elems_per_tag = NUM_ELEMS / NUM_TAGS;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Using library version %d.%d.%d.\n", plc_tag_get_int_attribute(0, "version_major", -1),
            plc_tag_get_int_attribute(0, "version_minor", -1), plc_tag_get_int_attribute(0, "version_patch", -1));

    /* turn off debugging output. */
    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /* create the tags */
    for(i = 0; i < NUM_TAGS; i++) {
        char tmp_tag_path[256] = {
            0,
        };

        // NOLINTNEXTLINE
        compat_snprintf(tmp_tag_path, sizeof tmp_tag_path, TAG_ATTRIBS, num_elems_per_tag, i);

        // NOLINTNEXTLINE
        fprintf(stderr, "Attempting to create tag with attribute string '%s'\n", tmp_tag_path);

        tag[i] = plc_tag_create(tmp_tag_path, 0);

        if(tag[i] < 0) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Error %s: could not create tag %d\n", plc_tag_decode_error(tag[i]), i);
        }
    }

    /* wait for all the tags to complete creation. */
    do {
        done = 1;

        for(i = 0; i < NUM_TAGS; i++) {
            rc = plc_tag_status(tag[i]);
            if(rc != PLCTAG_STATUS_OK) { done = 0; }
        }

        if(!done) { compat_sleep_ms(10, NULL); }
    } while(timeout > compat_time_ms() && !done);

    if(!done) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Timeout waiting for tags to be ready!\n");

        for(i = 0; i < NUM_TAGS; i++) { plc_tag_destroy(tag[i]); }

        return 1;
    }

    start = compat_time_ms();

    /* get the data */
    for(i = 0; i < NUM_TAGS; i++) {
        rc = plc_tag_read(tag[i], 0);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
            // NOLINTNEXTLINE
            fprintf(stderr, "ERROR: Unable to read the data! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));

            return 0;
        }

        /* try to read again without aborting.  Should get a PLCTAG_ERR_BUSY error. */
        rc = plc_tag_read(tag[i], 0);
        if(rc != PLCTAG_ERR_BUSY) {
            // NOLINTNEXTLINE
            fprintf(stderr, "ERROR: Expected PLCTAG_ERR_BUSY, got error code %d: %s\n", rc, plc_tag_decode_error(rc));

            return 0;
        }
    }

    /* wait for all to finish */
    do {
        done = 1;

        for(i = 0; i < NUM_TAGS; i++) {
            rc = plc_tag_status(tag[i]);
            if(rc != PLCTAG_STATUS_OK) { done = 0; }
        }

        if(!done) { compat_sleep_ms(10, NULL); }
    } while(timeout > compat_time_ms() && !done);

    if(!done) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Timeout waiting for tags to finish reading!\n");

        for(i = 0; i < NUM_TAGS; i++) { plc_tag_destroy(tag[i]); }

        return 1;
    }

    end = compat_time_ms();

    /* get any data we can */
    for(i = 0; i < NUM_TAGS; i++) {
        /* read complete! */
        // NOLINTNEXTLINE
        fprintf(stderr, "Tag %d data[0]=%d\n", i, plc_tag_get_int32(tag[i], 0));
    }


    /* we are done */
    for(i = 0; i < NUM_TAGS; i++) { plc_tag_destroy(tag[i]); }

    // NOLINTNEXTLINE
    fprintf(stderr, "Read %d tags in %dms\n", NUM_TAGS, (int)(end - start));

    return 0;
}
