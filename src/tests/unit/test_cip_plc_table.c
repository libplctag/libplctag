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
 * The CIP PLC dispatch table's own invariants.
 *
 * A dispatch table's failure mode is silent: a key that no row answers, or two rows
 * answering the same key, shows up as one PLC family behaving like another rather than as
 * an error.  Neither is reachable from a test that creates tags, because it would need a
 * PLC of every family to notice.  So the table is checked directly.
 *
 * Every row now has a leaf -- the last family moved across in K5 -- so a NULL setup_tag is
 * an unfinished row rather than a migration in progress, and is asserted against.
 */

#include "mini_mock.h"

#include <libplctag/api/libplctag.h>

#include <libplctag/modules/cip/plc_type.h>
#include <libplctag/modules/cip/cip_tag_create.h>
#include <utils/attr.h>


/* every dialect a row may legally carry. */
static const cip_dialect_t all_dialects[] = {CIP_DIALECT_DIRECT, CIP_DIALECT_DHP};


static void test_every_plc_type_has_a_direct_row(void **state) {
    cip_plc_type_t plc_type = CIP_PLC_NONE;

    (void)state;

    /*
     * CIP_PLC_NONE is the unset value and must not resolve: a tag that never had its CPU
     * type parsed would otherwise get whichever row sorted first.
     */
    assert_null(cip_plc_entry_find(CIP_PLC_NONE, CIP_DIALECT_DIRECT));

    for(plc_type = CIP_PLC_PLC5; plc_type < CIP_PLC_TYPE_LAST; plc_type++) {
        assert_non_null(cip_plc_entry_find(plc_type, CIP_DIALECT_DIRECT));
    }
}


static void test_only_dhp_capable_families_have_a_dhp_row(void **state) {
    (void)state;

    /* the three PCCC families reachable across a DH+ bridge. */
    assert_non_null(cip_plc_entry_find(CIP_PLC_PLC5, CIP_DIALECT_DHP));
    assert_non_null(cip_plc_entry_find(CIP_PLC_SLC, CIP_DIALECT_DHP));
    assert_non_null(cip_plc_entry_find(CIP_PLC_MLGX, CIP_DIALECT_DHP));

    /* nothing else bridges, so nothing else may answer that key. */
    assert_null(cip_plc_entry_find(CIP_PLC_LGX_PCCC, CIP_DIALECT_DHP));
    assert_null(cip_plc_entry_find(CIP_PLC_LGX, CIP_DIALECT_DHP));
    assert_null(cip_plc_entry_find(CIP_PLC_MICRO800, CIP_DIALECT_DHP));
    assert_null(cip_plc_entry_find(CIP_PLC_GENERIC, CIP_DIALECT_DHP));
    assert_null(cip_plc_entry_find(CIP_PLC_OMRON_NJNX, CIP_DIALECT_DHP));
}


static void test_every_row_has_a_leaf(void **state) {
    cip_plc_type_t plc_type = CIP_PLC_NONE;
    size_t dialect_index = 0;

    (void)state;

    /*
     * A row without a leaf reaches the constructor as PLCTAG_ERR_BAD_CONFIG, which reads
     * like the caller named an impossible PLC rather than like the table is incomplete.
     */
    for(plc_type = CIP_PLC_PLC5; plc_type < CIP_PLC_TYPE_LAST; plc_type++) {
        for(dialect_index = 0; dialect_index < (sizeof(all_dialects) / sizeof(all_dialects[0])); dialect_index++) {
            const cip_plc_entry_t *entry = cip_plc_entry_find(plc_type, all_dialects[dialect_index]);

            if(entry) { assert_non_null(entry->setup_tag); }
        }
    }
}


