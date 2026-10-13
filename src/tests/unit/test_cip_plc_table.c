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
 * Every row must carry what the constructor reads unconditionally: a name parser, a byte
 * order and standard-tag operations.
 */

#include "mini_mock.h"

#include <libplctag/api/libplctag.h>

#include <libplctag/modules/cip/plc_type.h>
#include <libplctag/modules/cip/cip_tag_create.h>
#include <libplctag/modules/cip/path.h>
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
     * The constructor calls through these without checking, so a missing one is a crash at
     * the first tag create for that family.
     */
    for(plc_type = CIP_PLC_PLC5; plc_type < CIP_PLC_TYPE_LAST; plc_type++) {
        for(dialect_index = 0; dialect_index < (sizeof(all_dialects) / sizeof(all_dialects[0])); dialect_index++) {
            const cip_plc_entry_t *entry = cip_plc_entry_find(plc_type, all_dialects[dialect_index]);

            if(entry) {
                assert_non_null(entry->setup_name);
                assert_non_null(entry->byte_order);
                assert_non_null(entry->std_ops);
            }
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
 * The dialect, which connection creation computes from the path: a DH+ path takes the DH+ row.
 *
 * The simulator suite cannot reach it -- ab_server is not a DH+ bridge -- so only hardware
 * produces CIP_DIALECT_DHP, which is exactly why it is pinned here.  This runs the parser the
 * way connection creation does: DH+ capable exactly when the family has a DH+ row.
 */
static int parse_path(cip_plc_type_t plc_type, const char *path, int *is_dhp, bool *is_bridged) {
    uint8_t encoded[MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN];
    int encoded_size = (int)sizeof(encoded);
    int needs_connection = 0;
    uint16_t dhp_dest = 0;
    int kind = (cip_plc_entry_find(plc_type, CIP_DIALECT_DHP) ? CIP_PLC_KIND_DHP_CAPABLE : CIP_PLC_KIND_OTHER);

    *is_dhp = 0;
    *is_bridged = false;

    return cip_encode_path(path, &needs_connection, kind, &encoded[0], &encoded_size, is_dhp, &dhp_dest, is_bridged);
}


static cip_dialect_t dialect_for(cip_plc_type_t plc_type, const char *path) {
    int is_dhp = 0;
    bool is_bridged = false;

    assert_int_equal(parse_path(plc_type, path, &is_dhp, &is_bridged), PLCTAG_STATUS_OK);

    return (is_dhp ? CIP_DIALECT_DHP : CIP_DIALECT_DIRECT);
}


static bool rejects(cip_plc_type_t plc_type, const char *path) {
    int is_dhp = 0;
    bool is_bridged = false;

    return parse_path(plc_type, path, &is_dhp, &is_bridged) != PLCTAG_STATUS_OK;
}


static bool bridges(const char *path) {
    int is_dhp = 0;
    bool is_bridged = false;

    return parse_path(CIP_PLC_PLC5, path, &is_dhp, &is_bridged) == PLCTAG_STATUS_OK && is_bridged;
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

    assert_int_equal(dialect_for(CIP_PLC_PLC5, ""), CIP_DIALECT_DIRECT);
    assert_int_equal(dialect_for(CIP_PLC_SLC, "1,0"), CIP_DIALECT_DIRECT);
    assert_int_equal(dialect_for(CIP_PLC_LGX, "1,0"), CIP_DIALECT_DIRECT);
}


static void test_a_family_that_cannot_bridge_rejects_dhp(void **state) {
    (void)state;

    /* a DH+ segment against a family with no DH+ row is a bad path, not a direct one. */
    assert_true(rejects(CIP_PLC_LGX, "1,2,A:27:1"));
    assert_true(rejects(CIP_PLC_LGX_PCCC, "1,2,A:27:1"));
    assert_true(rejects(CIP_PLC_MICRO800, "1,2,A:27:1"));
    assert_true(rejects(CIP_PLC_GENERIC, "1,2,A:27:1"));
}


static void test_an_unparseable_path_is_rejected(void **state) {
    (void)state;

    assert_true(rejects(CIP_PLC_PLC5, "not a path at all"));

    /* a DH+ segment must be last. */
    assert_true(rejects(CIP_PLC_PLC5, "1,A:27:1,2"));
}


static void test_a_path_bridges_when_it_leaves_the_chassis(void **state) {
    (void)state;

    /* leaves the chassis: IP out port 2 or 3, DH+, or a pair whose port is not the backplane */
    assert_true(bridges("1,4,18,10.206.1.36"));
    assert_true(bridges("1,2,19,10.206.1.38"));
    assert_true(bridges("1,2,A:27:1"));
    assert_true(bridges("1,3,2,5"));
    assert_true(bridges("1,4,18,10.206.1.39,1,0"));

    /* stays on the backplane, or is not a path at all */
    assert_true(!bridges("1,4"));
    assert_true(!bridges("1,0,1,2"));
    assert_true(!bridges(""));
    assert_true(!bridges("1,x"));
}


/*
 * The create-time path rules, which no simulator can check: ab_server has no CompactLogix
 * mode and refuses a ControlLogix connection without its configured path.  Nothing listens
 * on the gateway, so a create that passes the checks returns a pending tag id.
 */
static int32_t create_status(const char *attribs) {
    int32_t tag = plc_tag_create(attribs, 0);

    if(tag > 0) {
        plc_tag_destroy(tag);
        return PLCTAG_STATUS_OK;
    }

    return tag;
}


static void test_the_path_rules(void **state) {
    (void)state;

    /* a ControlLogix CPU sits behind a communication module, so it needs its slot. */
    assert_int_equal(create_status("protocol=ab-eip&gateway=127.0.0.1:1&plc=controllogix&name=X"), PLCTAG_ERR_BAD_PARAM);
    assert_int_equal(create_status("protocol=ab-eip&gateway=127.0.0.1:1&plc=lgx&name=X"), PLCTAG_ERR_BAD_PARAM);

    /* a CompactLogix and an NJ/NX answer on their own Ethernet port. */
    assert_int_equal(create_status("protocol=ab-eip&gateway=127.0.0.1:1&plc=compactlogix&name=X"), PLCTAG_STATUS_OK);
    assert_int_equal(create_status("protocol=ab-eip&gateway=127.0.0.1:1&plc=omron-njnx&name=X"), PLCTAG_STATUS_OK);

    /* a path that does not parse, and one that encodes too long. */
    assert_int_equal(create_status("protocol=ab-eip&gateway=127.0.0.1:1&path=1,zz&plc=controllogix&name=X"),
                     PLCTAG_ERR_BAD_PARAM);
    assert_int_equal(create_status("protocol=ab-eip&gateway=127.0.0.1:1&path=1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,"
                                   "1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,"
                                   "1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,"
                                   "1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,"
                                   "0&plc=controllogix&name=X"),
                     PLCTAG_ERR_TOO_LARGE);

    plc_tag_shutdown();
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
        cmocka_unit_test(test_a_family_that_cannot_bridge_rejects_dhp),
        cmocka_unit_test(test_an_unparseable_path_is_rejected),
        cmocka_unit_test(test_a_path_bridges_when_it_leaves_the_chassis),
        cmocka_unit_test(test_the_path_rules),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
