/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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

/* This is all cobbled together from many Internet sources such as
 * GitHub, StackExchange, Microsoft's site etc.
 *
 * WARNING: The use of volatile when C11 atomics are not present
 * _might_ not work on your CPU architecture.  Only some architectures
 * promise atomic reads and writes of data.   Those include all x86 and
 * ARM systems.  Possibly Power too.  YMMV.
 */

#if defined(__STDC_NO_ATOMICS__) || !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 11)

#    ifdef _WIN32
#       define WIN32_LEAN_AND_MEAN
#       include <Windows.h>
#    endif

void atomic_init_bool(atomic_bool *a, bool new_val) { *a = new_val; }

bool atomic_get_bool(atomic_bool *a) { return *a; }

bool atomic_set_bool(atomic_bool *a, bool new_val) {
    bool old = *a;
    *a = new_val;
    return old;
}

bool atomic_compare_and_set_bool(atomic_bool *a, bool old_val, bool new_val) {
#    ifdef _WIN32
    /* Windows does not have a native single byte atomic */
    return InterlockedCompareExchange16(a, new_val, old_val) == old_val;
#    else
    bool expected = old_val;
    return __atomic_compare_exchange_n(a, &expected, new_val, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#    endif
}

void atomic_init_int32(atomic_int32_t *a, int32_t new_val) { *a = new_val; }

int32_t atomic_get_int32(atomic_int32_t *a) { return *a; }

int32_t atomic_set_int32(atomic_int32_t *a, int32_t new_val) {
    int32_t old = *a;
    *a = new_val;
    return old;
}

int32_t atomic_add_int32(atomic_int32_t *a, int32_t other) {
#    ifdef _WIN32
    return InterlockedExchangeAdd((LONG *)a, other);
#    else
    return __atomic_fetch_add(a, other, __ATOMIC_SEQ_CST);
#    endif
}

int32_t atomic_compare_and_set_int32(atomic_int32_t *a, int32_t old_val, int32_t new_val) {
#    ifdef _WIN32
    return InterlockedCompareExchange((LONG *)a, new_val, old_val);
#    else
    int32_t expected = old_val;
    __atomic_compare_exchange_n(a, &expected, new_val, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
#    endif
}

void atomic_init_int64(atomic_int64_t *a, int64_t new_val) { *a = new_val; }

int64_t atomic_get_int64(atomic_int64_t *a) { return *a; }

int64_t atomic_set_int64(atomic_int64_t *a, int64_t new_val) {
    int64_t old = *a;
    *a = new_val;
    return old;
}

int64_t atomic_add_int64(atomic_int64_t *a, int64_t other) {
#    ifdef _WIN32
    return InterlockedExchangeAdd64((LONGLONG *)a, other);
#    else
    return __atomic_fetch_add(a, other, __ATOMIC_SEQ_CST);
#    endif
}

int64_t atomic_compare_and_set_int64(atomic_int64_t *a, int64_t old_val, int64_t new_val) {
#    ifdef _WIN32
    return InterlockedCompareExchange64((LONGLONG *)a, new_val, old_val);
#    else
    int64_t expected = old_val;
    __atomic_compare_exchange_n(a, &expected, new_val, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
#    endif
}

#else

#    include <stdatomic.h>

void atomic_init_bool(atomic_bool *a, bool new_val) { atomic_init(a, new_val); }

bool atomic_get_bool(atomic_bool *a) { return atomic_load(a); }

bool atomic_set_bool(atomic_bool *a, bool new_val) { return atomic_exchange(a, new_val); }

bool atomic_compare_and_set_bool(atomic_bool *a, bool old_val, bool new_val) {
    return atomic_compare_exchange_strong(a, &old_val, new_val);
}

void atomic_init_int32(atomic_int32_t *a, int32_t new_val) { atomic_init(a, new_val); }

int32_t atomic_get_int32(atomic_int32_t *a) { return atomic_load(a); }

int32_t atomic_set_int32(atomic_int32_t *a, int32_t new_val) { return atomic_exchange(a, new_val); }

int32_t atomic_add_int32(atomic_int32_t *a, int32_t other) { return atomic_fetch_add(a, other); }

int32_t atomic_compare_and_set_int32(atomic_int32_t *a, int32_t old_val, int32_t new_val) {
    return atomic_compare_exchange_strong(a, &old_val, new_val);
}

void atomic_init_int64(atomic_int64_t *a, int64_t new_val) { atomic_init(a, new_val); }

int64_t atomic_get_int64(atomic_int64_t *a) { return atomic_load(a); }

int64_t atomic_set_int64(atomic_int64_t *a, int64_t new_val) { return atomic_exchange(a, new_val); }

int64_t atomic_add_int64(atomic_int64_t *a, int64_t other) { return atomic_fetch_add(a, other); }

int64_t atomic_compare_and_set_int64(atomic_int64_t *a, int64_t old_val, int64_t new_val) {
    return atomic_compare_exchange_strong(a, &old_val, new_val);
}

#endif