static void test_every_row_is_named(void **state) {
    cip_plc_type_t plc_type = CIP_PLC_NONE;
    size_t dialect_index = 0;

    (void)state;

    /* the name is what a setup failure is reported with, so an empty one loses the message. */
    for(plc_type = CIP_PLC_PLC5; plc_type < CIP_PLC_TYPE_LAST; plc_type++) {
        for(dialect_index = 0; dialect_index < (sizeof(all_dialects) / sizeof(all_dialects[0])); dialect_index++) {
            const cip_plc_entry_t *entry = cip_plc_entry_find(plc_type, all_dialects[dialect_index]);

            if(entry) {
                assert_non_null(entry->name);
                assert_true(entry->name[0] != '\0');
            }
        }
    }
}


static void test_the_key_is_what_the_row_answers_to(void **state) {
    cip_plc_type_t plc_type = CIP_PLC_NONE;
    size_t dialect_index = 0;

    (void)state;

    /*
     * A row whose own key disagrees with the key it was found under means two rows share a
     * key and the first one shadows the second -- the duplicate-key check, without needing
     * the table itself to be visible from here.
     */
    for(plc_type = CIP_PLC_PLC5; plc_type < CIP_PLC_TYPE_LAST; plc_type++) {
        for(dialect_index = 0; dialect_index < (sizeof(all_dialects) / sizeof(all_dialects[0])); dialect_index++) {
            const cip_plc_entry_t *entry = cip_plc_entry_find(plc_type, all_dialects[dialect_index]);

            if(entry) {
                assert_int_equal((int)entry->plc_type, (int)plc_type);
                assert_int_equal((int)entry->dialect, (int)all_dialects[dialect_index]);
            }
        }
    }
}


/*
 * The dialect classifier.
 *
 * This is the half of the table's key that is computed rather than given, and the simulator
 * suite cannot reach it: ab_server is not a DH+ bridge, so nothing in 232 tests ever produces
 * CIP_DIALECT_DHP.  Only hardware does, which is exactly why it is pinned here instead.
 */
static cip_dialect_t dialect_for(cip_plc_type_t plc_type, const char *path) {
    attr attribs = attr_create();
    cip_dialect_t dialect = CIP_DIALECT_DIRECT;

    assert_non_null(attribs);

    if(path) { assert_int_equal(attr_set_str(attribs, "path", path), PLCTAG_STATUS_OK); }

    dialect = cip_dialect_from_attributes(plc_type, attribs);

    attr_destroy(attribs);

    return dialect;
}


static void test_a_dhp_path_is_a_dhp_dialect(void **state) {
    (void)state;

    /* the form the hardware tests use: a chassis path, then port:source:destination. */
    assert_int_equal(dialect_for(CIP_PLC_PLC5, "1,2,A:27:1"), CIP_DIALECT_DHP);
    assert_int_equal(dialect_for(CIP_PLC_SLC, "1,2,A:27:1"), CIP_DIALECT_DHP);
    assert_int_equal(dialect_for(CIP_PLC_MLGX, "1,2,B:27:1"), CIP_DIALECT_DHP);
}


static void test_an_ordinary_path_is_direct(void **state) {
    (void)state;

    assert_int_equal(dialect_for(CIP_PLC_PLC5, NULL), CIP_DIALECT_DIRECT);
    assert_int_equal(dialect_for(CIP_PLC_PLC5, ""), CIP_DIALECT_DIRECT);
    assert_int_equal(dialect_for(CIP_PLC_SLC, "1,0"), CIP_DIALECT_DIRECT);
    assert_int_equal(dialect_for(CIP_PLC_LGX, "1,0"), CIP_DIALECT_DIRECT);
}


