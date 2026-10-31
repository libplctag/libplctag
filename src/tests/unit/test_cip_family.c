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
 * The CIP families' own invariants, and how one is picked.
 *
 * A wrong family is silent: a spelling, an Identity reply or a DH+ path that picks the wrong
 * one shows up as one PLC behaving like another rather than as an error, and no tag-creating
 * test can see that without a PLC of every family.  So the picking is checked directly.
 */

#include "mini_mock.h"

#include <libplctag/api/libplctag.h>

#include <libplctag/modules/cip/cip_tag_create.h>
#include <libplctag/modules/cip/family.h>
#include <libplctag/modules/cip/path.h>
#include <libplctag/modules/cip/plc_type.h>
#include <utils/attr.h>


/* one spelling per family, and the PLC type it must come back as. */
static const struct {
    const char *plc;
    cip_plc_type_t plc_type;
    bool has_dhp;
} spellings[] = {
    {"plc5", CIP_PLC_PLC5, true},          {"slc500", CIP_PLC_SLC, true},         {"micrologix", CIP_PLC_MLGX, true},
    {"lgxpccc", CIP_PLC_LGX_PCCC, false},  {"controllogix", CIP_PLC_LGX, false},  {"micro800", CIP_PLC_MICRO800, false},
    {"omron-njnx", CIP_PLC_OMRON_NJNX, false}, {"generic", CIP_PLC_GENERIC, false},
};

#define NUM_SPELLINGS (sizeof(spellings) / sizeof(spellings[0]))


static const cip_family_t *family_named(const char *plc) {
    const cip_family_t *family = NULL;
    attr attribs = attr_create();

    assert_non_null(attribs);
    assert_int_equal(attr_set_str(attribs, "plc", plc), PLCTAG_STATUS_OK);

    family = cip_family_from_attributes(attribs);

    attr_destroy(attribs);

    return family;
}


static void test_every_spelling_picks_its_family(void **state) {
    (void)state;

    for(size_t i = 0; i < NUM_SPELLINGS; i++) {
        const cip_family_t *family = family_named(spellings[i].plc);

        assert_non_null(family);
        assert_int_equal(family->plc_type, spellings[i].plc_type);
        assert_true(!family->via_dhp);
    }

    /* the other spellings of one family are the same family. */
    assert_true(family_named("compactlogix") == family_named("lgx"));
    assert_true(family_named("slc") == family_named("slc500"));
    assert_true(family_named("nx1p2") == family_named("omron-njnx"));

    assert_null(family_named("no such plc"));
}


/* everything the connection and the tag setup read without checking. */
static void test_every_family_is_complete(void **state) {
    (void)state;

    for(size_t i = 0; i < NUM_SPELLINGS; i++) {
        const cip_family_t *family = family_named(spellings[i].plc);
        const cip_family_t *forms[2] = {family, family->dhp};

        for(size_t f = 0; f < 2 && forms[f]; f++) {
            assert_non_null(forms[f]->name);
            assert_non_null(forms[f]->setup_tag);
            assert_true(forms[f]->max_payload_capacity > 0 && forms[f]->max_payload_capacity <= CIP_FAMILY_MAX_PAYLOAD_CAPACITY);
            assert_true(forms[f]->fo_conn_size > 0);
            assert_true(forms[f]->min_payload_size > 0);
            assert_true(forms[f]->use_connected_msg >= -1 && forms[f]->use_connected_msg <= 1);
        }
    }
}


/* only PLC/5, SLC and MicroLogix can be reached across DH+, and that form is always connected. */
static void test_only_dhp_capable_families_have_a_dhp_form(void **state) {
    (void)state;

    for(size_t i = 0; i < NUM_SPELLINGS; i++) {
        const cip_family_t *family = family_named(spellings[i].plc);

        if(!spellings[i].has_dhp) {
            assert_null(family->dhp);
            continue;
        }

        assert_non_null(family->dhp);
        assert_true(family->dhp->via_dhp);
        assert_null(family->dhp->dhp);
        assert_int_equal(family->dhp->plc_type, family->plc_type);
        assert_true(family->dhp->setup_tag == family->setup_tag);
        assert_int_equal((int)family->dhp->use_connected_msg, 1);
    }
}


