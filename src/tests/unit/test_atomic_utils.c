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
 * Tests for utils/atomic_utils.
 *
 * Two things are checked.  First, that the set/compare-and-set functions
 * return the ORIGINAL value, which is the contract stated in atomic_utils.h
 * and which callers such as debug.c depend on.  Second, that set is a real
 * atomic exchange and not a read followed by a write, by building a spin
 * lock on it and running enough threads through a guarded counter that a
 * lost update shows up.
 *
 * atomic_utils.c has two entirely separate implementations: a C11 one using
 * <stdatomic.h>, and a fallback for compilers without it.  The build picks
 * one and the other never gets compiled, so this test is built twice -- see
 * the CMakeLists in this directory.  test_atomic_utils_no_c11 forces the
 * fallback with -D__STDC_NO_ATOMICS__.  Before both paths were made to
 * agree, the fallback returned a success flag from compare-and-set instead
 * of the original value, and its non-atomic set deadlocked this test's spin
 * lock outright.
 */

#include <stdbool.h>
#include <stdint.h>

#include <libplctag/lib/libplctag.h>
#include <utils/atomic_utils.h>
#include <utils/thread.h>

#include "mini_mock.h"


#define TEST_THREAD_COUNT (8)
#define TEST_ITERATIONS (20000)


static atomic_bool spin_lock_flag = ATOMIC_BOOL_STATIC_INIT;
static atomic_bool cas_once_flag = ATOMIC_BOOL_STATIC_INIT;
static atomic_int32_t atomic_counter = ATOMIC_INT_STATIC_INIT;
static int32_t guarded_counter = 0;
static int32_t cas_winners = 0;


static bool spin_lock_try(atomic_bool *flag) { return !atomic_set_bool(flag, true); }


static void spin_lock_get(atomic_bool *flag) {
    while(!spin_lock_try(flag)) { thread_yield(); }
}


static void spin_lock_put(atomic_bool *flag) { atomic_set_bool(flag, false); }


static THREAD_FUNC(hammer_thread) {
    (void)arg;

    for(int32_t iteration = 0; iteration < TEST_ITERATIONS; iteration++) {
        spin_lock_get(&spin_lock_flag);
        guarded_counter++;
        spin_lock_put(&spin_lock_flag);

        atomic_add_int32(&atomic_counter, 1);
    }

    /* exactly one thread may see the original value of false */
    if(atomic_compare_and_set_bool(&cas_once_flag, false, true) == false) {
        spin_lock_get(&spin_lock_flag);
        cas_winners++;
        spin_lock_put(&spin_lock_flag);
    }

    THREAD_RETURN(0);
}


/* set returns the value that was there before, not the value written. */
static void test_set_bool_returns_original(void **state) {
    atomic_bool val = ATOMIC_BOOL_STATIC_INIT;

    (void)state;

    atomic_init_bool(&val, false);

    assert_int_equal(atomic_set_bool(&val, true), false);
    assert_int_equal(atomic_get_bool(&val), true);
    assert_int_equal(atomic_set_bool(&val, true), true);
    assert_int_equal(atomic_set_bool(&val, false), true);
    assert_int_equal(atomic_get_bool(&val), false);
}


/*
 * compare_and_set returns the original value, so a caller that wanted the
 * swap tests the result against the value it expected to find.  Returning a
 * success flag here inverts every such caller.
 */
static void test_compare_and_set_bool_returns_original(void **state) {
    atomic_bool val = ATOMIC_BOOL_STATIC_INIT;

    (void)state;

    atomic_init_bool(&val, false);

    /* swap succeeds: original was false */
    assert_int_equal(atomic_compare_and_set_bool(&val, false, true), false);
    assert_int_equal(atomic_get_bool(&val), true);

    /* swap fails: original is true, and the value is left alone */
    assert_int_equal(atomic_compare_and_set_bool(&val, false, true), true);
    assert_int_equal(atomic_get_bool(&val), true);
}


static void test_set_int32_returns_original(void **state) {
    atomic_int32_t val = ATOMIC_INT_STATIC_INIT;

    (void)state;

    atomic_init_int32(&val, 7);

    assert_int_equal(atomic_set_int32(&val, 9), 7);
    assert_int_equal(atomic_get_int32(&val), 9);
    assert_int_equal(atomic_add_int32(&val, 3), 9);
    assert_int_equal(atomic_get_int32(&val), 12);
    assert_int_equal(atomic_compare_and_set_int32(&val, 12, 20), 12);
    assert_int_equal(atomic_get_int32(&val), 20);
    assert_int_equal(atomic_compare_and_set_int32(&val, 12, 30), 20);
    assert_int_equal(atomic_get_int32(&val), 20);
}


static void test_set_int64_returns_original(void **state) {
    atomic_int64_t val = ATOMIC_INT_STATIC_INIT;

    (void)state;

    atomic_init_int64(&val, 7);

    assert_int_equal(atomic_set_int64(&val, 9), 7);
    assert_int_equal(atomic_get_int64(&val), 9);
    assert_int_equal(atomic_add_int64(&val, 3), 9);
    assert_int_equal(atomic_get_int64(&val), 12);
    assert_int_equal(atomic_compare_and_set_int64(&val, 12, 20), 12);
    assert_int_equal(atomic_get_int64(&val), 20);
}


/*
 * The real check that set is an exchange.  If it is a read followed by a
 * write then two threads can both see false and both take the lock, and the
 * guarded counter loses updates.  A lost update on release is worse still:
 * the flag stays set and every thread spins forever, so a failure here can
 * show up as a hang rather than a wrong count.
 */
static void test_set_bool_is_atomic_exchange(void **state) {
    thread_p threads[TEST_THREAD_COUNT] = {0};

    (void)state;

    atomic_init_bool(&spin_lock_flag, false);
    atomic_init_bool(&cas_once_flag, false);
    atomic_init_int32(&atomic_counter, 0);
    guarded_counter = 0;
    cas_winners = 0;

    for(int32_t thread_id = 0; thread_id < TEST_THREAD_COUNT; thread_id++) {
        assert_int_equal(thread_create(&threads[thread_id], hammer_thread, 0, NULL), PLCTAG_STATUS_OK);
    }

    for(int32_t thread_id = 0; thread_id < TEST_THREAD_COUNT; thread_id++) {
        assert_int_equal(thread_join(&threads[thread_id]), PLCTAG_STATUS_OK);
    }

    assert_int_equal(guarded_counter, TEST_THREAD_COUNT * TEST_ITERATIONS);
    assert_int_equal(atomic_get_int32(&atomic_counter), TEST_THREAD_COUNT * TEST_ITERATIONS);

    /* only one thread can find cas_once_flag still false */
    assert_int_equal(cas_winners, 1);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_set_bool_returns_original),
        cmocka_unit_test(test_compare_and_set_bool_returns_original),
        cmocka_unit_test(test_set_int32_returns_original),
        cmocka_unit_test(test_set_int64_returns_original),
        cmocka_unit_test(test_set_bool_is_atomic_exchange),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
