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
 * connection_retry_max_delay_ms, read and written live through the public attribute API.
 *
 * The attribute is per-connection state reached through whichever tag the caller happens to
 * hold, and every protocol wires it up separately, so the same script runs against AB, Omron
 * and Modbus.  Taking a --tag= argument is what makes that possible.
 *
 * What a capped delay does to the reconnect timing is checked in the simulator suite against
 * a PLC that refuses every connection.  This checks the attribute plumbing itself: that the
 * value survives a round trip, that an out-of-range value is refused rather than stored, and
 * that the getter answers before there is a connection to answer for.
 */

#include "compat_utils.h"
#include <libplctag/api/libplctag.h>
#include <libplctag/lib/conn_timeouts.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define ATTR_NAME "connection_retry_max_delay_ms"

#define DATA_TIMEOUT_MS (5000)

/* comfortably inside the bounds and not equal to any protocol's default. */
#define TEST_DELAY_MS (4321)

#define GET_FAILED (-1)


static const char *tag_string = NULL;


static void usage(void) {
    // NOLINTNEXTLINE
    fprintf(stderr, "Usage: test_retry_attr --tag=<tag string>\n");
    exit(1);
}


static void parse_args(int argc, char **argv) {
    for(int i = 1; i < argc; i++) {
        if(!strncmp(argv[i], "--tag=", 6)) {
            tag_string = argv[i] + 6;
        } else {
            // NOLINTNEXTLINE
            fprintf(stderr, "Unknown argument \"%s\"!\n", argv[i]);
            usage();
        }
    }

    if(!tag_string) { usage(); }
}


/* every failure path has to destroy the tag, so say what went wrong and leave by one door. */
static void fail(int32_t tag, const char *what, int expected, int actual) {
    // NOLINTNEXTLINE
    fprintf(stderr, "ERROR: %s.  Expected %d, got %d.\n", what, expected, actual);

    if(tag > 0) { plc_tag_destroy(tag); }

    exit(1);
}


/*
 * Create a tag asking for delay_ms in its tag string and check the connection ended up with
 * want_ms.  group_id puts it on a connection of its own so the value is not ignored.
 */
static int check_create_time_clamp(int delay_ms, int want_ms, int group_id) {
    char tag_str[512];
    int32_t tag = 0;
    int got_ms = 0;
    int rc = PLCTAG_STATUS_OK;

    // NOLINTNEXTLINE
    snprintf(tag_str, sizeof(tag_str), "%s&connection_group_id=%d&" ATTR_NAME "=%d", tag_string, group_id, delay_ms);

    tag = plc_tag_create(tag_str, DATA_TIMEOUT_MS);
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: could not create tag asking for %d ms, %s!\n", delay_ms, plc_tag_decode_error(tag));
        return 0;
    }

    rc = plc_tag_status(tag);
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: tag asking for %d ms is not ready, %s!\n", delay_ms, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 0;
    }

    got_ms = plc_tag_get_int_attribute(tag, ATTR_NAME, GET_FAILED);

    plc_tag_destroy(tag);

    if(got_ms != want_ms) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: tag string asked for %d ms.  Expected it clamped to %d ms, got %d ms.\n", delay_ms, want_ms,
                got_ms);
        return 0;
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Tag string value %d ms clamped to %d ms.\n", delay_ms, got_ms);

    return 1;
}


