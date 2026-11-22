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
 * Reconnect to the same PLC many times in a row.  Each cycle reads the tag, which opens a
 * connection, then lets it idle out, which closes it with Forward Close.  Every reconnect sends a
 * new Forward Open with a connection ID of its own and restarts the sequence count; a PLC that
 * treats the connection ID as already in use refuses the Forward Open, and the read fails.
 *
 *   - every read succeeds.
 *   - the connection idles out after every read, so every read made a new connection.
 *   - the connection never waits after an error.
 *
 * Usage:
 *   test_reconnect_cycles --tag=TAG --conn-tag=CONN [--cycles=N]
 * CONN is an @connection tag string for the same connection as TAG.  N defaults to 20.
 */

#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 6, 0

#define DATA_TIMEOUT (5000)
#define POLL_MS (20)
#define IDLE_TIMEOUT_MS (500)
#define IDLE_WAIT_LIMIT_MS (5000) /* well past IDLE_TIMEOUT_MS */
#define DEFAULT_CYCLES (20)


static bool wait_for_idle(int32_t conn_tag, int32_t cycle);


int main(int argc, char **argv) {
    const char *tag_str = NULL;
    const char *conn_tag_str = NULL;
    int32_t cycles = DEFAULT_CYCLES;
    int32_t tag = 0;
    int32_t conn_tag = 0;
    int32_t rc = PLCTAG_STATUS_OK;
    bool ok = true;

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    for(int32_t i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) { tag_str = &argv[i][6]; }
        if(strncmp(argv[i], "--conn-tag=", 11) == 0) { conn_tag_str = &argv[i][11]; }
        if(strncmp(argv[i], "--cycles=", 9) == 0) { cycles = (int32_t)atoi(&argv[i][9]); }
    }

    if(tag_str == NULL || conn_tag_str == NULL || cycles < 1) {
        printf("Usage: test_reconnect_cycles --tag=TAG --conn-tag=CONN [--cycles=N]\n");
        return 1;
    }

    /* the create read brings the connection up before the @connection tag joins it. */
    tag = plc_tag_create(tag_str, DATA_TIMEOUT);
    if(tag < 0) {
        printf("FAILED: creating the tag: %s\n", plc_tag_decode_error(tag));
        return 1;
    }

    conn_tag = plc_tag_create(conn_tag_str, DATA_TIMEOUT);
    if(conn_tag < 0) {
        printf("FAILED: creating the @connection tag: %s\n", plc_tag_decode_error(conn_tag));
        plc_tag_destroy(tag);
        return 1;
    }

    rc = plc_tag_set_int_attribute(tag, "connection_inactivity_timeout_ms", IDLE_TIMEOUT_MS);
    if(rc != PLCTAG_STATUS_OK) {
        printf("FAILED: setting connection_inactivity_timeout_ms: %s\n", plc_tag_decode_error(rc));
        ok = false;
    }

    /* the create read's connection idles out first, so cycle 1 is already a reconnect. */
    ok = ok && wait_for_idle(conn_tag, 0);

    for(int32_t cycle = 1; ok && cycle <= cycles; cycle++) {
        rc = plc_tag_read(tag, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            printf("FAILED: cycle %d: read: %s\n", (int)cycle, plc_tag_decode_error(rc));
            ok = false;
            break;
        }

        ok = wait_for_idle(conn_tag, cycle);
    }

    plc_tag_destroy(conn_tag);
    plc_tag_destroy(tag);

    if(!ok) { return 1; }

    printf("PASSED: %d reconnects.\n", (int)cycles);

    return 0;
}


/* poll until the connection has idled out; an error wait means a connect or the link failed. */
static bool wait_for_idle(int32_t conn_tag, int32_t cycle) {
    int64_t end_ms = compat_time_ms() + IDLE_WAIT_LIMIT_MS;

    while(compat_time_ms() < end_ms) {
        int32_t status = (int32_t)plc_tag_get_int_attribute(conn_tag, "connection_status", -1);

        if(status == PLCTAG_CONN_STATUS_IDLE_WAIT) {
            printf("Cycle %d: read and idled out.\n", (int)cycle);
            return true;
        }

        if(status == PLCTAG_CONN_STATUS_ERR_WAIT) {
            printf("FAILED: cycle %d: the connection is waiting after an error.\n", (int)cycle);
            return false;
        }

        compat_sleep_ms(POLL_MS, NULL);
    }

    printf("FAILED: cycle %d: the connection did not idle out within %dms.\n", (int)cycle, IDLE_WAIT_LIMIT_MS);

    return false;
}