/*
 * Whether a path takes a family's DH+ form, the way connection creation decides it: the
 * family can bridge DH+ exactly when it has a DH+ form.
 *
 * The simulator suite cannot reach it -- ab_server is not a DH+ bridge -- so only hardware
 * produces a DH+ connection, which is exactly why it is pinned here.
 */
static int parse_path(const char *plc, const char *path, int *is_dhp, bool *is_bridged) {
    uint8_t encoded[MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN];
    int encoded_size = (int)sizeof(encoded);
    int needs_connection = 0;
    uint16_t dhp_dest = 0;
    int kind = (family_named(plc)->dhp ? CIP_PLC_KIND_DHP_CAPABLE : CIP_PLC_KIND_OTHER);

    *is_dhp = 0;
    *is_bridged = false;

    return cip_encode_path(path, &needs_connection, kind, &encoded[0], &encoded_size, is_dhp, &dhp_dest, is_bridged);
}


static bool takes_dhp(const char *plc, const char *path) {
    int is_dhp = 0;
    bool is_bridged = false;

    assert_int_equal(parse_path(plc, path, &is_dhp, &is_bridged), PLCTAG_STATUS_OK);

    return is_dhp != 0;
}


static bool rejects(const char *plc, const char *path) {
    int is_dhp = 0;
    bool is_bridged = false;

    return parse_path(plc, path, &is_dhp, &is_bridged) != PLCTAG_STATUS_OK;
}


static bool bridges(const char *path) {
    int is_dhp = 0;
    bool is_bridged = false;

    return parse_path("plc5", path, &is_dhp, &is_bridged) == PLCTAG_STATUS_OK && is_bridged;
}


static void test_a_dhp_path_takes_the_dhp_form(void **state) {
    (void)state;

    /* the form the hardware tests use: a chassis path, then port:source:destination. */
    assert_true(takes_dhp("plc5", "1,2,A:27:1"));
    assert_true(takes_dhp("slc500", "1,2,A:27:1"));
    assert_true(takes_dhp("micrologix", "1,2,B:27:1"));
}


static void test_an_ordinary_path_is_direct(void **state) {
    (void)state;

    assert_true(!takes_dhp("plc5", ""));
    assert_true(!takes_dhp("slc500", "1,0"));
    assert_true(!takes_dhp("controllogix", "1,0"));
}


static void test_a_family_that_cannot_bridge_rejects_dhp(void **state) {
    (void)state;

    /* a DH+ segment against a family with no DH+ form is a bad path, not a direct one. */
    assert_true(rejects("controllogix", "1,2,A:27:1"));
    assert_true(rejects("lgxpccc", "1,2,A:27:1"));
    assert_true(rejects("micro800", "1,2,A:27:1"));
    assert_true(rejects("generic", "1,2,A:27:1"));
}


static void test_an_unparseable_path_is_rejected(void **state) {
    (void)state;

    assert_true(rejects("plc5", "not a path at all"));

    /* a DH+ segment must be last. */
    assert_true(rejects("plc5", "1,A:27:1,2"));
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

    /* with no plc the connection identifies the device, except across DH+, where it cannot. */
    assert_int_equal(create_status("protocol=ab-eip&gateway=127.0.0.1:1&name=X"), PLCTAG_STATUS_OK);
    assert_int_equal(create_status("protocol=ab-eip&gateway=127.0.0.1:1&path=1,2,A:27:1&name=X"), PLCTAG_ERR_BAD_PARAM);

    plc_tag_shutdown();
}


