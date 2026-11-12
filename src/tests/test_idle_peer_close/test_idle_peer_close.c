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
 * A PLC that closes an idle connection is noticed at once, not at the next request.
 *
 * One read brings the session UP.  With no tag due and a long inactivity timeout, nothing
 * would be sent for a minute, so the only way connection_status can leave UP is the handler
 * seeing the socket close while it waits.  The test kills ab_server and requires the status to
 * leave UP within PEER_CLOSE_LIMIT_MS.
 *
 * Usage: test_idle_peer_close <ab_server path> <port>
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>


#define SETUP_TIMEOUT_MS (15000)
#define IDLE_BEFORE_KILL_MS (500)
#define PEER_CLOSE_LIMIT_MS (2000)

#define CONN_ATTRIBS_FMT \
    "protocol=ab-eip&gateway=127.0.0.1:%s&path=1,0&plc=ControlLogix&connection_inactivity_timeout_ms=60000&name=%s"

#ifdef WINDOWS_PLATFORM
/* a pidfile per port, as on POSIX, so only this test's server is killed.  The log name is unused. */
#    define SERVER_START \
        "powershell -NoProfile -Command \"(Start-Process -PassThru -WindowStyle Hidden '%s' -ArgumentList " \
        "'--plc=ControlLogix','--path=1,0','--port=%s','--tag=TestBigArray:DINT[10]').Id\" > ab_server_%.0s%s.pid"
#    define SERVER_STOP "for /f %%p in (ab_server_%s.pid) do taskkill /PID %%p /F >nul 2>&1 & del /f ab_server_%s.pid >nul 2>&1"
#else
/* a port-specific pidfile, so only this test's server is killed. */
#    define SERVER_START \
        "%s --plc=ControlLogix --path=1,0 --port=%s --tag=TestBigArray:DINT[10] > ab_server_%s.log 2>&1 & echo $! > ab_server_%s.pid"
#    define SERVER_STOP "kill -TERM $(cat ab_server_%s.pid 2>/dev/null) 2>/dev/null; rm -f ab_server_%s.pid"
#endif


static const char *port = NULL;

static void start_server(const char *ab_server_cmd);
static void stop_server(void);
static int32_t wait_for_status(int32_t tag, int32_t want, int timeout_ms);


int main(int argc, char **argv) {
    char attribs[512];
    int32_t conn_tag = 0;
    int32_t data_tag = 0;
    int32_t rc = PLCTAG_STATUS_OK;
    int64_t kill_time = 0;
    int64_t elapsed = 0;
    int32_t status = 0;

    if(argc != 3) {
        fprintf(stderr, "Usage: %s <ab_server path> <port>\n", argv[0]);
        return 1;
    }

    port = argv[2];

    stop_server();
    start_server(argv[1]);

    snprintf(attribs, sizeof(attribs), CONN_ATTRIBS_FMT, port, "@connection");
    conn_tag = plc_tag_create(attribs, SETUP_TIMEOUT_MS);
    if(conn_tag < 0) {
        fprintf(stderr, "FAIL: could not create the @connection tag, %s.\n", plc_tag_decode_error(conn_tag));
        stop_server();
        return 1;
    }

    snprintf(attribs, sizeof(attribs), CONN_ATTRIBS_FMT "&elem_type=DINT&elem_count=1", port, "TestBigArray[0]");
    data_tag = plc_tag_create(attribs, SETUP_TIMEOUT_MS);
    if(data_tag < 0) {
        fprintf(stderr, "FAIL: could not create the data tag, %s.\n", plc_tag_decode_error(data_tag));
        plc_tag_destroy(conn_tag);
        stop_server();
        return 1;
    }

    rc = plc_tag_read(data_tag, SETUP_TIMEOUT_MS);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "FAIL: the first read failed, %s.\n", plc_tag_decode_error(rc));
        rc = 1;
    } else if((status = wait_for_status(conn_tag, PLCTAG_CONN_STATUS_UP, SETUP_TIMEOUT_MS)) != PLCTAG_CONN_STATUS_UP) {
        fprintf(stderr, "FAIL: the connection never came UP (status %d).\n", (int)status);
        rc = 1;
    } else {
        /* idle: nothing due, nothing in flight. */
        compat_sleep_ms(IDLE_BEFORE_KILL_MS, NULL);

        kill_time = compat_time_ms();
        stop_server();

        /* anything but UP: the handler saw the close and went down. */
        status = PLCTAG_CONN_STATUS_UP;
        while(status == PLCTAG_CONN_STATUS_UP && compat_time_ms() - kill_time < PEER_CLOSE_LIMIT_MS) {
            compat_sleep_ms(5, NULL);
            status = plc_tag_get_int_attribute(conn_tag, "connection_status", PLCTAG_CONN_STATUS_UP);
        }

        elapsed = compat_time_ms() - kill_time;

        if(status == PLCTAG_CONN_STATUS_UP) {
            fprintf(stderr, "FAIL: connection still UP %" PRId64 "ms after the server was killed.\n", elapsed);
            rc = 1;
        } else {
            fprintf(stderr, "PASS: connection left UP (status %d) %" PRId64 "ms after the server was killed.\n",
                    (int)status, elapsed);
            rc = 0;
        }
    }

    plc_tag_destroy(data_tag);
    plc_tag_destroy(conn_tag);
    stop_server();

    return (int)rc;
}


static void start_server(const char *ab_server_cmd) {
    char cmd[1024];

    snprintf(cmd, sizeof(cmd), SERVER_START, ab_server_cmd, port, port, port);

    if(system(cmd) != 0) {
        fprintf(stderr, "FAIL: could not start ab_server: \"%s\"\n", cmd);
        exit(1);
    }

    if(!compat_wait_for_listener("127.0.0.1", (uint16_t)atoi(port), SETUP_TIMEOUT_MS)) {
        fprintf(stderr, "FAIL: ab_server is not listening on port %s.\n", port);
        exit(1);
    }
}


static void stop_server(void) {
    char cmd[256];

    snprintf(cmd, sizeof(cmd), SERVER_STOP, port, port);

    /* the server may already be gone; only a shell failure matters. */
    if(system(cmd) < 0) { fprintf(stderr, "Could not run \"%s\".\n", cmd); }
}


static int32_t wait_for_status(int32_t tag, int32_t want, int timeout_ms) {
    int64_t end_time = compat_time_ms() + timeout_ms;
    int32_t status = plc_tag_get_int_attribute(tag, "connection_status", -1);

    while(status != want && compat_time_ms() < end_time) {
        compat_sleep_ms(10, NULL);
        status = plc_tag_get_int_attribute(tag, "connection_status", -1);
    }

    return status;
}
