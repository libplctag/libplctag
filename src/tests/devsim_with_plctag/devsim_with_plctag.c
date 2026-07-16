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
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/*
 * devsim_with_plctag — proof that a server tag (role=server) and a client tag
 * can coexist in one process, both through the ordinary plc_tag_* API: no
 * separate simulator library or lifecycle.
 *
 * It creates an in-process ControlLogix server tag holding a single DINT,
 * seeds its value with plc_tag_set_int32, then connects to it over loopback
 * with a second, ordinary client tag and reads the value back.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "libplctag.h"

#define SIM_PORT ((uint16_t)44818)
#define EXPECTED ((int32_t)1234567)

int main(void) {
    /* --- server tag: the backing store served over the wire --- */
    int32_t server_tag = plc_tag_create(
        "protocol=ab-eip&role=server&gateway=127.0.0.1:44818&plc=ControlLogix"
        "&name=TestDINT&elem_type=DINT&elem_count=1",
        5000);
    if(plc_tag_status(server_tag) != PLCTAG_STATUS_OK) {
        printf("server tag create failed: %s\n", plc_tag_decode_error(plc_tag_status(server_tag)));
        return 1;
    }

    plc_tag_set_int32(server_tag, 0, EXPECTED);
    plc_tag_write(server_tag, 5000); /* push local value into the served buffer */

    /* --- client tag: connect to that server tag and read the value back --- */
    int32_t client_tag = plc_tag_create(
        "protocol=ab-eip&gateway=127.0.0.1:44818&path=1,0&plc=ControlLogix&elem_count=1&name=TestDINT",
        5000);

    int32_t rc = 1;
    if(plc_tag_status(client_tag) != PLCTAG_STATUS_OK) {
        printf("plc_tag_create failed: %s\n", plc_tag_decode_error(plc_tag_status(client_tag)));
    } else if(plc_tag_read(client_tag, 5000) != PLCTAG_STATUS_OK) {
        printf("plc_tag_read failed\n");
    } else {
        int32_t got = plc_tag_get_int32(client_tag, 0);
        printf("read TestDINT = %d (expected %d): %s\n", got, EXPECTED, got == EXPECTED ? "OK" : "MISMATCH");
        rc = (got == EXPECTED) ? 0 : 1;
    }

    plc_tag_destroy(client_tag);
    plc_tag_destroy(server_tag);
    return rc;
}
