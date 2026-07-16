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
 * server_tag_basic — end-to-end verification for role=server tags
 * (SERVER_TAGS.md / the P3+P4 work in REFACTOR_PLAN.md). Exercises, in one
 * process, entirely through the ordinary plc_tag_* API (no device_sim_*):
 *
 *   1. basic round trip: a server tag seeded locally, read back by a real
 *      client tag over loopback.
 *   2. multiple server tags sharing one endpoint (same gateway:port).
 *   3. callback direction: a client write to the server tag fires
 *      PLCTAG_EVENT_WRITE_COMPLETED on the server tag's own callback.
 *   4. destroy-while-serving: destroy one server tag while a sibling on the
 *      same endpoint keeps answering requests.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "libplctag.h"
#include "platform.h"

#define TEST_PORT 44820
#define TIMEOUT_MS 2000

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                                     \
    do {                                                                                                                     \
        if(!(cond)) {                                                                                                        \
            printf("FAIL: %s\n", msg);                                                                                       \
            g_failures++;                                                                                                    \
        } else {                                                                                                             \
            printf("OK:   %s\n", msg);                                                                                       \
        }                                                                                                                    \
    } while(0)

static volatile int g_write_completed = 0;

static void on_server_event(int32_t tag_id, int event, int status) {
    (void)tag_id;
    (void)status;
    if(event == PLCTAG_EVENT_WRITE_COMPLETED) { g_write_completed = 1; }
}

