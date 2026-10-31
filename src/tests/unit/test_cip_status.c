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
 * The CIP error status prefix is read straight out of a received packet, so its own word
 * count and the number of bytes that actually arrived are two different things and a PLC
 * controls both.  Every read has to be bounded by the second, not the first.
 *
 * These also pin the status codes the Forward Open negotiation reads back, because which
 * retry it takes is decided entirely by them: a size error has to be distinguishable from
 * a bad path, or a renegotiable refusal is treated as a configuration mistake.
 */

#include "mini_mock.h"

#include <libplctag/api/libplctag.h>
#include <libplctag/modules/cip/error_codes.h>
#include <string.h>


/* an error reply: general status, extended word count, then that many little-endian words. */
static uint8_t status_0109[] = {0x01, 0x02, 0x09, 0x01, 0xA2, 0x0F};

/* the same refusal from a PLC whose firmware does not name a size. */
static uint8_t status_0109_bare[] = {0x01, 0x01, 0x09, 0x01};

/* a success reply: no extended status at all. */
static uint8_t status_none[] = {0x00, 0x00};


static void test_extended_status_words_are_read(void **state) {
    (void)state;

    /* 0x0109, "invalid connection size", offering 4002. */
    assert_int_equal(cip_extended_status_word(status_0109, sizeof(status_0109), 0), 0x0109);
    assert_int_equal(cip_extended_status_word(status_0109, sizeof(status_0109), 1), 4002);

    assert_int_equal(cip_extended_status_word(status_0109_bare, sizeof(status_0109_bare), 0), 0x0109);

    assert_int_equal(cip_extended_status_word(status_none, sizeof(status_none), 0), 0);
}


static void test_missing_status_words_read_as_zero(void **state) {
    (void)state;

    /* asking past the count the PLC declared. */
    assert_int_equal(cip_extended_status_word(status_0109, sizeof(status_0109), 2), 0);
    assert_int_equal(cip_extended_status_word(status_0109_bare, sizeof(status_0109_bare), 1), 0);
    assert_int_equal(cip_extended_status_word(status_0109, sizeof(status_0109), 255), 0);
}


/*
 * The declared word count and the bytes that arrived disagree.  Trusting the count reads
 * past the end of the packet, so every truncation has to come back as "not there".
 */
static void test_truncated_status_is_bounded_by_the_buffer(void **state) {
    size_t len = 0;

    (void)state;

    /* the count claims two words; feed it every prefix shorter than that. */
    for(len = 0; len < sizeof(status_0109); len++) {
        uint16_t word0 = cip_extended_status_word(status_0109, len, 0);
        uint16_t word1 = cip_extended_status_word(status_0109, len, 1);

        assert_int_equal(word0, (len >= 4) ? 0x0109 : 0);
        assert_int_equal(word1, (len >= 6) ? 4002 : 0);
    }
}


/* cip_error_data_size() measures that buffer, and must not wrap when there is nothing left. */
static void test_error_data_size_never_wraps(void **state) {
    uint8_t buf[4] = {0};

    (void)state;

    assert_int_equal(cip_error_data_size(&buf[0], &buf[4]), 4);
    assert_int_equal(cip_error_data_size(&buf[3], &buf[4]), 1);

    /* at the end, and past it -- a reply too short to reach this field at all. */
    assert_int_equal(cip_error_data_size(&buf[4], &buf[4]), 0);
    assert_int_equal(cip_error_data_size(&buf[4], &buf[0]), 0);
}


/*
 * What the Forward Open negotiation branches on.  A size refusal must not look like a bad
 * path: one is renegotiated immediately, the other backs off and waits for a person.
 */
static void test_forward_open_statuses_are_distinguishable(void **state) {
    uint8_t status[6] = {0x01, 0x02, 0, 0, 0, 0};

    (void)state;

    struct {
        uint16_t extended;
        int expected_rc;
    } cases[] = {
        {0x0109, PLCTAG_ERR_TOO_LARGE}, /* invalid connection size */
        {0x0127, PLCTAG_ERR_TOO_LARGE}, /* invalid originator to target size */
        {0x0128, PLCTAG_ERR_TOO_LARGE}, /* invalid target to originator size */
        {0x0100, PLCTAG_ERR_DUPLICATE}, /* duplicate forward open */
        {0x0113, PLCTAG_ERR_NO_RESOURCES} /* out of connections */,
        {0x0204, PLCTAG_ERR_TIMEOUT},    /* nothing answered along the route */
        {0x0311, PLCTAG_ERR_BAD_DEVICE}, /* port not available */
        {0x0315, PLCTAG_ERR_BAD_PARAM},  /* invalid segment in the path */
    };

    for(size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        status[2] = (uint8_t)(cases[i].extended & 0xFF);
        status[3] = (uint8_t)((cases[i].extended >> 8) & 0xFF);

        assert_int_equal(decode_cip_error_code(status, sizeof(status)), cases[i].expected_rc);
    }

    /* the service itself is not implemented: no extended status, general status alone. */
    {
        uint8_t unsupported[2] = {0x08, 0x00};

        assert_int_equal(decode_cip_error_code(unsupported, sizeof(unsupported)), PLCTAG_ERR_UNSUPPORTED);
    }
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_extended_status_words_are_read),
        cmocka_unit_test(test_missing_status_words_read_as_zero),
        cmocka_unit_test(test_truncated_status_is_bounded_by_the_buffer),
        cmocka_unit_test(test_error_data_size_never_wraps),
        cmocka_unit_test(test_forward_open_statuses_are_distinguishable),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
