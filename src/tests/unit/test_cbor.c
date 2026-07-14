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
 * Unit tests for utils/cbor.c against RFC 8949 Appendix A examples, plus the
 * size/write agreement and bounds-checking behavior the format/schema
 * subsystem (ENIP-METADATA-AND-DISCOVERY-DESIGN.md §0) depends on.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "cmocka.h"

#include <utils/cbor.h>

/* ============================================================================
 * RFC 8949 Appendix A examples
 * ============================================================================ */

static void test_uint_direct(void **state) {
    (void)state;
    uint8_t buf[8];
    Bytes dest = bytes_from_buf(buf, sizeof(buf));
    size_t pos = 0;

    assert_true(cbor_write_uint(dest, &pos, 10));
    assert_int_equal(pos, 1);
    assert_int_equal(buf[0], 0x0a);
}

static void test_uint_boundary_23_24(void **state) {
    (void)state;
    uint8_t buf[8];
    Bytes dest = bytes_from_buf(buf, sizeof(buf));
    size_t pos = 0;

    /* 23 is the last value that encodes directly in the initial byte. */
    assert_true(cbor_write_uint(dest, &pos, 23));
    assert_int_equal(pos, 1);
    assert_int_equal(buf[0], 23);
    assert_int_equal(cbor_size_uint(23), 1);

    /* 24 is the first value needing a following length byte (0x18). */
    pos = 0;
    assert_true(cbor_write_uint(dest, &pos, 24));
    assert_int_equal(pos, 2);
    assert_int_equal(buf[0], 0x18);
    assert_int_equal(buf[1], 24);
    assert_int_equal(cbor_size_uint(24), 2);
}

static void test_uint_25(void **state) {
    (void)state;
    uint8_t buf[8];
    Bytes dest = bytes_from_buf(buf, sizeof(buf));
    size_t pos = 0;

    assert_true(cbor_write_uint(dest, &pos, 25));
    uint8_t expected[] = {0x18, 0x19};
    assert_int_equal(pos, sizeof(expected));
    assert_memory_equal(buf, expected, sizeof(expected));
}

static void test_uint_1000000(void **state) {
    (void)state;
    uint8_t buf[8];
    Bytes dest = bytes_from_buf(buf, sizeof(buf));
    size_t pos = 0;

    assert_true(cbor_write_uint(dest, &pos, 1000000));
    uint8_t expected[] = {0x1a, 0x00, 0x0f, 0x42, 0x40};
    assert_int_equal(pos, sizeof(expected));
    assert_memory_equal(buf, expected, sizeof(expected));
}

static void test_negint_minus_1(void **state) {
    (void)state;
    uint8_t buf[8];
    Bytes dest = bytes_from_buf(buf, sizeof(buf));
    size_t pos = 0;

    assert_true(cbor_write_int(dest, &pos, -1));
    assert_int_equal(pos, 1);
    assert_int_equal(buf[0], 0x20);
    assert_int_equal(cbor_size_int(-1), 1);
}

static void test_negint_minus_500(void **state) {
    (void)state;
    uint8_t buf[8];
    Bytes dest = bytes_from_buf(buf, sizeof(buf));
    size_t pos = 0;

    /* RFC 8949 §A: -500 -> 0x3901f3 */
    assert_true(cbor_write_int(dest, &pos, -500));
    uint8_t expected[] = {0x39, 0x01, 0xf3};
    assert_int_equal(pos, sizeof(expected));
    assert_memory_equal(buf, expected, sizeof(expected));
}

static void test_text_ietf(void **state) {
    (void)state;
    uint8_t buf[16];
    Bytes dest = bytes_from_buf(buf, sizeof(buf));
    size_t pos = 0;

    /* RFC 8949 §A: "IETF" -> 0x6449455446 */
    assert_true(cbor_write_text(dest, &pos, "IETF", 4));
    uint8_t expected[] = {0x64, 'I', 'E', 'T', 'F'};
    assert_int_equal(pos, sizeof(expected));
    assert_memory_equal(buf, expected, sizeof(expected));
    assert_int_equal(cbor_size_text(4), sizeof(expected));
}

static void test_array_123(void **state) {
    (void)state;
    uint8_t buf[16];
    Bytes dest = bytes_from_buf(buf, sizeof(buf));
    size_t pos = 0;

    /* RFC 8949 §A: [1, 2, 3] -> 0x83010203 */
    assert_true(cbor_write_array_header(dest, &pos, 3));
    assert_true(cbor_write_uint(dest, &pos, 1));
    assert_true(cbor_write_uint(dest, &pos, 2));
    assert_true(cbor_write_uint(dest, &pos, 3));

    uint8_t expected[] = {0x83, 0x01, 0x02, 0x03};
    assert_int_equal(pos, sizeof(expected));
    assert_memory_equal(buf, expected, sizeof(expected));
}

