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
 * Unit tests for initialize_modules() in src/libplctag/lib/init.c.
 *
 * The module-init functions it calls are replaced by the mocks below. init.c
 * is compiled directly into this test binary (see CMakeLists.txt), so its
 * calls are redirected at compile time via -Dlib_init=mock_lib_init and
 * friends. No linker features are involved, which is what lets this test
 * build on MSVC and Apple's ld64 as well as GNU ld.
 *
 * Link-order shadowing was the other option and does not work here: every
 * mocked *_init shares an object file with the *_teardown that
 * destroy_modules() calls (lib_init/lib_teardown in lib.c, ab_init/ab_teardown
 * in ab_common.c, and so on), so the linker must pull that member from the
 * archive and the real *_init comes with it, colliding with the mock.
 *
 * The teardown functions are deliberately not mocked; they resolve to the
 * real implementations in the library.
 */

#include "mini_mock.h"

#include <libplctag/lib/init.h>
#include <libplctag/lib/libplctag.h>
#include <utils/rc.h>

/*
 * These return int rather than int32_t to stay type-compatible with the real
 * declarations they replace (extern int lib_init(void) in tag.h, and so on).
 * The -D redirection renames the declaration in the header, so the definition
 * here has to match it exactly.
 */

int mock_refcount_startup(void) { return mock_type(int); }

int mock_lib_init(void) { return mock_type(int); }

int mock_ab_init(void) { return mock_type(int); }

int mock_mb_init(void) { return mock_type(int); }

int mock_omron_init(void) { return mock_type(int); }


/* If lib_init() fails, initialization must bail out and report the failure. */
static void test_initialization_fails_if_lib_init_fails(void **state) {
    (void)state;

    will_return(mock_refcount_startup, PLCTAG_STATUS_OK);
    will_return(mock_lib_init, PLCTAG_ERR_BAD_STATUS);

    int rc = initialize_modules();

    assert_int_equal(rc, PLCTAG_ERR_BAD_STATUS);
}


/* Every module starts cleanly, so initialization must succeed. */
static void test_initialization_success(void **state) {
    (void)state;

    will_return(mock_refcount_startup, PLCTAG_STATUS_OK);
    will_return(mock_lib_init, PLCTAG_STATUS_OK);
    will_return(mock_ab_init, PLCTAG_STATUS_OK);
    will_return(mock_mb_init, PLCTAG_STATUS_OK);
    will_return(mock_omron_init, PLCTAG_STATUS_OK);

    int rc = initialize_modules();

    assert_int_equal(rc, PLCTAG_STATUS_OK);

    destroy_modules();
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_initialization_fails_if_lib_init_fails),
        cmocka_unit_test(test_initialization_success),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
