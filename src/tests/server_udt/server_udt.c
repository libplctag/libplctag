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
 * server_udt -- verifies role=server's udt=/elem_type=@name attributes
 * (ENIP-UPDATES-PLAN.md item 2): declaring a UDT template purely through the
 * plc_tag_create() attribute string (no device_sim_add_udt_type C call), then
 * reading it back through a real client tag over "@udt/<id>" -- the same
 * class 0x6C walk src/tests/omron_udt_walk proves for the OMRON dialect, here
 * exercised against the Rockwell/Logix dialect and an attribute-declared
 * template instead of a C-API one.
 *
 * The endpoint is fresh (unique port, nothing else registers a template on
 * it), so the first udt= tag's template is deterministically id 1 --
 * plc_tag_create() has no way to hand back the assigned id, so the test
 * relies on that determinism rather than needing one.
 *
 * Known gap this test does NOT cover: reading a udt=/elem_type=@name tag's
 * raw data through an ordinary (non-@udt) client tag. The generic ENIP
 * client's reply-type decoder (client/enip_type.c: enip_type_decode) does not
 * yet recognize structure symbol type codes (0x8000|id) and rejects the
 * reply before the data-read path (client/enip_session.c: apply_tag_reply)
 * ever gets to the bytes -- a client-side gap, not a server-side one; add
 * that case once enip_type_decode grows structure-type support.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libplctag.h"
#include "platform.h"

#define TEST_PORT 44821
#define TIMEOUT_MS 5000

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

static bool contains(const uint8_t *buf, int len, const char *needle) {
    int needle_len = (int)strlen(needle);
    for(int i = 0; i + needle_len <= len; i++) {
        if(memcmp(buf + i, needle, (size_t)needle_len) == 0) { return true; }
    }
    return false;
}

