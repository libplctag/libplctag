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

#define READ_TIMEOUT (100)
#define FIRST_RUN_TIME (10000)
#define DISCONNECT_TIME_MS (60000)
#define SECOND_RUN_TIME (30000)
#define TEST_DURATION_MS (FIRST_RUN_TIME + SECOND_RUN_TIME + DISCONNECT_TIME_MS)

#define STRINGIFY(x) #x
#define TO_STRING(x) STRINGIFY(x)


/* uses auto_sync_read_ms for automatic reads */
#define AUTO_SYNC_TAG_ATTRIBS \
    "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_type=DINT&elem_count=1&name=TestBigArray[0]&auto_sync_read_ms=100"

#define MANUAL_SYNC_TAG_ATTRIBS \
    "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_type=DINT&elem_count=1&name=TestBigArray[0]"

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
    int read_timeout_ms;
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
static void setup_tag(test_state_t *test_state, const char *tag_attribs);
static void stop_server(void);
static void start_server(test_state_t *test_state);
static void do_disconnect(int64_t current_time, test_state_t *test_state);
static void do_reconnect(int64_t current_time, test_state_t *test_state);
static void tag_callback(int32_t tag_id, int event, int status, void *data);
static int run_auto_test(const char *ab_server_cmd);
static int run_manual_test(const char *ab_server_cmd);
static void wait_until_time_ms(int64_t time_ms);