static void test_map_a1_b2(void **state) {
    (void)state;
    uint8_t buf[16];
    Bytes dest = bytes_from_buf(buf, sizeof(buf));
    size_t pos = 0;

    /* RFC 8949 §A: {"a": 1, "b": 2} -> 0xa26161016162 02 */
    assert_true(cbor_write_map_header(dest, &pos, 2));
    assert_true(cbor_write_text(dest, &pos, "a", 1));
    assert_true(cbor_write_uint(dest, &pos, 1));
    assert_true(cbor_write_text(dest, &pos, "b", 1));
    assert_true(cbor_write_uint(dest, &pos, 2));

    uint8_t expected[] = {0xa2, 0x61, 'a', 0x01, 0x61, 'b', 0x02};
    assert_int_equal(pos, sizeof(expected));
    assert_memory_equal(buf, expected, sizeof(expected));
}

static void test_bytes_header(void **state) {
    (void)state;
    uint8_t buf[16];
    Bytes dest = bytes_from_buf(buf, sizeof(buf));
    size_t pos = 0;

    uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    /* RFC 8949 §A: h'01020304' -> 0x4401020304 */
    assert_true(cbor_write_bytes(dest, &pos, payload, sizeof(payload)));
    uint8_t expected[] = {0x44, 0x01, 0x02, 0x03, 0x04};
    assert_int_equal(pos, sizeof(expected));
    assert_memory_equal(buf, expected, sizeof(expected));
    assert_int_equal(cbor_size_bytes(4), sizeof(expected));
}

/* ============================================================================
 * size/write agreement -- the format/schema subsystem sizes a caller's buffer
 * from cbor_size_* alone, then fills it with cbor_write_*; the two must
 * always land on the same byte count.
 * ============================================================================ */

static void test_size_matches_write_for_map(void **state) {
    (void)state;
    uint8_t buf[64];
    Bytes dest = bytes_from_buf(buf, sizeof(buf));
    size_t pos = 0;

    size_t predicted = cbor_size_map_header(2) + cbor_size_text(1) + cbor_size_uint(1) + cbor_size_text(1)
                      + cbor_size_uint(2);

    assert_true(cbor_write_map_header(dest, &pos, 2));
    assert_true(cbor_write_text(dest, &pos, "a", 1));
    assert_true(cbor_write_uint(dest, &pos, 1));
    assert_true(cbor_write_text(dest, &pos, "b", 1));
    assert_true(cbor_write_uint(dest, &pos, 2));

    assert_int_equal(pos, predicted);
}

/* ============================================================================
 * Bounds checking -- a too-small destination must fail cleanly and leave
 * *pos unchanged (the caller relies on this to detect PLCTAG_ERR_TOO_SMALL
 * without partial, unusable output).
 * ============================================================================ */

static void test_write_too_small_leaves_pos_unchanged(void **state) {
    (void)state;
    uint8_t buf[1];
    Bytes dest = bytes_from_buf(buf, sizeof(buf));
    size_t pos = 0;

    assert_false(cbor_write_text(dest, &pos, "ab", 2));
    assert_int_equal(pos, 0);
}

static void test_write_header_too_small_leaves_pos_unchanged(void **state) {
    (void)state;
    uint8_t buf[1];
    Bytes dest = bytes_from_buf(buf, sizeof(buf));
    size_t pos = 0;

    /* 1000000 needs a 5-byte header; a 1-byte buffer must reject it. */
    assert_false(cbor_write_uint(dest, &pos, 1000000));
    assert_int_equal(pos, 0);
}

static void test_write_partial_room_for_header_only_fails(void **state) {
    (void)state;
    /* Room for the 1-byte text header but not the payload byte. */
    uint8_t buf[1];
    Bytes dest = bytes_from_buf(buf, sizeof(buf));
    size_t pos = 0;

    assert_false(cbor_write_text(dest, &pos, "x", 1));
    assert_int_equal(pos, 0);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_uint_direct),
        cmocka_unit_test(test_uint_boundary_23_24),
        cmocka_unit_test(test_uint_25),
        cmocka_unit_test(test_uint_1000000),
        cmocka_unit_test(test_negint_minus_1),
        cmocka_unit_test(test_negint_minus_500),
        cmocka_unit_test(test_text_ietf),
        cmocka_unit_test(test_array_123),
        cmocka_unit_test(test_map_a1_b2),
        cmocka_unit_test(test_bytes_header),
        cmocka_unit_test(test_size_matches_write_for_map),
        cmocka_unit_test(test_write_too_small_leaves_pos_unchanged),
        cmocka_unit_test(test_write_header_too_small_leaves_pos_unchanged),
        cmocka_unit_test(test_write_partial_room_for_header_only_fails),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
