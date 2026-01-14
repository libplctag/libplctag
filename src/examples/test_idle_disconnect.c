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


#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 1, 10
#define DATA_TIMEOUT 5000
#define NEW_TIMEOUT_MS 6000        /* 6 seconds */
#define INVALID_TIMEOUT_MS 1000000 /* Way too high, should be clamped */
#define NEAR_MAX_TIMEOUT_MS 29900  /* Just under 30000ms max */


static char *tag_path = NULL;


static void parse_args(int argc, char **argv) {
    if(argc < 2) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Usage: test_idle_disconnect --tag=TAG_STRING\n");
        // NOLINTNEXTLINE
        fprintf(stderr, "  --tag=TAG_STRING: tag path string\n");
        exit(1);
    }

    for(int i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) { tag_path = &argv[i][6]; }
    }

    if(tag_path == NULL || strlen(tag_path) == 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error: tag path must be specified\n");
        exit(1);
    }
}


static int32_t create_tag(void) {
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;

    /* create the tag */
    tag = plc_tag_create(tag_path, DATA_TIMEOUT);

    /* everything OK? */
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        exit(-tag);
    }

    if((rc = plc_tag_status(tag)) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error setting up tag internal state. Error %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        exit(-rc);
    }

    return tag;
}


static const char *status_to_string(int status) {
    switch(status) {
        case PLCTAG_CONN_STATUS_UP: return "UP";
        case PLCTAG_CONN_STATUS_DOWN: return "DOWN";
        case PLCTAG_CONN_STATUS_CONNECTING: return "CONNECTING";
        case PLCTAG_CONN_STATUS_DISCONNECTING: return "DISCONNECTING";
        case PLCTAG_CONN_STATUS_WAIT: return "WAIT";
        default: return "UNKNOWN";
    }
}


static void read_tag(int32_t tag) {
    int rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Unable to read the data! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        exit(-rc);
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Read successful\n");
}


