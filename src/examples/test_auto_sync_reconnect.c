/***************************************************************************
 *   Copyright (C) 2025 by Simon Labrecque                                 *
 *   Author Simon Labrecque  simon@wegel.ca                                *
 *   Changes by Kyle Hayes kyle.hayes@gmail.com                            *
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
#include <errno.h>
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>


#define REQUIRED_VERSION 2, 6, 6

#define DATA_TIMEOUT (500)
#define DISCONNECT_TIME_MS (3000)
#define TEST_DURATION_MS (10000)
#define READ_POLL_MS (100)

/* uses auto_sync_read_ms for automatic reads */
#define TAG_ATTRIBS \
    "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_type=DINT&elem_count=1&name=TestBigArray[0]&auto_sync_read_ms=100"

#ifdef WINDOWS_PLATFORM
#    define SERVER_START "start /B %s --plc=ControlLogix --path=1,0 --tag=TestBigArray:DINT[10] >nul 2>&1"
#    define SERVER_STOP "taskkill /IM ab_server.exe /F"
#else
#    define SERVER_START "%s --plc=ControlLogix --path=1,0 --tag=TestBigArray:DINT[10] --debug > ab_server.log 2>&1 &"
#    define SERVER_STOP "killall -q -TERM ab_server"
#endif

#define log(...)                         \
    compat_fprintf(stderr, __VA_ARGS__); \
    fflush(stderr)

typedef struct {
    const char *ab_server_cmd; /* command to start the AB server */
    int64_t start_time;
    int64_t end_time;
    int64_t disconnect_time;
    int64_t reconnect_time;
    int32_t tag;
    int read_start_count;
    int read_success_count;
    int read_timeout_count;
    int read_error_count;
    int read_success_after_reconnect;
    int test_passed;
    int errors_before_disconnect; /* error count before disconnect */
    int errors_after_reconnect;   /* error count after reconnect */
    int errors_during_disconnect; /* calculated: errors that occurred during disconnect */
    bool reconnect_done;          /* flag to indicate reconnection has been done */
} test_state_t;


static void check_library_version(void);
static void setup_tag(test_state_t *test_state);
static void stop_server(void);
static void start_server(test_state_t *test_state);
static void do_disconnect(int64_t current_time, test_state_t *test_state);
static void do_reconnect(int64_t current_time, test_state_t *test_state);
static void tag_callback(int32_t tag_id, int event, int status, void *data);
static void run_test(test_state_t *test_state);


int main(int argc, char **argv) {
    const char *ab_server_cmd = NULL;
    test_state_t test_state = {0};

    if(argc > 1) {
        ab_server_cmd = argv[1];
        test_state.ab_server_cmd = ab_server_cmd;
    } else {
        log("Usage: %s <ab_server_command>\n", argv[0]);
        log("Example: %s \"./build/bin_dist/ab_server\"\n", argv[0]);
        exit(1);
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_NONE);

    /* make sure the library is a usable version */
    check_library_version();

    /* kill any existing ab_server instances for cleanup */
    log("Cleaning up any existing ab_server instances...\n");
    stop_server();

    /* start the AB server first */
    log("Starting AB server for the test...\n");
    start_server(&test_state);

    /* initialize test state */
    test_state.start_time = compat_time_ms();
    test_state.end_time = test_state.start_time + TEST_DURATION_MS;
    test_state.disconnect_time = test_state.start_time + (TEST_DURATION_MS / 4); /* Disconnect after 25% of the test */
    test_state.reconnect_time = 0;                                               /* Will be set when disconnect happens */
    test_state.test_passed = 0;
    test_state.reconnect_done = 0;

    /* set up the tag and set the callback */
    setup_tag(&test_state);

    /* print status while waiting for test completion */
    run_test(&test_state);

    /* calculate the expected number of reads based on auto_sync_read_ms */
    int expected_reads = TEST_DURATION_MS / READ_POLL_MS;

    /* print results */
    log("\nTest Results:\n");
    log("Total reads started: %d, completed: %d, expected: %d\n", test_state.read_start_count, test_state.read_success_count,
        expected_reads);
    log("Read errors: %d\n", test_state.read_error_count);
    log("Successful reads after reconnect: %d\n", test_state.read_success_after_reconnect);
    log("Time from reconnect to end: %" PRId64 " ms\n", test_state.end_time - test_state.reconnect_time);

    /* manually attempt to trigger a read after the test to verify the connection works */
    log("Attempting a final manual read to verify connection state...\n");
    int manual_read_rc = plc_tag_read(test_state.tag, 0);
    if(manual_read_rc == PLCTAG_STATUS_OK) {
        log("Manual read succeeded. Tag value: %d\n", plc_tag_get_int32(test_state.tag, 0));
    } else {
        log("Manual read failed with status: %s\n", plc_tag_decode_error(manual_read_rc));
    }

    /* log the count of successful reads after reconnect */
    log("Final successful reads after reconnect: %d\n", test_state.read_success_after_reconnect);
    log("Errors during disconnection period: %d\n", test_state.errors_during_disconnect);

    /* clean up */
    plc_tag_destroy(test_state.tag);

    /* kill any remaining ab_server instances. */
    log("Cleaning up ab_server...\n");
    stop_server();

    /* determine if test passed - needs both:
     * 1. Successful reads after reconnect (showing auto_sync_read resumed)
     * 2. Evidence of disconnection (either errors or pending reads)
     */
    int pending_operations = test_state.read_start_count - test_state.read_success_count;
    int disconnect_evidence = test_state.errors_during_disconnect > 0;
    int reconnect_success = test_state.read_success_after_reconnect > 0;

    log("Disconnect evidence: %d errors, %d pending operations\n",
        test_state.errors_after_reconnect - test_state.errors_before_disconnect, pending_operations);

    test_state.test_passed = (reconnect_success && disconnect_evidence);

    if(test_state.test_passed) {
        log("TEST PASSED - Auto-sync reads successfully resumed after PLC reconnect\n");
        log("             Detected %d disconnection issues and %d successful reads after reconnect\n",
            test_state.errors_during_disconnect, test_state.read_success_after_reconnect);
    } else {
        log("TEST FAILED\n");

        if(test_state.read_success_after_reconnect == 0) {
            log("- No successful reads after PLC reconnect\n");
            log("  The auto-sync reads aren't resuming correctly\n");
        }

        if(!disconnect_evidence) {
            log("- No disconnection evidence detected\n");
            log("  The disconnection simulation may not be working correctly\n");
        }
    }

    return test_state.test_passed ? 0 : 1;
}


