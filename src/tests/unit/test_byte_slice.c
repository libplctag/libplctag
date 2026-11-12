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
 * Unit tests for utils/byte_slice.h.
 *
 * The properties that matter: values round trip in both byte orders, a failure anywhere
 * in a chain sticks to the end and records where it happened, a failed read leaves 0, a
 * failed write leaves the buffer alone, and cut/skip/used never step outside the parent.
 */

#include "mini_mock.h"

#include <stdint.h>
#include <string.h>
#include <utils/byte_slice.h>


static void test_round_trip_both_orders(void **state) {
    uint8_t buf[2 + 2 + 4 + 4 + 8 + 8 + 1] = {0};
    byte_slice_t s = byte_slice_make(buf, (int32_t)sizeof(buf));
    uint16_t u16_le = 0, u16_be = 0;
    uint32_t u32_le = 0, u32_be = 0;
    uint64_t u64_le = 0, u64_be = 0;
    uint8_t u8 = 0;

    (void)state;

    s = byte_slice_write_u16_le(s, 0x1234);
    s = byte_slice_write_u16_be(s, 0x1234);
    s = byte_slice_write_u32_le(s, 0x12345678);
    s = byte_slice_write_u32_be(s, 0x12345678);
    s = byte_slice_write_u64_le(s, 0x0102030405060708ULL);
    s = byte_slice_write_u64_be(s, 0x0102030405060708ULL);
    s = byte_slice_write_u8(s, 0xAB);
    assert_true(!byte_slice_has_error(s));
    assert_int_equal(byte_slice_len(s), 0);

    /* wire order */
    assert_int_equal(buf[0], 0x34);
    assert_int_equal(buf[1], 0x12);
    assert_int_equal(buf[2], 0x12);
    assert_int_equal(buf[3], 0x34);
    assert_int_equal(buf[4], 0x78);
    assert_int_equal(buf[8], 0x12);
    assert_int_equal(buf[12], 0x08);
    assert_int_equal(buf[20], 0x01);

    s = byte_slice_make(buf, (int32_t)sizeof(buf));
    s = byte_slice_read_u16_le(s, &u16_le);
    s = byte_slice_read_u16_be(s, &u16_be);
    s = byte_slice_read_u32_le(s, &u32_le);
    s = byte_slice_read_u32_be(s, &u32_be);
    s = byte_slice_read_u64_le(s, &u64_le);
    s = byte_slice_read_u64_be(s, &u64_be);
    s = byte_slice_read_u8(s, &u8);
    assert_true(!byte_slice_has_error(s));
    assert_int_equal(u16_le, 0x1234);
    assert_int_equal(u16_be, 0x1234);
    assert_int_equal(u32_le, 0x12345678);
    assert_int_equal(u32_be, 0x12345678);
    assert_true(u64_le == 0x0102030405060708ULL);
    assert_true(u64_be == 0x0102030405060708ULL);
    assert_int_equal(u8, 0xAB);
}


static void test_error_sticks_and_records_offset(void **state) {
    uint8_t buf[5] = {1, 2, 3, 4, 5};
    byte_slice_t s = byte_slice_make(buf, (int32_t)sizeof(buf));
    uint16_t a = 0;
    uint32_t b = 0xFFFFFFFF;
    uint8_t c = 0xFF;

    (void)state;

    s = byte_slice_read_u16_le(s, &a); /* ok, offset 2 */
    s = byte_slice_read_u32_le(s, &b); /* only 3 left: fails here */
    s = byte_slice_read_u8(s, &c);     /* would fit, but must not run */

    assert_true(byte_slice_has_error(s));
    assert_int_equal(byte_slice_get_error(s), PLCTAG_ERR_TOO_SMALL);
    assert_int_equal(s.start, 2);
    assert_true(s.data == buf);
    assert_int_equal(a, 0x0201);
    assert_int_equal(b, 0);
    assert_int_equal(c, 0);
    assert_int_equal(byte_slice_len(s), 0);
    assert_null(byte_slice_get_data(s));
}


static void test_failed_write_leaves_buffer(void **state) {
    uint8_t buf[3] = {0xEE, 0xEE, 0xEE};
    byte_slice_t s = byte_slice_make(buf, (int32_t)sizeof(buf));

    (void)state;

    s = byte_slice_write_u32_be(s, 0x11223344);
    s = byte_slice_write_u8(s, 0x55);

    assert_true(byte_slice_has_error(s));
    assert_int_equal(buf[0], 0xEE);
    assert_int_equal(buf[1], 0xEE);
    assert_int_equal(buf[2], 0xEE);
}


static void test_make_rejects_bad_args(void **state) {
    uint8_t buf[1] = {0};

    (void)state;

    assert_int_equal(byte_slice_get_error(byte_slice_make(buf, -1)), PLCTAG_ERR_BAD_PARAM);
    assert_int_equal(byte_slice_get_error(byte_slice_make(NULL, 4)), PLCTAG_ERR_NULL_PTR);
    assert_true(!byte_slice_has_error(byte_slice_make(NULL, 0)));
}


