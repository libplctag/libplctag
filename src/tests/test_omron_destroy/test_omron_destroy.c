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
 * Regression test for GitHub issue #625:
 *   plc_tag_destroy hangs when called after an Omron connection is lost.
 *
 * Sequence:
 *   1. Start ab_server in Omron mode.
 *   2. Create an Omron tag and do a successful read to confirm the connection is up.
 *   3. Kill the server to simulate a cable/CPU disconnect.
 *   4. Call plc_tag_read() with a timeout — it returns PLCTAG_ERR_TIMEOUT,
 *      exactly as the user's app did.
 *   5. Immediately call plc_tag_destroy from a thread watched by a timeout.
 *   6. If destroy does not return within DESTROY_TIMEOUT_MS the test fails.
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef WINDOWS_PLATFORM
#    include <process.h>
#    define compat_getpid _getpid
#else
#    include <unistd.h>
#    define compat_getpid getpid
#endif

#define REQUIRED_VERSION 2, 7, 0

/* Timeout passed to plc_tag_read() after the server is killed — mirrors the
 * synchronous read call the user made that returned PLCTAG_ERR_TIMEOUT. */
#define READ_TIMEOUT_MS 5000

/* Maximum time plc_tag_destroy is allowed to take after the timed-out read.
 * If it has not returned by this deadline the bug is present. */
#define DESTROY_TIMEOUT_MS 10000

/* Dedicated port for this test's private server so it doesn't collide with
 * ab_server's default port (44818), which other sections of the parallel
 * test suite already occupy. */
#define STRINGIFY_(x) #x
#define STRINGIFY(x)  STRINGIFY_(x)
#define SERVER_PORT   44900

#define TAG_ATTRIBS \
    "protocol=ab-eip&gateway=127.0.0.1:" STRINGIFY(SERVER_PORT) "&path=18,127.0.0.1&plc=omron-njnx" \
    "&elem_count=1&name=TestDINTArray[0]"

/* Capture the backgrounded server's PID into a pidfile keyed on this test
 * process's own PID, so stop_server() kills exactly this instance --
 * pkill/taskkill by image name would also kill any other ab_server instance
 * running concurrently elsewhere in the test suite (see issue found by the
 * parallel test runner: it collaterally SIGTERM'd every ab_server on the
 * host). */
#ifdef WINDOWS_PLATFORM
#    define SERVER_START \
        "powershell -NoProfile -Command \"(Start-Process -PassThru -WindowStyle Hidden '%s' -ArgumentList " \
        "'--plc=Omron','--port=" STRINGIFY(SERVER_PORT) "','--tag=TestDINTArray:DINT[10]').Id\" > ab_server_%d.pid"
#    define SERVER_STOP "for /f %%p in (ab_server_%d.pid) do taskkill /PID %%p /F >nul 2>&1 & del /f ab_server_%d.pid >nul 2>&1"
#else
#    define SERVER_START \
        "%s --plc=Omron --port=" STRINGIFY( \
            SERVER_PORT) " --tag=TestDINTArray:DINT[10] > /tmp/omron_destroy_test_server.log 2>&1 & echo $! > /tmp/ab_server_%d.pid"
#    define SERVER_STOP "kill -TERM $(cat /tmp/ab_server_%d.pid 2>/dev/null) 2>/dev/null; rm -f /tmp/ab_server_%d.pid"
#endif

#define log(...)                          \
    compat_fprintf(stderr, __VA_ARGS__);  \
    fflush(stderr)

/* State shared between main and the destroy thread. */
typedef struct {
    int32_t tag;
    compat_mutex_t mutex;
    compat_cond_t cond;
    volatile int done;
} destroy_state_t;


static void start_server(const char *ab_server_path, int test_pid) {
    char cmd[1024] = {0};
    snprintf(cmd, sizeof(cmd), SERVER_START, ab_server_path, test_pid);
    if(system(cmd) != 0) {
        log("Failed to start ab_server.\n");
        exit(1);
    }
    compat_sleep_ms(2000, NULL);
}


static void stop_server(int test_pid) {
    char cmd[512] = {0};
    snprintf(cmd, sizeof(cmd), SERVER_STOP, test_pid, test_pid);
    system(cmd);
    compat_sleep_ms(500, NULL);
}


static void *destroy_thread_func(void *arg) {
    destroy_state_t *state = (destroy_state_t *)arg;

    plc_tag_destroy(state->tag);

    compat_mutex_lock(&state->mutex);
    state->done = 1;
    compat_cond_signal(&state->cond);
    compat_mutex_unlock(&state->mutex);

    return NULL;
}


int main(int argc, char **argv) {
    if(argc < 2) {
        log("Usage: %s <path/to/ab_server>\n", argv[0]);
        return 1;
    }

    const char *ab_server_path = argv[1];

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        log("Library version %d.%d.%d or later required.\n", REQUIRED_VERSION);
        return 1;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);

    int test_pid = (int)compat_getpid();

    log("Starting Omron simulator...\n");
    start_server(ab_server_path, test_pid);

    log("Creating Omron tag...\n");
    int32_t tag = plc_tag_create(TAG_ATTRIBS, 5000);
    if(tag < 0) {
        log("FAIL: could not create tag: %s\n", plc_tag_decode_error(tag));
        stop_server(test_pid);
        return 1;
    }

    log("Performing initial read to confirm connection...\n");
    int rc = plc_tag_read(tag, READ_TIMEOUT_MS);
    if(rc != PLCTAG_STATUS_OK) {
        log("FAIL: initial read failed: %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        stop_server(test_pid);
        return 1;
    }
    log("Initial read OK.\n");

    log("Killing server to simulate connection loss...\n");
    stop_server(test_pid);

    /* Reproduce the user's exact sequence: a synchronous read that times out
     * because the connection is gone.  plc_tag_destroy is then called
     * immediately after that timeout returns — no additional waiting. */
    log("Calling plc_tag_read with %d ms timeout (expect PLCTAG_ERR_TIMEOUT)...\n", READ_TIMEOUT_MS);
    rc = plc_tag_read(tag, READ_TIMEOUT_MS);
    if(rc != PLCTAG_ERR_TIMEOUT) {
        log("NOTE: plc_tag_read returned %s (expected PLCTAG_ERR_TIMEOUT).\n", plc_tag_decode_error(rc));
    }

    /* Run plc_tag_destroy in a thread so we can apply a timeout. */
    destroy_state_t state = {0};
    state.tag = tag;
    compat_mutex_init(&state.mutex);
    compat_cond_init(&state.cond);

    compat_thread_t thread;
    compat_thread_create(&thread, destroy_thread_func, &state);

    log("Calling plc_tag_destroy (timeout %d ms)...\n", DESTROY_TIMEOUT_MS);

    compat_mutex_lock(&state.mutex);
    int timed_out = 0;
    if(!state.done) {
        int wait_rc = compat_cond_timedwait(&state.cond, &state.mutex, DESTROY_TIMEOUT_MS);
        timed_out = (wait_rc != 0 && !state.done);
    }
    compat_mutex_unlock(&state.mutex);

    if(timed_out) {
        log("FAIL: plc_tag_destroy hung after %d ms (issue #625 regression).\n", DESTROY_TIMEOUT_MS);
        /* Do not join the hung thread — just exit with failure so the test
         * script can move on.  The OS will clean up the process resources. */
        return 1;
    }

    compat_thread_join(thread, NULL);
    compat_mutex_destroy(&state.mutex);
    compat_cond_destroy(&state.cond);

    log("PASS: plc_tag_destroy returned cleanly after connection loss.\n");
    return 0;
}
