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
 * What an Omron client does with a CIP partial-transfer status.
 *
 * Omron does not use CIP's 0x06 to report a transfer it could not finish -- it answers with an
 * error -- and it implements no fragmented read service, so there is no request that could ask
 * for the rest of one.  The client therefore treats any status but zero as an error.
 *
 * It used to accept 0x06 and try to continue, which it cannot do: omron_tag_abort_request()
 * zeroes tag->offset before the continuation runs, so the client re-read the same bytes into
 * the same place forever.  A PLC answering every read that way -- a hostile or MITM'd one,
 * since CIP has no authentication -- drove tens of thousands of round trips per second and a
 * pinned core until the caller's timeout, with the tag never completing or failing.
 *
 * The cases are the Allen-Bradley partial-transfer tests in the simulator suite, read back
 * against Omron, where the answer is refusal rather than recovery:
 *
 *   - a partial status carrying data, injected without end.  AB follows these; Omron cannot.
 *   - a partial status carrying no data, injected without end.  AB treats a few of these as
 *     packing starvation and retries, then gives up once they stop making progress.  Omron used
 *     to do the same, so this is the case that says the retry path is gone rather than narrowed.
 *   - a read genuinely too large for one packet, with nothing injected at all.  This is the
 *     ordinary way AB gets a partial status, and the one shape here a correct server produces
 *     on its own.  An Omron client has no way to ask for the rest, so it must refuse rather
 *     than return a short buffer as if it were the whole tag.
 *
 * The server is started and stopped per case on the one port the runner allocates, because
 * --empty_frag and --corrupt count down across the whole process.
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
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

/*
 * Generous, because it is not the thing being measured: every case here must reach a verdict
 * long before this expires.  The bug this test exists for presents precisely as a tag that
 * never reaches one, so a timeout is a failure however long the timeout is.
 */
#define TAG_TIMEOUT_MS (15000)

/*
 * More fragments than any client that refuses the first one will ask for.  The old code
 * consumed about 65000 data-carrying ones in under five seconds, so a client that still loops
 * will not run these out and will fail on the timeout instead.
 */
#define ENDLESS_FRAGMENTS (1000000)

/* Arms nothing.  An empty string would reach Windows as an empty argument; this does not. */
#define NO_FAULT "--empty_frag=0"

/* CIP 0x06 decodes to this.  Every case below is the same status, so all of them report it. */
#define EXPECTED_ERROR (PLCTAG_ERR_TOO_LARGE)

/* only for running this by hand; the parallel runner allocates the port. */
#define DEFAULT_SERVER_PORT (44901)

#define TAG_ATTRIBS_FMT \
    "protocol=ab-eip&gateway=127.0.0.1:%d&path=18,127.0.0.1&plc=omron-njnx" \
    "&elem_count=%d&name=%s"

/* small enough to answer in one packet, so the injected faults are the only thing at fault. */
#define SMALL_TAG "TestDINTArray"
#define SMALL_TAG_ELEMENTS (10)

/*
 * 1000 DINTs is 4000 bytes, well past what one packet carries, so the server has to report a
 * partial transfer without being told to.  Same tag and count the AB tests use.
 */
#define BIG_TAG "TestBigArray"
#define BIG_TAG_ELEMENTS (1000)

/*
 * The pidfile is keyed on this test process's own PID so that stopping the server kills exactly
 * this instance.  Killing by image name would take out every other ab_server the parallel
 * runner has running at the same time.
 */
#ifdef WINDOWS_PLATFORM
#    define SERVER_START \
        "powershell -NoProfile -Command \"(Start-Process -PassThru -WindowStyle Hidden '%s' -ArgumentList " \
        "'--plc=Omron','--port=%d','--tag=TestDINTArray:DINT[10]','--tag=TestBigArray:DINT[2000]','%s').Id\"" \
        " > ab_server_frag_%d.pid"
#    define SERVER_STOP \
        "for /f %%p in (ab_server_frag_%d.pid) do taskkill /PID %%p /F >nul 2>&1 & del /f ab_server_frag_%d.pid >nul 2>&1"
#else
#    define SERVER_START \
        "%s --plc=Omron --port=%d --tag=TestDINTArray:DINT[10] --tag=TestBigArray:DINT[2000] %s > " \
        "/tmp/omron_frag_test_server_%d.log 2>&1 & echo $! > " \
        "/tmp/ab_server_frag_%d.pid"
#    define SERVER_STOP "kill -TERM $(cat /tmp/ab_server_frag_%d.pid 2>/dev/null) 2>/dev/null; rm -f /tmp/ab_server_frag_%d.pid"
#endif

#define log(...)                         \
    compat_fprintf(stderr, __VA_ARGS__); \
    fflush(stderr)


static void stop_server(int32_t test_pid) {
    char cmd[512] = {0};

    snprintf(cmd, sizeof(cmd), SERVER_STOP, (int)test_pid, (int)test_pid);
    system(cmd);

    compat_sleep_ms(500, NULL);
}


/*
 * Start ab_server with one fault armed and wait for it to listen.
 *
 * The retry mirrors test_omron_destroy: this test backgrounds the server itself through an
 * extra shell hop and gets none of the harness's retry-on-vanish help, and a loaded CI machine
 * can push that hop past a one-shot wait.  A real failure reproduces on the second attempt too.
 */
