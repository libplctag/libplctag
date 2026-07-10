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

#include <libplctag/protocols/enip/client/enip_cpf.h>
#include <libplctag/protocols/enip/client/enip_eip.h>
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

/* ============================================================================
 * EIP header encode/decode
 * ============================================================================ */

static void test_eip_register_session_layout(void **state) {
    Arena *a = (Arena *)*state;

    Bytes frame = enip_eip_register_session(a);

    /* 24-byte header + 4-byte payload (protocol_version=1, options=0) */
    uint8_t expected[] = {
        0x65, 0x00,                         /* command = 0x0065 */
        0x04, 0x00,                         /* length = 4 */
        0x00, 0x00, 0x00, 0x00,             /* session_handle = 0 */
        0x00, 0x00, 0x00, 0x00,             /* status = 0 */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* sender_context = 0 */
        0x00, 0x00, 0x00, 0x00,             /* options = 0 */
        0x01, 0x00,                         /* protocol_version = 1 */
        0x00, 0x00,                         /* options = 0 */
    };

    assert_false(bytes_is_null(frame));
    assert_int_equal(frame.len, sizeof(expected));
    assert_memory_equal(frame.data, expected, sizeof(expected));
}

static void test_eip_decode_round_trip(void **state) {
    Arena *a = (Arena *)*state;

    Bytes frame = enip_eip_register_session(a);
    assert_false(bytes_is_null(frame));

    enip_eip_hdr_t hdr = {0};
    Bytes payload = bytes_null();

    assert_true(enip_eip_decode(frame, &hdr, &payload));

    assert_int_equal(hdr.command, ENIP_CMD_REGISTER_SESSION);
    assert_int_equal(hdr.length, 4);
    assert_int_equal(hdr.session_handle, 0);
    assert_int_equal(hdr.status, 0);
    assert_int_equal(hdr.sender_context, 0);
    assert_int_equal(hdr.options, 0);

    assert_int_equal(payload.len, 4);

    uint8_t expected_payload[] = {0x01, 0x00, 0x00, 0x00};
    assert_memory_equal(payload.data, expected_payload, sizeof(expected_payload));
}

static void test_eip_decode_too_short_fails(void **state) {
    uint8_t raw[16] = {0};
    Bytes in = bytes_from_buf(raw, sizeof(raw));

    enip_eip_hdr_t hdr = {0};
    Bytes payload = bytes_null();

    assert_false(enip_eip_decode(in, &hdr, &payload));
}

static void test_eip_decode_truncated_payload_fails(void **state) {
    uint8_t raw[ENIP_EIP_HEADER_SIZE] = {0};
    /* claim a 10-byte payload that isn't actually present */
    raw[2] = 10;
    raw[3] = 0;

    Bytes in = bytes_from_buf(raw, sizeof(raw));

    enip_eip_hdr_t hdr = {0};
    Bytes payload = bytes_null();

    assert_false(enip_eip_decode(in, &hdr, &payload));
}

static void test_eip_send_rr_data_command(void **state) {
    Arena *a = (Arena *)*state;

    uint8_t cip_raw[] = {0x01, 0x02, 0x03};
    Bytes cip = bytes_from_buf(cip_raw, sizeof(cip_raw));

    Bytes cpf = enip_cpf_wrap_unconnected(a, cip);
    assert_false(bytes_is_null(cpf));

    Bytes frame = enip_eip_send_rr_data(a, 0x12345678, cpf);
    assert_false(bytes_is_null(frame));

    enip_eip_hdr_t hdr = {0};
    Bytes payload = bytes_null();

    assert_true(enip_eip_decode(frame, &hdr, &payload));
    assert_int_equal(hdr.command, ENIP_CMD_UNCONNECTED_SEND);
    assert_int_equal(hdr.session_handle, 0x12345678);
    assert_int_equal(payload.len, cpf.len);
}

static void test_eip_send_unit_data_command(void **state) {
    Arena *a = (Arena *)*state;

    uint8_t cip_raw[] = {0xAA, 0xBB};
    Bytes cip = bytes_from_buf(cip_raw, sizeof(cip_raw));

    Bytes cpf = enip_cpf_wrap_connected(a, 0xDEADBEEF, 7, cip);
    assert_false(bytes_is_null(cpf));

    Bytes frame = enip_eip_send_unit_data(a, 0x99, cpf);
    assert_false(bytes_is_null(frame));

    enip_eip_hdr_t hdr = {0};
    Bytes payload = bytes_null();

    assert_true(enip_eip_decode(frame, &hdr, &payload));
    assert_int_equal(hdr.command, ENIP_CMD_CONNECTED_SEND);
    assert_int_equal(hdr.session_handle, 0x99);
    assert_int_equal(payload.len, cpf.len);
}

/* ============================================================================
 * CPF wrap/unwrap round trips
 * ============================================================================ */

