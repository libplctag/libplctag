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
 * The structural bounds on the two copy paths that used to rely on every caller checking
 * first: mem_copy_bounded() (platform) and encode_data() (PCCC address encoding).  The
 * destination buffers are malloc'd at exactly their capacity so the sanitizer build turns a
 * missing bound into a heap-buffer-overflow -- on the old unbounded code these same calls
 * would write past the end rather than return an error.
 */

#include "mini_mock.h"

#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/ab/tag.h>
#include <libplctag/protocols/ab/pccc.h>
#include <platform.h>
#include <stdlib.h>
#include <string.h>


static void test_mem_copy_bounded(void **state) {
    (void)state;

    uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    /* an oversized copy is refused and writes nothing -- this would overflow dst on a raw memcpy. */
    uint8_t *dst = (uint8_t *)malloc(4);
    assert_true(dst != NULL);
    assert_int_equal(mem_copy_bounded(dst, 4, src, 8), PLCTAG_ERR_TOO_SMALL);
    free(dst);

    /* an exact fit copies every byte. */
    dst = (uint8_t *)malloc(4);
    assert_true(dst != NULL);
    assert_int_equal(mem_copy_bounded(dst, 4, src, 4), 4);
    assert_true(memcmp(dst, src, 4) == 0);
    free(dst);

    /* a copy smaller than the capacity is fine and reports what it moved. */
    dst = (uint8_t *)malloc(8);
    assert_true(dst != NULL);
    assert_int_equal(mem_copy_bounded(dst, 8, src, 3), 3);
    assert_true(memcmp(dst, src, 3) == 0);
    free(dst);

    /* a zero-length copy is a no-op success. */
    assert_int_equal(mem_copy_bounded(src, 8, src, 0), 0);

    /* negative size or capacity, and NULL pointers, are rejected rather than acted on. */
    assert_int_equal(mem_copy_bounded(src, 8, src, -1), PLCTAG_ERR_BAD_PARAM);
    assert_int_equal(mem_copy_bounded(src, -1, src, 1), PLCTAG_ERR_BAD_PARAM);
    assert_int_equal(mem_copy_bounded(NULL, 8, src, 1), PLCTAG_ERR_NULL_PTR);
    assert_int_equal(mem_copy_bounded(src, 8, NULL, 1), PLCTAG_ERR_NULL_PTR);
}


static void test_encode_data_bounded(void **state) {
    (void)state;

    /* a small value takes one byte; it fits a one-byte buffer exactly. */
    uint8_t *buf = (uint8_t *)malloc(1);
    int index = 0;
    assert_true(buf != NULL);
    assert_int_equal(encode_data(buf, &index, 1, 42), PLCTAG_STATUS_OK);
    assert_int_equal(index, 1);
    assert_int_equal(buf[0], 42);
    free(buf);

    /* a value over 254 takes three bytes; one byte of room is refused, not overrun. */
    buf = (uint8_t *)malloc(1);
    index = 0;
    assert_true(buf != NULL);
    assert_int_equal(encode_data(buf, &index, 1, 300), PLCTAG_ERR_TOO_SMALL);
    assert_int_equal(index, 0); /* nothing was written */
    free(buf);

    /* the same three-byte value fits a three-byte buffer: 0xFF escape then little-endian value. */
    buf = (uint8_t *)malloc(3);
    index = 0;
    assert_true(buf != NULL);
    assert_int_equal(encode_data(buf, &index, 3, 300), PLCTAG_STATUS_OK);
    assert_int_equal(index, 3);
    assert_int_equal(buf[0], 0xFF);
    assert_int_equal(buf[1], 300 & 0xFF);
    assert_int_equal(buf[2], (300 >> 8) & 0xFF);
    free(buf);

    /* appending past a buffer that is already full is refused. */
    buf = (uint8_t *)malloc(2);
    index = 2;
    assert_true(buf != NULL);
    assert_int_equal(encode_data(buf, &index, 2, 7), PLCTAG_ERR_TOO_SMALL);
    assert_int_equal(index, 2);
    free(buf);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_mem_copy_bounded),
        cmocka_unit_test(test_encode_data_bounded),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
