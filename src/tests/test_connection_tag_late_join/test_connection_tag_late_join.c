/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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
 * Test for the @connection tag "late join" scenario.
 *
 * A data tag is created first to establish the session (bringing it UP).
 * A @connection tag is then created on the same session while it is
 * already connected.  The test verifies that the late-joining @connection
 * tag receives the UP event within the timeout — i.e., it correctly
 * delivers the current session state rather than silently missing the
 * event that fired before it was created.
 *
 * Returns 0 (PASS) when the UP event is received within the timeout.
 * Returns 1 (FAIL) if ERR_WAIT is received or the timeout expires first.
 *
 * Usage:
 *   test_connection_tag_late_join \
 *     --data-tag=ATTRIBS \
 *     --tag=ATTRIBS \
 *     [--timeout=ms] [--debug=N]
 *
 * Example (AB/EIP with simulator):
 *   test_connection_tag_late_join \
 *     "--data-tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]" \
 *     "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=@connection" \
 *     --timeout=5000
 */

#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 7, 0
#define DEFAULT_TIMEOUT_MS 5000
#define POLL_SLEEP_MS 50

static const char *data_tag_attribs = NULL;
static const char *conn_tag_attribs = NULL;
static int timeout_ms = DEFAULT_TIMEOUT_MS;

static compat_atomic_int32_t got_up = {0};
static compat_atomic_int32_t got_err_wait = {0};

static void conn_tag_callback(int32_t tag_id, int event, int status, void *userdata) {
    (void)tag_id;
    (void)status;
    (void)userdata;

    switch(event) {
        case PLCTAG_EVENT_CONN_STATUS_UP:
            fprintf(stderr, "  @connection tag received UP event.\n");
            compat_atomic_store_int32(&got_up, 1);
            break;
        case PLCTAG_EVENT_CONN_STATUS_ERR_WAIT:
            fprintf(stderr, "  @connection tag received ERR_WAIT — session failed.\n");
            compat_atomic_store_int32(&got_err_wait, 1);
            break;
        default: break;
    }
}

static void parse_args(int argc, char **argv) {
    int i;

    for(i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--data-tag=", 11) == 0) {
            data_tag_attribs = &argv[i][11];
        } else if(strncmp(argv[i], "--tag=", 6) == 0) {
            conn_tag_attribs = &argv[i][6];
        } else if(strncmp(argv[i], "--timeout=", 10) == 0) {
            timeout_ms = atoi(&argv[i][10]);
            if(timeout_ms <= 0) { timeout_ms = DEFAULT_TIMEOUT_MS; }
        } else if(strncmp(argv[i], "--debug=", 8) == 0) {
            plc_tag_set_debug_level(atoi(&argv[i][8]));
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            fprintf(stderr,
                    "Usage: test_connection_tag_late_join --data-tag=ATTRIBS --tag=ATTRIBS "
                    "[--timeout=ms] [--debug=N]\n");
            exit(1);
        }
    }

    if(data_tag_attribs == NULL || data_tag_attribs[0] == '\0') {
        fprintf(stderr, "Error: --data-tag=ATTRIBS is required.\n");
        exit(1);
    }

    if(conn_tag_attribs == NULL || conn_tag_attribs[0] == '\0') {
        fprintf(stderr, "Error: --tag=ATTRIBS is required.\n");
        exit(1);
    }
}

int main(int argc, char **argv) {
    int32_t data_tag = 0;
    int32_t conn_tag = 0;
    int rc = PLCTAG_STATUS_OK;
    int64_t deadline;

    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);

    parse_args(argc, argv);

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required library version %d.%d.%d not available, found %d.%d.%d!\n", REQUIRED_VERSION,
                version_major, version_minor, version_patch);
        return 1;
    }

    fprintf(stderr, "Library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    /* Step 1: Create the data tag and wait for the session to come UP. */
    fprintf(stderr, "Creating data tag: %s\n", data_tag_attribs);
    data_tag = plc_tag_create(data_tag_attribs, timeout_ms);
    if(data_tag < 0) {
        fprintf(stderr, "ERROR: could not create data tag: %s\n", plc_tag_decode_error((int)data_tag));
        return 1;
    }

    rc = plc_tag_status(data_tag);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "ERROR: data tag not ready: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(data_tag);
        return 1;
    }
    fprintf(stderr, "Data tag ready — session is UP.\n");

    /* Step 2: Create the @connection tag while the session is already live. */
    fprintf(stderr, "Creating @connection tag (late join): %s\n", conn_tag_attribs);
    conn_tag = plc_tag_create_ex(conn_tag_attribs, conn_tag_callback, NULL, 0);
    if(conn_tag < 0) {
        fprintf(stderr, "ERROR: could not create @connection tag: %s\n", plc_tag_decode_error((int)conn_tag));
        plc_tag_destroy(data_tag);
        return 1;
    }

    /* Step 3: Wait for UP — should arrive quickly since the session is already connected. */
    fprintf(stderr, "Waiting up to %d ms for UP event on late-joining @connection tag.\n", timeout_ms);
    deadline = compat_time_ms() + (int64_t)timeout_ms;

    while(!compat_atomic_load_int32(&got_up) && !compat_atomic_load_int32(&got_err_wait) &&
          compat_time_ms() < deadline) {
        compat_sleep_ms(POLL_SLEEP_MS, NULL);
    }

    plc_tag_destroy(conn_tag);
    plc_tag_destroy(data_tag);

    if(compat_atomic_load_int32(&got_err_wait)) {
        fprintf(stderr, "RESULT: FAIL — @connection tag received ERR_WAIT instead of UP.\n");
        return 1;
    }

    if(!compat_atomic_load_int32(&got_up)) {
        fprintf(stderr, "RESULT: FAIL — UP event not received within %d ms.\n", timeout_ms);
        return 1;
    }

    fprintf(stderr, "RESULT: PASS\n");
    return 0;
}