int main(void) {
    char attr[300];

    printf("=== Test 1: basic round trip ===\n");

    snprintf(attr, sizeof(attr),
             "protocol=ab-eip&role=server&gateway=0.0.0.0:%d&name=TestDINT&elem_type=DINT&elem_count=1", TEST_PORT);
    int32_t srv1 = plc_tag_create(attr, TIMEOUT_MS);
    CHECK(srv1 >= 0, "create server tag 1");

    CHECK(plc_tag_set_int32(srv1, 0, 1234567) == PLCTAG_STATUS_OK, "seed server tag 1 locally");
    CHECK(plc_tag_write(srv1, TIMEOUT_MS) == PLCTAG_STATUS_OK, "push seeded value to the served buffer");

    snprintf(attr, sizeof(attr), "protocol=ab-eip&gateway=127.0.0.1:%d&path=1,0&plc=ControlLogix&elem_count=1&name=TestDINT",
             TEST_PORT);
    int32_t cli1 = plc_tag_create(attr, TIMEOUT_MS);
    CHECK(cli1 >= 0, "create client tag");
    CHECK(plc_tag_read(cli1, TIMEOUT_MS) == PLCTAG_STATUS_OK, "client read");
    CHECK(plc_tag_get_int32(cli1, 0) == 1234567, "client sees seeded value");

    printf("\n=== Test 2: second server tag on the same endpoint ===\n");

    snprintf(attr, sizeof(attr),
             "protocol=ab-eip&role=server&gateway=0.0.0.0:%d&name=TestDINT2&elem_type=DINT&elem_count=1", TEST_PORT);
    int32_t srv2 = plc_tag_create(attr, TIMEOUT_MS);
    CHECK(srv2 >= 0, "create server tag 2 (joins existing endpoint)");
    CHECK(plc_tag_set_int32(srv2, 0, 42) == PLCTAG_STATUS_OK, "seed server tag 2 locally");
    CHECK(plc_tag_write(srv2, TIMEOUT_MS) == PLCTAG_STATUS_OK, "push server tag 2's value");

    snprintf(attr, sizeof(attr), "protocol=ab-eip&gateway=127.0.0.1:%d&path=1,0&plc=ControlLogix&elem_count=1&name=TestDINT2",
             TEST_PORT);
    int32_t cli2 = plc_tag_create(attr, TIMEOUT_MS);
    CHECK(cli2 >= 0, "create second client tag");
    CHECK(plc_tag_read(cli2, TIMEOUT_MS) == PLCTAG_STATUS_OK, "second client read");
    CHECK(plc_tag_get_int32(cli2, 0) == 42, "second client sees its own tag's value");

    /* tag 1 must still be independently correct after tag 2 joined. */
    CHECK(plc_tag_read(cli1, TIMEOUT_MS) == PLCTAG_STATUS_OK, "re-read client tag 1");
    CHECK(plc_tag_get_int32(cli1, 0) == 1234567, "client tag 1 unaffected by tag 2");

    printf("\n=== Test 3: callback fires when a remote client writes ===\n");

    CHECK(plc_tag_register_callback(srv1, on_server_event) == PLCTAG_STATUS_OK, "register server tag 1 callback");
    CHECK(plc_tag_set_int32(cli1, 0, 999) == PLCTAG_STATUS_OK, "client sets new local value");
    CHECK(plc_tag_write(cli1, TIMEOUT_MS) == PLCTAG_STATUS_OK, "client writes new value to server tag 1");

    {
        int waited_ms = 0;
        while(!g_write_completed && waited_ms < TIMEOUT_MS) {
            sleep_ms(50);
            waited_ms += 50;
        }
        CHECK(g_write_completed, "server tag 1 callback observed WRITE_COMPLETED");
    }

    CHECK(plc_tag_read(srv1, TIMEOUT_MS) == PLCTAG_STATUS_OK, "server tag 1 local read after remote write");
    CHECK(plc_tag_get_int32(srv1, 0) == 999, "server tag 1 sees the client's written value");

    printf("\n=== Test 4: destroy one server tag while its sibling keeps serving ===\n");

    plc_tag_destroy(srv2);
    CHECK(plc_tag_read(cli1, TIMEOUT_MS) == PLCTAG_STATUS_OK, "client tag 1 still reads fine after sibling destroyed");
    CHECK(plc_tag_get_int32(cli1, 0) == 999, "client tag 1's value unaffected");

    /* tag 2's name should no longer resolve: expect a create-time failure to
     * reach it as a CLIENT (the server side is gone), not a hang or crash. */
    snprintf(attr, sizeof(attr), "protocol=ab-eip&gateway=127.0.0.1:%d&path=1,0&plc=ControlLogix&elem_count=1&name=TestDINT2",
             TEST_PORT);
    int32_t cli2b = plc_tag_create(attr, TIMEOUT_MS);
    if(cli2b >= 0) {
        int rc = plc_tag_read(cli2b, TIMEOUT_MS);
        CHECK(rc != PLCTAG_STATUS_OK, "reading destroyed server tag's name fails cleanly");
        plc_tag_destroy(cli2b);
    } else {
        CHECK(true, "reading destroyed server tag's name fails cleanly (create itself failed)");
    }

    printf("\n=== Test 5: sim_fault forces a CIP error status ===\n");

    snprintf(attr, sizeof(attr),
             "protocol=ab-eip&role=server&gateway=0.0.0.0:%d&name=TestFault&elem_type=DINT&elem_count=1&sim_fault=0x05",
             TEST_PORT);
    int32_t srv3 = plc_tag_create(attr, TIMEOUT_MS);
    CHECK(srv3 >= 0, "create fault-injecting server tag");

    snprintf(attr, sizeof(attr), "protocol=ab-eip&gateway=127.0.0.1:%d&path=1,0&plc=ControlLogix&elem_count=1&name=TestFault",
             TEST_PORT);
    /* ab-eip does an implicit type-discovery read as part of connecting, so
     * the fault may surface at create() itself rather than waiting for an
     * explicit plc_tag_read(). Either way it must never report success. */
    int32_t cli3 = plc_tag_create(attr, TIMEOUT_MS);
    if(cli3 >= 0) {
        CHECK(plc_tag_read(cli3, TIMEOUT_MS) != PLCTAG_STATUS_OK, "client read gets the injected fault, not success");
        plc_tag_destroy(cli3);
    } else {
        CHECK(true, "client read gets the injected fault, not success (create itself failed)");
    }

    /* the sibling tag on the same endpoint (srv1) must be unaffected --
     * fault_status is per-tag, not per-endpoint. */
    CHECK(plc_tag_read(cli1, TIMEOUT_MS) == PLCTAG_STATUS_OK, "unrelated tag on the same endpoint still reads fine");

    plc_tag_destroy(srv3);

    plc_tag_destroy(cli1);
    plc_tag_destroy(cli2);
    plc_tag_destroy(srv1);

    printf("\n=== Results: %d failure(s) ===\n", g_failures);
    return (g_failures == 0) ? 0 : 1;
}