void check_library_version(void) {
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        log("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        log("Available library version is %d.%d.%d.\n", version_major, version_minor, version_patch);
        exit(1);
    }

    log("Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);
}


void setup_tag(test_state_t *test_state) {
    int rc = PLCTAG_STATUS_OK;

    /* create tag */
    test_state->tag = plc_tag_create(TAG_ATTRIBS, DATA_TIMEOUT);
    if(test_state->tag < 0) {
        log("Error %s creating tag!\n", plc_tag_decode_error(test_state->tag));
        exit(1);
    }

    log("Tag created with ID %d, status %s.\n", test_state->tag, plc_tag_decode_error(plc_tag_status(test_state->tag)));

    /* verify auto_sync_read_ms setting is correctly set */
    int auto_sync_read_ms = plc_tag_get_int_attribute(test_state->tag, "auto_sync_read_ms", 0);
    log("Tag auto_sync_read_ms setting: %d ms\n", auto_sync_read_ms);

    /* register the callback for tag events */
    rc = plc_tag_register_callback_ex(test_state->tag, tag_callback, test_state);
    if(rc != PLCTAG_STATUS_OK) {
        log("Error %s registering callback!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(test_state->tag);
        exit(1);
    }
}


void start_server(test_state_t *test_state) {
    char start_cmd[1024];

    snprintf(start_cmd, sizeof(start_cmd), SERVER_START, test_state->ab_server_cmd);

    if(system(start_cmd) != 0) {
        log("Error starting AB server! Make sure it's compiled.\n");
        log("Server start command line: \"%s\"\n", start_cmd);
        exit(1);
    }

    compat_sleep_ms(500, NULL);
}


void stop_server(void) {
    if(system(SERVER_STOP) < 0) {
        log("Error stopping AB server!\n");
        log("Server stop command line: \"%s\"\n", SERVER_STOP);
        exit(1);
    }

    /* wait for it to die */
    compat_sleep_ms(500, NULL);
}


void tag_callback(int32_t tag_id, int event, int status, void *data) {
    test_state_t *test_state = (test_state_t *)data;
    int tag_status = plc_tag_status(tag_id);

    switch(event) {
        case PLCTAG_EVENT_ABORTED:
            if(status == PLCTAG_ERR_TIMEOUT) {
                test_state->read_timeout_count++;

                log("[ABORTED] Tag %d w/status %s, automatic read operation timed out and aborted.\n", tag_id,
                    plc_tag_decode_error(tag_status));
            } else if(status != PLCTAG_ERR_ABORT) {
                test_state->read_error_count++;

                log("[ABORTED] Tag %d w/status %s, automatic read operation aborted with ERROR status %s.\n", tag_id,
                    plc_tag_decode_error(tag_status), plc_tag_decode_error(status));
            }
            break;

        case PLCTAG_EVENT_READ_STARTED:
            test_state->read_start_count++;

            if(status != PLCTAG_STATUS_OK && status != PLCTAG_STATUS_PENDING) {
                log("[READ START] Tag %d w/status %s, automatic read operation started with ERROR status %s (errors=%d).\n",
                    tag_id, plc_tag_decode_error(tag_status), plc_tag_decode_error(status), test_state->read_error_count);
            }
            break;

        case PLCTAG_EVENT_READ_COMPLETED:
            if(status == PLCTAG_STATUS_OK) {
                test_state->read_success_count++;

                if(test_state->reconnect_done) { test_state->read_success_after_reconnect++; }
            } else {
                test_state->read_error_count++;

                log("[READ COMPLETE] Tag %d w/status %s, automatic read operation completed with ERROR status %s (errors=%d).\n",
                    tag_id, plc_tag_decode_error(tag_status), plc_tag_decode_error(status), test_state->read_error_count);
            }
            break;

        default: break;
    }
}


void do_disconnect(int64_t current_time, test_state_t *test_state) {
    log("\n[DISCONNECT] Simulating PLC disconnect at time %" PRId64 " ms\n", current_time - test_state->start_time);

    test_state->errors_before_disconnect = test_state->read_error_count;

    log("[DISCONNECT] Read stats before disconnect: started=%d, completed=%d, errors=%d\n", test_state->read_start_count,
        test_state->read_success_count, test_state->errors_before_disconnect);

    /* kill the ab_server to truly simulate disconnect */
    stop_server();

    /* schedule reconnect time */
    test_state->reconnect_time = current_time + DISCONNECT_TIME_MS;

    log("[DISCONNECT] Disconnect phase complete, will reconnect in %d ms\n", DISCONNECT_TIME_MS);
}


void do_reconnect(int64_t current_time, test_state_t *test_state) {
    log("\n[RECONNECT] Simulating PLC reconnect at time %" PRId64 " ms\n", current_time - test_state->start_time);

    test_state->errors_after_reconnect = test_state->read_error_count;

    /* calculate "errors" during disconnection - include both actual errors and reads stuck in pending */
    int pending_reads = test_state->read_start_count - test_state->read_success_count;
    test_state->errors_during_disconnect =
        (test_state->errors_after_reconnect - test_state->errors_before_disconnect) + pending_reads;

    log("[RECONNECT] Read stats before reconnect: started=%d, completed=%d, errors=%d\n", test_state->read_start_count,
        test_state->read_success_count, test_state->errors_after_reconnect);
    log("[RECONNECT] Disconnect evidence: %d errors, %d pending reads, %d total\n",
        test_state->errors_after_reconnect - test_state->errors_before_disconnect, pending_reads,
        test_state->errors_during_disconnect);

    /* start a new ab_server to simulate reconnect */
    start_server(test_state);

    test_state->reconnect_time = current_time;
    test_state->reconnect_done = 1;

    int tag_status = plc_tag_status(test_state->tag);
    log("[RECONNECT] Tag status after reconnect: %s\n", plc_tag_decode_error(tag_status));

    log("[RECONNECT] Reconnection complete - monitoring for auto_sync_read resumption\n");
}


void run_test(test_state_t *test_state) {
    int64_t current_time = 0;
    int64_t next_pct = 0;

    log("Test running for %dms...\n", TEST_DURATION_MS);

    while((current_time = compat_time_ms()) < test_state->end_time) {
        int64_t complete_pct = (current_time - test_state->start_time) * 100 / TEST_DURATION_MS;

        /* log stats every 5% */
        if(complete_pct >= next_pct) {
            int tag_status = plc_tag_status(test_state->tag);
            int64_t elapsed_ms = current_time - test_state->start_time;

            next_pct = complete_pct + 5;

            log("\n[STATUS] Test progress: %" PRId64 "%% complete (elapsed: %" PRId64 " ms)\n", complete_pct, elapsed_ms);
            log("[STATUS] Tag status: %s\n", plc_tag_decode_error(tag_status));
            log("[STATUS] Read stats: started=%d, completed=%d, timeouts=%d, errors=%d\n", test_state->read_start_count,
                test_state->read_success_count, test_state->read_timeout_count, test_state->read_error_count);

            /* If we're past the reconnect time, print additional diagnostics */
            if(test_state->reconnect_time > 0 && elapsed_ms > test_state->reconnect_time) {
                log("[STATUS] Time since reconnect: %" PRId64 "ms, read completions after reconnect: %d\n",
                    current_time - test_state->reconnect_time, test_state->read_success_after_reconnect);
            }
        }

        /* check if it's time to simulate a PLC disconnect */
        if(test_state->disconnect_time > 0 && current_time >= test_state->disconnect_time && test_state->reconnect_time == 0) {
            do_disconnect(current_time, test_state);
        }

        /* check if it's time to simulate a PLC reconnect - only do once */
        if(test_state->reconnect_time > 0 && current_time >= test_state->reconnect_time && !test_state->reconnect_done) {
            do_reconnect(current_time, test_state);

            /* there is a data race here but we do not need to be exact. */
            test_state->reconnect_done = true;
        }

        compat_sleep_ms(READ_POLL_MS / 2, NULL);
    }

    log("[STATUS] Test completed.\n");
}