int main(int argc, char **argv) {
    int32_t tag = 0;
    int timeout_value = 0;
    int status = 0;
    int wait_time_ms = 0;

    /* check library API version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    parse_args(argc, argv);

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /* create the tag */
    tag = create_tag();

    /* perform initial read to establish connection */
    read_tag(tag);

    /* read the existing inactivity timeout (should be max by default) */
    int initial_timeout_value = plc_tag_get_int_attribute(tag, "connection_inactivity_timeout_ms", 0);
    if(initial_timeout_value <= 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Failed to read initial inactivity timeout\n");
        plc_tag_destroy(tag);
        exit(1);
    }
    // NOLINTNEXTLINE
    fprintf(stderr, "Initial inactivity timeout (default max): %d ms\n", initial_timeout_value);

    /* set the inactivity timeout to a lower value */
    // NOLINTNEXTLINE
    fprintf(stderr, "Setting inactivity timeout to %d ms\n", NEW_TIMEOUT_MS);
    plc_tag_set_int_attribute(tag, "connection_inactivity_timeout_ms", NEW_TIMEOUT_MS);

    /* read it back and verify */
    timeout_value = plc_tag_get_int_attribute(tag, "connection_inactivity_timeout_ms", 0);
    if(timeout_value != NEW_TIMEOUT_MS) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Failed to set inactivity timeout. Expected %d ms but got %d ms\n", NEW_TIMEOUT_MS, timeout_value);
        plc_tag_destroy(tag);
        exit(1);
    }
    // NOLINTNEXTLINE
    fprintf(stderr, "Successfully set inactivity timeout to %d ms\n", timeout_value);

    /* check connection status is UP before we start waiting */
    status = plc_tag_get_int_attribute(tag, "connection_status", PLCTAG_CONN_STATUS_DOWN);
    // NOLINTNEXTLINE
    fprintf(stderr, "Connection status before idle wait: %s\n", status_to_string(status));

    /* wait 50% of the timeout - connection should still be UP */
    wait_time_ms = (NEW_TIMEOUT_MS / 2);
    // NOLINTNEXTLINE
    fprintf(stderr, "Waiting %d ms (50%% of timeout)...\n", wait_time_ms);
    int remaining_ms = wait_time_ms;
    while(remaining_ms > 0) {
        int sleep_ms = (remaining_ms > 500) ? 500 : remaining_ms;
        compat_sleep_ms((uint32_t)sleep_ms, NULL);
        remaining_ms -= sleep_ms;
    }

    /* check connection status - should still be UP */
    status = plc_tag_get_int_attribute(tag, "connection_status", PLCTAG_CONN_STATUS_DOWN);
    // NOLINTNEXTLINE
    fprintf(stderr, "Connection status after 50%% wait: %s\n", status_to_string(status));
    if(status != PLCTAG_CONN_STATUS_UP) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Connection should be UP after 50%% wait, but is %s\n", status_to_string(status));
        plc_tag_destroy(tag);
        exit(1);
    }

    /* read the tag to demonstrate it's still working */
    read_tag(tag);

    /* wait 150% of the timeout - connection should be DOWN/WAIT */
    wait_time_ms = (NEW_TIMEOUT_MS * 3 / 2);
    // NOLINTNEXTLINE
    fprintf(stderr, "Waiting %d ms (150%% of timeout)...\n", wait_time_ms);
    remaining_ms = wait_time_ms;
    while(remaining_ms > 0) {
        int sleep_ms = (remaining_ms > 500) ? 500 : remaining_ms;
        compat_sleep_ms((uint32_t)sleep_ms, NULL);
        remaining_ms -= sleep_ms;
    }

    /* check connection status - should be DOWN or WAIT */
    status = plc_tag_get_int_attribute(tag, "connection_status", PLCTAG_CONN_STATUS_DOWN);
    // NOLINTNEXTLINE
    fprintf(stderr, "Connection status after 150%% wait: %s\n", status_to_string(status));
    if(status != PLCTAG_CONN_STATUS_DOWN && status != PLCTAG_CONN_STATUS_WAIT) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Connection should be DOWN or WAIT after 150%% wait, but is %s\n", status_to_string(status));
        plc_tag_destroy(tag);
        exit(1);
    }

    /* read the tag - should reconnect and be UP */
    read_tag(tag);

    /* check connection status - should be UP */
    status = plc_tag_get_int_attribute(tag, "connection_status", PLCTAG_CONN_STATUS_DOWN);
    // NOLINTNEXTLINE
    fprintf(stderr, "Connection status after reconnect read: %s\n", status_to_string(status));
    if(status != PLCTAG_CONN_STATUS_UP) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Connection should be UP after reconnect read, but is %s\n", status_to_string(status));
        plc_tag_destroy(tag);
        exit(1);
    }

    /* Test 2: Try to set an invalid (too high) timeout value */
    // NOLINTNEXTLINE
    fprintf(stderr, "\n=== Test 2: Invalid timeout value (too high) ===\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "Attempting to set inactivity timeout to invalid value %d ms\n", INVALID_TIMEOUT_MS);
    int set_rc = plc_tag_set_int_attribute(tag, "connection_inactivity_timeout_ms", INVALID_TIMEOUT_MS);

    if(set_rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Got expected error: %s\n", plc_tag_decode_error(set_rc));
    } else {
        // NOLINTNEXTLINE
        fprintf(stderr, "Warning: No error returned, checking if value was clamped\n");
    }

    /* read the attribute and verify it was clamped/restored to the initial maximum */
    timeout_value = plc_tag_get_int_attribute(tag, "connection_inactivity_timeout_ms", 0);
    // NOLINTNEXTLINE
    fprintf(stderr, "After invalid set attempt, timeout is: %d ms\n", timeout_value);
    if(timeout_value != initial_timeout_value) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Timeout was not restored to initial value. Expected %d ms but got %d ms\n", initial_timeout_value,
                timeout_value);
        plc_tag_destroy(tag);
        exit(1);
    }
    // NOLINTNEXTLINE
    fprintf(stderr, "Timeout correctly restored to initial maximum value: %d ms\n", timeout_value);

    /* Test 3: Wait almost the maximum timeout and verify immediate read */
    // NOLINTNEXTLINE
    fprintf(stderr, "\n=== Test 3: Wait near-maximum timeout ===\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "Setting inactivity timeout to near-maximum %d ms\n", NEAR_MAX_TIMEOUT_MS);
    plc_tag_set_int_attribute(tag, "connection_inactivity_timeout_ms", NEAR_MAX_TIMEOUT_MS);

    timeout_value = plc_tag_get_int_attribute(tag, "connection_inactivity_timeout_ms", 0);
    if(timeout_value != NEAR_MAX_TIMEOUT_MS) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Failed to set near-max timeout. Expected %d ms but got %d ms\n", NEAR_MAX_TIMEOUT_MS,
                timeout_value);
        plc_tag_destroy(tag);
        exit(1);
    }
    // NOLINTNEXTLINE
    fprintf(stderr, "Successfully set inactivity timeout to %d ms\n", timeout_value);

    /* check connection status is UP before we start waiting */
    status = plc_tag_get_int_attribute(tag, "connection_status", PLCTAG_CONN_STATUS_DOWN);
    // NOLINTNEXTLINE
    fprintf(stderr, "Connection status before near-max wait: %s\n", status_to_string(status));

    /* wait the near-maximum timeout - connection should still be UP (not timed out) */
    wait_time_ms = NEAR_MAX_TIMEOUT_MS;
    // NOLINTNEXTLINE
    fprintf(stderr, "Waiting %d ms (near-maximum timeout)...\n", wait_time_ms);
    remaining_ms = wait_time_ms;
    while(remaining_ms > 0) {
        int sleep_ms = (remaining_ms > 500) ? 500 : remaining_ms;
        compat_sleep_ms((uint32_t)sleep_ms, NULL);
        remaining_ms -= sleep_ms;
    }

    /* check connection status - should still be UP */
    status = plc_tag_get_int_attribute(tag, "connection_status", PLCTAG_CONN_STATUS_DOWN);
    // NOLINTNEXTLINE
    fprintf(stderr, "Connection status after near-max wait: %s\n", status_to_string(status));
    if(status != PLCTAG_CONN_STATUS_UP) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Connection should be UP after near-max wait, but is %s\n", status_to_string(status));
        plc_tag_destroy(tag);
        exit(1);
    }

    /* read the tag - should complete immediately with existing connection */
    // NOLINTNEXTLINE
    fprintf(stderr, "Reading tag after near-max timeout wait (should be immediate)...\n");

    int64_t start_time_ms = compat_time_ms();

    read_tag(tag);

    int64_t end_time_ms = compat_time_ms();
    uint32_t read_time_ms = (uint32_t)(end_time_ms - start_time_ms);
    // NOLINTNEXTLINE
    fprintf(stderr, "Read completed in %u ms (should be fast, < 100ms)\n", read_time_ms);
    if(read_time_ms > 100) {
        // NOLINTNEXTLINE
        fprintf(stderr, "WARNING: Read took longer than expected (%u ms), possible reconnection\n", read_time_ms);
    }

    /* check connection status - should still be UP */
    status = plc_tag_get_int_attribute(tag, "connection_status", PLCTAG_CONN_STATUS_DOWN);
    // NOLINTNEXTLINE
    fprintf(stderr, "Connection status after near-max timeout read: %s\n", status_to_string(status));
    if(status != PLCTAG_CONN_STATUS_UP) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Connection should be UP after near-max read, but is %s\n", status_to_string(status));
        plc_tag_destroy(tag);
        exit(1);
    }

    /* clean up */
    plc_tag_destroy(tag);

    // NOLINTNEXTLINE
    fprintf(stderr, "\nAll tests PASSED!\n");

    return 0;
}