/* A length-prefixed item: the read length feeds cut() with no check in between. */
static void test_cut_skip_length_prefixed(void **state) {
    uint8_t buf[] = {0xAA, 3, 0, 'a', 'b', 'c', 0x42};
    uint8_t name_bytes[] = {'a', 'b', 'c'};
    byte_slice_t s = byte_slice_make(buf, (int32_t)sizeof(buf));
    byte_slice_t name;
    byte_slice_t expected = byte_slice_make(name_bytes, (int32_t)sizeof(name_bytes));
    uint16_t name_len = 0;
    uint8_t tail = 0;

    (void)state;

    s = byte_slice_skip(s, 1);
    s = byte_slice_read_u16_le(s, &name_len);
    byte_slice_cut(s, name_len, &name, &s);
    s = byte_slice_read_u8(s, &tail);

    assert_true(!byte_slice_has_error(s));
    assert_int_equal(name.start, 3);
    assert_int_equal(byte_slice_len(name), 3);
    assert_int_equal(byte_slice_compare(name, expected, INT32_MAX), 0);
    assert_int_equal(tail, 0x42);

    /* declared length past the end: cut fails and both halves get the error */
    buf[1] = 9;
    s = byte_slice_make(buf, (int32_t)sizeof(buf));
    s = byte_slice_skip(s, 1);
    s = byte_slice_read_u16_le(s, &name_len);
    byte_slice_cut(s, name_len, &name, &s);
    assert_true(byte_slice_has_error(s));
    assert_true(byte_slice_has_error(name));
    assert_int_equal(s.start, 3);

    assert_true(byte_slice_has_error(byte_slice_skip(byte_slice_make(buf, 2), -1)));
}


/* Header filled after the payload, then the whole response returned via used(). */
static void test_used_header_after_payload(void **state) {
    uint8_t buf[16] = {0};
    uint8_t other[16] = {0};
    byte_slice_t out = byte_slice_make(buf, (int32_t)sizeof(buf));
    byte_slice_t hdr;
    byte_slice_t body;
    byte_slice_t rest;
    byte_slice_t payload;
    byte_slice_t response;

    (void)state;

    assert_int_equal(byte_slice_cut(out, 2, &hdr, &body), PLCTAG_STATUS_OK);
    assert_int_equal(body.start, 2);
    assert_int_equal(byte_slice_len(hdr), 2);

    rest = byte_slice_write_u8(body, 0xC0);
    rest = byte_slice_write_u16_le(rest, 0xBEEF);
    payload = byte_slice_used(body, rest);
    hdr = byte_slice_write_u16_le(hdr, (uint16_t)byte_slice_len(payload));
    response = byte_slice_used(out, rest);

    assert_true(!byte_slice_has_error(hdr));
    assert_int_equal(byte_slice_len(payload), 3);
    assert_int_equal(response.start, 0);
    assert_int_equal(byte_slice_len(response), 5);
    assert_int_equal(buf[0], 3);
    assert_int_equal(buf[2], 0xC0);
    assert_int_equal(buf[3], 0xEF);

    /* cut past the end: both outs get the error; NULL outs are allowed */
    assert_int_equal(byte_slice_cut(out, 17, &hdr, &body), PLCTAG_ERR_TOO_SMALL);
    assert_true(byte_slice_has_error(hdr));
    assert_true(byte_slice_has_error(body));
    assert_int_equal(byte_slice_cut(out, 16, NULL, &body), PLCTAG_STATUS_OK);
    assert_int_equal(byte_slice_len(body), 0);

    /* a remainder from another buffer, or from before src, is rejected */
    assert_int_equal(byte_slice_get_error(byte_slice_used(out, byte_slice_make(other, 4))), PLCTAG_ERR_BAD_PARAM);
    assert_int_equal(byte_slice_get_error(byte_slice_used(body, out)), PLCTAG_ERR_BAD_PARAM);
}


static void test_compare(void **state) {
    uint8_t abc[] = {'a', 'b', 'c'};
    uint8_t ab[] = {'a', 'b'};
    uint8_t abd[] = {'a', 'b', 'd'};
    byte_slice_t s_abc = byte_slice_make(abc, 3);
    byte_slice_t s_ab = byte_slice_make(ab, 2);
    byte_slice_t s_abd = byte_slice_make(abd, 3);

    (void)state;

    assert_int_equal(byte_slice_compare(s_abc, s_abc, INT32_MAX), 0);
    assert_int_equal(byte_slice_compare(s_ab, s_abc, INT32_MAX), -1);
    assert_int_equal(byte_slice_compare(s_abc, s_ab, INT32_MAX), 1);
    assert_int_equal(byte_slice_compare(s_abc, s_abd, INT32_MAX), -1);
    assert_int_equal(byte_slice_compare(s_abc, s_ab, byte_slice_len(s_ab)), 0); /* prefix */
    assert_int_equal(byte_slice_compare(s_ab, s_abc, byte_slice_len(s_abc)), -1);
    assert_true(byte_slice_compare(byte_slice_make(NULL, -1), byte_slice_make(NULL, -1), INT32_MAX) != 0);
}


