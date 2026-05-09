/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever   *
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
 * Test program for the @device tag type.
 *
 * Creates a device tag using a user-supplied attribute string, registers a
 * callback that checks connection state transitions in order, and runs until
 * all expected transitions are seen or the timeout expires.
 *
 * Expected sequence: CONNECTING -> UP -> DISCONNECTING -> DOWN -> IDLE_WAIT
 * The last three occur when the session idle-disconnect timeout fires (~30s).
 *
 * Returns 0 (pass) if all expected transitions are received in order.
 * Returns 1 (fail) if an unexpected transition is received or the timeout
 * expires before all transitions are seen.
 *
 * Usage:
 *   test_device_tag --tag=<attribute-string> [--debug=N]
 *
 * Example:
 *   test_device_tag \
 *     "--tag=protocol=ab-eip&gateway=10.0.0.1&path=1,0&plc=ControlLogix&name=@device"
 */

#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 6, 16
#define RUN_DURATION_MS ((int64_t)40000)
#define POLL_INTERVAL_MS ((uint32_t)500)

/* Expected connection state sequence for one connect + idle-disconnect cycle. */
static const int32_t expected_states[] = {
    PLCTAG_CONN_STATUS_CONNECTING, PLCTAG_CONN_STATUS_UP,        PLCTAG_CONN_STATUS_DISCONNECTING,
    PLCTAG_CONN_STATUS_DOWN,       PLCTAG_CONN_STATUS_IDLE_WAIT,
};

#define NUM_EXPECTED_STATES ((int)(sizeof(expected_states) / sizeof(expected_states[0])))

static const char *tag_path = NULL;
static volatile bool running = true;
static volatile int next_expected_idx = 0;
static volatile bool test_failed = false;
static volatile int read_started_count = 0;
static volatile int read_completed_count = 0;
static volatile int write_started_count = 0;
static volatile int write_completed_count = 0;


static void interrupt_handler(void) { running = false; }


static const char *conn_status_name(int32_t conn_status) {
    switch(conn_status) {
        case PLCTAG_CONN_STATUS_UP: return "UP";
        case PLCTAG_CONN_STATUS_DOWN: return "DOWN";
        case PLCTAG_CONN_STATUS_DISCONNECTING: return "DISCONNECTING";
        case PLCTAG_CONN_STATUS_CONNECTING: return "CONNECTING";
        case PLCTAG_CONN_STATUS_IDLE_WAIT: return "IDLE_WAIT";
        case PLCTAG_CONN_STATUS_ERR_WAIT: return "ERR_WAIT";
        default: return "UNKNOWN";
    }
}


static void tag_callback(int32_t tag_id, int event, int status, void *userdata) {
    int32_t conn = 0;
    int32_t reason = 0;
    int idx = 0;

    (void)userdata;

    switch(event) {
        case PLCTAG_EVENT_CREATED: fprintf(stderr, "EVENT CREATED: status=%s.\n", plc_tag_decode_error(status)); break;

        case PLCTAG_EVENT_DESTROYED: fprintf(stderr, "EVENT DESTROYED: status=%s.\n", plc_tag_decode_error(status)); break;

        case PLCTAG_EVENT_ABORTED: fprintf(stderr, "EVENT ABORTED: status=%s.\n", plc_tag_decode_error(status)); break;

        case PLCTAG_EVENT_READ_COMPLETED:
            fprintf(stderr, "EVENT READ_COMPLETED: status=%s.\n", plc_tag_decode_error(status));
            read_completed_count++;
            if(read_completed_count > read_started_count) {
                fprintf(stderr, "ERROR: READ_COMPLETED observed before matching READ_STARTED.\n");
                test_failed = true;
                running = false;
            }
            break;

        case PLCTAG_EVENT_READ_STARTED:
            fprintf(stderr, "EVENT READ_STARTED: status=%s.\n", plc_tag_decode_error(status));
            read_started_count++;
            break;

        case PLCTAG_EVENT_WRITE_STARTED:
            fprintf(stderr, "EVENT WRITE_STARTED: status=%s.\n", plc_tag_decode_error(status));
            write_started_count++;
            break;

        case PLCTAG_EVENT_WRITE_COMPLETED:
            fprintf(stderr, "EVENT WRITE_COMPLETED: status=%s.\n", plc_tag_decode_error(status));
            write_completed_count++;
            if(write_completed_count > write_started_count) {
                fprintf(stderr, "ERROR: WRITE_COMPLETED observed before matching WRITE_STARTED.\n");
                test_failed = true;
                running = false;
            }
            break;

        case PLCTAG_EVENT_CONNECTION_CHANGED_STATE:
            conn = plc_tag_get_int_attribute(tag_id, "connection_status", -1);
            reason = (int32_t)status;
            fprintf(stderr, "EVENT CONNECTION_CHANGED_STATE: state=%s (%d), reason=%s.\n", conn_status_name(conn), (int)conn,
                    plc_tag_decode_error((int)reason));

            idx = next_expected_idx;
            if(idx < NUM_EXPECTED_STATES) {
                if(conn == expected_states[idx]) {
                    next_expected_idx = idx + 1;
                    if(next_expected_idx == NUM_EXPECTED_STATES) {
                        fprintf(stderr, "All %d expected state transitions received.\n", NUM_EXPECTED_STATES);
                        running = false;
                    }
                } else {
                    fprintf(stderr, "ERROR: expected state %s but got %s.\n", conn_status_name(expected_states[idx]),
                            conn_status_name(conn));
                    test_failed = true;
                    running = false;
                }
            }
            break;

        default: fprintf(stderr, "EVENT unknown (%d): status=%s.\n", event, plc_tag_decode_error(status)); break;
    }
}


