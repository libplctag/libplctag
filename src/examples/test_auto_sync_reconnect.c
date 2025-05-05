/***************************************************************************
 *   Copyright (C) 2025 by Simon Labrecque                                 *
 *   Author Simon Labrecque  simon@wegel.ca                                *
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
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>


#define REQUIRED_VERSION 2, 6, 4

/* uses auto_sync_read_ms for automatic reads */
#define TAG_ATTRIBS \
    "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_type=DINT&elem_count=1&name=TestBigArray[0]&auto_sync_read_ms=100"

#define DATA_TIMEOUT (500)
#define DISCONNECT_TIME_MS (6000)
#define TEST_DURATION_MS (13000)
#define READ_POLL_MS (100)

static volatile int read_start_count = 0;
static volatile int read_complete_count = 0;
static volatile int read_errors = 0;
static volatile int read_success_after_reconnect = 0;

typedef struct {
    int32_t tag;
    int64_t disconnect_time;
    int64_t reconnect_time;
    int64_t start_time;
    int64_t end_time;
    int test_passed;
    int errors_before_disconnect; /* error count before disconnect */
    int errors_after_reconnect;   /* error count after reconnect */
    int errors_during_disconnect; /* calculated: errors that occurred during disconnect */
    int reconnect_done;           /* flag to indicate reconnection has been done */
    const char *ab_server_cmd;    /* command to start the AB server */
} test_state_t;

static test_state_t global_state = {.tag = 0,
                                    .disconnect_time = 0,
                                    .reconnect_time = 0,
                                    .start_time = 0,
                                    .end_time = 0,
                                    .test_passed = 0,
                                    .errors_before_disconnect = 0,
                                    .errors_after_reconnect = 0,
                                    .errors_during_disconnect = 0,
                                    .reconnect_done = 0,
                                    .ab_server_cmd = NULL};

void do_disconnect(int64_t current_time);
void do_reconnect(int64_t current_time);