int main(int argc, char **argv) {
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;
    int default_delay_ms = 0;
    int delay_ms = 0;

    parse_args(argc, argv);

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /*
     * Read the attribute before the tag has a connection.  It has to answer with the
     * default the next connection will use rather than failing: a caller checking what it
     * is about to change has no connection yet either.
     */
    tag = plc_tag_create(tag_string, 0);
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: could not create tag, %s!\n", plc_tag_decode_error(tag));
        exit(1);
    }

    default_delay_ms = plc_tag_get_int_attribute(tag, ATTR_NAME, GET_FAILED);
    if(default_delay_ms < CONN_RETRY_MAX_DELAY_MIN_MS || default_delay_ms > CONN_RETRY_MAX_DELAY_LIMIT_MS) {
        fail(tag, "Default " ATTR_NAME " is outside the attribute's own bounds", CONN_RETRY_MAX_DELAY_MIN_MS, default_delay_ms);
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Default %s before connecting: %d ms\n", ATTR_NAME, default_delay_ms);

    /* now connect, so the rest of this runs against a real connection. */
    rc = plc_tag_status(tag);
    while(rc == PLCTAG_STATUS_PENDING) {
        compat_sleep_ms(10, NULL);
        rc = plc_tag_status(tag);
    }

    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: tag creation failed, %s!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        exit(1);
    }

    if((rc = plc_tag_read(tag, DATA_TIMEOUT_MS)) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: read failed, %s!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        exit(1);
    }

    /* the connection exists now, and still reports the default nobody has changed. */
    delay_ms = plc_tag_get_int_attribute(tag, ATTR_NAME, GET_FAILED);
    if(delay_ms != default_delay_ms) {
        fail(tag, "Connected " ATTR_NAME " differs from the pre-connection default", default_delay_ms, delay_ms);
    }

    /* Test 1: a valid value survives the round trip. */
    // NOLINTNEXTLINE
    fprintf(stderr, "\n=== Test 1: set %s to %d ms ===\n", ATTR_NAME, TEST_DELAY_MS);

    rc = plc_tag_set_int_attribute(tag, ATTR_NAME, TEST_DELAY_MS);
    if(rc != PLCTAG_STATUS_OK) { fail(tag, "Setting a valid " ATTR_NAME " failed", PLCTAG_STATUS_OK, rc); }

    delay_ms = plc_tag_get_int_attribute(tag, ATTR_NAME, GET_FAILED);
    if(delay_ms != TEST_DELAY_MS) { fail(tag, ATTR_NAME " did not keep the value it was set to", TEST_DELAY_MS, delay_ms); }

    // NOLINTNEXTLINE
    fprintf(stderr, "Read back %d ms.\n", delay_ms);

    /* Test 2: below the floor is clamped and reported, not stored. */
    // NOLINTNEXTLINE
    fprintf(stderr, "\n=== Test 2: set %s below the floor ===\n", ATTR_NAME);

    rc = plc_tag_set_int_attribute(tag, ATTR_NAME, 1);
    if(rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
        fail(tag, "Setting " ATTR_NAME " below the floor was not reported", PLCTAG_ERR_OUT_OF_BOUNDS, rc);
    }

    delay_ms = plc_tag_get_int_attribute(tag, ATTR_NAME, GET_FAILED);
    if(delay_ms != CONN_RETRY_MAX_DELAY_MIN_MS) {
        fail(tag, ATTR_NAME " was not clamped up to the floor", CONN_RETRY_MAX_DELAY_MIN_MS, delay_ms);
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Clamped to %d ms and reported out of bounds.\n", delay_ms);

    /* Test 3: above the ceiling, likewise. */
    // NOLINTNEXTLINE
    fprintf(stderr, "\n=== Test 3: set %s above the ceiling ===\n", ATTR_NAME);

    rc = plc_tag_set_int_attribute(tag, ATTR_NAME, CONN_RETRY_MAX_DELAY_LIMIT_MS + 1);
    if(rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
        fail(tag, "Setting " ATTR_NAME " above the ceiling was not reported", PLCTAG_ERR_OUT_OF_BOUNDS, rc);
    }

    delay_ms = plc_tag_get_int_attribute(tag, ATTR_NAME, GET_FAILED);
    if(delay_ms != CONN_RETRY_MAX_DELAY_LIMIT_MS) {
        fail(tag, ATTR_NAME " was not clamped down to the ceiling", CONN_RETRY_MAX_DELAY_LIMIT_MS, delay_ms);
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Clamped to %d ms and reported out of bounds.\n", delay_ms);

    /*
     * Test 4: the same clamping when the value arrives in the tag string.
     *
     * The value only reaches a connection that does not exist yet, so each of these needs a
     * connection of its own -- reusing this test's would silently ignore it, which is the
     * documented behaviour and not what is under test here.  connection_group_id is what
     * forces a separate one.
     */
    // NOLINTNEXTLINE
    fprintf(stderr, "\n=== Test 4: out-of-range values in the tag string ===\n");

    if(!check_create_time_clamp(1, CONN_RETRY_MAX_DELAY_MIN_MS, 1)) {
        plc_tag_destroy(tag);
        exit(1);
    }

    if(!check_create_time_clamp(CONN_RETRY_MAX_DELAY_LIMIT_MS + 1, CONN_RETRY_MAX_DELAY_LIMIT_MS, 2)) {
        plc_tag_destroy(tag);
        exit(1);
    }

    /* Test 5: the connection still works after all that, and the value is still the last one set. */
    // NOLINTNEXTLINE
    fprintf(stderr, "\n=== Test 5: the connection still works ===\n");

    if((rc = plc_tag_read(tag, DATA_TIMEOUT_MS)) != PLCTAG_STATUS_OK) {
        fail(tag, "Read after changing " ATTR_NAME " failed", PLCTAG_STATUS_OK, rc);
    }

    delay_ms = plc_tag_get_int_attribute(tag, ATTR_NAME, GET_FAILED);
    if(delay_ms != CONN_RETRY_MAX_DELAY_LIMIT_MS) {
        fail(tag, ATTR_NAME " changed on its own across a read", CONN_RETRY_MAX_DELAY_LIMIT_MS, delay_ms);
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "\nAll %s attribute tests passed.\n", ATTR_NAME);

    plc_tag_destroy(tag);

    return 0;
}
