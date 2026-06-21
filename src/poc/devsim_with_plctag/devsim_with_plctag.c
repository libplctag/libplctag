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
 * devsim_with_plctag — proof that one executable can link BOTH libplctag and
 * libdevsim.  libdevsim already bundles the static libplctag, so linking
 * libdevsim is enough to reach the plc_tag_* public API too.
 *
 * It starts an in-process ControlLogix simulator with a single DINT tag, seeds
 * the value through the device_sim API, then connects to it over loopback with
 * the libplctag client API and reads the value back.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "libplctag.h"
#include "device_sim.h"

#define SIM_PORT ((uint16_t)44818)
#define EXPECTED ((int32_t)1234567)

int main(void) {
    /* --- libdevsim: stand up an embedded simulator with one DINT tag --- */
    device_sim_t *sim = device_sim_create(PLC_CONTROL_LOGIX, NULL, SIM_PORT);
    if(!sim) {
        printf("device_sim_create failed\n");
        return 1;
    }

    uint32_t dims[1] = {1};
    if(device_sim_add_tag(sim, "TestDINT", TAG_CIP_TYPE_DINT, dims, 1, NULL, NULL, NULL) != PLCTAG_STATUS_OK) {
        printf("device_sim_add_tag failed\n");
        device_sim_destroy(sim);
        return 1;
    }

    int32_t seed = EXPECTED;
    device_sim_tag_set(sim, "TestDINT", 0, &seed, sizeof(seed));

    if(device_sim_start(sim) != PLCTAG_STATUS_OK) {
        printf("device_sim_start failed\n");
        device_sim_destroy(sim);
        return 1;
    }

    /* --- libplctag: connect to that simulator and read the tag back --- */
    int32_t tag = plc_tag_create(
        "protocol=ab-eip&gateway=127.0.0.1:44818&path=1,0&plc=ControlLogix&elem_count=1&name=TestDINT",
        5000);

    int32_t rc = 1;
    if(plc_tag_status(tag) != PLCTAG_STATUS_OK) {
        printf("plc_tag_create failed: %s\n", plc_tag_decode_error(plc_tag_status(tag)));
    } else if(plc_tag_read(tag, 5000) != PLCTAG_STATUS_OK) {
        printf("plc_tag_read failed\n");
    } else {
        int32_t got = plc_tag_get_int32(tag, 0);
        printf("read TestDINT = %d (expected %d): %s\n", got, EXPECTED, got == EXPECTED ? "OK" : "MISMATCH");
        rc = (got == EXPECTED) ? 0 : 1;
    }

    plc_tag_destroy(tag);
    device_sim_stop(sim);
    device_sim_destroy(sim);
    return rc;
}
