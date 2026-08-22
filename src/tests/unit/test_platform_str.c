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
 * Unit tests for str_to_int() and str_to_float() in the platform shim.
 *
 * Both call a strto*() function and then test errno for ERANGE. strto*() sets
 * errno on failure but never clears it on success, so without an explicit
 * errno = 0 first, a stale ERANGE left by any earlier call is read back as this
 * conversion's own failure. The tests below poison errno the way a real earlier
 * failure would, then check the conversion still succeeds.
 *
 * str_to_float() is the one that bites in practice: its ERANGE test also covers
 * underflow-to-zero, so a stale ERANGE rejects a plain "0".
 *
 * No mocking here, so this test gets none of the -D redirection that test_init.c
 * needs -- it links the real library and calls the real functions.
 */

#include "mini_mock.h"

#include <errno.h>
#include <platform.h>


/* A stale ERANGE must not turn a valid integer conversion into a failure. */
static void test_str_to_int_ignores_stale_errno(void **state) {
    (void)state;

    int val = 0;

    errno = ERANGE;

    assert_int_equal(str_to_int("42", &val), 0);
    assert_int_equal(val, 42);
}


/* A stale ERANGE must not reject "0", whose value matches the underflow check. */
static void test_str_to_float_ignores_stale_errno(void **state) {
    (void)state;

    float val = 1.0f;

    errno = ERANGE;

    assert_int_equal(str_to_float("0", &val), 0);
    assert_int_equal(val == 0.0f, 1);

    errno = ERANGE;

    assert_int_equal(str_to_float("1.5", &val), 0);
    assert_int_equal(val == 1.5f, 1);
}


/* Clearing errno must not stop a real out-of-range value being reported. */
static void test_str_to_int_still_detects_overflow(void **state) {
    (void)state;

    int val = 0;

    errno = 0;

    assert_int_equal(str_to_int("999999999999999999999999", &val), -1);
}


/*
 * A value that fits in a long but not in an int must be reported, not truncated.  On LP64
 * "4294967296" casts to zero, which a caller cannot tell from a real zero -- and callers do
 * divide by what they get back.
 */
static void test_str_to_int_rejects_values_wider_than_int(void **state) {
    (void)state;

    int val = 12345;

    errno = 0;

    assert_int_equal(str_to_int("4294967296", &val), -1);
    assert_int_equal(val, 12345); /* unchanged on failure. */

    errno = 0;

    assert_int_equal(str_to_int("-4294967296", &val), -1);
    assert_int_equal(val, 12345);

    /* the boundary values themselves must still convert. */
    errno = 0;

    assert_int_equal(str_to_int("2147483647", &val), 0);
    assert_int_equal(val, 2147483647);

    errno = 0;

    assert_int_equal(str_to_int("-2147483648", &val), 0);
    assert_int_equal(val, -2147483647 - 1);
}


/* A string with no digits at all is still an error. */
static void test_str_to_int_rejects_non_numeric(void **state) {
    (void)state;

    int val = 0;

    errno = 0;

    assert_int_equal(str_to_int("notanumber", &val), -1);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_str_to_int_ignores_stale_errno),
        cmocka_unit_test(test_str_to_float_ignores_stale_errno),
        cmocka_unit_test(test_str_to_int_still_detects_overflow),
        cmocka_unit_test(test_str_to_int_rejects_values_wider_than_int),
        cmocka_unit_test(test_str_to_int_rejects_non_numeric),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
