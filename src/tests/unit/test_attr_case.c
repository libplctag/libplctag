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
 * Attribute names ignore case and '_'.  Tag-string keys used to match exactly, so a key
 * like "Gateway" or "ELEM_COUNT" was silently ignored.  Names are made canonical (lowercase,
 * no '_') before they are stored or looked up.
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


/* '_' does not count: "ElemCount" and "elem_count" are one name. */
static void test_attr_lookup_ignores_underscore(void **state) {
    (void)state;

    attr attrs = attr_create_from_str("ElemCount=5&read__cache_ms=7");

    assert_non_null(attrs);
    assert_int_equal(attr_get_int(attrs, "elem_count", 0), 5);
    assert_int_equal(attr_get_int(attrs, "readcachems", 0), 7);

    attr_destroy(attrs);
}


/* Only ASCII letters fold and only '_' goes; every other byte, including non-ASCII ones, stays. */
static void test_canonical_name(void **state) {
    (void)state;

    char buf[ATTR_NAME_MAX_SIZE];
    char in_place[] = "Elem_Count";

    assert_int_equal(attr_canonical_name(buf, (int32_t)sizeof(buf), "Raw_Tag_Type_Bytes.Length"), 22);
    assert_int_equal(str_cmp(buf, "rawtagtypebytes.length"), 0);

    /* the result is never longer than the input, so it can be made in place. */
    assert_int_equal(attr_canonical_name(in_place, (int32_t)sizeof(in_place), in_place), 9);
    assert_int_equal(str_cmp(in_place, "elemcount"), 0);

    /* overlong UTF-8 for 'A' and '_' is not decoded: it stays two bytes and matches nothing ASCII. */
    assert_int_equal(attr_canonical_name(buf, (int32_t)sizeof(buf), "\xC1\x81\xC1\x9F"), 4);
    assert_int_equal(str_cmp(buf, "\xC1\x81\xC1\x9F"), 0);

    /* too small for the result and its terminator. */
    assert_int_equal(attr_canonical_name(buf, 9, "elem_count"), PLCTAG_ERR_TOO_SMALL);
    assert_int_equal(attr_canonical_name(buf, 10, "elem_count"), 9);
    assert_int_equal(attr_canonical_name(NULL, 10, "elem_count"), PLCTAG_ERR_NULL_PTR);
}


/* The find_or_create_tag key: names canonical and trimmed as the parser trims them, values untouched. */
static void test_canonical_str(void **state) {
    (void)state;

    char *key = attr_canonical_str(" Elem_Count =5&Name=My_Tag&gateway=PLC-1&path=1,0");

    assert_non_null(key);
    assert_int_equal(str_cmp(key, "elemcount=5&name=My_Tag&gateway=PLC-1&path=1,0"), 0);

    mem_free(key);
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


/* End to end: runtime attribute names, and the find_or_create_tag key, ignore case and '_'. */
static void test_runtime_and_reuse_spelling(void **state) {
    (void)state;

    int32_t tag = plc_tag_create("protocol=system&name=version&read_cache_ms=20&find_or_create_tag=1", 0);
    int32_t same = plc_tag_create("Protocol=system&Name=version&ReadCacheMs=20&FIND_OR_CREATE_TAG=1", 0);

    assert_true(tag > 0);
    assert_int_equal(same, tag);

    assert_int_equal(plc_tag_get_int_attribute(tag, "read_cache_ms", -1), 20);
    assert_int_equal(plc_tag_get_int_attribute(tag, "ReadCacheMs", -1), 20);
    assert_int_equal(plc_tag_get_int_attribute(0, "VERSION_MAJOR", -1), plc_tag_get_int_attribute(0, "versionmajor", -2));

    assert_int_equal(plc_tag_destroy(tag), PLCTAG_STATUS_OK);

    plc_tag_shutdown();
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_attr_lookup_ignores_case),
        cmocka_unit_test(test_attr_case_duplicates_collapse),
        cmocka_unit_test(test_attr_lookup_ignores_underscore),
        cmocka_unit_test(test_canonical_name),
        cmocka_unit_test(test_canonical_str),
        cmocka_unit_test(test_create_with_upper_case_keys),
        cmocka_unit_test(test_runtime_and_reuse_spelling),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
