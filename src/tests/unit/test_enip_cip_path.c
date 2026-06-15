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

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#include "cmocka.h"

#include <libplctag/protocols/enip/enip_cip.h>
#include <utils/arena.h>

#define ARENA_SIZE ((size_t)4096)

static int setup(void **state) {
    Arena *a = malloc(sizeof(Arena));
    if(arena_init(a, ARENA_SIZE) != 0) { return -1; }
    *state = a;
    return 0;
}

static int teardown(void **state) {
    Arena *a = (Arena *)*state;
    arena_free(a);
    free(a);
    return 0;
}

/* "TestTag" -> 0x91 0x07 "TestTag" 0x00 (odd length 7, padded to even) */
static void test_simple_name(void **state) {
    Arena *a = (Arena *)*state;

    Bytes path = enip_cip_encode_path(a, "TestTag");

    uint8_t expected[] = {0x91, 0x07, 'T', 'e', 's', 't', 'T', 'a', 'g', 0x00};

    assert_false(bytes_is_null(path));
    assert_int_equal(path.len, sizeof(expected));
    assert_memory_equal(path.data, expected, sizeof(expected));
}

/* "Abcd" -> 0x91 0x04 'A' 'b' 'c' 'd' (even length, no pad) */
static void test_even_length_name(void **state) {
    Arena *a = (Arena *)*state;

    Bytes path = enip_cip_encode_path(a, "Abcd");

    uint8_t expected[] = {0x91, 0x04, 'A', 'b', 'c', 'd'};

    assert_false(bytes_is_null(path));
    assert_int_equal(path.len, sizeof(expected));
    assert_memory_equal(path.data, expected, sizeof(expected));
}

/* "MyArray[3]" -> 0x91 0x07 "MyArray" (pad) 0x00, then 0x28 0x03 (1-byte index) */
static void test_array_index_small(void **state) {
    Arena *a = (Arena *)*state;

    Bytes path = enip_cip_encode_path(a, "MyArray[3]");

    uint8_t expected[] = {0x91, 0x07, 'M', 'y', 'A', 'r', 'r', 'a', 'y', 0x00, 0x28, 0x03};

    assert_false(bytes_is_null(path));
    assert_int_equal(path.len, sizeof(expected));
    assert_memory_equal(path.data, expected, sizeof(expected));
}

/* "Arr[1000]" -> 0x91 0x03 "Arr" 0x00, then 0x29 0x00 (2-byte LE index) 1000=0x03E8 */
static void test_array_index_medium(void **state) {
    Arena *a = (Arena *)*state;

    Bytes path = enip_cip_encode_path(a, "Arr[1000]");

    uint8_t expected[] = {0x91, 0x03, 'A', 'r', 'r', 0x00, 0x29, 0x00, 0xE8, 0x03};

    assert_false(bytes_is_null(path));
    assert_int_equal(path.len, sizeof(expected));
    assert_memory_equal(path.data, expected, sizeof(expected));
}

/* "MyUDT.Member" -> 0x91 0x05 "MyUDT" (pad) 0x00 + 0x91 0x06 "Member" */
static void test_member_navigation(void **state) {
    Arena *a = (Arena *)*state;

    Bytes path = enip_cip_encode_path(a, "MyUDT.Member");

    uint8_t expected[] = {
        0x91, 0x05, 'M', 'y', 'U', 'D', 'T', 0x00,
        0x91, 0x06, 'M', 'e', 'm', 'b', 'e', 'r',
    };

    assert_false(bytes_is_null(path));
    assert_int_equal(path.len, sizeof(expected));
    assert_memory_equal(path.data, expected, sizeof(expected));
}

static void test_null_name_fails(void **state) {
    Arena *a = (Arena *)*state;

    Bytes path = enip_cip_encode_path(a, NULL);
    assert_true(bytes_is_null(path));
}

static void test_empty_name_fails(void **state) {
    Arena *a = (Arena *)*state;

    Bytes path = enip_cip_encode_path(a, "");
    assert_true(bytes_is_null(path));
}

static void test_invalid_array_index_fails(void **state) {
    Arena *a = (Arena *)*state;

    Bytes path = enip_cip_encode_path(a, "Tag[");
    assert_true(bytes_is_null(path));
}

/* ============================================================================
 * enip_cip_encode_route
 * ============================================================================ */

static void test_route_single_element(void **state) {
    Arena *a = (Arena *)*state;

    Bytes route = enip_cip_encode_route(a, "1,0");

    uint8_t expected[] = {0x01, 0x00};

    assert_false(bytes_is_null(route));
    assert_int_equal(route.len, sizeof(expected));
    assert_memory_equal(route.data, expected, sizeof(expected));
}

static void test_route_multi_element(void **state) {
    Arena *a = (Arena *)*state;

    Bytes route = enip_cip_encode_route(a, "1,0,2,5");

    uint8_t expected[] = {0x01, 0x00, 0x02, 0x05};

    assert_false(bytes_is_null(route));
    assert_int_equal(route.len, sizeof(expected));
    assert_memory_equal(route.data, expected, sizeof(expected));
}

static void test_route_single_number_no_link(void **state) {
    Arena *a = (Arena *)*state;

    Bytes route = enip_cip_encode_route(a, "1");

    uint8_t expected[] = {0x01, 0x00};

    assert_false(bytes_is_null(route));
    assert_int_equal(route.len, sizeof(expected));
    assert_memory_equal(route.data, expected, sizeof(expected));
}

static void test_route_null_fails(void **state) {
    Arena *a = (Arena *)*state;

    Bytes route = enip_cip_encode_route(a, NULL);
    assert_true(bytes_is_null(route));
}