static void parse_args(int argc, char **argv) {
    int i = 0;

    if(argc < 2) {
        fprintf(stderr, "Usage: test_device_tag --tag=TAG_ATTRIBUTE_STRING [--debug=N]\n");
        fprintf(stderr, "  --tag=TAG_ATTRIBUTE_STRING  device tag attribute string, e.g.\n");
        fprintf(stderr, "    \"protocol=ab-eip&gateway=10.0.0.1&path=1,0&plc=ControlLogix&name=@device\"\n");
        fprintf(stderr, "  --debug=N                   debug level (0=none, 4=detail)\n");
        exit(1);
    }

    for(i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) {
            tag_path = &argv[i][6];
        } else if(strncmp(argv[i], "--debug=", 8) == 0) {
            plc_tag_set_debug_level(atoi(&argv[i][8]));
        }
    }

    if(tag_path == NULL || tag_path[0] == '\0') {
        fprintf(stderr, "Error: --tag argument is required and must not be empty.\n");
        exit(1);
    }
}


int main(int argc, char **argv) {
    int32_t tag = 0;
    int64_t end_time = 0;
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);

    parse_args(argc, argv);

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required library version %d.%d.%d not available, found %d.%d.%d!\n", REQUIRED_VERSION, version_major,
                version_minor, version_patch);
        return 1;
    }

    fprintf(stderr, "Library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    compat_set_interrupt_handler(interrupt_handler);

    fprintf(stderr, "Creating device tag: %s\n", tag_path);

    tag = plc_tag_create_ex(tag_path, tag_callback, NULL, 0);
    if(tag < 0) {
        fprintf(stderr, "ERROR: could not create device tag: %s\n", plc_tag_decode_error((int)tag));
        return 1;
    }

    fprintf(stderr, "Device tag created (id=%d). Waiting up to %d seconds for expected state transitions.\n", (int)tag,
            (int)(RUN_DURATION_MS / 1000));

    end_time = compat_time_ms() + RUN_DURATION_MS;

    while(running && compat_time_ms() < end_time) {
        // int conn = plc_tag_get_int_attribute(tag, "connection_status", -1);
        // fprintf(stderr, "POLL: connection_status=%s (%d)\n", conn_status_name(conn), (int)conn);
        compat_sleep_ms(POLL_INTERVAL_MS, NULL);
    }

    fprintf(stderr, "Shutting down.\n");

    plc_tag_destroy(tag);

    if(test_failed) {
        fprintf(stderr, "RESULT: FAIL - received unexpected state transition.\n");
        return 1;
    }

    if(next_expected_idx < NUM_EXPECTED_STATES) {
        fprintf(stderr, "RESULT: FAIL - only %d of %d expected state transitions received (last expected: %s).\n",
                next_expected_idx, NUM_EXPECTED_STATES, conn_status_name(expected_states[next_expected_idx]));
        return 1;
    }

    if(read_started_count < 1 || read_completed_count < 1 || write_started_count < 1 || write_completed_count < 1) {
        fprintf(stderr,
                "RESULT: FAIL - missing IO events. read_started=%d read_completed=%d write_started=%d write_completed=%d.\n",
                read_started_count, read_completed_count, write_started_count, write_completed_count);
        return 1;
    }

    if(read_started_count < read_completed_count || write_started_count < write_completed_count) {
        fprintf(
            stderr,
            "RESULT: FAIL - IO event ordering invalid. read_started=%d read_completed=%d write_started=%d write_completed=%d.\n",
            read_started_count, read_completed_count, write_started_count, write_completed_count);
        return 1;
    }

    fprintf(stderr, "RESULT: PASS\n");
    return 0;
}
