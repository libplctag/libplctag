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
 * Attribute names are case-insensitive.  Tag-string keys used to match exactly, so a key
 * like "Gateway" or "ELEM_COUNT" was silently ignored.
 *
 * Links the real library; no mocking.
 */

#include "mini_mock.h"

#include <libplctag/lib/libplctag.h>
#include <platform.h>
#include <utils/attr.h>


/* A key is found whatever case the tag string and the lookup use. */
static void test_attr_lookup_ignores_case(void **state) {
    (void)state;

    attr attrs = attr_create_from_str("Gateway=10.1.2.3&ELEM_COUNT=5");

    assert_non_null(attrs);
    assert_int_equal(str_cmp(attr_get_str(attrs, "gateway", "missing"), "10.1.2.3"), 0);
    assert_int_equal(attr_get_int(attrs, "elem_count", 0), 5);
    assert_int_equal(attr_get_int(attrs, "Elem_Count", 0), 5);

    attr_destroy(attrs);
}


/* Keys that differ only in case are one attribute: the last one wins, as for exact duplicates. */
static void test_attr_case_duplicates_collapse(void **state) {
    (void)state;

    attr attrs = attr_create_from_str("elem_count=1&ELEM_COUNT=2");

    assert_non_null(attrs);
    assert_int_equal(attr_get_int(attrs, "elem_count", 0), 2);

    attr_destroy(attrs);
}


/* End to end: a tag created with upper- and mixed-case keys works. */
static void test_create_with_upper_case_keys(void **state) {
    (void)state;

    int32_t tag = plc_tag_create("PROTOCOL=system&Name=version", 0);

    assert_true(tag > 0);
    assert_int_equal(plc_tag_read(tag, 0), PLCTAG_STATUS_OK);
    assert_int_equal(plc_tag_destroy(tag), PLCTAG_STATUS_OK);

    plc_tag_shutdown();
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_attr_lookup_ignores_case),
        cmocka_unit_test(test_attr_case_duplicates_collapse),
        cmocka_unit_test(test_create_with_upper_case_keys),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