void tag_callback(int32_t tag_id, int event, int status) {
    switch(event) {
        case PLCTAG_EVENT_ABORTED:
            fprintf(stderr, "Tag %d automatic operation was aborted with status %s!\n", tag_id, plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_READ_STARTED:
            read_start_count++;
            fprintf(stderr, "[READ_START] Tag %d automatic read operation started with status %s (count=%d).\n", tag_id,
                    plc_tag_decode_error(status), read_start_count);
            break;

        case PLCTAG_EVENT_READ_COMPLETED:
            read_complete_count++;

            if(status != PLCTAG_STATUS_OK) {
                read_errors++;
                fprintf(stderr, "[READ_COMPLETE] Tag %d automatic read operation completed with ERROR status %s (errors=%d).\n",
                        tag_id, plc_tag_decode_error(status), read_errors);
            } else {
                /* check if we're in the period after reconnection */
                int64_t current_time = compat_time_ms();
                if(global_state.reconnect_time > 0 && current_time > global_state.reconnect_time) {
                    read_success_after_reconnect++;
                    fprintf(
                        stderr,
                        "[READ_COMPLETE] Tag %d automatic read operation SUCCEEDED after reconnection (success_after_reconnect=%d).\n",
                        tag_id, read_success_after_reconnect);
                } else {
                    fprintf(stderr,
                            "[READ_COMPLETE] Tag %d automatic read operation completed with status OK (complete_count=%d).\n",
                            tag_id, read_complete_count);
                }
            }
            break;

        default: break;
    }
}

void *monitor_thread_func(void *arg) {
    int64_t current_time;

    fprintf(stderr, "Monitor thread started\n");

    while((current_time = compat_time_ms()) < global_state.end_time) {
        /* check if it's time to simulate a PLC disconnect */
        if(global_state.disconnect_time > 0 && current_time >= global_state.disconnect_time && global_state.reconnect_time == 0) {
            do_disconnect(current_time);
        }

        /* check if it's time to simulate a PLC reconnect - only do once */
        if(global_state.reconnect_time > 0 && current_time >= global_state.reconnect_time && !global_state.reconnect_done) {
            do_reconnect(current_time);
        }

        compat_sleep_ms(READ_POLL_MS, NULL);
    }

    fprintf(stderr, "Monitor thread exiting\n");
    return NULL;
}

void do_disconnect(int64_t current_time) {
    fprintf(stderr, "\n[DISCONNECT] Simulating PLC disconnect at time %" PRId64 " ms\n", current_time - global_state.start_time);

    global_state.errors_before_disconnect = read_errors;

    fprintf(stderr, "[DISCONNECT] Read stats before disconnect: started=%d, completed=%d, errors=%d\n", read_start_count,
            read_complete_count, global_state.errors_before_disconnect);

    /* kill the ab_server to truly simulate disconnect */
    if(system("killall ab_server") != 0) {
        fprintf(stderr, "[DISCONNECT] Warning: Unable to kill ab_server, make sure it's running\n");
    } else {
        fprintf(stderr, "[DISCONNECT] Successfully killed ab_server\n");
    }

    /* schedule reconnect time */
    global_state.reconnect_time = current_time + DISCONNECT_TIME_MS;

    fprintf(stderr, "[DISCONNECT] Disconnect phase complete, will reconnect in %d ms\n", DISCONNECT_TIME_MS);
}

void do_reconnect(int64_t current_time) {
    fprintf(stderr, "\n[RECONNECT] Simulating PLC reconnect at time %" PRId64 " ms\n", current_time - global_state.start_time);

    global_state.errors_after_reconnect = read_errors;

    /* calculate "errors" during disconnection - include both actual errors and reads stuck in pending */
    int pending_reads = read_start_count - read_complete_count;
    global_state.errors_during_disconnect =
        (global_state.errors_after_reconnect - global_state.errors_before_disconnect) + pending_reads;

    fprintf(stderr, "[RECONNECT] Read stats before reconnect: started=%d, completed=%d, errors=%d\n", read_start_count,
            read_complete_count, global_state.errors_after_reconnect);
    fprintf(stderr, "[RECONNECT] Disconnect evidence: %d errors, %d pending reads, %d total\n",
            global_state.errors_after_reconnect - global_state.errors_before_disconnect, pending_reads,
            global_state.errors_during_disconnect);

    /* start a new ab_server to simulate reconnect */
    char restart_cmd[1024];
    snprintf(restart_cmd, sizeof(restart_cmd), "%s --plc=ControlLogix --path=1,0 --tag=TestBigArray:DINT[10] > /dev/null 2>&1 &",
             global_state.ab_server_cmd);

    if(system(restart_cmd) != 0) {
        fprintf(stderr, "[RECONNECT] Warning: Unable to start new ab_server\n");
    } else {
        fprintf(stderr, "[RECONNECT] Successfully started new ab_server\n");
    }

    fprintf(stderr, "[RECONNECT] Sleeping for 200ms to allow ab_server to fully resume\n");
    compat_sleep_ms(200, NULL);

    global_state.reconnect_time = current_time;
    global_state.reconnect_done = 1;

    int tag_status = plc_tag_status(global_state.tag);
    fprintf(stderr, "[RECONNECT] Tag status after reconnect: %s\n", plc_tag_decode_error(tag_status));

    fprintf(stderr, "[RECONNECT] Reconnection complete - monitoring for auto_sync_read resumption\n");
}


int main(int argc, char **argv) {
    const char *ab_server_cmd = NULL;

    if(argc > 1) {
        ab_server_cmd = argv[1];
        global_state.ab_server_cmd = ab_server_cmd;
    } else {
        fprintf(stderr, "Usage: %s <ab_server_command>\n", argv[0]);
        fprintf(stderr, "Example: %s \"./build/bin_dist/ab_server\"\n", argv[0]);
        exit(1);
    }
    int rc = PLCTAG_STATUS_OK;
    pthread_t monitor_thread;
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);
    int reads_during_disconnect = 0;

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        fprintf(stderr, "Available library version is %d.%d.%d.\n", version_major, version_minor, version_patch);
        exit(1);
    }

    fprintf(stderr, "Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    /* kill any existing ab_server instances for cleanup */
    fprintf(stderr, "Cleaning up any existing ab_server instances...\n");
    system("killall ab_server 2>/dev/null");
    compat_sleep_ms(500, NULL);

    /* start the AB server first */
    fprintf(stderr, "Starting AB server for the test...\n");

    char start_cmd[1024];
    snprintf(start_cmd, sizeof(start_cmd), "%s --plc=ControlLogix --path=1,0 --tag=TestBigArray:DINT[10] > /dev/null 2>&1 &",
             ab_server_cmd);

    if(system(start_cmd) != 0) {
        fprintf(stderr, "Error starting AB server! Make sure it's compiled.\n");
        exit(1);
    }

    fprintf(stderr, "Waiting for AB server to initialize...\n");
    compat_sleep_ms(1000, NULL);

    /* initialize test state */
    global_state.start_time = compat_time_ms();
    global_state.end_time = global_state.start_time + TEST_DURATION_MS;
    global_state.disconnect_time = global_state.start_time + (TEST_DURATION_MS / 4); /* Disconnect after 25% of the test */
    global_state.reconnect_time = 0;                                                 /* Will be set when disconnect happens */
    global_state.test_passed = 0;
    global_state.reconnect_done = 0;

    /* create tag with auto_sync_read enabled */
    global_state.tag = plc_tag_create(TAG_ATTRIBS, DATA_TIMEOUT);
    if(global_state.tag < 0) {
        fprintf(stderr, "Error %s creating tag!\n", plc_tag_decode_error(global_state.tag));
        return 1;
    }

    fprintf(stderr, "Tag created with ID %d, status %s.\n", global_state.tag,
            plc_tag_decode_error(plc_tag_status(global_state.tag)));

    /* verify auto_sync_read_ms setting is correctly set */
    int auto_sync_read_ms = plc_tag_get_int_attribute(global_state.tag, "auto_sync_read_ms", 0);
    fprintf(stderr, "Tag auto_sync_read_ms setting: %d ms\n", auto_sync_read_ms);

    /* register the callback for tag events */
    rc = plc_tag_register_callback(global_state.tag, tag_callback);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error %s registering callback!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(global_state.tag);
        return 1;
    }

    /* start the monitoring thread */
    pthread_create(&monitor_thread, NULL, monitor_thread_func, NULL);

    /* wait for test completion */
    fprintf(stderr, "Test running for %d ms with auto_sync_read_ms=%d...\n", TEST_DURATION_MS, auto_sync_read_ms);

    /* add periodic status updates during the test */
    for(int i = 0; i < 5; i++) {
        compat_sleep_ms(TEST_DURATION_MS / 5, NULL);

        int tag_status = plc_tag_status(global_state.tag);
        int64_t elapsed_ms = compat_time_ms() - global_state.start_time;

        fprintf(stderr, "\n[STATUS] Test progress: %d%% complete (elapsed: %" PRId64 " ms)\n", (i + 1) * 20, elapsed_ms);
        fprintf(stderr, "[STATUS] Tag status: %s\n", plc_tag_decode_error(tag_status));
        fprintf(stderr, "[STATUS] Read stats: started=%d, completed=%d, errors=%d, successes_after_reconnect=%d\n",
                read_start_count, read_complete_count, read_errors, read_success_after_reconnect);

        /* If we're past the reconnect time, print additional diagnostics */
        if(global_state.reconnect_time > 0 && elapsed_ms > global_state.reconnect_time) {
            fprintf(stderr, "[STATUS] Time since reconnect: %" PRId64 " ms\n", elapsed_ms - global_state.reconnect_time);
        }
    }

    /* wait for thread to finish */
    pthread_join(monitor_thread, NULL);

    /* calculate the expected number of reads based on auto_sync_read_ms */
    int expected_reads = TEST_DURATION_MS / auto_sync_read_ms;

    /* calculate reads during disconnection */
    reads_during_disconnect = DISCONNECT_TIME_MS / auto_sync_read_ms;

    /* print results */
    fprintf(stderr, "\nTest Results:\n");
    fprintf(stderr, "Total reads started: %d, completed: %d, expected: %d\n", read_start_count, read_complete_count,
            expected_reads);
    fprintf(stderr, "Read errors: %d\n", read_errors);
    fprintf(stderr, "Successful reads after reconnect: %d\n", read_success_after_reconnect);
    fprintf(stderr, "Time from reconnect to end: %" PRId64 " ms\n", global_state.end_time - global_state.reconnect_time);

    /* manually attempt to trigger a read after the test to verify the connection works */
    fprintf(stderr, "Attempting a final manual read to verify connection state...\n");
    int manual_read_rc = plc_tag_read(global_state.tag, 0);
    if(manual_read_rc == PLCTAG_STATUS_OK) {
        fprintf(stderr, "Manual read succeeded. Tag value: %d\n", plc_tag_get_int32(global_state.tag, 0));
    } else {
        fprintf(stderr, "Manual read failed with status: %s\n", plc_tag_decode_error(manual_read_rc));
    }

    /* update the count of successful reads after reconnect */
    fprintf(stderr, "Final successful reads after reconnect: %d\n", read_success_after_reconnect);
    fprintf(stderr, "Errors during disconnection period: %d\n", global_state.errors_during_disconnect);

    /* clean up */
    plc_tag_destroy(global_state.tag);

    /* kill any remaining ab_server instances */
    fprintf(stderr, "Cleaning up ab_server instances...\n");
    system("killall ab_server 2>/dev/null");

    /* determine if test passed - needs both:
     * 1. Successful reads after reconnect (showing auto_sync_read resumed)
     * 2. Evidence of disconnection (either errors or pending reads)
     */
    int pending_operations = read_start_count - read_complete_count;
    int disconnect_evidence = global_state.errors_during_disconnect > 0;
    int reconnect_success = read_success_after_reconnect > 0;

    fprintf(stderr, "Disconnect evidence: %d errors, %d pending operations\n",
            global_state.errors_after_reconnect - global_state.errors_before_disconnect, pending_operations);

    global_state.test_passed = (reconnect_success && disconnect_evidence);

    if(global_state.test_passed) {
        fprintf(stderr, "TEST PASSED - Auto-sync reads successfully resumed after PLC reconnect\n");
        fprintf(stderr, "             Detected %d disconnection issues and %d successful reads after reconnect\n",
                global_state.errors_during_disconnect, read_success_after_reconnect);
    } else {
        fprintf(stderr, "TEST FAILED\n");

        if(read_success_after_reconnect == 0) {
            fprintf(stderr, "- No successful reads after PLC reconnect\n");
            fprintf(stderr, "  The auto-sync reads aren't resuming correctly\n");
        }

        if(!disconnect_evidence) {
            fprintf(stderr, "- No disconnection evidence detected\n");
            fprintf(stderr, "  The disconnection simulation may not be working correctly\n");
        }
    }

    return global_state.test_passed ? 0 : 1;
}