/* Leftover after a consumed packet moves to offset 0; the next read appends after it. */
static void test_compress_leftover(void **state) {
    uint8_t buf[8] = {1, 2, 3, 4, 5, 6, 0, 0};
    byte_slice_t in = byte_slice_make(buf, 6);
    byte_slice_t rest = byte_slice_skip(in, 4);
    byte_slice_t pending = byte_slice_compress(rest);
    byte_slice_t space = byte_slice_skip(byte_slice_make(buf, (int32_t)sizeof(buf)), byte_slice_len(pending));

    (void)state;

    assert_int_equal(pending.start, 0);
    assert_int_equal(byte_slice_len(pending), 2);
    assert_int_equal(buf[0], 5);
    assert_int_equal(buf[1], 6);
    assert_int_equal(space.start, 2);
    assert_int_equal(byte_slice_len(space), 6);

    /* nothing left: empty slice at 0 */
    pending = byte_slice_compress(byte_slice_skip(in, 6));
    assert_int_equal(pending.start, 0);
    assert_int_equal(byte_slice_len(pending), 0);

    /* error passes through */
    assert_true(byte_slice_has_error(byte_slice_compress(byte_slice_skip(in, 7))));
}


/* take() in a read chain and as a reserved header, end(), and the error each failure gives. */
static void test_take_end_and_error_codes(void **state) {
    uint8_t buf[6] = {3, 'a', 'b', 'c', 9, 9};
    byte_slice_t s = byte_slice_make(buf, (int32_t)sizeof(buf));
    byte_slice_t field;
    byte_slice_t left;
    byte_slice_t right;
    byte_slice_t rest;
    uint8_t name_len = 0;
    uint32_t u32 = 0;

    (void)state;

    /* length-prefixed field, then two bytes left over */
    s = byte_slice_read_u8(s, &name_len);
    s = byte_slice_take(s, name_len, &field);
    assert_int_equal(field.start, 1);
    assert_int_equal(byte_slice_len(field), 3);
    assert_int_equal(byte_slice_len(s), 2);
    assert_int_equal(byte_slice_get_error(byte_slice_end(s)), PLCTAG_ERR_TOO_LARGE);
    assert_true(!byte_slice_has_error(byte_slice_end(byte_slice_skip(s, 2))));

    /* running out is TOO_SMALL for every operation, and end() passes it through */
    assert_int_equal(byte_slice_get_error(byte_slice_end(byte_slice_skip(s, 3))), PLCTAG_ERR_TOO_SMALL);
    assert_int_equal(byte_slice_get_error(byte_slice_take(s, 3, &field)), PLCTAG_ERR_TOO_SMALL);
    assert_true(byte_slice_has_error(field));
    assert_int_equal(byte_slice_cut(s, 3, &left, &right), PLCTAG_ERR_TOO_SMALL);
    assert_int_equal(byte_slice_get_error(byte_slice_read_u32_le(s, &u32)), PLCTAG_ERR_TOO_SMALL);
    assert_int_equal(byte_slice_get_error(byte_slice_read_bytes(s, buf, 3)), PLCTAG_ERR_TOO_SMALL);
    assert_int_equal(byte_slice_get_error(byte_slice_write_u32_le(s, 1)), PLCTAG_ERR_TOO_SMALL);

    /* negative lengths are BAD_PARAM */
    s = byte_slice_make(buf, (int32_t)sizeof(buf));
    assert_int_equal(byte_slice_get_error(byte_slice_skip(s, -1)), PLCTAG_ERR_BAD_PARAM);
    assert_int_equal(byte_slice_get_error(byte_slice_take(s, -1, &field)), PLCTAG_ERR_BAD_PARAM);
    assert_int_equal(byte_slice_get_error(byte_slice_read_bytes(s, buf, -1)), PLCTAG_ERR_BAD_PARAM);
    assert_int_equal(byte_slice_get_error(byte_slice_write_bytes(s, buf, -1)), PLCTAG_ERR_BAD_PARAM);

    /* a header reserved with take(), filled in after the body */
    rest = byte_slice_take(s, 2, &field);
    rest = byte_slice_write_u32_le(rest, 0xDEADBEEF);
    field = byte_slice_write_u16_le(field, 4);
    assert_true(!byte_slice_has_error(field));
    assert_int_equal(byte_slice_len(rest), 0);
    assert_int_equal(buf[0], 4);
    assert_int_equal(buf[1], 0);
    assert_int_equal(buf[2], 0xEF);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_round_trip_both_orders),
        cmocka_unit_test(test_error_sticks_and_records_offset),
        cmocka_unit_test(test_failed_write_leaves_buffer),
        cmocka_unit_test(test_make_rejects_bad_args),
        cmocka_unit_test(test_cut_skip_length_prefixed),
        cmocka_unit_test(test_used_header_after_payload),
        cmocka_unit_test(test_compare),
        cmocka_unit_test(test_compress_leftover),
        cmocka_unit_test(test_take_end_and_error_codes),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
