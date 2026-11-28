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

#include <utils/atomic_utils.h>
#include <utils/debug.h>

#if defined(_MSC_VER)

#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>

/*
 * Every access goes through an Interlocked call, loads included: a plain access is not atomic
 * for 64-bit values on 32-bit Windows, and a plain read-then-write is not an exchange (U1).
 * InterlockedCompareExchange(a, 0, 0) is the atomic load.
 */

void atomic_init_bool(atomic_bool *a, bool new_val) { *a = (long)new_val; }

bool atomic_get_bool(atomic_bool *a) { return InterlockedCompareExchange(a, 0, 0) != 0; }

bool atomic_set_bool(atomic_bool *a, bool new_val) { return InterlockedExchange(a, (long)new_val) != 0; }

/* returns the original value, like every compare-and-set here. */
bool atomic_compare_and_set_bool(atomic_bool *a, bool old_val, bool new_val) {
    return InterlockedCompareExchange(a, (long)new_val, (long)old_val) != 0;
}

void atomic_init_int32(atomic_int32_t *a, int32_t new_val) { *a = new_val; }

int32_t atomic_get_int32(atomic_int32_t *a) { return (int32_t)InterlockedCompareExchange((volatile LONG *)a, 0, 0); }

int32_t atomic_set_int32(atomic_int32_t *a, int32_t new_val) {
    return (int32_t)InterlockedExchange((volatile LONG *)a, (LONG)new_val);
}

int32_t atomic_add_int32(atomic_int32_t *a, int32_t other) {
    return (int32_t)InterlockedExchangeAdd((volatile LONG *)a, (LONG)other);
}

int32_t atomic_compare_and_set_int32(atomic_int32_t *a, int32_t old_val, int32_t new_val) {
    return (int32_t)InterlockedCompareExchange((volatile LONG *)a, (LONG)new_val, (LONG)old_val);
}

void atomic_init_int64(atomic_int64_t *a, int64_t new_val) { *a = new_val; }

int64_t atomic_get_int64(atomic_int64_t *a) { return InterlockedCompareExchange64((volatile LONGLONG *)a, 0, 0); }

int64_t atomic_set_int64(atomic_int64_t *a, int64_t new_val) { return InterlockedExchange64((volatile LONGLONG *)a, new_val); }

int64_t atomic_add_int64(atomic_int64_t *a, int64_t other) { return InterlockedExchangeAdd64((volatile LONGLONG *)a, other); }

int64_t atomic_compare_and_set_int64(atomic_int64_t *a, int64_t old_val, int64_t new_val) {
    return InterlockedCompareExchange64((volatile LONGLONG *)a, new_val, old_val);
}

#else

#    include <stdatomic.h>

void atomic_init_bool(atomic_bool *a, bool new_val) { atomic_init(a, new_val); }

bool atomic_get_bool(atomic_bool *a) { return atomic_load(a); }

bool atomic_set_bool(atomic_bool *a, bool new_val) { return atomic_exchange(a, new_val); }

bool atomic_compare_and_set_bool(atomic_bool *a, bool old_val, bool new_val) {
    bool expected = old_val;
    atomic_compare_exchange_strong(a, &expected, new_val);
    return expected;
}

void atomic_init_int32(atomic_int32_t *a, int32_t new_val) { atomic_init(a, new_val); }

int32_t atomic_get_int32(atomic_int32_t *a) { return atomic_load(a); }

int32_t atomic_set_int32(atomic_int32_t *a, int32_t new_val) { return atomic_exchange(a, new_val); }

int32_t atomic_add_int32(atomic_int32_t *a, int32_t other) { return atomic_fetch_add(a, other); }

int32_t atomic_compare_and_set_int32(atomic_int32_t *a, int32_t old_val, int32_t new_val) {
    int32_t expected = old_val;
    atomic_compare_exchange_strong(a, &expected, new_val);
    return expected;
}

void atomic_init_int64(atomic_int64_t *a, int64_t new_val) { atomic_init(a, new_val); }

int64_t atomic_get_int64(atomic_int64_t *a) { return atomic_load(a); }

int64_t atomic_set_int64(atomic_int64_t *a, int64_t new_val) { return atomic_exchange(a, new_val); }

int64_t atomic_add_int64(atomic_int64_t *a, int64_t other) { return atomic_fetch_add(a, other); }

int64_t atomic_compare_and_set_int64(atomic_int64_t *a, int64_t old_val, int64_t new_val) {
    int64_t expected = old_val;
    atomic_compare_exchange_strong(a, &expected, new_val);
    return expected;
}

#endif