static cip_plc_type_t identified_as(uint16_t vendor_id, const char *product_name, bool has_path) {
    const cip_family_t *family = NULL;

    if(cip_family_from_identity(vendor_id, product_name, has_path, &family) != PLCTAG_STATUS_OK) { return CIP_PLC_NONE; }

    return family->plc_type;
}


/* the product names are Identity replies from the bench hardware, pylogix and pycomm3. */
static void test_identity_picks_the_family(void **state) {
    (void)state;

    assert_int_equal(identified_as(1, "1756-L81E/B", false), CIP_PLC_LGX);
    assert_int_equal(identified_as(1, "1756-L55/A 1756-M22/A LOGIX5555", true), CIP_PLC_LGX);
    assert_int_equal(identified_as(1, "1756-L61/B LOGIX5561", true), CIP_PLC_LGX);
    assert_int_equal(identified_as(1, "PLC-5/30 C/K - 1785-ENET 2.17 ", false), CIP_PLC_PLC5);
    assert_int_equal(identified_as(1, "1763-L16BWA B/12.00", false), CIP_PLC_MLGX);
    assert_int_equal(identified_as(1, "1769-L23E-QBFC1 LOGIX5323E-QBFC1", false), CIP_PLC_LGX);
    assert_int_equal(identified_as(1, "1769-L30ER/A LOGIX5330ER", false), CIP_PLC_LGX);
    assert_int_equal(identified_as(1, "5069-L310ER/A", false), CIP_PLC_LGX);
    assert_int_equal(identified_as(1, "2080-LC50-48QWBS", false), CIP_PLC_MICRO800);

    /* a chassis module is not a CPU, with or without a path that stops at it. */
    assert_int_equal(identified_as(1, "1756-ENBT/A", false), CIP_PLC_NONE);
    assert_int_equal(identified_as(1, "1756-ENBT/A", true), CIP_PLC_NONE);

    /* anything else is generic CIP, which serves only special tags. */
    assert_int_equal(identified_as(1, "1734-AENT/B", false), CIP_PLC_GENERIC);
    assert_int_equal(identified_as(99, "1756-L81E/B", false), CIP_PLC_GENERIC);
    assert_int_equal(identified_as(47, "NX102-9000", false), CIP_PLC_GENERIC);
}


/*
 * Whether requests are connected is the family's, because the connection is created before
 * any tag exists.  Nothing in the simulator suite distinguishes a wrong answer here from a
 * working one until the PLC refuses the framing, so pin the rule.
 */
static void test_the_family_settles_connected_messaging(void **state) {
    attr attribs = attr_create();

    (void)state;

    assert_non_null(attribs);

    /* the one family that defers to the tag string, and what it does with no answer. */
    assert_true(cip_family_use_connected_msg(family_named("controllogix"), attribs));
    assert_int_equal(attr_set_str(attribs, "use_connected_msg", "0"), PLCTAG_STATUS_OK);
    assert_true(!cip_family_use_connected_msg(family_named("controllogix"), attribs));

    /* and a family that does not defer ignores the same attribute. */
    assert_true(cip_family_use_connected_msg(family_named("micro800"), attribs));

    attr_destroy(attribs);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_every_spelling_picks_its_family),
        cmocka_unit_test(test_every_family_is_complete),
        cmocka_unit_test(test_only_dhp_capable_families_have_a_dhp_form),
        cmocka_unit_test(test_the_family_settles_connected_messaging),
        cmocka_unit_test(test_a_dhp_path_takes_the_dhp_form),
        cmocka_unit_test(test_an_ordinary_path_is_direct),
        cmocka_unit_test(test_a_family_that_cannot_bridge_rejects_dhp),
        cmocka_unit_test(test_an_unparseable_path_is_rejected),
        cmocka_unit_test(test_a_path_bridges_when_it_leaves_the_chassis),
        cmocka_unit_test(test_the_path_rules),
        cmocka_unit_test(test_identity_picks_the_family),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