static bool start_server(const char *ab_server_path, int32_t test_pid, int32_t server_port, const char *fault_arg) {
    char cmd[1024] = {0};

    snprintf(cmd, sizeof(cmd), SERVER_START, ab_server_path, (int)server_port, fault_arg, (int)test_pid, (int)test_pid);

    for(int32_t attempt = 0; attempt < 2; attempt++) {
        if(system(cmd) != 0) {
            log("Failed to start ab_server.\n");
            return false;
        }

        if(compat_wait_for_listener("127.0.0.1", (uint16_t)server_port, 30000)) { return true; }

        log("Warning: ab_server did not start listening on port %d in time%s.\n", (int)server_port,
            attempt == 0 ? ", retrying" : "");

        stop_server(test_pid);
    }

    log("Error: ab_server did not start listening on port %d in time!\n", (int)server_port);

    return false;
}


/*
 * Create a tag against a server armed with one fault and report what the read came back with.
 *
 * The creation does the first read, so its return code is the read's.  A tag that survives is
 * destroyed; a negative return is the answer on its own.
 */
static int32_t read_against_fault(const char *ab_server_path, int32_t test_pid, int32_t server_port, const char *fault_arg,
                                  const char *tag_name, int32_t elem_count) {
    char tag_attribs[256] = {0};
    int32_t tag = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    if(!start_server(ab_server_path, test_pid, server_port, fault_arg)) { return PLCTAG_ERR_NO_RESOURCES; }

    snprintf(tag_attribs, sizeof(tag_attribs), TAG_ATTRIBS_FMT, (int)server_port, (int)elem_count, tag_name);

    tag = plc_tag_create(tag_attribs, TAG_TIMEOUT_MS);

    if(tag < 0) {
        rc = tag;
    } else {
        rc = plc_tag_status(tag);
        plc_tag_destroy(tag);
    }

    stop_server(test_pid);

    return rc;
}


/* returns the number of failures, so main() can run every case and report all of them. */
static int32_t check(const char *what, int32_t rc, int32_t expected) {
    if(rc == expected) {
        log("PASS: %s -- %s.\n", what, plc_tag_decode_error(rc));
        return 0;
    }

    log("FAIL: %s -- got %s, expected %s.\n", what, plc_tag_decode_error(rc), plc_tag_decode_error(expected));

    return 1;
}


int main(int argc, char **argv) {
    const char *ab_server_path = NULL;
    int32_t server_port = DEFAULT_SERVER_PORT;
    int32_t test_pid = 0;
    int32_t failures = 0;
    char fault_arg[64] = {0};

    if(argc < 2) {
        log("Usage: %s <path/to/ab_server> [port]\n", argv[0]);
        return 1;
    }

    ab_server_path = argv[1];

    if(argc >= 3) {
        server_port = (int32_t)atoi(argv[2]);

        if(server_port <= 0 || server_port > 65535) {
            log("Invalid port \"%s\"!\n", argv[2]);
            return 1;
        }
    }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        log("Library version %d.%d.%d or later required.\n", REQUIRED_VERSION);
        return 1;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);

    test_pid = (int32_t)compat_getpid();

    /* a partial status carrying data.  This is the one that used to spin until the timeout. */
    snprintf(fault_arg, sizeof(fault_arg), "--corrupt=endless_frag:%d", ENDLESS_FRAGMENTS);
    failures += check("a partial status carrying data is refused",
                      read_against_fault(ab_server_path, test_pid, server_port, fault_arg, SMALL_TAG, SMALL_TAG_ELEMENTS),
                      EXPECTED_ERROR);

    /* a partial status carrying nothing.  This one used to be retried. */
    snprintf(fault_arg, sizeof(fault_arg), "--empty_frag=%d", ENDLESS_FRAGMENTS);
    failures += check("a partial status carrying no data is refused",
                      read_against_fault(ab_server_path, test_pid, server_port, fault_arg, SMALL_TAG, SMALL_TAG_ELEMENTS),
                      EXPECTED_ERROR);

    /*
     * No fault at all: the read simply does not fit in one packet.  A short buffer returned as
     * if it were the whole tag would be the worst outcome here, so the refusal matters even
     * though nothing is being attacked.
     */
    failures += check("a read too large for one packet is refused",
                      read_against_fault(ab_server_path, test_pid, server_port, NO_FAULT, BIG_TAG, BIG_TAG_ELEMENTS),
                      EXPECTED_ERROR);

    /*
     * The control for the case above.  The same tag, read in a size that fits, has to come back
     * clean -- otherwise that refusal would pass just as well for a tag the server never had.
     */
    failures += check("the same tag reads cleanly when it fits",
                      read_against_fault(ab_server_path, test_pid, server_port, NO_FAULT, BIG_TAG, SMALL_TAG_ELEMENTS),
                      PLCTAG_STATUS_OK);

    plc_tag_shutdown();

    if(failures > 0) {
        log("%d of 4 cases failed.\n", (int)failures);
        return 1;
    }

    log("All 4 cases passed.\n");

    return 0;
}
