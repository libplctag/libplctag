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
 * byte_buf is header-only and every operation is a few lines, so this checks the
 * edges rather than the happy path: the error buffer propagating untouched
 * through a chain, bounds refusal at each width, and the reserve-then-fill split
 * that exists so an outer header can be written after the payload it describes.
 */

#include "mini_mock.h"

#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <string.h>
#include <utils/byte_buf.h>


static void test_encode_decode_roundtrip(void **state) {
    uint8_t raw[32];
    byte_buf out = byte_buf_make(raw, (intptr_t)sizeof(raw));
    byte_buf rem = out;
    uint8_t v8 = 0;
    uint16_t v16 = 0;
    uint32_t v32 = 0;
    uint64_t v64 = 0;

    (void)state;

    rem = byte_buf_encode_uint8(rem, 0xA5);
    rem = byte_buf_encode_uint16_le(rem, 0x1234);
    rem = byte_buf_encode_uint32_be(rem, 0xDEADBEEF);
    rem = byte_buf_encode_uint64_le(rem, 0x0102030405060708ULL);

    assert_int_equal(byte_buf_has_err(rem), false);
    assert_int_equal(byte_buf_written(out, rem), 15);

    /* byte order actually reached the buffer */
    assert_int_equal(raw[1], 0x34); /* uint16 LE low byte first */
    assert_int_equal(raw[2], 0x12);
    assert_int_equal(raw[3], 0xDE); /* uint32 BE high byte first */
    assert_int_equal(raw[6], 0xEF);

    rem = byte_buf_truncate(out, byte_buf_written(out, rem));

    rem = byte_buf_decode_uint8(rem, &v8);
    rem = byte_buf_decode_uint16_le(rem, &v16);
    rem = byte_buf_decode_uint32_be(rem, &v32);
    rem = byte_buf_decode_uint64_le(rem, &v64);

    assert_int_equal(byte_buf_has_err(rem), false);
    assert_int_equal(byte_buf_len(rem), 0);
    assert_int_equal(v8, 0xA5);
    assert_int_equal(v16, 0x1234);
    assert_int_equal(v32, 0xDEADBEEF);
    assert_int_equal(v64, 0x0102030405060708ULL);
}


/* An error buffer passes through every operation unchanged and keeps its message. */
static void test_error_propagates(void **state) {
    uint8_t raw[2];
    byte_buf out = byte_buf_make(raw, (intptr_t)sizeof(raw));
    byte_buf rem = out;
    uint8_t v8 = 0;

    (void)state;

    rem = byte_buf_encode_uint16_le(rem, 0x1234); /* fills the buffer */
    rem = byte_buf_encode_uint8(rem, 0xFF);       /* fails here */

    assert_int_equal(byte_buf_has_err(rem), true);
    assert_int_equal(byte_buf_get_err(rem), PLCTAG_ERR_OUT_OF_BOUNDS);

    /* every later call is a no-op that keeps the first error */
    rem = byte_buf_encode_uint32_le(rem, 0);
    rem = byte_buf_decode_uint8(rem, &v8);
    rem = byte_buf_slice(rem, 0, 1);

    assert_int_equal(byte_buf_has_err(rem), true);
    assert_int_equal(byte_buf_get_err(rem), PLCTAG_ERR_OUT_OF_BOUNDS);
    assert_int_equal(strcmp(byte_buf_get_err_msg(rem), "byte_buf_encode_uint8: out of bounds"), 0);

    /* the value that could not be decoded was not touched */
    assert_int_equal(v8, 0);

    /* and a length taken from an error buffer is the error, not a size */
    assert_int_equal(byte_buf_written(out, rem), PLCTAG_ERR_OUT_OF_BOUNDS);
}


/* Each width refuses to run off the end rather than truncating. */
static void test_bounds_refusal(void **state) {
    uint8_t raw[8];
    uint16_t v16 = 0;
    uint64_t v64 = 0;

    (void)state;

    assert_int_equal(byte_buf_has_err(byte_buf_encode_uint16_le(byte_buf_make(raw, 1), 0)), true);
    assert_int_equal(byte_buf_has_err(byte_buf_encode_uint32_le(byte_buf_make(raw, 3), 0)), true);
    assert_int_equal(byte_buf_has_err(byte_buf_encode_uint64_le(byte_buf_make(raw, 7), 0)), true);

    assert_int_equal(byte_buf_has_err(byte_buf_decode_uint16_le(byte_buf_make(raw, 1), &v16)), true);
    assert_int_equal(byte_buf_has_err(byte_buf_decode_uint64_be(byte_buf_make(raw, 7), &v64)), true);

    /* exactly enough is enough */
    assert_int_equal(byte_buf_has_err(byte_buf_encode_uint16_le(byte_buf_make(raw, 2), 0)), false);
    assert_int_equal(byte_buf_has_err(byte_buf_encode_uint64_le(byte_buf_make(raw, 8), 0)), false);

    /* a zero length buffer is not an error, it just has no room */
    assert_int_equal(byte_buf_has_err(byte_buf_make(raw, 0)), false);
    assert_int_equal(byte_buf_has_err(byte_buf_encode_uint8(byte_buf_make(raw, 0), 0)), true);
}


