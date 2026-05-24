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
 * Test program for the @connection tag type.
 *
 * Creates one or more @connection tags, registers a callback on each, and
 * verifies that connection state transitions arrive in the expected order.
 *
 * Modes
 * -----
 * Default (1 tag, 1 cycle):
 *   Verify CONNECTING -> UP -> DISCONNECTING -> DOWN -> IDLE_WAIT.
 *
 * --num-tags=N:
 *   Create N simultaneous @connection tags on the same session.  Every tag
 *   must independently receive the complete expected sequence.
 *
 * --expect-err:
 *   Point at an unreachable address and verify CONNECTING -> ERR_WAIT.
 *   No simulator needed; run when no server is listening on the target host.
 *
 * --cycles=N --data-tag=ATTRIBS [--idle-timeout-ms=T]:
 *   Verify N consecutive connect/idle-disconnect cycles.  A data tag on the
 *   same session is read after each IDLE_WAIT to trigger a reconnect.
 *   --idle-timeout-ms sets connection_inactivity_timeout_ms on the data tag.
 *
 * Also verifies the connection_status attribute on each @connection tag
 * returns a valid numeric status code (not the "not found" sentinel).
 *
 * Returns 0 (PASS) when all expected transitions are received.
 * Returns 1 (FAIL) on unexpected transition, ordering error, or timeout.
 *
 * Usage:
 *   test_connection_tag --tag=ATTRIBS [options]
 *
 * Examples:
 *   test_connection_tag \
 *     "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=@connection"
 *
 *   test_connection_tag \
 *     "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=@connection" \
 *     --num-tags=3
 *
 *   test_connection_tag \
 *     "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=@connection" \
 *     --expect-err
 *
 *   test_connection_tag \
 *     "--tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=@connection" \
 *     --cycles=2 \
 *     "--data-tag=protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]" \
 *     --idle-timeout-ms=5000
 */

#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 7, 0
#define BASE_RUN_DURATION_MS ((int64_t)40000)
#define ERR_WAIT_RUN_DURATION_MS ((int64_t)15000)
#define POLL_INTERVAL_MS ((uint32_t)200)
#define DEFAULT_TIMEOUT_MS 5000

#define MAX_TAGS 8
#define HAPPY_PATH_STATES 5
#define MAX_CYCLES 8
#define MAX_EXPECTED_STATES (HAPPY_PATH_STATES * MAX_CYCLES)

/* -------------------------------------------------------------------------
 * Per-tag state — fields written by callback, read by main thread.
 * compat_atomic_int32_t is used for all shared fields; 0 = false, 1 = true
 * for the flag fields.
 * ---------------------------------------------------------------------- */

typedef struct {
    int32_t tag_id;                         /* written once before callbacks start; plain int */
    compat_atomic_int32_t next_expected_idx;
    compat_atomic_int32_t failed;
    compat_atomic_int32_t completed;
} tag_state_t;

static tag_state_t tag_states[MAX_TAGS];
static int32_t tag_handles[MAX_TAGS];

/* -------------------------------------------------------------------------
 * Options (set from main thread before callbacks start; no atomics needed)
 * ---------------------------------------------------------------------- */

static const char *tag_path = NULL;
static int num_tags = 1;
static bool expect_err = false;
static int num_cycles = 1;
static const char *data_tag_attribs = NULL;
static int idle_timeout_ms = 0;

/* -------------------------------------------------------------------------
 * Runtime state shared between callback threads and the main thread
 * ---------------------------------------------------------------------- */

static compat_atomic_int32_t running = {1};            /* 1 = keep looping */
static compat_atomic_int32_t need_reconnect = {0};
static compat_atomic_int32_t read_started_count = {0};
static compat_atomic_int32_t read_completed_count = {0};
static compat_atomic_int32_t write_started_count = {0};
static compat_atomic_int32_t write_completed_count = {0};
static compat_atomic_int32_t io_order_failed = {0};

/* -------------------------------------------------------------------------
 * Expected states (built at startup from options; read-only after that)
 * ---------------------------------------------------------------------- */

static int32_t expected_states[MAX_EXPECTED_STATES];
static int num_expected_states = 0;

