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
 * omron_udt_walk — end-to-end proof that the OMRON class-0x6C @udt
 * member-sibling-chain walk (OMRON-SPECIFIC-DESIGN.md §5.3,
 * dialects/omron/omron_listing.c) and its client-side driver
 * (enip_omron_apply_listing, client/enip_session.c) actually traverse a
 * nested UDT over real loopback CIP messages: no direct calls into either
 * module's internals, just device_sim on one side and plc_tag on the other.
 *
 * Registers Outer{a:DINT, inner:Inner, b:INT} where Inner{x:DINT}, reads
 * "@udt/<Outer id>", and checks the accumulated raw reply bytes contain
 * every member name and the nested template's own name -- proof that the
 * walk visited Outer's top-level reply, both of Outer's members (including
 * following "inner"'s nesting_variable_type_instance_id into Inner), and
 * Inner's own top-level reply.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libplctag.h"
#include <libplctag/protocols/enip/server/device_sim.h>

#define SIM_PORT ((uint16_t)44819)

static int contains(const uint8_t *buf, int len, const char *needle) {
    int needle_len = (int)strlen(needle);
    for(int i = 0; i + needle_len <= len; i++) {
        if(memcmp(buf + i, needle, (size_t)needle_len) == 0) { return 1; }
    }
    return 0;
}

int main(void) {
    int rc = 1;

    device_sim_t *sim = device_sim_create(ENIP_PLC_OMRON_NJNX, "NJ101-1000", "127.0.0.1", SIM_PORT);
    if(!sim) {
        printf("device_sim_create failed\n");
        return 1;
    }

    uint16_t inner_id = 0, outer_id = 0;
    udt_member_t inner_members[1] = {
        {.name = "x", .type = TAG_CIP_TYPE_DINT, .array_count = 0, .offset = 0},
    };
    if(device_sim_add_udt_type(sim, "Inner", 4, inner_members, 1, &inner_id) != PLCTAG_STATUS_OK) {
        printf("add Inner UDT failed\n");
        goto done;
    }

    udt_member_t outer_members[3] = {
        {.name = "a", .type = TAG_CIP_TYPE_DINT, .array_count = 0, .offset = 0},
        {.name = "inner", .type = DEVICE_SIM_STRUCTURE_TYPE(inner_id), .array_count = 0, .offset = 4},
        {.name = "b", .type = TAG_CIP_TYPE_INT, .array_count = 0, .offset = 8},
    };
    if(device_sim_add_udt_type(sim, "Outer", 10, outer_members, 3, &outer_id) != PLCTAG_STATUS_OK) {
        printf("add Outer UDT failed\n");
        goto done;
    }

    uint32_t dims[1] = {1};
    if(device_sim_add_tag(sim, "OuterTag", DEVICE_SIM_STRUCTURE_TYPE(outer_id), dims, 1, NULL, NULL, NULL)
       != PLCTAG_STATUS_OK) {
        printf("add OuterTag failed\n");
        goto done;
    }

    if(device_sim_start(sim) != PLCTAG_STATUS_OK) {
        printf("device_sim_start failed\n");
        goto done;
    }

    char tag_str[256];
    snprintf(tag_str, sizeof(tag_str),
             "protocol=enip-tcp&gateway=127.0.0.1:%u&path=1,0&plc=omron-njnx&name=@udt/%u", (unsigned)SIM_PORT,
             (unsigned)outer_id);

    int32_t tag = plc_tag_create(tag_str, 5000);
    if(plc_tag_status(tag) != PLCTAG_STATUS_OK) {
        printf("plc_tag_create failed: %s\n", plc_tag_decode_error(plc_tag_status(tag)));
        goto done;
    }

    if(plc_tag_read(tag, 5000) != PLCTAG_STATUS_OK) {
        printf("plc_tag_read failed: %s\n", plc_tag_decode_error(plc_tag_status(tag)));
        plc_tag_destroy(tag);
        goto done;
    }

    int size = plc_tag_get_size(tag);
    uint8_t buf[4096];
    if(size <= 0 || (size_t)size > sizeof(buf)) {
        printf("unexpected @udt reply size %d\n", size);
        plc_tag_destroy(tag);
        goto done;
    }
    for(int i = 0; i < size; i++) { buf[i] = plc_tag_get_uint8(tag, i); }
    plc_tag_destroy(tag);

    printf("accumulated %d bytes across the @udt walk\n", size);

    /* A single top-level GetAttributeAll reply for "Outer" alone is well
     * under 40 bytes; five replies (Outer, a, inner, Inner, x) concatenated
     * must be noticeably larger -- proof the walk made more than one
     * request. */
    int walked_multiple = size > 60;
    int saw_outer = contains(buf, size, "Outer");
    int saw_inner_member = contains(buf, size, "inner");
    int saw_inner_type = contains(buf, size, "Inner");
    int saw_x = contains(buf, size, "x");
    int saw_b = contains(buf, size, "b");

    printf("walked_multiple=%d saw_outer=%d saw_inner_member=%d saw_inner_type=%d saw_x=%d saw_b=%d\n", walked_multiple,
           saw_outer, saw_inner_member, saw_inner_type, saw_x, saw_b);

    if(walked_multiple && saw_outer && saw_inner_member && saw_inner_type && saw_x && saw_b) {
        printf("OK\n");
        rc = 0;
    } else {
        printf("MISMATCH: member-sibling/nesting walk did not visit every expected reply\n");
    }

done:
    device_sim_destroy(sim);
    return rc;
}
