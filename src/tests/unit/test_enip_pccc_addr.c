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

#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/enip/enip_pccc_addr.h>

/* ============================================================================
 * enip_pccc_parse_logical_address
 * ============================================================================ */

static void test_parse_int_simple(void **state) {
    (void)state;
    pccc_addr_t addr = {0};

    assert_int_equal(enip_pccc_parse_logical_address("N7:0", &addr), PLCTAG_STATUS_OK);
    assert_int_equal(addr.file_type, PCCC_FILE_INT);
    assert_int_equal(addr.file, 7);
    assert_int_equal(addr.element, 0);
    assert_int_equal(addr.sub_element, -1);
    assert_int_equal(addr.element_size_bytes, 2);
    assert_false(addr.is_bit);
}

static void test_parse_float_element(void **state) {
    (void)state;
    pccc_addr_t addr = {0};

    assert_int_equal(enip_pccc_parse_logical_address("F8:3", &addr), PLCTAG_STATUS_OK);
    assert_int_equal(addr.file_type, PCCC_FILE_FLOAT);
    assert_int_equal(addr.file, 8);
    assert_int_equal(addr.element, 3);
    assert_int_equal(addr.element_size_bytes, 4);
}

static void test_parse_bit(void **state) {
    (void)state;
    pccc_addr_t addr = {0};

    assert_int_equal(enip_pccc_parse_logical_address("B3:0/2", &addr), PLCTAG_STATUS_OK);
    assert_int_equal(addr.file_type, PCCC_FILE_BIT);
    assert_int_equal(addr.file, 3);
    assert_int_equal(addr.element, 0);
    assert_true(addr.is_bit);
    assert_int_equal(addr.bit, 2);
}

static void test_parse_numeric_subelement(void **state) {
    (void)state;
    pccc_addr_t addr = {0};

    assert_int_equal(enip_pccc_parse_logical_address("T4:0.2", &addr), PLCTAG_STATUS_OK);
    assert_int_equal(addr.file_type, PCCC_FILE_TIMER);
    assert_int_equal(addr.file, 4);
    assert_int_equal(addr.element, 0);
    assert_int_equal(addr.sub_element, 2);
}

/* Timer .acc mnemonic resolves to sub-element 2 (the accumulator word). */
static void test_parse_timer_acc_mnemonic(void **state) {
    (void)state;
    pccc_addr_t addr = {0};

    assert_int_equal(enip_pccc_parse_logical_address("T4:0.acc", &addr), PLCTAG_STATUS_OK);
    assert_int_equal(addr.file_type, PCCC_FILE_TIMER);
    assert_int_equal(addr.file, 4);
    assert_int_equal(addr.element, 0);
    assert_int_equal(addr.sub_element, 2);
}

/* Timer .pre mnemonic resolves to sub-element 1 (the preset word). */
static void test_parse_timer_pre_mnemonic(void **state) {
    (void)state;
    pccc_addr_t addr = {0};

    assert_int_equal(enip_pccc_parse_logical_address("T4:0.pre", &addr), PLCTAG_STATUS_OK);
    assert_int_equal(addr.sub_element, 1);
}

static void test_parse_logix_name_fails(void **state) {
    (void)state;
    pccc_addr_t addr = {0};

    /* A Logix symbolic name is not a PCCC logical address. */
    assert_int_not_equal(enip_pccc_parse_logical_address("MyTag", &addr), PLCTAG_STATUS_OK);
}

static void test_parse_missing_element_fails(void **state) {
    (void)state;
    pccc_addr_t addr = {0};

    assert_int_not_equal(enip_pccc_parse_logical_address("N7", &addr), PLCTAG_STATUS_OK);
}

/* ============================================================================
 * enip_pccc_encode_plc5_address
 * ============================================================================ */

static void test_encode_plc5_simple(void **state) {
    (void)state;
    pccc_addr_t addr = {0};
    uint8_t buf[32];

    assert_int_equal(enip_pccc_parse_logical_address("N7:0", &addr), PLCTAG_STATUS_OK);

    Bytes out = enip_pccc_encode_plc5_address(&addr, bytes_from_buf(buf, sizeof(buf)));

    /* level byte 0x06 (levels 1+2), file 7, element 0 */
    uint8_t expected[] = {0x06, 0x07, 0x00};

    assert_false(bytes_is_null(out));
    assert_int_equal(out.len, sizeof(expected));
    assert_memory_equal(out.data, expected, sizeof(expected));
}

static void test_encode_plc5_subelement(void **state) {
    (void)state;
    pccc_addr_t addr = {0};
    uint8_t buf[32];

    assert_int_equal(enip_pccc_parse_logical_address("T4:0.acc", &addr), PLCTAG_STATUS_OK);

    Bytes out = enip_pccc_encode_plc5_address(&addr, bytes_from_buf(buf, sizeof(buf)));

    /* level byte 0x0E (levels 1+2+3), file 4, element 0, subelement 2 (acc) */
    uint8_t expected[] = {0x0E, 0x04, 0x00, 0x02};

    assert_false(bytes_is_null(out));
    assert_int_equal(out.len, sizeof(expected));
    assert_memory_equal(out.data, expected, sizeof(expected));
}