static const int32_t happy_path_cycle[HAPPY_PATH_STATES] = {
    PLCTAG_CONN_STATUS_CONNECTING, PLCTAG_CONN_STATUS_UP,        PLCTAG_CONN_STATUS_DISCONNECTING,
    PLCTAG_CONN_STATUS_DOWN,       PLCTAG_CONN_STATUS_IDLE_WAIT,
};

static void build_expected_states(void) {
    int c;

    if(expect_err) {
        /* Connection fails: CONNECTING → DOWN → ERR_WAIT (DOWN always precedes ERR_WAIT). */
        expected_states[0] = PLCTAG_CONN_STATUS_CONNECTING;
        expected_states[1] = PLCTAG_CONN_STATUS_DOWN;
        expected_states[2] = PLCTAG_CONN_STATUS_ERR_WAIT;
        num_expected_states = 3;
    } else if(data_tag_attribs != NULL) {
        /* Session is already UP when @connection tags are created.
         * The sentinel fires UP immediately; then num_cycles idle-disconnects follow. */
        int pos = 0;
        expected_states[pos++] = PLCTAG_CONN_STATUS_UP;
        for(c = 0; c < num_cycles; c++) {
            expected_states[pos++] = PLCTAG_CONN_STATUS_DISCONNECTING;
            expected_states[pos++] = PLCTAG_CONN_STATUS_DOWN;
            expected_states[pos++] = PLCTAG_CONN_STATUS_IDLE_WAIT;
            if(c < num_cycles - 1) {
                expected_states[pos++] = PLCTAG_CONN_STATUS_CONNECTING;
                expected_states[pos++] = PLCTAG_CONN_STATUS_UP;
            }
        }
        num_expected_states = pos;
    } else {
        for(c = 0; c < num_cycles; c++) {
            memcpy(&expected_states[c * HAPPY_PATH_STATES], happy_path_cycle, sizeof(happy_path_cycle));
        }
        num_expected_states = num_cycles * HAPPY_PATH_STATES;
    }
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static void interrupt_handler(void) { compat_atomic_store_int32(&running, 0); }

static const char *conn_status_name(int32_t s) {
    if(s >= PLCTAG_EVENT_CONN_STATUS_OFFSET) { s -= PLCTAG_EVENT_CONN_STATUS_OFFSET; }
    switch(s) {
        case PLCTAG_CONN_STATUS_UP: return "UP";
        case PLCTAG_CONN_STATUS_DOWN: return "DOWN";
        case PLCTAG_CONN_STATUS_DISCONNECTING: return "DISCONNECTING";
        case PLCTAG_CONN_STATUS_CONNECTING: return "CONNECTING";
        case PLCTAG_CONN_STATUS_IDLE_WAIT: return "IDLE_WAIT";
        case PLCTAG_CONN_STATUS_ERR_WAIT: return "ERR_WAIT";
        default: return "UNKNOWN";
    }
}

static tag_state_t *find_tag_state(int32_t tag_id) {
    int i;
    for(i = 0; i < num_tags; i++) {
        if(tag_states[i].tag_id == tag_id) { return &tag_states[i]; }
    }
    return NULL;
}

/* Advance a tag's expected-state machine from within the callback. */
static void handle_conn_status_event(tag_state_t *ts, int32_t conn_status) {
    int idx = compat_atomic_load_int32(&ts->next_expected_idx);
    int new_idx;
    int i;

    if(idx >= num_expected_states) { return; }

    if(expected_states[idx] == conn_status) {
        new_idx = idx + 1;
        compat_atomic_store_int32(&ts->next_expected_idx, new_idx);

        /* IDLE_WAIT at a non-final position means another cycle follows. */
        if(conn_status == PLCTAG_CONN_STATUS_IDLE_WAIT && new_idx < num_expected_states) {
            compat_atomic_store_int32(&need_reconnect, 1);
        }

        if(new_idx >= num_expected_states) { compat_atomic_store_int32(&ts->completed, 1); }
    } else {
        fprintf(stderr, "ERROR [tag %d]: expected %s but got %s (at index %d).\n", (int)ts->tag_id,
                conn_status_name(expected_states[idx]), conn_status_name(conn_status), idx);
        compat_atomic_store_int32(&ts->failed, 1);
    }

    /* Stop as soon as all tags are done or any tag failed. */
    if(compat_atomic_load_int32(&ts->completed) || compat_atomic_load_int32(&ts->failed)) {
        bool all_done = true;
        for(i = 0; i < num_tags; i++) {
            if(!compat_atomic_load_int32(&tag_states[i].completed) &&
               !compat_atomic_load_int32(&tag_states[i].failed)) {
                all_done = false;
                break;
            }
        }
        if(all_done) { compat_atomic_store_int32(&running, 0); }
    }
}

/* -------------------------------------------------------------------------
 * Callback
 * ---------------------------------------------------------------------- */

static void tag_callback(int32_t tag_id, int event, int status, void *userdata) {
    tag_state_t *ts;
    int32_t rc, ws;

    (void)userdata;

    ts = find_tag_state(tag_id);
    if(!ts) { return; }

    switch(event) {
        case PLCTAG_EVENT_CREATED:
            fprintf(stderr, "[tag %d] CREATED: %s\n", (int)tag_id, plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_DESTROYED:
            fprintf(stderr, "[tag %d] DESTROYED: %s\n", (int)tag_id, plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_ABORTED:
            fprintf(stderr, "[tag %d] ABORTED: %s\n", (int)tag_id, plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_READ_STARTED:
            compat_atomic_inc_int32(&read_started_count);
            break;

        case PLCTAG_EVENT_READ_COMPLETED:
            rc = compat_atomic_inc_int32(&read_completed_count);
            if(rc > compat_atomic_load_int32(&read_started_count)) {
                fprintf(stderr, "ERROR [tag %d]: READ_COMPLETED before READ_STARTED.\n", (int)tag_id);
                compat_atomic_store_int32(&io_order_failed, 1);
                compat_atomic_store_int32(&running, 0);
            }
            break;

        case PLCTAG_EVENT_WRITE_STARTED:
            compat_atomic_inc_int32(&write_started_count);
            break;

        case PLCTAG_EVENT_WRITE_COMPLETED:
            ws = compat_atomic_inc_int32(&write_completed_count);
            if(ws > compat_atomic_load_int32(&write_started_count)) {
                fprintf(stderr, "ERROR [tag %d]: WRITE_COMPLETED before WRITE_STARTED.\n", (int)tag_id);
                compat_atomic_store_int32(&io_order_failed, 1);
                compat_atomic_store_int32(&running, 0);
            }
            break;

        case PLCTAG_EVENT_CONN_STATUS_UP:
            fprintf(stderr, "[tag %d] CONN_STATUS_UP\n", (int)tag_id);
            handle_conn_status_event(ts, PLCTAG_CONN_STATUS_UP);
            break;

        case PLCTAG_EVENT_CONN_STATUS_DOWN:
            fprintf(stderr, "[tag %d] CONN_STATUS_DOWN\n", (int)tag_id);
            handle_conn_status_event(ts, PLCTAG_CONN_STATUS_DOWN);
            break;

        case PLCTAG_EVENT_CONN_STATUS_DISCONNECTING:
            fprintf(stderr, "[tag %d] CONN_STATUS_DISCONNECTING\n", (int)tag_id);
            handle_conn_status_event(ts, PLCTAG_CONN_STATUS_DISCONNECTING);
            break;

        case PLCTAG_EVENT_CONN_STATUS_CONNECTING:
            fprintf(stderr, "[tag %d] CONN_STATUS_CONNECTING\n", (int)tag_id);
            handle_conn_status_event(ts, PLCTAG_CONN_STATUS_CONNECTING);
            break;

        case PLCTAG_EVENT_CONN_STATUS_IDLE_WAIT:
            fprintf(stderr, "[tag %d] CONN_STATUS_IDLE_WAIT\n", (int)tag_id);
            handle_conn_status_event(ts, PLCTAG_CONN_STATUS_IDLE_WAIT);
            break;

        case PLCTAG_EVENT_CONN_STATUS_ERR_WAIT:
            fprintf(stderr, "[tag %d] CONN_STATUS_ERR_WAIT\n", (int)tag_id);
            handle_conn_status_event(ts, PLCTAG_CONN_STATUS_ERR_WAIT);
            break;

        default:
            fprintf(stderr, "[tag %d] unknown event %d: %s\n", (int)tag_id, event, plc_tag_decode_error(status));
            break;
    }
}

/* -------------------------------------------------------------------------
 * Argument parsing
 * ---------------------------------------------------------------------- */

static void parse_args(int argc, char **argv) {
    int i;

    if(argc < 2) {
        fprintf(stderr, "Usage: test_connection_tag --tag=ATTRIBS [options]\n\n");
        fprintf(stderr, "  --tag=ATTRIBS            @connection tag attribute string (required)\n");
        fprintf(stderr, "  --num-tags=N             simultaneous @connection tags (default 1, max %d)\n", MAX_TAGS);
        fprintf(stderr, "  --expect-err             expect CONNECTING -> ERR_WAIT (unreachable host)\n");
        fprintf(stderr, "  --cycles=N               connect/disconnect cycles to verify (default 1, max %d)\n", MAX_CYCLES);
        fprintf(stderr, "  --data-tag=ATTRIBS       data tag on same session; required when --cycles > 1\n");
        fprintf(stderr, "  --idle-timeout-ms=N      set connection_inactivity_timeout_ms on data tag\n");
        fprintf(stderr, "  --debug=N                debug level (0=none, 4=detail)\n");
        exit(1);
    }

    for(i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) {
            tag_path = &argv[i][6];
        } else if(strncmp(argv[i], "--num-tags=", 11) == 0) {
            num_tags = atoi(&argv[i][11]);
        } else if(strcmp(argv[i], "--expect-err") == 0) {
            expect_err = true;
        } else if(strncmp(argv[i], "--cycles=", 9) == 0) {
            num_cycles = atoi(&argv[i][9]);
        } else if(strncmp(argv[i], "--data-tag=", 11) == 0) {
            data_tag_attribs = &argv[i][11];
        } else if(strncmp(argv[i], "--idle-timeout-ms=", 18) == 0) {
            idle_timeout_ms = atoi(&argv[i][18]);
        } else if(strncmp(argv[i], "--debug=", 8) == 0) {
            plc_tag_set_debug_level(atoi(&argv[i][8]));
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            exit(1);
        }
    }

    if(tag_path == NULL || tag_path[0] == '\0') {
        fprintf(stderr, "Error: --tag argument is required.\n");
        exit(1);
    }

    if(num_tags < 1 || num_tags > MAX_TAGS) {
        fprintf(stderr, "Error: --num-tags must be between 1 and %d.\n", MAX_TAGS);
        exit(1);
    }

    if(num_cycles < 1 || num_cycles > MAX_CYCLES) {
        fprintf(stderr, "Error: --cycles must be between 1 and %d.\n", MAX_CYCLES);
        exit(1);
    }

    if(num_cycles > 1 && data_tag_attribs == NULL) {
        fprintf(stderr, "Error: --data-tag=ATTRIBS is required when --cycles > 1.\n");
        exit(1);
    }

    if(expect_err && num_cycles > 1) {
        fprintf(stderr, "Error: --expect-err and --cycles cannot be combined.\n");
        exit(1);
    }
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(int argc, char **argv) {
    int32_t data_tag = 0;
    int64_t run_duration_ms;
    int64_t end_time;
    int total_failed = 0;
    int i;

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

    compat_set_interrupt_handler(interrupt_handler);

    build_expected_states();

    fprintf(stderr, "Expected sequence (%d states):", num_expected_states);
    for(i = 0; i < num_expected_states; i++) { fprintf(stderr, " %s", conn_status_name(expected_states[i])); }
    fprintf(stderr, "\n");

    if(expect_err) {
        run_duration_ms = ERR_WAIT_RUN_DURATION_MS;
    } else if(idle_timeout_ms > 0) {
        run_duration_ms = (int64_t)num_cycles * ((int64_t)idle_timeout_ms + 15000LL);
    } else {
        run_duration_ms = (int64_t)num_cycles * BASE_RUN_DURATION_MS;
    }

    if(data_tag_attribs) {
        int rc;

        fprintf(stderr, "Creating data tag: %s\n", data_tag_attribs);
        data_tag = plc_tag_create(data_tag_attribs, DEFAULT_TIMEOUT_MS);
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
        if(idle_timeout_ms > 0) {
            fprintf(stderr, "Setting connection_inactivity_timeout_ms = %d ms\n", idle_timeout_ms);
            plc_tag_set_int_attribute(data_tag, "connection_inactivity_timeout_ms", idle_timeout_ms);
        }
    }

    for(i = 0; i < num_tags; i++) {
        int32_t tag;

        tag_states[i].tag_id = 0;
        compat_atomic_store_int32(&tag_states[i].next_expected_idx, 0);
        compat_atomic_store_int32(&tag_states[i].failed, 0);
        compat_atomic_store_int32(&tag_states[i].completed, 0);

        fprintf(stderr, "Creating @connection tag %d/%d: %s\n", i + 1, num_tags, tag_path);
        tag = plc_tag_create_ex(tag_path, tag_callback, NULL, 0);
        if(tag < 0) {
            fprintf(stderr, "ERROR: could not create @connection tag %d: %s\n", i + 1, plc_tag_decode_error((int)tag));
            while(--i >= 0) { plc_tag_destroy(tag_handles[i]); }
            if(data_tag) { plc_tag_destroy(data_tag); }
            return 1;
        }
        tag_states[i].tag_id = tag;
        tag_handles[i] = tag;
    }

    fprintf(stderr, "Waiting up to %.1f seconds for %d tag(s), %d cycle(s).\n", (double)run_duration_ms / 1000.0,
            num_tags, num_cycles);

    end_time = compat_time_ms() + run_duration_ms;

    while(compat_atomic_load_int32(&running) && compat_time_ms() < end_time) {
        if(compat_atomic_load_int32(&need_reconnect) && data_tag != 0) {
            compat_atomic_store_int32(&need_reconnect, 0);
            plc_tag_read(data_tag, 0);
            fprintf(stderr, "Triggered reconnect read on data tag.\n");
        }
        compat_sleep_ms(POLL_INTERVAL_MS, NULL);
    }

    fprintf(stderr, "Shutting down.\n");

    /* Verify the connection_status attribute on each live tag before destroying. */
    for(i = 0; i < num_tags; i++) {
        int attr_val = plc_tag_get_int_attribute(tag_handles[i], "connection_status", -999);
        if(attr_val == -999) {
            fprintf(stderr, "ERROR [tag %d]: connection_status attribute not found.\n", (int)tag_handles[i]);
            total_failed++;
        } else {
            fprintf(stderr, "[tag %d] connection_status attribute = %s (%d)\n", (int)tag_handles[i],
                    conn_status_name(attr_val), attr_val);
        }
    }

    for(i = 0; i < num_tags; i++) { plc_tag_destroy(tag_handles[i]); }
    if(data_tag) { plc_tag_destroy(data_tag); }

    for(i = 0; i < num_tags; i++) {
        if(compat_atomic_load_int32(&tag_states[i].failed)) {
            fprintf(stderr, "FAIL [tag %d]: unexpected state transition.\n", (int)tag_handles[i]);
            total_failed++;
        } else if(!compat_atomic_load_int32(&tag_states[i].completed)) {
            int idx = compat_atomic_load_int32(&tag_states[i].next_expected_idx);
            fprintf(stderr, "FAIL [tag %d]: only %d of %d expected states received (next expected: %s).\n",
                    (int)tag_handles[i], idx, num_expected_states, conn_status_name(expected_states[idx]));
            total_failed++;
        }
    }

    if(compat_atomic_load_int32(&io_order_failed)) { total_failed++; }

    if(total_failed > 0) {
        fprintf(stderr, "RESULT: FAIL\n");
        return 1;
    }

    fprintf(stderr, "RESULT: PASS\n");
    return 0;
}
