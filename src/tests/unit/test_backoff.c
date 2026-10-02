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
 * Unit tests for the shared connection retry backoff.
 *
 * These exist because of a reported field failure: the old per-protocol backoff shifted
 * a 32-bit 1 by the attempt count, so once a device stayed down long enough the shift
 * overflowed and the delay collapsed to roughly nothing.  Reporters saw hundreds of
 * reconnects per second after a few minutes of outage.  Two properties have to hold for
 * every attempt count, however large:
 *
 *   - the wait never exceeds the configured maximum, and
 *   - the wait never collapses below half the maximum once the backoff has saturated.
 *
 * The attempt counts the loops below reach are far past the 26 and 32 where the old code
 * broke on 64-bit and ARMv7.  The whole file is also an undefined-behavior check in its
 * own right: these builds run with -fsanitize=undefined -fno-sanitize-recover=all, so a
 * shift or signed overflow anywhere in backoff_wait_ms() aborts the binary rather than
 * quietly producing a wrong number.
 *
 * No mocking, so no -D redirection: this links the real library and calls the real
 * functions.  The jitter makes each wait random, so the assertions are on the range the
 * wait must fall in, never on an exact value.
 */

#include "mini_mock.h"

#include <stdint.h>
#include <utils/backoff.h>


/* Ranges, not values: every wait is jittered, so an exact expectation would be flaky. */
#define assert_in_range(value, low, high)                                                                            \
    do {                                                                                                             \
        int64_t range_value = (int64_t)(value);                                                                      \
        int64_t range_low = (int64_t)(low);                                                                          \
        int64_t range_high = (int64_t)(high);                                                                        \
        if(range_value < range_low || range_value > range_high) {                                                    \
            fprintf(stderr, "%s:%d: assert_in_range failed: %s (%" PRId64 ") not in [%" PRId64 ", %" PRId64 "]\n",   \
                    __FILE__, __LINE__, #value, range_value, range_low, range_high);                                 \
            abort();                                                                                                 \
        }                                                                                                            \
    } while(0)


/* The bounds the CIP module uses, so the tests exercise the real configuration. */
#define TEST_INITIAL_MS ((int64_t)100)
#define TEST_MAX_MS ((int64_t)32000)

/*
 * Enough attempts to pass every count the field report named -- 26 and 32, where the old
 * 32-bit shift broke -- and then keep going by four orders of magnitude.
 */
#define TEST_LONG_OUTAGE_ATTEMPTS (100000)

/* By this attempt the delay has reached the maximum for any sane set of bounds. */
#define TEST_SATURATED_AFTER (20)


/* The first wait is drawn from the initial delay, not from the maximum. */
static void test_first_wait_is_near_initial(void **state) {
    (void)state;

    backoff_t backoff;

    backoff_init(&backoff, TEST_INITIAL_MS, TEST_MAX_MS);

    assert_in_range(backoff_wait_ms(&backoff), TEST_INITIAL_MS / 2, TEST_INITIAL_MS);
}


/* Growth is monotonic up to the cap and then stops, so the base never runs away. */
static void test_base_saturates_at_max(void **state) {
    (void)state;

    backoff_t backoff;

    backoff_init(&backoff, TEST_INITIAL_MS, TEST_MAX_MS);

    for(int32_t i = 0; i < TEST_SATURATED_AFTER; i++) { (void)backoff_wait_ms(&backoff); }

    assert_int_equal(backoff.base_ms, TEST_MAX_MS);

    /* and it stays there rather than wrapping or drifting. */
    for(int32_t i = 0; i < TEST_SATURATED_AFTER; i++) {
        (void)backoff_wait_ms(&backoff);
        assert_int_equal(backoff.base_ms, TEST_MAX_MS);
    }
}


/*
 * The regression itself.  Over an outage far longer than the one that broke the old code,
 * every single wait stays inside the configured range, and once saturated never drops
 * below half the maximum.  A collapse to near-zero -- the reported failure -- fails here.
 */
static void test_long_outage_never_collapses_or_overshoots(void **state) {
    (void)state;

    backoff_t backoff;
    int32_t attempt = 0;

    backoff_init(&backoff, TEST_INITIAL_MS, TEST_MAX_MS);

    for(attempt = 0; attempt < TEST_LONG_OUTAGE_ATTEMPTS; attempt++) {
        int64_t wait_ms = backoff_wait_ms(&backoff);

        /* never longer than asked for, and never zero or negative. */
        assert_in_range(wait_ms, 1, TEST_MAX_MS);

        /* and once the backoff has saturated, never shorter than half the maximum. */
        if(attempt >= TEST_SATURATED_AFTER) { assert_in_range(wait_ms, TEST_MAX_MS / 2, TEST_MAX_MS); }
    }
}


/*
 * The counts the field report called out by name.  Checked individually so a failure
 * names the attempt rather than being buried in the bulk loop above.
 */
static void test_reported_attempt_counts(void **state) {
    (void)state;

    static const int32_t reported[] = {0, 6, 7, 26, 31, 32, 255};
    size_t i = 0;

    for(i = 0; i < sizeof(reported) / sizeof(reported[0]); i++) {
        backoff_t backoff;
        int32_t skip = 0;
        int64_t wait_ms = 0;

        backoff_init(&backoff, TEST_INITIAL_MS, TEST_MAX_MS);

        for(skip = 0; skip < reported[i]; skip++) { (void)backoff_wait_ms(&backoff); }

        wait_ms = backoff_wait_ms(&backoff);

        assert_in_range(wait_ms, 1, TEST_MAX_MS);

        if(reported[i] >= TEST_SATURATED_AFTER) { assert_in_range(wait_ms, TEST_MAX_MS / 2, TEST_MAX_MS); }
    }
}


/*
 * The attempt counter is unsigned and reported only, so its wrap must not reach the delay.
 * Wind it to the edge and step over.
 */
static void test_attempt_counter_wrap_is_harmless(void **state) {
    (void)state;

    backoff_t backoff;
    int32_t i = 0;

    backoff_init(&backoff, TEST_INITIAL_MS, TEST_MAX_MS);

    for(i = 0; i < TEST_SATURATED_AFTER; i++) { (void)backoff_wait_ms(&backoff); }

    backoff.attempts = UINT32_MAX;

    (void)backoff_wait_ms(&backoff);

    assert_int_equal(backoff.attempts, 0);

    /* the wrap changed the count and nothing else. */
    for(i = 0; i < TEST_SATURATED_AFTER; i++) {
        assert_in_range(backoff_wait_ms(&backoff), TEST_MAX_MS / 2, TEST_MAX_MS);
    }
}


/* After a success the next failure waits from the beginning again, not from the cap. */
static void test_reset_returns_to_initial(void **state) {
    (void)state;

    backoff_t backoff;
    int32_t i = 0;

    backoff_init(&backoff, TEST_INITIAL_MS, TEST_MAX_MS);

    for(i = 0; i < TEST_SATURATED_AFTER; i++) { (void)backoff_wait_ms(&backoff); }

    backoff_reset(&backoff);

    assert_int_equal(backoff.attempts, 0);
    assert_in_range(backoff_wait_ms(&backoff), TEST_INITIAL_MS / 2, TEST_INITIAL_MS);
}


/*
 * Jitter has to be redrawn on every call.  A backoff that computed its random share once
 * and held it would let a fleet of clients knocked down by one device reboot retry in
 * lockstep; this is the check that the value actually moves.
 */
static void test_saturated_waits_still_vary(void **state) {
    (void)state;

    backoff_t backoff;
    int64_t first = 0;
    int32_t i = 0;
    int32_t distinct = 0;

    backoff_init(&backoff, TEST_INITIAL_MS, TEST_MAX_MS);

    for(i = 0; i < TEST_SATURATED_AFTER; i++) { (void)backoff_wait_ms(&backoff); }

    first = backoff_wait_ms(&backoff);

    /*
     * 64 draws from a 16000 ms window.  All of them landing on the same millisecond is
     * not a plausible outcome of a working jitter, so no retry or tolerance is needed.
     */
    for(i = 0; i < 64; i++) {
        if(backoff_wait_ms(&backoff) != first) { distinct++; }
    }

    if(distinct == 0) {
        fprintf(stderr, "%s:%d: 65 saturated waits were all %" PRId64 "ms -- jitter is not being redrawn\n", __FILE__,
                __LINE__, first);
        abort();
    }
}


/*
 * Degenerate bounds must not produce a zero, negative or shrinking delay.  Nothing in the
 * library passes these today; they are here so that a future caller reading its bounds
 * from an attribute cannot turn a retry loop into a spin loop.
 */
static void test_degenerate_bounds_are_clamped(void **state) {
    (void)state;

    backoff_t backoff;
    int32_t i = 0;

    /* zero bounds */
    backoff_init(&backoff, 0, 0);
    for(i = 0; i < 64; i++) { assert_in_range(backoff_wait_ms(&backoff), 1, 1); }

    /* negative bounds */
    backoff_init(&backoff, -5, -10);
    for(i = 0; i < 64; i++) { assert_in_range(backoff_wait_ms(&backoff), 1, 1); }

    /* a maximum below the initial delay: the initial delay wins and nothing grows. */
    backoff_init(&backoff, 100, 10);
    assert_int_equal(backoff.max_ms, 100);
    for(i = 0; i < 64; i++) { assert_in_range(backoff_wait_ms(&backoff), 1, 100); }
}


/* The Modbus module's bounds, which differ from CIP's, must behave the same way. */
static void test_modbus_bounds_behave_the_same(void **state) {
    (void)state;

    backoff_t backoff;
    int32_t attempt = 0;

    backoff_init(&backoff, 50, 5000);

    for(attempt = 0; attempt < TEST_LONG_OUTAGE_ATTEMPTS; attempt++) {
        int64_t wait_ms = backoff_wait_ms(&backoff);

        assert_in_range(wait_ms, 1, 5000);

        if(attempt >= TEST_SATURATED_AFTER) { assert_in_range(wait_ms, 2500, 5000); }
    }
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_first_wait_is_near_initial),
        cmocka_unit_test(test_base_saturates_at_max),
        cmocka_unit_test(test_long_outage_never_collapses_or_overshoots),
        cmocka_unit_test(test_reported_attempt_counts),
        cmocka_unit_test(test_attempt_counter_wrap_is_harmless),
        cmocka_unit_test(test_reset_returns_to_initial),
        cmocka_unit_test(test_saturated_waits_still_vary),
        cmocka_unit_test(test_degenerate_bounds_are_clamped),
        cmocka_unit_test(test_modbus_bounds_behave_the_same),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
