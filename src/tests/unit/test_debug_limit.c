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
 * Unit tests for the per call site log limit in pdebug().
 *
 * A warning hit on every packet once wrote over 100000 lines a second from the I/O thread and
 * stalled the exchanges behind it. Such a message now prints the first time and then at most once
 * per DEBUG_LIMIT_MS from each call site that ORs DEBUG_RATE_LIMIT into its level. The values in
 * the message do not matter: the call site is the key. Sites without the flag always print.
 */

#include "mini_mock.h"

#include <stdint.h>
#include <string.h>
#include <utils/debug.h>


static int32_t lines = 0;
static int last_level = 0;
static char last_line[1000];

static void count_line(int32_t tag_id, int debug_level, const char *message) {
    (void)tag_id;
    last_level = debug_level;
    lines++;
    strncpy(last_line, message, sizeof(last_line) - 1);
}


static void setup(void) {
    lines = 0;
    debug_set_all_modules(DEBUG_DETAIL);
    assert_int_equal(debug_register_logger(count_line), PLCTAG_STATUS_OK);
}


static void teardown(void) { assert_int_equal(debug_unregister_logger(), PLCTAG_STATUS_OK); }


/* One call site, different values each time: one line. */
static void test_warn_logs_once_per_site(void **state) {
    (void)state;

    setup();

    for(int32_t i = 0; i < 1000; i++) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN | DEBUG_RATE_LIMIT, i, "Request of %d bytes does not fit.", i);
    }

    assert_int_equal(lines, 1);
    assert_int_equal(last_level, DEBUG_WARN);
    teardown();
}


/* The limit is per site: a second site still logs. */
static void test_sites_are_independent(void **state) {
    (void)state;

    setup();

    for(int32_t i = 0; i < 10; i++) { pdebug(DEBUG_MODULE_LIB, DEBUG_WARN | DEBUG_RATE_LIMIT, 0, "first site %d", i); }
    for(int32_t i = 0; i < 10; i++) { pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL | DEBUG_RATE_LIMIT, 0, "second site %d", i); }

    assert_int_equal(lines, 2);
    teardown();
}


/* Without the flag, every message prints at every level. */
static void test_unflagged_is_never_limited(void **state) {
    (void)state;

    setup();

    for(int32_t i = 0; i < 10; i++) { pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "warn %d", i); }
    for(int32_t i = 0; i < 10; i++) { pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, 0, "detail %d", i); }

    assert_int_equal(lines, 20);
    teardown();
}


/* After the interval the site logs again, preceded by the count it dropped. */
static void test_reports_suppressed_count(void **state) {
    (void)state;

    setup();

    debug_limit_t limit;

    memset(&limit, 0, sizeof(limit));

    assert_true(debug_limit_pass(&limit, __func__, __LINE__, DEBUG_WARN, DEBUG_MODULE_LIB, 0));
    assert_true(!debug_limit_pass(&limit, __func__, __LINE__, DEBUG_WARN, DEBUG_MODULE_LIB, 0));
    assert_true(!debug_limit_pass(&limit, __func__, __LINE__, DEBUG_WARN, DEBUG_MODULE_LIB, 0));
    assert_int_equal(lines, 0);

    /* as if DEBUG_LIMIT_MS had passed. */
    atomic_set_int64(&limit.next_ms, 0);

    assert_true(debug_limit_pass(&limit, __func__, __LINE__, DEBUG_WARN, DEBUG_MODULE_LIB, 0));
    assert_int_equal(lines, 1);
    assert_non_null(strstr(last_line, "Suppressed 2 messages"));
    teardown();
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_warn_logs_once_per_site),
        cmocka_unit_test(test_sites_are_independent),
        cmocka_unit_test(test_unflagged_is_never_limited),
        cmocka_unit_test(test_reports_suppressed_count),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