int main(int argc, char **argv) {
    const char *ab_server_cmd = NULL;

    if(argc > 1) {
        ab_server_cmd = argv[1];
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

    /* print status while waiting for test completion */
    log("\n\nRunning auto-sync test...\n");
    if(run_auto_test(ab_server_cmd)) {
        log("Auto-sync test failed!\n");
        return 1;
    }

    /* print status while waiting for test completion */
    log("\n\nRunning manual-sync test...\n");
    if(run_manual_test(ab_server_cmd)) {
        log("Manual-sync test failed!\n");
        return 1;
    }

    return 0;
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


void setup_tag(test_state_t *test_state, const char *tag_attribs) {
    /* create tag */
    test_state->tag = plc_tag_create_ex(tag_attribs, tag_callback, test_state, READ_TIMEOUT);
    if(test_state->tag < 0) {
        log("Error %s creating tag!\n", plc_tag_decode_error(test_state->tag));
        exit(1);
    }

    log("Tag created with ID %d, status %s.\n", test_state->tag, plc_tag_decode_error(plc_tag_status(test_state->tag)));

    /* verify auto_sync_read_ms setting is correctly set */
    int auto_sync_read_ms = plc_tag_get_int_attribute(test_state->tag, "auto_sync_read_ms", 0);
    log("Tag auto_sync_read_ms setting: %d ms\n", auto_sync_read_ms);
}


void start_server(test_state_t *test_state) {
    char start_cmd[1024] = {0};

    snprintf(start_cmd, sizeof(start_cmd), SERVER_START, test_state->ab_server_cmd);

    if(system(start_cmd) != 0) {
        log("Error starting AB server! Make sure it's compiled.\n");
        log("Server start command line: \"%s\"\n", start_cmd);
        exit(1);
    }

    compat_sleep_ms(3000, NULL);
}


void stop_server(void) {
    if(system(SERVER_STOP) < 0) {
        log("Error stopping AB server!\n");
        log("Server stop command line: \"%s\"\n", SERVER_STOP);
        exit(1);
    }

    /* wait for it to die */
    compat_sleep_ms(1000, NULL);
}

void tag_callback(int32_t tag_id, int event, int status, void *data) {
    test_state_t *test_state = (test_state_t *)data;

    (void)tag_id;

    switch(event) {
        case PLCTAG_EVENT_ABORTED:
            if(status == PLCTAG_ERR_TIMEOUT) {
                test_state->read_timeout_count++;

                fputs("At ", stderr);
                fflush(stderr);

                // log("[ABORTED] Tag %d w/status %s, automatic read operation timed out and aborted.\n", tag_id,
                //     plc_tag_decode_error(tag_status));
            } else if(status != PLCTAG_ERR_ABORT) {
                test_state->read_error_count++;

                fputs("Ae ", stderr);
                fflush(stderr);

                // log("[ABORTED] Tag %d w/status %s, automatic read operation aborted with ERROR status %s.\n", tag_id,
                //     plc_tag_decode_error(tag_status), plc_tag_decode_error(status));
            } else {
                fputs("A ", stderr);
                fflush(stderr);
            }
            break;

        case PLCTAG_EVENT_READ_STARTED:
            test_state->read_start_count++;

            if(status != PLCTAG_STATUS_OK && status != PLCTAG_STATUS_PENDING) {
                fputs("RSe ", stderr);
                fflush(stderr);

                // log("[READ START]")
                // log("[READ START] Tag %d w/status %s, automatic read operation started with ERROR status %s (errors=%d).\n",
                //     tag_id, plc_tag_decode_error(tag_status), plc_tag_decode_error(status), test_state->read_error_count);
            } else {
                fputs("RS ", stderr);
                fflush(stderr);
            }
            break;

        case PLCTAG_EVENT_READ_COMPLETED:
            if(status == PLCTAG_STATUS_OK) {
                test_state->read_success_count++;

                if(test_state->reconnect_done) { test_state->read_success_after_reconnect++; }

                fputs("RC ", stderr);
                fflush(stderr);

                // log("[READ COMPLETE] Tag %d w/status %s)
            } else {
                test_state->read_error_count++;

                fputs("RCe ", stderr);
                fflush(stderr);

                // log("[READ COMPLETE] Tag %d w/status %s, automatic read operation completed with ERROR status %s
                // (errors=%d).\n",
                //     tag_id, plc_tag_decode_error(tag_status), plc_tag_decode_error(status), test_state->read_error_count);
            }
            break;

        default: break;
    }
}


void do_disconnect(int64_t current_time, test_state_t *test_state) {
    (void)current_time;
    // log("\n[DISCONNECT] Simulating PLC disconnect at time %" PRId64 " ms\n", current_time - test_state->start_time);

    test_state->errors_before_disconnect = test_state->read_error_count;

    // log("[DISCONNECT] Read stats before disconnect: started=%d, completed=%d, errors=%d\n", test_state->read_start_count,
    //     test_state->read_success_count, test_state->errors_before_disconnect);

    /* kill the ab_server to truly simulate disconnect */
    stop_server();

    /* schedule reconnect time */
    // test_state->reconnect_time = current_time + DISCONNECT_TIME_MS;

    // log("[DISCONNECT] Disconnect phase complete, will reconnect in %d ms\n", DISCONNECT_TIME_MS);
}


void do_reconnect(int64_t current_time, test_state_t *test_state) {
    // log("\n[RECONNECT] Simulating PLC reconnect at time %" PRId64 " ms\n", current_time - test_state->start_time);

    test_state->errors_after_reconnect = test_state->read_error_count;

    /* calculate "errors" during disconnection - include both actual errors and reads stuck in pending */
    int pending_reads = test_state->read_start_count - test_state->read_success_count;
    test_state->errors_during_disconnect =
        (test_state->errors_after_reconnect - test_state->errors_before_disconnect) + pending_reads;

    // log("[RECONNECT] Read stats before reconnect: started=%d, completed=%d, errors=%d\n", test_state->read_start_count,
    // test_state->read_success_count, test_state->errors_after_reconnect);
    // log("[RECONNECT] Disconnect evidence: %d errors, %d pending reads, %d total\n",
    // test_state->errors_after_reconnect - test_state->errors_before_disconnect, pending_reads,
    // test_state->errors_during_disconnect);

    /* start a new ab_server to simulate reconnect */
    start_server(test_state);

    test_state->reconnect_time = current_time;
    test_state->reconnect_done = 1;

    // int tag_status = plc_tag_status(test_state->tag);
    // log("[RECONNECT] Tag status after reconnect: %s\n", plc_tag_decode_error(tag_status));

    // log("[RECONNECT] Reconnection complete - monitoring for auto_sync_read resumption\n");
}


int calc_test_stats(test_state_t *test_state) {
    /* calculate the expected number of reads based on auto_sync_read_ms */
    int expected_reads = TEST_DURATION_MS / test_state->read_timeout_ms;

    /* print results */
    log("\nTest Results:\n");
    log("Total reads started: %d, completed: %d, expected: %d\n", test_state->read_start_count, test_state->read_success_count,
        expected_reads);
    log("Read errors: %d\n", test_state->read_error_count);
    log("Time from reconnect to end: %" PRId64 " ms\n", test_state->end_time - test_state->reconnect_time);
    log("Final successful reads after reconnect: %d\n", test_state->read_success_after_reconnect);
    log("Errors during disconnection period: %d\n", test_state->errors_during_disconnect);

    /*
     * determine if test passed - needs both:
     * 1. Successful reads after reconnect (showing auto_sync_read resumed)
     * 2. Evidence of disconnection (either errors or pending reads)
     */
    int pending_operations = test_state->read_start_count - test_state->read_success_count;
    int disconnect_evidence = test_state->errors_during_disconnect > 0;
    int reconnect_success = test_state->read_success_after_reconnect > 0;

    log("Disconnect evidence: %d errors, %d pending operations\n",
        test_state->errors_after_reconnect - test_state->errors_before_disconnect, pending_operations);

    test_state->test_passed = (reconnect_success && disconnect_evidence);

    if(test_state->test_passed) {
        log("TEST PASSED - reads successfully resumed after PLC reconnect\n");
        log("             Detected %d disconnection issues and %d successful reads after reconnect\n",
            test_state->errors_during_disconnect, test_state->read_success_after_reconnect);
    } else {
        log("TEST FAILED\n");

        if(test_state->read_success_after_reconnect == 0) {
            log("- No successful reads after PLC reconnect\n");
            log("  The reads aren't resuming correctly\n");
        }

        if(!disconnect_evidence) {
            log("- No disconnection evidence detected\n");
            log("  The disconnection simulation may not be working correctly\n");
        }
    }

    return test_state->test_passed ? 0 : 1;
}


int run_auto_test(const char *ab_server_cmd) {
    int64_t current_time = 0;
    test_state_t auto_test_state = {0};

    log("Auto tag test running for %dms...\n", TEST_DURATION_MS);

    /* initialize test state */
    auto_test_state.ab_server_cmd = ab_server_cmd;
    auto_test_state.start_time = compat_time_ms();
    auto_test_state.end_time = auto_test_state.start_time + FIRST_RUN_TIME + DISCONNECT_TIME_MS + SECOND_RUN_TIME;
    auto_test_state.disconnect_time = auto_test_state.start_time + FIRST_RUN_TIME;
    auto_test_state.read_timeout_ms = READ_TIMEOUT;
    auto_test_state.reconnect_time = auto_test_state.start_time + FIRST_RUN_TIME + DISCONNECT_TIME_MS;
    auto_test_state.test_passed = 0;
    auto_test_state.reconnect_done = 0;

    log("[INFO] Test starting at time %" PRId64 " ms\n", auto_test_state.start_time);
    log("[INFO] Disconnect time %" PRId64 " ms\n", auto_test_state.disconnect_time);
    log("[INFO] Reconnect time %" PRId64 " ms\n", auto_test_state.reconnect_time);
    log("[INFO] Test ending at time %" PRId64 " ms\n", auto_test_state.end_time);

    start_server(&auto_test_state);

    /* set up the tag and set the callback */
    setup_tag(&auto_test_state, AUTO_SYNC_TAG_ATTRIBS);


    while((current_time = compat_time_ms()) < auto_test_state.disconnect_time) { compat_sleep_ms(READ_TIMEOUT, NULL); }

    int before_disconnect_read_success_count = auto_test_state.read_success_count;
    int read_error_before_disconnect = auto_test_state.read_error_count;


    do_disconnect(current_time, &auto_test_state);

    fputs("\nD\n", stderr);
    fflush(stderr);

    while((current_time = compat_time_ms()) < auto_test_state.reconnect_time) {
        compat_sleep_ms(1000, NULL); 
        fputs(".", stderr); fflush(stderr);
    }

    int after_disconnect_read_success_count = auto_test_state.read_success_count - before_disconnect_read_success_count;
    int read_error_after_disconnect = auto_test_state.read_error_count - read_error_before_disconnect;

    do_reconnect(current_time, &auto_test_state);

    fputs("\nR\n", stderr);
    fflush(stderr);

    auto_test_state.reconnect_done = 1;

    while(compat_time_ms() < auto_test_state.end_time) { compat_sleep_ms(READ_TIMEOUT, NULL); }

    int after_reconnect_read_success_count =
        auto_test_state.read_success_count - (before_disconnect_read_success_count + after_disconnect_read_success_count);
    int read_error_after_reconnect =
        auto_test_state.read_error_count - (read_error_before_disconnect + read_error_after_disconnect);


    log("\n[STATUS] Test completed.\n");

    auto_test_state.errors_before_disconnect = read_error_before_disconnect;
    auto_test_state.errors_after_reconnect = read_error_after_reconnect;
    auto_test_state.read_success_after_reconnect = after_reconnect_read_success_count;

    log("[STATUS] Test completed.\n");

    plc_tag_destroy(auto_test_state.tag);

    stop_server();

    return calc_test_stats(&auto_test_state);
}


int run_manual_test(const char *ab_server_cmd) {
    int64_t current_time = 0;
    int64_t wait_until_ms = 0;
    test_state_t manual_test_state = {0};

    log("Manual tag test running for %dms...\n", TEST_DURATION_MS);

    /* initialize test state */
    manual_test_state.ab_server_cmd = ab_server_cmd;
    manual_test_state.start_time = compat_time_ms();
    manual_test_state.end_time = manual_test_state.start_time + FIRST_RUN_TIME + DISCONNECT_TIME_MS + SECOND_RUN_TIME;
    manual_test_state.disconnect_time = manual_test_state.start_time + FIRST_RUN_TIME;
    manual_test_state.read_timeout_ms = READ_TIMEOUT;
    manual_test_state.reconnect_time = manual_test_state.start_time + FIRST_RUN_TIME + DISCONNECT_TIME_MS;
    manual_test_state.test_passed = 0;
    manual_test_state.reconnect_done = 0;

    log("[INFO] Test starting at time %" PRId64 " ms\n", manual_test_state.start_time);
    log("[INFO] Disconnect time in %" PRId64 " ms\n", manual_test_state.disconnect_time - manual_test_state.start_time);
    log("[INFO] Reconnect time in %" PRId64 " ms\n", manual_test_state.reconnect_time - manual_test_state.start_time);
    log("[INFO] Test ending at in time %" PRId64 " ms\n", manual_test_state.end_time - manual_test_state.start_time);

    start_server(&manual_test_state);

    /* set up the tag and set the callback */
    setup_tag(&manual_test_state, MANUAL_SYNC_TAG_ATTRIBS);

    while((current_time = compat_time_ms()) < manual_test_state.disconnect_time) {
        if(wait_until_ms < current_time) { wait_until_ms = current_time + READ_TIMEOUT; }

        plc_tag_read(manual_test_state.tag, READ_TIMEOUT);

        /* wait at least the timeout time */
        wait_until_time_ms(wait_until_ms);
    }

    int before_disconnect_read_success_count = manual_test_state.read_success_count;
    int read_error_before_disconnect = manual_test_state.read_error_count;


    do_disconnect(current_time, &manual_test_state);

    fputs("\nD\n", stderr);
    fflush(stderr);

    while((current_time = compat_time_ms()) < manual_test_state.reconnect_time) {
        if(wait_until_ms < current_time) { wait_until_ms = current_time + READ_TIMEOUT; }

        plc_tag_read(manual_test_state.tag, READ_TIMEOUT);

        /* wait at least the timeout time */
        wait_until_time_ms(wait_until_ms);
    }

    int after_disconnect_read_success_count = manual_test_state.read_success_count - before_disconnect_read_success_count;
    int read_error_after_disconnect = manual_test_state.read_error_count - read_error_before_disconnect;

    do_reconnect(current_time, &manual_test_state);

    fputs("\nR\n", stderr);
    fflush(stderr);

    manual_test_state.reconnect_done = 1;

    while((current_time = compat_time_ms()) < manual_test_state.end_time) {
        if(wait_until_ms < current_time) { wait_until_ms = current_time + READ_TIMEOUT; }

        plc_tag_read(manual_test_state.tag, READ_TIMEOUT);

        /* wait at least the timeout time */
        wait_until_time_ms(wait_until_ms);
    }

    int after_reconnect_read_success_count =
        manual_test_state.read_success_count - (before_disconnect_read_success_count + after_disconnect_read_success_count);
    int read_error_after_reconnect =
        manual_test_state.read_error_count - (read_error_before_disconnect + read_error_after_disconnect);


    log("\n[STATUS] Test completed.\n");

    manual_test_state.errors_before_disconnect = read_error_before_disconnect;
    manual_test_state.errors_after_reconnect = read_error_after_reconnect;
    manual_test_state.read_success_after_reconnect = after_reconnect_read_success_count;

    plc_tag_destroy(manual_test_state.tag);

    stop_server();

    return calc_test_stats(&manual_test_state);
}


void wait_until_time_ms(int64_t time_ms) {
    int64_t current_time = compat_time_ms();
    int64_t remaining_time = time_ms - current_time;

    if(remaining_time <= 0) { return; }

    compat_sleep_ms((uint32_t)(uint64_t)remaining_time, NULL);
}