/*
 * The reason split() exists.  A CIP packet's EIP header carries the length of
 * everything after it, which is not known until the payload has been built.
 */
static void test_split_reserve_then_fill(void **state) {
    enum { HEADER_SIZE = 4 };

    uint8_t raw[32];
    byte_buf out = byte_buf_make(raw, (intptr_t)sizeof(raw));
    byte_buf header = byte_buf_split_front(out, HEADER_SIZE);
    byte_buf body = byte_buf_split_back(out, HEADER_SIZE);
    byte_buf rem = body;
    intptr_t payload_len = 0;

    (void)state;

    assert_int_equal(byte_buf_has_err(header), false);
    assert_int_equal(byte_buf_len(header), HEADER_SIZE);
    assert_int_equal(byte_buf_len(body), (intptr_t)sizeof(raw) - HEADER_SIZE);

    /* the two halves meet exactly, with no gap and no overlap */
    assert_int_equal(byte_buf_data(header) + byte_buf_len(header) == byte_buf_data(body), true);

    /* build the payload without knowing where the header ends up */
    rem = byte_buf_encode_uint32_le(rem, 0x11223344);
    rem = byte_buf_encode_uint16_le(rem, 0x5566);

    payload_len = byte_buf_written(body, rem);
    assert_int_equal(payload_len, 6);

    /* now the header can be written, because the length is known */
    rem = byte_buf_encode_uint16_le(header, 0x0070);
    rem = byte_buf_encode_uint16_le(rem, (uint16_t)payload_len);

    assert_int_equal(byte_buf_has_err(rem), false);
    assert_int_equal(byte_buf_len(rem), 0); /* the reservation was filled exactly */

    assert_int_equal(raw[0], 0x70);
    assert_int_equal(raw[2], 0x06); /* the payload length the header describes */
    assert_int_equal(raw[4], 0x44); /* payload starts right after it */

    /* the whole packet, ready to send */
    assert_int_equal(byte_buf_len(byte_buf_truncate(out, HEADER_SIZE + payload_len)), 10);
}


/* Reserving more than exists must fail rather than hand back a short buffer. */
static void test_split_refuses_over_reservation(void **state) {
    uint8_t raw[4];
    byte_buf src = byte_buf_make(raw, (intptr_t)sizeof(raw));

    (void)state;

    assert_int_equal(byte_buf_has_err(byte_buf_split_front(src, 8)), true);
    assert_int_equal(byte_buf_has_err(byte_buf_split_back(src, 8)), true);
    assert_int_equal(byte_buf_get_err(byte_buf_split_front(src, 8)), PLCTAG_ERR_TOO_SMALL);

    /* a negative index is refused too */
    assert_int_equal(byte_buf_has_err(byte_buf_split_front(src, -1)), true);

    /* splitting at exactly the length is fine: a full front and an empty back */
    assert_int_equal(byte_buf_len(byte_buf_split_front(src, (intptr_t)sizeof(raw))), (intptr_t)sizeof(raw));
    assert_int_equal(byte_buf_len(byte_buf_split_back(src, (intptr_t)sizeof(raw))), 0);

    /* and at zero: an empty front and the whole thing behind it */
    assert_int_equal(byte_buf_len(byte_buf_split_front(src, 0)), 0);
    assert_int_equal(byte_buf_len(byte_buf_split_back(src, 0)), (intptr_t)sizeof(raw));
}


static void test_bulk_bytes(void **state) {
    static const uint8_t addr[] = {'1', '0', '.', '2', '0', '6', '.', '1', '.', '3', '9'};

    uint8_t raw[16];
    uint8_t back[16] = {0};
    byte_buf out = byte_buf_make(raw, (intptr_t)sizeof(raw));
    byte_buf rem = byte_buf_encode_bytes(out, addr, (intptr_t)sizeof(addr));

    (void)state;

    assert_int_equal(byte_buf_written(out, rem), (intptr_t)sizeof(addr));

    rem = byte_buf_decode_bytes(byte_buf_truncate(out, (intptr_t)sizeof(addr)), back, (intptr_t)sizeof(addr));

    assert_int_equal(byte_buf_has_err(rem), false);
    assert_int_equal(byte_buf_len(rem), 0);
    assert_int_equal(memcmp(addr, back, sizeof(addr)), 0);

    /* asking for more than is there fails instead of copying what it can */
    assert_int_equal(byte_buf_has_err(byte_buf_decode_bytes(byte_buf_make(raw, 4), back, 8)), true);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_encode_decode_roundtrip), cmocka_unit_test(test_error_propagates),
        cmocka_unit_test(test_bounds_refusal),          cmocka_unit_test(test_split_reserve_then_fill),
        cmocka_unit_test(test_split_refuses_over_reservation), cmocka_unit_test(test_bulk_bytes),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
