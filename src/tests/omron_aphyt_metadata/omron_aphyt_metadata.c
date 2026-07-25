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
 * omron_aphyt_metadata — proof for APHYT-COMPAT-PLAN.md Phases 1-2: no
 * sockets, no device_sim_start(). Drives common/cip.c's
 * cip_dispatch_unconnected() directly with hand-built CIP request bytes,
 * the same bytes a real EIP connection's SendRRData payload carries after
 * CPF unwrapping (mirrors omron_udt_walk's "no internals, real wire bytes"
 * approach, minus the socket/plc_tag round trip since there's no client-side
 * API for a kind-filtered list or a symbolic GetAttributesAll).
 *
 * Phase 1: class 0x6A GetInstanceListEx2 (0x5F) kind filtering
 * (dialects/omron/omron_listing.c's handle_tag_name_server) -- kind=2
 * (user) must return every configured tag, kind=1 (system) must return an
 * empty, well-formed list (device_sim tags default to system=false, and
 * there is no public API to flip it, so this is the only observable case
 * without adding test-only surface to device_sim.h).
 *
 * Phase 2: symbolic (0x91) GetAttributesAll (0x01) variable metadata
 * (common/cip.c's handle_omron_variable_attrs) -- an atomic tag must report
 * variable_type_instance_id=0 and its CIP type byte; a UDT tag must report
 * the struct type byte (0xA0) and a template id that both fits aphyt's
 * 2-byte re-query and resolves back via device_udt_find.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libplctag.h"
#include <libplctag/protocols/enip/common/cip.h>
#include <libplctag/protocols/enip/common/cip_path.h>
#include <libplctag/protocols/enip/server/device.h>
#include <libplctag/protocols/enip/server/device_sim.h>
#include <utils/arena.h>
#include <utils/bytes.h>

#define ARENA_SIZE ((size_t)8192)

static int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if(!(cond)) {                                                           \
            printf("FAIL: %s (line %d)\n", (msg), __LINE__);                    \
            g_failures++;                                                       \
        }                                                                       \
    } while(0)

/* Builds a 0x5F GetInstanceListEx2 request on class 0x6A instance 0, mirroring
 * aphyt's _get_instance_list_subset(user=...). */
static Bytes build_list_request(Arena *a, uint16_t kind) {
    Bytes path = cip_path_encode(a, bytes_null(), 0x6Au, 0u, -1);
    if(bytes_is_null(path)) { return bytes_null(); }
    uint8_t path_len_words = (uint8_t)(path.len / 2);
    return bytes_pack(a, BYTES_LE, (uint8_t)0x5F, path_len_words, path, (uint32_t)0 /* start_instance */,
                      (uint32_t)100 /* max_count */, kind);
}

/* Builds a symbolic (0x91) GetAttributesAll (0x01) request, mirroring aphyt's
 * _get_instance_from_variable_name(). */
static Bytes build_symbolic_attrs_request(Arena *a, const char *name) {
    uint8_t name_len = (uint8_t)strlen(name);
    Bytes name_path = bytes_pack(a, BYTES_LE, (uint8_t)0x91, name_len, bytes_from_buf((const uint8_t *)name, name_len));
    Bytes path = bytes_pad_even(a, name_path);
    if(bytes_is_null(path)) { return bytes_null(); }
    uint8_t path_len_words = (uint8_t)(path.len / 2);
    return bytes_pack(a, BYTES_LE, (uint8_t)0x01, path_len_words, path);
}

static void test_kind_filter(Arena *a, device_t *dev) {
    eip_session_t sess;
    memset(&sess, 0, sizeof(sess));

    /* kind=2 (user): every configured tag defaults to user, so all 3 come back. */
    {
        Bytes req = build_list_request(a, 2);
        Bytes reply = cip_dispatch_unconnected(a, req, &sess, dev);
        CHECK(!bytes_is_null(reply), "kind=2 request produced no reply");
        if(!bytes_is_null(reply)) {
            uint8_t reply_svc = 0, reserved = 0, general_status = 0, ext_status_size = 0;
            uint16_t count = 0;
            Bytes rest = bytes_unpack(reply, BYTES_LE, &reply_svc, &reserved, &general_status, &ext_status_size, &count);
            CHECK(!bytes_is_null(rest), "kind=2 reply too short to unpack header");
            CHECK(reply_svc == (0x5F | 0x80), "kind=2 reply service byte wrong");
            CHECK(general_status == 0x00, "kind=2 reply general_status not OK");
            CHECK(count == 3, "kind=2 (user) reply did not list all 3 tags");
        }
    }

    /* kind=1 (system): no tag is ever marked system today -- must be an empty,
     * well-formed list, not a duplicate of the user list (the bug this phase fixes). */
    {
        Bytes req = build_list_request(a, 1);
        Bytes reply = cip_dispatch_unconnected(a, req, &sess, dev);
        CHECK(!bytes_is_null(reply), "kind=1 request produced no reply");
        if(!bytes_is_null(reply)) {
            uint8_t reply_svc = 0, reserved = 0, general_status = 0, ext_status_size = 0;
            uint16_t count = 0;
            Bytes rest = bytes_unpack(reply, BYTES_LE, &reply_svc, &reserved, &general_status, &ext_status_size, &count);
            CHECK(!bytes_is_null(rest), "kind=1 reply too short to unpack header");
            CHECK(general_status == 0x00, "kind=1 reply general_status not OK");
            CHECK(count == 0, "kind=1 (system) reply was not empty -- duplicate-tag bug regressed");
        }
    }
}

static void test_symbolic_attrs(Arena *a, device_t *dev) {
    eip_session_t sess;
    memset(&sess, 0, sizeof(sess));

    /* Atomic tag: STRING, variable_type_instance_id must be 0 (no UDT walk needed). */
    {
        Bytes req = build_symbolic_attrs_request(a, "MyString");
        Bytes reply = cip_dispatch_unconnected(a, req, &sess, dev);
        CHECK(!bytes_is_null(reply), "symbolic attrs on atomic tag produced no reply");
        if(!bytes_is_null(reply)) {
            uint8_t reply_svc = 0, reserved = 0, general_status = 0, ext_status_size = 0;
            uint32_t size = 0;
            uint8_t cip_data_type = 0, cip_type_of_array = 0, array_dimension = 0;
            uint32_t variable_type_instance_id = 0xFFFFFFFFu;
            Bytes rest = bytes_unpack(reply, BYTES_LE, &reply_svc, &reserved, &general_status, &ext_status_size, &size,
                                      &cip_data_type, &cip_type_of_array, &array_dimension, BYTES_SKIP(1),
                                      &variable_type_instance_id);
            CHECK(!bytes_is_null(rest), "atomic symbolic-attrs reply too short to unpack");
            CHECK(reply_svc == (0x01 | 0x80), "atomic symbolic-attrs reply service byte wrong");
            CHECK(general_status == 0x00, "atomic symbolic-attrs general_status not OK");
            CHECK(cip_data_type == 0xD0, "atomic symbolic-attrs type byte is not STRING (0xD0)");
            CHECK(variable_type_instance_id == 0, "atomic symbolic-attrs variable_type_instance_id is not 0");
        }
    }

    /* UDT tag: struct type byte, and a variable_type_instance_id that both
     * fits aphyt's 2-byte re-query and resolves back to a real template. */
    {
        Bytes req = build_symbolic_attrs_request(a, "MyUdt");
        Bytes reply = cip_dispatch_unconnected(a, req, &sess, dev);
        CHECK(!bytes_is_null(reply), "symbolic attrs on UDT tag produced no reply");
        if(!bytes_is_null(reply)) {
            uint8_t reply_svc = 0, reserved = 0, general_status = 0, ext_status_size = 0;
            uint32_t size = 0;
            uint8_t cip_data_type = 0, cip_type_of_array = 0, array_dimension = 0;
            uint32_t variable_type_instance_id = 0;
            Bytes rest = bytes_unpack(reply, BYTES_LE, &reply_svc, &reserved, &general_status, &ext_status_size, &size,
                                      &cip_data_type, &cip_type_of_array, &array_dimension, BYTES_SKIP(1),
                                      &variable_type_instance_id);
            CHECK(!bytes_is_null(rest), "UDT symbolic-attrs reply too short to unpack");
            CHECK(general_status == 0x00, "UDT symbolic-attrs general_status not OK");
            CHECK(cip_data_type == 0xA0, "UDT symbolic-attrs type byte is not CIPAbbreviatedStructure (0xA0)");
            CHECK(variable_type_instance_id != 0, "UDT symbolic-attrs variable_type_instance_id is 0");
            CHECK(variable_type_instance_id <= 0x0FFF, "UDT variable_type_instance_id does not fit aphyt's 2-byte re-query");
            CHECK(device_udt_find(dev, (uint16_t)variable_type_instance_id) != NULL,
                  "UDT variable_type_instance_id does not resolve back to a real class-0x6C template");
        }
    }

    /* Unknown tag: must be a CIP error, not a silent empty success. */
    {
        Bytes req = build_symbolic_attrs_request(a, "NoSuchTag");
        Bytes reply = cip_dispatch_unconnected(a, req, &sess, dev);
        CHECK(!bytes_is_null(reply), "symbolic attrs on unknown tag produced no reply");
        if(!bytes_is_null(reply)) {
            uint8_t reply_svc = 0, reserved = 0, general_status = 0;
            Bytes rest = bytes_unpack(reply, BYTES_LE, &reply_svc, &reserved, &general_status);
            CHECK(!bytes_is_null(rest), "unknown-tag reply too short to unpack");
            CHECK(general_status != 0x00, "unknown-tag reply reported general_status OK");
        }
    }
}

int main(void) {
    Arena arena;
    if(arena_init(&arena, ARENA_SIZE) != 0) {
        printf("arena_init failed\n");
        return 1;
    }

    device_sim_t *sim = device_sim_create(ENIP_PLC_OMRON_NJNX, NULL, NULL, 0);
    if(!sim) {
        printf("device_sim_create failed\n");
        arena_free(&arena);
        return 1;
    }

    uint32_t dims[1] = {1};
    int setup_ok = 1;
    setup_ok &= (device_sim_add_tag(sim, "MyDint", TAG_CIP_TYPE_DINT, dims, 1, NULL, NULL, NULL) == PLCTAG_STATUS_OK);
    setup_ok &= (device_sim_add_tag(sim, "MyString", TAG_CIP_TYPE_STRING, dims, 1, NULL, NULL, NULL) == PLCTAG_STATUS_OK);

    uint16_t udt_id = 0;
    udt_member_t members[1] = {{.name = "x", .type = TAG_CIP_TYPE_DINT, .array_count = 0, .offset = 0}};
    setup_ok &= (device_sim_add_udt_type(sim, "Inner", 4, members, 1, &udt_id) == PLCTAG_STATUS_OK);
    setup_ok &= (device_sim_add_tag(sim, "MyUdt", DEVICE_SIM_STRUCTURE_TYPE(udt_id), dims, 1, NULL, NULL, NULL)
                 == PLCTAG_STATUS_OK);

    if(!setup_ok) {
        printf("test setup (add_tag/add_udt_type) failed\n");
        device_sim_destroy(sim);
        arena_free(&arena);
        return 1;
    }

    device_t *dev = device_sim_get_device(sim);
    test_kind_filter(&arena, dev);
    test_symbolic_attrs(&arena, dev);

    device_sim_destroy(sim);
    arena_free(&arena);

    if(g_failures == 0) {
        printf("OK\n");
        return 0;
    }
    printf("%d check(s) failed\n", g_failures);
    return 1;
}
