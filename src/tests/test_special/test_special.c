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


#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>

#define REQUIRED_VERSION 2, 1, 0

#define TAG_CREATE_TIMEOUT (100)

int test_version(void) {
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;
    char ver[16] = {
        0,
    };

    // NOLINTNEXTLINE
    fprintf(stderr, "Testing version tag.\n");

    tag = plc_tag_create("protocol=system&name=version&debug=4", TAG_CREATE_TIMEOUT);
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return tag;
    }

    plc_tag_read(tag, 0);

    rc = plc_tag_get_string(tag, 0, ver, (int)(unsigned int)sizeof(ver));
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: %s: Could not get version string!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Library version %s\n", ver);

    plc_tag_destroy(tag);

    return PLCTAG_STATUS_OK;
}


int test_debug(void) {
    int rc = PLCTAG_STATUS_OK;
    int32_t tag = 0;
    uint32_t old_debug, new_debug;

    // NOLINTNEXTLINE
    fprintf(stderr, "Testing debug tag.\n");

    tag = plc_tag_create("protocol=system&name=debug&debug=4", TAG_CREATE_TIMEOUT);
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return tag;
    }

    rc = plc_tag_read(tag, 0);
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error %s trying to read debug level!\n", plc_tag_decode_error(rc));
        return rc;
    }

    old_debug = plc_tag_get_uint32(tag, 0);

    // NOLINTNEXTLINE
    fprintf(stderr, "Current debug level is %d\n", old_debug);

    new_debug = (old_debug == 3 ? 4 : 3);

    // NOLINTNEXTLINE
    fprintf(stderr, "Changing debug level to %d\n", new_debug);

    plc_tag_set_uint32(tag, 0, new_debug);

    rc = plc_tag_write(tag, 0);
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error %s trying to write debug level!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    rc = plc_tag_read(tag, 0);
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error %s trying to read debug level!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    new_debug = plc_tag_get_uint32(tag, 0);

    if(old_debug == 3) {
        if(new_debug == 4) {
            // NOLINTNEXTLINE
            fprintf(stderr, "New debug level is correctly 4.\n.");
        } else {
            // NOLINTNEXTLINE
            fprintf(stderr, "New debug level is %d but should be 4.\n", new_debug);
            plc_tag_destroy(tag);
            return PLCTAG_ERR_BAD_REPLY;
        }
    } else {
        if(new_debug == 3) {
            // NOLINTNEXTLINE
            fprintf(stderr, "New debug level is correctly 3.\n.");
        } else {
            // NOLINTNEXTLINE
            fprintf(stderr, "New debug level is %d but should be 3.\n", new_debug);
            plc_tag_destroy(tag);
            return PLCTAG_ERR_BAD_DATA;
        }
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Changing debug level back to %d\n", old_debug);

    plc_tag_set_uint32(tag, 0, old_debug);

    rc = plc_tag_write(tag, 0);
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error %s trying to write debug level!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    rc = plc_tag_read(tag, 0);
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error %s trying to read debug level!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    new_debug = plc_tag_get_uint32(tag, 0);

    if(old_debug == new_debug) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Correctly set debug level back to old value %d.\n", old_debug);
    } else {
        // NOLINTNEXTLINE
        fprintf(stderr, "Unable to set debug level back to old value %d, was %d!\n", old_debug, new_debug);
        plc_tag_destroy(tag);
        return PLCTAG_ERR_BAD_DATA;
    }

    plc_tag_destroy(tag);

    return rc;
}


/*
 * A system tag has no handler, so it cannot auto-destroy, and only "version" and "debug"
 * exist: the special names of the PLC protocols are refused.
 */
static int test_refusals(void) {
    static const char *names[] = {"@connection", "@raw", "@tags", "@udt/1", "no_such_tag"};
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;
    int failed = 0;

    tag = plc_tag_create("protocol=system&name=version&destroy_tag_after_idle_sec=10", TAG_CREATE_TIMEOUT);
    if(tag != PLCTAG_ERR_BAD_PARAM) {
        fprintf(stderr, "Create with destroy_tag_after_idle_sec gave %d (%s), expected PLCTAG_ERR_BAD_PARAM!\n", (int)tag, plc_tag_decode_error(tag));
        failed++;
    }
    if(tag >= 0) { plc_tag_destroy(tag); }

    tag = plc_tag_create("protocol=system&name=version", TAG_CREATE_TIMEOUT);
    if(tag < 0) {
        fprintf(stderr, "Unable to create the version tag: %s!\n", plc_tag_decode_error(tag));
        return tag;
    }

    rc = plc_tag_set_int_attribute(tag, "destroy_tag_after_idle_sec", 10);
    if(rc != PLCTAG_ERR_NOT_ALLOWED) {
        fprintf(stderr, "Setting destroy_tag_after_idle_sec gave %s, expected PLCTAG_ERR_NOT_ALLOWED!\n", plc_tag_decode_error(rc));
        failed++;
    }
    plc_tag_destroy(tag);

    for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char attribs[128];

        snprintf(attribs, sizeof(attribs), "protocol=system&name=%s", names[i]);
        tag = plc_tag_create(attribs, TAG_CREATE_TIMEOUT);
        if(tag != PLCTAG_ERR_BAD_PARAM) {
            fprintf(stderr, "Create of system tag %s gave %d (%s), expected PLCTAG_ERR_BAD_PARAM!\n", names[i], (int)tag, plc_tag_decode_error(tag));
            failed++;
        }
        if(tag >= 0) { plc_tag_destroy(tag); }
    }

    return failed ? PLCTAG_ERR_BAD_STATUS : PLCTAG_STATUS_OK;
}


int main(void) {
    int rc = PLCTAG_STATUS_OK;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    rc = test_version();
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error %s trying to test \"version\" special tag!\n", plc_tag_decode_error(rc));
        return rc;
    }

    rc = test_debug();
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error %s trying to test \"debug\" special tag!\n", plc_tag_decode_error(rc));
        return rc;
    }

    rc = test_refusals();
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error %s: system tags accepted what they must refuse!\n", plc_tag_decode_error(rc));
        return rc;
    }

    fprintf(stderr, "System tag refusals ok.\n");

    return 0;
}
