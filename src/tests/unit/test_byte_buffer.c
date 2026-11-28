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
 * Unit tests for utils/byte_buffer.h growth.
 *
 * byte_buffer_set_len() doubles the capacity, adding at most BYTE_BUFFER_MAX_STEP, and never
 * less than the length asked for.  Shrinking keeps the capacity.  New bytes are zero, old
 * bytes survive, and a failed call leaves the buffer alone.
 */

#include "mini_mock.h"

#include <stdint.h>
#include <string.h>
#include <utils/byte_buffer.h>


/* set the length and check the capacity that results. */
static void grow(byte_buffer_t **buf, int32_t len, int32_t want_capacity) {
    assert_int_equal(byte_buffer_set_len(buf, len), PLCTAG_STATUS_OK);
    assert_non_null(*buf);
    assert_int_equal((*buf)->len, len);
    assert_int_equal((*buf)->capacity, want_capacity);
}


static void test_null_is_empty(void **state) {
    byte_buffer_t *buf = NULL;

    (void)state;

    assert_int_equal(byte_buffer_len(buf), 0);
    assert_null(byte_buffer_data(buf));

    /* length 0 on an empty buffer allocates nothing. */
    assert_int_equal(byte_buffer_set_len(&buf, 0), PLCTAG_STATUS_OK);
    assert_null(buf);
}


static void test_first_allocation_is_exact(void **state) {
    byte_buffer_t *buf = NULL;

    (void)state;

    grow(&buf, 4, 4);

    byte_buffer_free(&buf);
    assert_null(buf);
}


static void test_doubles_below_max_step(void **state) {
    byte_buffer_t *buf = NULL;

    (void)state;

    grow(&buf, 100, 100);
    grow(&buf, 101, 200);
    grow(&buf, 200, 200); /* fits, no change */
    grow(&buf, 201, 400);

    byte_buffer_free(&buf);
}


static void test_step_caps_at_max_step(void **state) {
    byte_buffer_t *buf = NULL;

    (void)state;

    grow(&buf, 3000, 3000);
    grow(&buf, 3001, 3000 + BYTE_BUFFER_MAX_STEP);
    grow(&buf, 3000 + BYTE_BUFFER_MAX_STEP + 1, 3000 + 2 * BYTE_BUFFER_MAX_STEP);

    byte_buffer_free(&buf);
}


static void test_jump_past_one_step_is_exact(void **state) {
    byte_buffer_t *buf = NULL;

    (void)state;

    grow(&buf, 100, 100);
    grow(&buf, 500, 500); /* more than double */
    grow(&buf, 3000, 3000);
    grow(&buf, 9000, 9000); /* more than one max step */

    byte_buffer_free(&buf);
}


static void test_shrink_keeps_capacity(void **state) {
    byte_buffer_t *buf = NULL;

    (void)state;

    grow(&buf, 100, 100);
    grow(&buf, 101, 200);
    grow(&buf, 0, 200);
    grow(&buf, 150, 200); /* regrows inside the capacity */

    byte_buffer_free(&buf);
}


static void test_new_bytes_zero_old_bytes_kept(void **state) {
    byte_buffer_t *buf = NULL;

    (void)state;

    grow(&buf, 8, 8);
    memset(buf->data, 0xAA, 8);

    /* shrink, then grow back inside the capacity: the bytes past the short length are zeroed. */
    grow(&buf, 4, 8);
    grow(&buf, 8, 8);
    for(int32_t i = 0; i < 4; i++) { assert_int_equal(buf->data[i], 0xAA); }
    for(int32_t i = 4; i < 8; i++) { assert_int_equal(buf->data[i], 0); }

    /* grow past the capacity: the old bytes move with it and the new ones are zero. */
    grow(&buf, 1000, 1000);
    for(int32_t i = 0; i < 4; i++) { assert_int_equal(buf->data[i], 0xAA); }
    for(int32_t i = 4; i < 1000; i++) { assert_int_equal(buf->data[i], 0); }

    byte_buffer_free(&buf);
}


static void test_failure_leaves_buffer(void **state) {
    byte_buffer_t *buf = NULL;
    byte_buffer_t *before = NULL;

    (void)state;

    grow(&buf, 16, 16);
    before = buf;

    assert_int_equal(byte_buffer_set_len(&buf, -1), PLCTAG_ERR_BAD_PARAM);
    assert_int_equal(byte_buffer_set_len(&buf, INT32_MAX), PLCTAG_ERR_TOO_LARGE);

    assert_true(buf == before);
    assert_int_equal(buf->len, 16);
    assert_int_equal(buf->capacity, 16);

    byte_buffer_free(&buf);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_null_is_empty),
        cmocka_unit_test(test_first_allocation_is_exact),
        cmocka_unit_test(test_doubles_below_max_step),
        cmocka_unit_test(test_step_caps_at_max_step),
        cmocka_unit_test(test_jump_past_one_step_is_exact),
        cmocka_unit_test(test_shrink_keeps_capacity),
        cmocka_unit_test(test_new_bytes_zero_old_bytes_kept),
        cmocka_unit_test(test_failure_leaves_buffer),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