static void test_route_empty_fails(void **state) {
    Arena *a = (Arena *)*state;

    Bytes route = enip_cip_encode_route(a, "");
    assert_true(bytes_is_null(route));
}

static void test_route_malformed_fails(void **state) {
    Arena *a = (Arena *)*state;

    Bytes route = enip_cip_encode_route(a, "1,a");
    assert_true(bytes_is_null(route));
}

static void test_route_out_of_range_fails(void **state) {
    Arena *a = (Arena *)*state;

    Bytes route = enip_cip_encode_route(a, "1,256");
    assert_true(bytes_is_null(route));
}

/* ============================================================================
 * enip_cip_read
 * ============================================================================ */

static void test_cip_read_layout(void **state) {
    Arena *a = (Arena *)*state;

    Bytes path = enip_cip_encode_path(a, "Abcd");
    assert_false(bytes_is_null(path));

    Bytes req = enip_cip_read(a, path, 1);

    uint8_t expected[] = {0x4C, 0x02, 0x91, 0x04, 'A', 'b', 'c', 'd', 0x01, 0x00};

    assert_false(bytes_is_null(req));
    assert_int_equal(req.len, sizeof(expected));
    assert_memory_equal(req.data, expected, sizeof(expected));
}

static void test_cip_read_null_path_fails(void **state) {
    Arena *a = (Arena *)*state;

    Bytes req = enip_cip_read(a, bytes_null(), 1);
    assert_true(bytes_is_null(req));
}

static void test_cip_read_odd_path_fails(void **state) {
    Arena *a = (Arena *)*state;

    uint8_t raw[] = {0x91, 0x01, 'A'};
    Bytes path = bytes_from_buf(raw, sizeof(raw));

    Bytes req = enip_cip_read(a, path, 1);
    assert_true(bytes_is_null(req));
}

/* ============================================================================
 * enip_cip_parse_reply
 * ============================================================================ */

static void test_parse_reply_success_no_ext(void **state) {
    uint8_t raw[] = {0xCC, 0x00, 0x00, 0x00, 0xC3, 0x01, 0x02, 0x03, 0x04};
    Bytes in = bytes_from_buf(raw, sizeof(raw));

    cip_reply_t reply;
    assert_true(enip_cip_parse_reply(in, &reply));

    assert_int_equal(reply.service, 0xCC);
    assert_int_equal(reply.status, 0x00);
    assert_int_equal(reply.ext_status, 0);
    assert_int_equal(reply.data.len, 5);
    assert_memory_equal(reply.data.data, &raw[4], 5);
}

static void test_parse_reply_with_ext_status(void **state) {
    uint8_t raw[] = {0xCC, 0x00, 0x05, 0x01, 0x34, 0x12, 0xAA, 0xBB};
    Bytes in = bytes_from_buf(raw, sizeof(raw));

    cip_reply_t reply;
    assert_true(enip_cip_parse_reply(in, &reply));

    assert_int_equal(reply.service, 0xCC);
    assert_int_equal(reply.status, 0x05);
    assert_int_equal(reply.ext_status, 0x1234);
    assert_int_equal(reply.data.len, 2);
    assert_memory_equal(reply.data.data, &raw[6], 2);
}

static void test_parse_reply_too_short_fails(void **state) {
    uint8_t raw[] = {0xCC, 0x00, 0x00};
    Bytes in = bytes_from_buf(raw, sizeof(raw));

    cip_reply_t reply;
    assert_false(enip_cip_parse_reply(in, &reply));
}

static void test_parse_reply_truncated_ext_fails(void **state) {
    uint8_t raw[] = {0xCC, 0x00, 0x05, 0x01, 0x34};
    Bytes in = bytes_from_buf(raw, sizeof(raw));

    cip_reply_t reply;
    assert_false(enip_cip_parse_reply(in, &reply));
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_simple_name, setup, teardown),
        cmocka_unit_test_setup_teardown(test_even_length_name, setup, teardown),
        cmocka_unit_test_setup_teardown(test_array_index_small, setup, teardown),
        cmocka_unit_test_setup_teardown(test_array_index_medium, setup, teardown),
        cmocka_unit_test_setup_teardown(test_member_navigation, setup, teardown),
        cmocka_unit_test_setup_teardown(test_null_name_fails, setup, teardown),
        cmocka_unit_test_setup_teardown(test_empty_name_fails, setup, teardown),
        cmocka_unit_test_setup_teardown(test_invalid_array_index_fails, setup, teardown),
        cmocka_unit_test_setup_teardown(test_route_single_element, setup, teardown),
        cmocka_unit_test_setup_teardown(test_route_multi_element, setup, teardown),
        cmocka_unit_test_setup_teardown(test_route_single_number_no_link, setup, teardown),
        cmocka_unit_test_setup_teardown(test_route_null_fails, setup, teardown),
        cmocka_unit_test_setup_teardown(test_route_empty_fails, setup, teardown),
        cmocka_unit_test_setup_teardown(test_route_malformed_fails, setup, teardown),
        cmocka_unit_test_setup_teardown(test_route_out_of_range_fails, setup, teardown),
        cmocka_unit_test_setup_teardown(test_cip_read_layout, setup, teardown),
        cmocka_unit_test_setup_teardown(test_cip_read_null_path_fails, setup, teardown),
        cmocka_unit_test_setup_teardown(test_cip_read_odd_path_fails, setup, teardown),
        cmocka_unit_test(test_parse_reply_success_no_ext),
        cmocka_unit_test(test_parse_reply_with_ext_status),
        cmocka_unit_test(test_parse_reply_too_short_fails),
        cmocka_unit_test(test_parse_reply_truncated_ext_fails),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
