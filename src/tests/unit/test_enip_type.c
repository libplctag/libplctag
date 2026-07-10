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
#include <setjmp.h>
#include "cmocka.h"

#include <libplctag/protocols/enip/client/enip_type.h>

static void test_decode_dint(void **state) {
    (void)state;

    uint8_t raw[] = {0xC4, 0x00, 0x2A, 0x00, 0x00, 0x00};
    Bytes data = bytes_from_buf(raw, sizeof(raw));

    uint8_t header_len = 0;
    uint32_t elem_size = 0;
    tag_byte_order_t order = {0};

    assert_true(enip_type_decode(data, &header_len, &elem_size, &order));
    assert_int_equal(header_len, ENIP_TYPE_HEADER_LEN_ATOMIC);
    assert_int_equal(elem_size, 4);
    assert_int_equal(order.int32_order[0], 0);
    assert_int_equal(order.int32_order[3], 3);
}

static void test_decode_real(void **state) {
    (void)state;

    uint8_t raw[] = {0xCA, 0x00, 0x00, 0x00, 0x80, 0x3F};
    Bytes data = bytes_from_buf(raw, sizeof(raw));

    uint8_t header_len = 0;
    uint32_t elem_size = 0;
    tag_byte_order_t order = {0};

    assert_true(enip_type_decode(data, &header_len, &elem_size, &order));
    assert_int_equal(header_len, ENIP_TYPE_HEADER_LEN_ATOMIC);
    assert_int_equal(elem_size, 4);
}

static void test_decode_bool(void **state) {
    (void)state;

    uint8_t raw[] = {0xC1, 0x00, 0x01};
    Bytes data = bytes_from_buf(raw, sizeof(raw));

    uint8_t header_len = 0;
    uint32_t elem_size = 0;
    tag_byte_order_t order = {0};

    assert_true(enip_type_decode(data, &header_len, &elem_size, &order));
    assert_int_equal(header_len, ENIP_TYPE_HEADER_LEN_ATOMIC);
    assert_int_equal(elem_size, 1);
}

static void test_decode_structure_header(void **state) {
    (void)state;

    /* 0x02A0 little-endian = {0xA0, 0x02}, followed by a 2-byte CRC and opaque data */
    uint8_t raw[] = {0xA0, 0x02, 0x12, 0x34, 0xDE, 0xAD, 0xBE, 0xEF};
    Bytes data = bytes_from_buf(raw, sizeof(raw));

    uint8_t header_len = 0;
    uint32_t elem_size = 0;
    tag_byte_order_t order = {0};

    assert_true(enip_type_decode(data, &header_len, &elem_size, &order));
    assert_int_equal(header_len, ENIP_TYPE_HEADER_LEN_STRUCT);
    assert_int_equal(elem_size, 0);
}

static void test_decode_unknown_type_fails(void **state) {
    (void)state;

    uint8_t raw[] = {0xFF, 0xFF, 0x00, 0x00};
    Bytes data = bytes_from_buf(raw, sizeof(raw));

    uint8_t header_len = 0;
    uint32_t elem_size = 0;
    tag_byte_order_t order = {0};

    assert_false(enip_type_decode(data, &header_len, &elem_size, &order));
}

static void test_decode_too_short_fails(void **state) {
    (void)state;

    uint8_t raw[] = {0xC4};
    Bytes data = bytes_from_buf(raw, sizeof(raw));

    uint8_t header_len = 0;
    uint32_t elem_size = 0;
    tag_byte_order_t order = {0};

    assert_false(enip_type_decode(data, &header_len, &elem_size, &order));
}

static void test_decode_struct_header_too_short_fails(void **state) {
    (void)state;

    /* type code claims structure, but only 3 bytes total (need 4) */
    uint8_t raw[] = {0xA0, 0x02, 0x12};
    Bytes data = bytes_from_buf(raw, sizeof(raw));

    uint8_t header_len = 0;
    uint32_t elem_size = 0;
    tag_byte_order_t order = {0};

    assert_false(enip_type_decode(data, &header_len, &elem_size, &order));
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_decode_dint),
        cmocka_unit_test(test_decode_real),
        cmocka_unit_test(test_decode_bool),
        cmocka_unit_test(test_decode_structure_header),
        cmocka_unit_test(test_decode_unknown_type_fails),
        cmocka_unit_test(test_decode_too_short_fails),
        cmocka_unit_test(test_decode_struct_header_too_short_fails),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
