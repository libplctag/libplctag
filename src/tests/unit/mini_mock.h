#pragma once

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
 * Minimal stand-in for the handful of CMocka macros the unit tests use:
 * will_return()/mock_type() value queuing, assert_int_equal(), and a
 * bare-bones test-array runner. Replaces the CMocka dependency, which had
 * to be downloaded at configure time via FetchContent and repeatedly failed
 * to do so on CI.
 *
 * This is not a general mocking framework. A failed assertion aborts the
 * whole test binary rather than failing just that one test and continuing;
 * real CMocka uses setjmp/longjmp per test to allow that. Acceptable for
 * the current set of independent tests, and the first thing to add back if
 * that ever matters.
 *
 * The queue below is file-static, so a test binary built from more than one
 * translation unit would get one queue per unit and cross-unit will_return()
 * would not work. Single translation unit today.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINI_MOCK_MAX_QUEUED (64)

typedef struct {
    const char *function;
    int64_t value;
} mini_mock_value_t;

typedef void (*mini_mock_test_fn)(void **state);

/* Named to match CMocka's type so the test bodies did not have to change. */
struct CMUnitTest {
    const char *name;
    mini_mock_test_fn test_func;
};

typedef struct CMUnitTest CMUnitTest;

/*
 * Global by necessity: the will_return()/mock_type() macro pair has no
 * object to hang state on, since mock_type() is expanded inside the mocked
 * function and has only its own name to go by.
 */
static mini_mock_value_t mini_mock_queue[MINI_MOCK_MAX_QUEUED];
static size_t mini_mock_queue_len = 0;


static inline void mini_mock_will_return(const char *function, int64_t value) {
    if(mini_mock_queue_len >= MINI_MOCK_MAX_QUEUED) {
        fprintf(stderr, "mini_mock: queue full, increase MINI_MOCK_MAX_QUEUED\n");
        abort();
    }

    mini_mock_queue[mini_mock_queue_len].function = function;
    mini_mock_queue[mini_mock_queue_len].value = value;
    mini_mock_queue_len++;
}


/* Values are consumed in FIFO order per function name, matching CMocka. */
static inline int64_t mini_mock_type(const char *function) {
    for(size_t i = 0; i < mini_mock_queue_len; i++) {
        if(strcmp(mini_mock_queue[i].function, function) == 0) {
            int64_t value = mini_mock_queue[i].value;

            memmove(&mini_mock_queue[i], &mini_mock_queue[i + 1], (mini_mock_queue_len - i - 1) * sizeof(mini_mock_queue[0]));
            mini_mock_queue_len--;

            return value;
        }
    }

    fprintf(stderr, "mini_mock: no queued value for %s()\n", function);
    abort();
}


static inline int32_t mini_mock_run_group_tests(const struct CMUnitTest tests[], size_t num_tests,
                                                int32_t (*group_setup)(void **), int32_t (*group_teardown)(void **)) {
    if(group_setup) { group_setup(NULL); }

    for(size_t i = 0; i < num_tests; i++) {
        printf("[ RUN      ] %s\n", tests[i].name);

        tests[i].test_func(NULL);

        /*
         * A queued value nobody consumed means the code under test did not
         * make a call the test expected. Silently dropping it would let the
         * test keep passing after the call it exists to verify has gone away.
         */
        if(mini_mock_queue_len != 0) {
            fprintf(stderr, "%s: %zu queued mock value(s) never consumed (first: %s)\n", tests[i].name, mini_mock_queue_len,
                    mini_mock_queue[0].function);
            abort();
        }

        printf("[       OK ] %s\n", tests[i].name);
    }

    if(group_teardown) { group_teardown(NULL); }

    printf("[==========] %zu test(s) passed.\n", num_tests);

    return 0;
}


#define will_return(function, value) mini_mock_will_return(#function, (int64_t)(value))

#define mock_type(type) ((type)mini_mock_type(__func__))

#define assert_int_equal(a, b)                                                                                                \
    do {                                                                                                                      \
        int64_t mini_mock_a = (int64_t)(a);                                                                                   \
        int64_t mini_mock_b = (int64_t)(b);                                                                                   \
        if(mini_mock_a != mini_mock_b) {                                                                                      \
            fprintf(stderr, "%s:%d: assert_int_equal failed: %s (%" PRId64 ") != %s (%" PRId64 ")\n", __FILE__, __LINE__, #a, \
                    mini_mock_a, #b, mini_mock_b);                                                                            \
            abort();                                                                                                          \
        }                                                                                                                     \
    } while(0)

#define cmocka_unit_test(f) {#f, (f)}

#define cmocka_run_group_tests(tests, setup, teardown) \
    mini_mock_run_group_tests((tests), sizeof(tests) / sizeof((tests)[0]), (setup), (teardown))