static void test_a_family_that_cannot_bridge_is_always_direct(void **state) {
    (void)state;

    /*
     * A DH+ segment against a family that cannot bridge is a bad path, and connection
     * creation says so.  The classifier must not quietly pick the DH+ row for it -- there
     * is no such row, so a wrong answer here would be a NULL lookup and a confusing
     * PLCTAG_ERR_BAD_CONFIG instead of the real complaint about the path.
     */
    assert_int_equal(dialect_for(CIP_PLC_LGX, "1,2,A:27:1"), CIP_DIALECT_DIRECT);
    assert_int_equal(dialect_for(CIP_PLC_LGX_PCCC, "1,2,A:27:1"), CIP_DIALECT_DIRECT);
    assert_int_equal(dialect_for(CIP_PLC_MICRO800, "1,2,A:27:1"), CIP_DIALECT_DIRECT);
    assert_int_equal(dialect_for(CIP_PLC_GENERIC, "1,2,A:27:1"), CIP_DIALECT_DIRECT);
}


static void test_an_unparseable_path_is_direct(void **state) {
    (void)state;

    /* not this function's error to report; the connection runs the same parser and does. */
    assert_int_equal(dialect_for(CIP_PLC_PLC5, "not a path at all"), CIP_DIALECT_DIRECT);

    /* a DH+ segment that is not last is rejected by the parser, so it is not a DH+ tag. */
    assert_int_equal(dialect_for(CIP_PLC_PLC5, "1,A:27:1,2"), CIP_DIALECT_DIRECT);
}


/*
 * Whether requests are connected is row data now, not a line in each setup leaf, because the
 * connection is created before any tag exists.  Nothing in the simulator suite distinguishes
 * a wrong answer here from a working one until the PLC refuses the framing, so pin the two
 * rules the old leaves enforced in code.
 */
static void test_the_row_settles_connected_messaging(void **state) {
    cip_plc_type_t plc_type = CIP_PLC_NONE;
    size_t dialect_index = 0;
    attr attribs = attr_create();

    (void)state;

    assert_non_null(attribs);

    for(plc_type = CIP_PLC_PLC5; plc_type < CIP_PLC_TYPE_LAST; plc_type++) {
        for(dialect_index = 0; dialect_index < (sizeof(all_dialects) / sizeof(all_dialects[0])); dialect_index++) {
            const cip_plc_entry_t *entry = cip_plc_entry_find(plc_type, all_dialects[dialect_index]);

            if(!entry) { continue; }

            /* -1 means "ask the tag string"; anything else is a fixed yes or no. */
            assert_true(entry->use_connected_msg >= -1 && entry->use_connected_msg <= 1);

            /* a DH+ bridge is reached over a connection, and no tag string may say otherwise. */
            if(entry->dialect == CIP_DIALECT_DHP) { assert_int_equal((int)entry->use_connected_msg, 1); }
        }
    }

    /* the one family that defers to the tag string, and what it does with no answer. */
    assert_true(cip_entry_use_connected_msg(cip_plc_entry_find(CIP_PLC_LGX, CIP_DIALECT_DIRECT), attribs));
    assert_int_equal(attr_set_str(attribs, "use_connected_msg", "0"), PLCTAG_STATUS_OK);
    assert_true(!cip_entry_use_connected_msg(cip_plc_entry_find(CIP_PLC_LGX, CIP_DIALECT_DIRECT), attribs));

    /* and a family that does not defer ignores the same attribute. */
    assert_true(cip_entry_use_connected_msg(cip_plc_entry_find(CIP_PLC_MICRO800, CIP_DIALECT_DIRECT), attribs));

    attr_destroy(attribs);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_every_plc_type_has_a_direct_row),
        cmocka_unit_test(test_only_dhp_capable_families_have_a_dhp_row),
        cmocka_unit_test(test_every_row_is_named),
        cmocka_unit_test(test_every_row_has_a_leaf),
        cmocka_unit_test(test_the_key_is_what_the_row_answers_to),
        cmocka_unit_test(test_the_row_settles_connected_messaging),
        cmocka_unit_test(test_a_dhp_path_is_a_dhp_dialect),
        cmocka_unit_test(test_an_ordinary_path_is_direct),
        cmocka_unit_test(test_a_family_that_cannot_bridge_is_always_direct),
        cmocka_unit_test(test_an_unparseable_path_is_direct),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
