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
 * Pure logic test for the class 0x6C UDT template registry
 * (device_sim_add_udt_type / device_udt_find): no sockets, no
 * device_sim_start(), so it needs no network access. Exercises the
 * definition-blob encoding (member entries + NUL-delimited name blob,
 * ROCKWELL-SPECIFIC-DESIGN.md §5.2) and the structure-typed tag path in
 * device_sim_add_tag.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "cmocka.h"

#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/enip/server/device.h>
#include <libplctag/protocols/enip/server/device_sim.h>

static void test_add_udt_type_basic(void **state) {
    (void)state;

    device_sim_t *sim = device_sim_create(ENIP_PLC_LGX, NULL, NULL, 0);
    assert_non_null(sim);

    udt_member_t members[2] = {
        {.name = "count", .type = TAG_CIP_TYPE_DINT, .array_count = 0, .offset = 0},
        {.name = "flags", .type = TAG_CIP_TYPE_INT, .array_count = 0, .offset = 4},
    };

    uint16_t template_id = 0;
    assert_int_equal(device_sim_add_udt_type(sim, "MyUdt", 8, members, 2, &template_id), PLCTAG_STATUS_OK);
    assert_int_equal(template_id, 1);

    device_t *dev = device_sim_get_device(sim);
    udt_template_t *tmpl = device_udt_find(dev, template_id);
    assert_non_null(tmpl);
    assert_int_equal(tmpl->template_id, 1);
    assert_int_equal(tmpl->instance_size, 8);
    assert_int_equal(tmpl->num_members, 2);

    /* Definition blob: 2 members * 8 bytes + "MyUdt;\0" (7) + "count\0" (6) + "flags\0" (6) = 35 */
    assert_int_equal(tmpl->definition_len, 16 + 7 + 6 + 6);

    /* Member 0: type(u16 LE)=DINT, info(u16 LE)=1 (scalar), offset(u32 LE)=0 */
    assert_int_equal(tmpl->definition[0], (uint8_t)(TAG_CIP_TYPE_DINT & 0xFF));
    assert_int_equal(tmpl->definition[1], (uint8_t)(TAG_CIP_TYPE_DINT >> 8));
    assert_int_equal(tmpl->definition[2], 1);
    assert_int_equal(tmpl->definition[3], 0);
    assert_int_equal(tmpl->definition[4], 0);
    assert_int_equal(tmpl->definition[5], 0);
    assert_int_equal(tmpl->definition[6], 0);
    assert_int_equal(tmpl->definition[7], 0);

    /* Member 1: type=INT, offset=4 */
    assert_int_equal(tmpl->definition[8], (uint8_t)(TAG_CIP_TYPE_INT & 0xFF));
    assert_int_equal(tmpl->definition[9], (uint8_t)(TAG_CIP_TYPE_INT >> 8));
    assert_int_equal(tmpl->definition[12], 4);

    /* Name blob starts right after the 16 bytes of member entries. */
    const uint8_t *names = tmpl->definition + 16;
    assert_memory_equal(names, "MyUdt;\0count\0flags\0", 19);

    device_sim_destroy(sim);
}

static void test_add_udt_type_no_members(void **state) {
    (void)state;

    device_sim_t *sim = device_sim_create(ENIP_PLC_LGX, NULL, NULL, 0);
    assert_non_null(sim);

    uint16_t template_id = 0;
    assert_int_equal(device_sim_add_udt_type(sim, "Empty", 4, NULL, 0, &template_id), PLCTAG_STATUS_OK);

    device_t *dev = device_sim_get_device(sim);
    udt_template_t *tmpl = device_udt_find(dev, template_id);
    assert_non_null(tmpl);
    assert_int_equal(tmpl->num_members, 0);
    /* "Empty;\0" only. */
    assert_int_equal(tmpl->definition_len, 7);
    assert_memory_equal(tmpl->definition, "Empty;\0", 7);

    device_sim_destroy(sim);
}

static void test_add_udt_type_assigns_increasing_ids(void **state) {
    (void)state;

    device_sim_t *sim = device_sim_create(ENIP_PLC_LGX, NULL, NULL, 0);
    assert_non_null(sim);

    uint16_t id1 = 0, id2 = 0;
    assert_int_equal(device_sim_add_udt_type(sim, "First", 4, NULL, 0, &id1), PLCTAG_STATUS_OK);
    assert_int_equal(device_sim_add_udt_type(sim, "Second", 4, NULL, 0, &id2), PLCTAG_STATUS_OK);
    assert_int_equal(id1, 1);
    assert_int_equal(id2, 2);
    assert_ptr_not_equal(device_udt_find(device_sim_get_device(sim), id1), device_udt_find(device_sim_get_device(sim), id2));

    device_sim_destroy(sim);
}

static void test_add_tag_structure_type_uses_template_size(void **state) {
    (void)state;

    device_sim_t *sim = device_sim_create(ENIP_PLC_LGX, NULL, NULL, 0);
    assert_non_null(sim);

    uint16_t template_id = 0;
    assert_int_equal(device_sim_add_udt_type(sim, "MyUdt", 8, NULL, 0, &template_id), PLCTAG_STATUS_OK);

    uint32_t dims[1] = {3};
    assert_int_equal(device_sim_add_tag(sim, "MyTag", DEVICE_SIM_STRUCTURE_TYPE(template_id), dims, 1, NULL, NULL, NULL),
                     PLCTAG_STATUS_OK);

    /* No direct tag_def_t accessor is exposed; device_sim_tag_get/set only
     * validate offset/len are in range, which they are only if elem_size (8)
     * * elem_count (3) = 24 was actually allocated from the template size. */
    uint8_t buf[8] = {0};
    assert_int_equal(device_sim_tag_get(sim, "MyTag", 16, buf, 8), PLCTAG_STATUS_OK); /* last element, in range */
    assert_int_equal(device_sim_tag_get(sim, "MyTag", 24, buf, 1), PLCTAG_ERR_OUT_OF_BOUNDS); /* one byte past the end */

    device_sim_destroy(sim);
}

static void test_add_tag_unknown_structure_type_fails(void **state) {
    (void)state;

    device_sim_t *sim = device_sim_create(ENIP_PLC_LGX, NULL, NULL, 0);
    assert_non_null(sim);

    uint32_t dims[1] = {1};
    assert_int_equal(device_sim_add_tag(sim, "MyTag", DEVICE_SIM_STRUCTURE_TYPE(99), dims, 1, NULL, NULL, NULL),
                     PLCTAG_ERR_BAD_PARAM);

    device_sim_destroy(sim);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_add_udt_type_basic),
        cmocka_unit_test(test_add_udt_type_no_members),
        cmocka_unit_test(test_add_udt_type_assigns_increasing_ids),
        cmocka_unit_test(test_add_tag_structure_type_uses_template_size),
        cmocka_unit_test(test_add_tag_unknown_structure_type_fails),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