int main(void) {
    char attr[400];

    printf("=== Test 1: udt= declares a template and instantiates it in one tag ===\n");

    snprintf(attr, sizeof(attr),
             "protocol=ab-eip&role=server&gateway=0.0.0.0:%d&name=Point1&udt=Point:x:DINT,y:DINT", TEST_PORT);
    int32_t srv1 = plc_tag_create(attr, TIMEOUT_MS);
    CHECK(srv1 >= 0, "create server tag with udt= (declares Point, instantiates it)");
    CHECK(plc_tag_get_size(srv1) == 8, "Point1 instance size is 8 bytes (two DINT members)");

    printf("\n=== Test 2: elem_type=@Point instantiates the same template again ===\n");

    snprintf(attr, sizeof(attr), "protocol=ab-eip&role=server&gateway=0.0.0.0:%d&name=Point2&elem_type=@Point", TEST_PORT);
    int32_t srv2 = plc_tag_create(attr, TIMEOUT_MS);
    CHECK(srv2 >= 0, "create server tag 2 via elem_type=@Point (reuses the template Point1 declared)");
    CHECK(plc_tag_get_size(srv2) == 8, "Point2 instance size matches the shared template");

    printf("\n=== Test 3: elem_type=@<unknown> fails cleanly ===\n");

    snprintf(attr, sizeof(attr), "protocol=ab-eip&role=server&gateway=0.0.0.0:%d&name=Bogus&elem_type=@NoSuchType",
             TEST_PORT);
    int32_t srv3 = plc_tag_create(attr, TIMEOUT_MS);
    CHECK(srv3 < 0, "elem_type=@NoSuchType is rejected at create time");

    printf("\n=== Test 4: read the udt=-declared template back over the wire via @udt/1 ===\n");

    /* Point is the only template ever registered on this endpoint, so it is
     * deterministically template id 1 -- see the file comment. */
    snprintf(attr, sizeof(attr), "protocol=enip-tcp&gateway=127.0.0.1:%d&path=1,0&plc=ControlLogix&name=@udt/1", TEST_PORT);
    int32_t udt_tag = plc_tag_create(attr, TIMEOUT_MS);
    CHECK(udt_tag >= 0, "create @udt/1 client tag");

    if(udt_tag >= 0) {
        CHECK(plc_tag_read(udt_tag, TIMEOUT_MS) == PLCTAG_STATUS_OK, "read @udt/1");

        int size = plc_tag_get_size(udt_tag);
        uint8_t buf[512];
        bool size_ok = size > 0 && (size_t)size <= sizeof(buf);
        CHECK(size_ok, "@udt/1 reply has a sane size");

        if(size_ok) {
            for(int i = 0; i < size; i++) { buf[i] = plc_tag_get_uint8(udt_tag, i); }
            CHECK(contains(buf, size, "Point"), "@udt/1 reply contains the struct_name \"Point\"");
            CHECK(contains(buf, size, "x"), "@udt/1 reply contains member name \"x\"");
            CHECK(contains(buf, size, "y"), "@udt/1 reply contains member name \"y\"");
        }

        plc_tag_destroy(udt_tag);
    }

    printf("\n=== Test 5: the same tag reads identically connected and unconnected ===\n");

    /*
     * plc=ControlLogix makes the endpoint report a Logix identity, which the
     * client classifies as a family that prefers connected messaging, so
     * use_connected_msg= is what actually separates these two reads. They also
     * land on separate connections: the attribute is part of the registry key.
     */
    snprintf(attr, sizeof(attr), "protocol=ab-eip&role=server&gateway=0.0.0.0:%d&name=Counter&elem_type=DINT&elem_count=1",
             TEST_PORT);
    int32_t srv4 = plc_tag_create(attr, TIMEOUT_MS);
    CHECK(srv4 >= 0, "create DINT server tag Counter");

    if(srv4 >= 0) {
        plc_tag_set_int32(srv4, 0, 0x5A5A1234);
        CHECK(plc_tag_write(srv4, TIMEOUT_MS) == PLCTAG_STATUS_OK, "push seeded value to the served buffer");

        snprintf(attr, sizeof(attr),
                 "protocol=enip-tcp&gateway=127.0.0.1:%d&path=1,0&plc=ControlLogix&elem_count=1&name=Counter"
                 "&use_connected_msg=1",
                 TEST_PORT);
        int32_t cli_conn = plc_tag_create(attr, TIMEOUT_MS);
        CHECK(cli_conn >= 0, "create connected client tag");

        snprintf(attr, sizeof(attr),
                 "protocol=enip-tcp&gateway=127.0.0.1:%d&path=1,0&plc=ControlLogix&elem_count=1&name=Counter"
                 "&use_connected_msg=0",
                 TEST_PORT);
        int32_t cli_unconn = plc_tag_create(attr, TIMEOUT_MS);
        CHECK(cli_unconn >= 0, "create unconnected client tag");

        if(cli_conn >= 0 && cli_unconn >= 0) {
            CHECK(plc_tag_read(cli_conn, TIMEOUT_MS) == PLCTAG_STATUS_OK, "connected read");
            CHECK(plc_tag_get_int32(cli_conn, 0) == 0x5A5A1234, "connected read returns the served value");

            CHECK(plc_tag_read(cli_unconn, TIMEOUT_MS) == PLCTAG_STATUS_OK, "unconnected read");
            CHECK(plc_tag_get_int32(cli_unconn, 0) == 0x5A5A1234, "unconnected read returns the served value");

            /* A write proves the Unconnected_Send envelope is sized right in
             * both directions, not just for the smaller read request. */
            plc_tag_set_int32(cli_unconn, 0, 0x0BADCAFE);
            CHECK(plc_tag_write(cli_unconn, TIMEOUT_MS) == PLCTAG_STATUS_OK, "unconnected write");
            CHECK(plc_tag_read(srv4, TIMEOUT_MS) == PLCTAG_STATUS_OK, "pull the served buffer back into the server tag");
            CHECK(plc_tag_get_int32(srv4, 0) == 0x0BADCAFE, "server tag sees the unconnected write");
        }

        if(cli_conn >= 0) { plc_tag_destroy(cli_conn); }
        if(cli_unconn >= 0) { plc_tag_destroy(cli_unconn); }
        plc_tag_destroy(srv4);
    }

    plc_tag_destroy(srv1);
    plc_tag_destroy(srv2);

    printf("\n=== Results: %d failure(s) ===\n", g_failures);
    return (g_failures == 0) ? 0 : 1;
}