static void test_cpf_unconnected_round_trip(void **state) {
    Arena *a = (Arena *)*state;

    uint8_t cip_raw[] = {0x4C, 0x02, 0x91, 0x04, 'T', 'e', 's', 't'};
    Bytes cip = bytes_from_buf(cip_raw, sizeof(cip_raw));

    Bytes wrapped = enip_cpf_wrap_unconnected(a, cip);
    assert_false(bytes_is_null(wrapped));
    assert_int_equal(wrapped.len, ENIP_CPF_UNCONNECTED_OVERHEAD + cip.len);

    uint16_t seq = 0xFFFF;
    Bytes out = bytes_null();

    assert_true(enip_cpf_unwrap(wrapped, false, &seq, &out));
    assert_int_equal(seq, 0);
    assert_int_equal(out.len, cip.len);
    assert_memory_equal(out.data, cip.data, cip.len);
}

static void test_cpf_connected_round_trip(void **state) {
    Arena *a = (Arena *)*state;

    uint8_t cip_raw[] = {0xCC, 0xC1, 0x00, 0x01};
    Bytes cip = bytes_from_buf(cip_raw, sizeof(cip_raw));

    Bytes wrapped = enip_cpf_wrap_connected(a, 0x11223344, 0x55, cip);
    assert_false(bytes_is_null(wrapped));
    assert_int_equal(wrapped.len, ENIP_CPF_CONNECTED_OVERHEAD + cip.len);

    uint16_t seq = 0;
    Bytes out = bytes_null();

    assert_true(enip_cpf_unwrap(wrapped, true, &seq, &out));
    assert_int_equal(seq, 0x55);
    assert_int_equal(out.len, cip.len);
    assert_memory_equal(out.data, cip.data, cip.len);
}

static void test_cpf_unwrap_wrong_kind_fails(void **state) {
    Arena *a = (Arena *)*state;

    uint8_t cip_raw[] = {0x01};
    Bytes cip = bytes_from_buf(cip_raw, sizeof(cip_raw));

    Bytes wrapped = enip_cpf_wrap_unconnected(a, cip);
    assert_false(bytes_is_null(wrapped));

    uint16_t seq = 0;
    Bytes out = bytes_null();

    /* an unconnected frame has no Connected Data Item */
    assert_false(enip_cpf_unwrap(wrapped, true, &seq, &out));
}

static void test_cpf_unwrap_too_short_fails(void **state) {
    uint8_t raw[4] = {0};
    Bytes in = bytes_from_buf(raw, sizeof(raw));

    uint16_t seq = 0;
    Bytes out = bytes_null();

    assert_false(enip_cpf_unwrap(in, false, &seq, &out));
}

/* ============================================================================
 * Full stack: EIP(CPF(CIP)) round trip
 * ============================================================================ */

static void test_full_unconnected_round_trip(void **state) {
    Arena *a = (Arena *)*state;

    uint8_t cip_raw[] = {0x4C, 0x02, 0x91, 0x04, 'T', 'e', 's', 't'};
    Bytes cip = bytes_from_buf(cip_raw, sizeof(cip_raw));

    Bytes cpf = enip_cpf_wrap_unconnected(a, cip);
    Bytes frame = enip_eip_send_rr_data(a, 0xABCD1234, cpf);
    assert_false(bytes_is_null(frame));

    enip_eip_hdr_t hdr = {0};
    Bytes eip_payload = bytes_null();
    assert_true(enip_eip_decode(frame, &hdr, &eip_payload));
    assert_int_equal(hdr.command, ENIP_CMD_UNCONNECTED_SEND);
    assert_int_equal(hdr.session_handle, 0xABCD1234);

    uint16_t seq = 0xFFFF;
    Bytes cip_out = bytes_null();
    assert_true(enip_cpf_unwrap(eip_payload, false, &seq, &cip_out));

    assert_int_equal(cip_out.len, cip.len);
    assert_memory_equal(cip_out.data, cip.data, cip.len);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_eip_register_session_layout, setup, teardown),
        cmocka_unit_test_setup_teardown(test_eip_decode_round_trip, setup, teardown),
        cmocka_unit_test(test_eip_decode_too_short_fails),
        cmocka_unit_test(test_eip_decode_truncated_payload_fails),
        cmocka_unit_test_setup_teardown(test_eip_send_rr_data_command, setup, teardown),
        cmocka_unit_test_setup_teardown(test_eip_send_unit_data_command, setup, teardown),
        cmocka_unit_test_setup_teardown(test_cpf_unconnected_round_trip, setup, teardown),
        cmocka_unit_test_setup_teardown(test_cpf_connected_round_trip, setup, teardown),
        cmocka_unit_test_setup_teardown(test_cpf_unwrap_wrong_kind_fails, setup, teardown),
        cmocka_unit_test(test_cpf_unwrap_too_short_fails),
        cmocka_unit_test_setup_teardown(test_full_unconnected_round_trip, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