static void test_encode_plc5_large_file_num(void **state) {
    (void)state;
    pccc_addr_t addr = {0};
    uint8_t buf[32];

    assert_int_equal(enip_pccc_parse_logical_address("N300:0", &addr), PLCTAG_STATUS_OK);

    Bytes out = enip_pccc_encode_plc5_address(&addr, bytes_from_buf(buf, sizeof(buf)));

    /* file 300 > 254 -> 0xFF,lo,hi (0x2C,0x01); element 0 */
    uint8_t expected[] = {0x06, 0xFF, 0x2C, 0x01, 0x00};

    assert_false(bytes_is_null(out));
    assert_int_equal(out.len, sizeof(expected));
    assert_memory_equal(out.data, expected, sizeof(expected));
}

static void test_encode_plc5_buffer_too_small(void **state) {
    (void)state;
    pccc_addr_t addr = {0};
    uint8_t buf[4];

    assert_int_equal(enip_pccc_parse_logical_address("N7:0", &addr), PLCTAG_STATUS_OK);

    Bytes out = enip_pccc_encode_plc5_address(&addr, bytes_from_buf(buf, sizeof(buf)));
    assert_true(bytes_is_null(out));
}

/* ============================================================================
 * enip_pccc_encode_slc_address
 * ============================================================================ */

static void test_encode_slc_int(void **state) {
    (void)state;
    pccc_addr_t addr = {0};
    uint8_t buf[32];

    assert_int_equal(enip_pccc_parse_logical_address("N7:0", &addr), PLCTAG_STATUS_OK);

    Bytes out = enip_pccc_encode_slc_address(&addr, bytes_from_buf(buf, sizeof(buf)));

    /* file 7, file_type 0x89 (INT), element 0, sub 0 */
    uint8_t expected[] = {0x07, 0x89, 0x00, 0x00};

    assert_false(bytes_is_null(out));
    assert_int_equal(out.len, sizeof(expected));
    assert_memory_equal(out.data, expected, sizeof(expected));
}

static void test_encode_slc_float(void **state) {
    (void)state;
    pccc_addr_t addr = {0};
    uint8_t buf[32];

    assert_int_equal(enip_pccc_parse_logical_address("F8:1", &addr), PLCTAG_STATUS_OK);

    Bytes out = enip_pccc_encode_slc_address(&addr, bytes_from_buf(buf, sizeof(buf)));

    /* file 8, file_type 0x8A (FLOAT), element 1, sub 0 */
    uint8_t expected[] = {0x08, 0x8A, 0x01, 0x00};

    assert_false(bytes_is_null(out));
    assert_int_equal(out.len, sizeof(expected));
    assert_memory_equal(out.data, expected, sizeof(expected));
}

static void test_encode_slc_bit_addr(void **state) {
    (void)state;
    pccc_addr_t addr = {0};
    uint8_t buf[32];

    assert_int_equal(enip_pccc_parse_logical_address("B3:0/2", &addr), PLCTAG_STATUS_OK);

    Bytes out = enip_pccc_encode_slc_address(&addr, bytes_from_buf(buf, sizeof(buf)));

    /* the bit is selected by the lib layer; the encoded address is just the word. */
    uint8_t expected[] = {0x03, 0x85, 0x00, 0x00};

    assert_false(bytes_is_null(out));
    assert_int_equal(out.len, sizeof(expected));
    assert_memory_equal(out.data, expected, sizeof(expected));
}

static void test_encode_slc_buffer_too_small(void **state) {
    (void)state;
    pccc_addr_t addr = {0};
    uint8_t buf[4];

    assert_int_equal(enip_pccc_parse_logical_address("N7:0", &addr), PLCTAG_STATUS_OK);

    Bytes out = enip_pccc_encode_slc_address(&addr, bytes_from_buf(buf, sizeof(buf)));
    assert_true(bytes_is_null(out));
}

static void test_encode_null_buffer_fails(void **state) {
    (void)state;
    pccc_addr_t addr = {0};

    assert_int_equal(enip_pccc_parse_logical_address("N7:0", &addr), PLCTAG_STATUS_OK);

    assert_true(bytes_is_null(enip_pccc_encode_plc5_address(&addr, bytes_null())));
    assert_true(bytes_is_null(enip_pccc_encode_slc_address(&addr, bytes_null())));
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_parse_int_simple),
        cmocka_unit_test(test_parse_float_element),
        cmocka_unit_test(test_parse_bit),
        cmocka_unit_test(test_parse_numeric_subelement),
        cmocka_unit_test(test_parse_timer_acc_mnemonic),
        cmocka_unit_test(test_parse_timer_pre_mnemonic),
        cmocka_unit_test(test_parse_logix_name_fails),
        cmocka_unit_test(test_parse_missing_element_fails),
        cmocka_unit_test(test_encode_plc5_simple),
        cmocka_unit_test(test_encode_plc5_subelement),
        cmocka_unit_test(test_encode_plc5_large_file_num),
        cmocka_unit_test(test_encode_plc5_buffer_too_small),
        cmocka_unit_test(test_encode_slc_int),
        cmocka_unit_test(test_encode_slc_float),
        cmocka_unit_test(test_encode_slc_bit_addr),
        cmocka_unit_test(test_encode_slc_buffer_too_small),
        cmocka_unit_test(test_encode_null_buffer_fails),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
