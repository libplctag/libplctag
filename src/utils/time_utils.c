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
 * Monotonic time.
 *
 * Linux uses CLOCK_BOOTTIME because CLOCK_MONOTONIC stops while suspended there.  macOS
 * CLOCK_MONOTONIC already counts sleep.  CLOCK_BOOTTIME is used on Linux only: other systems that
 * define it may give it other meanings.  The BSDs use CLOCK_MONOTONIC, not yet checked across a
 * suspend.  Windows uses the performance counter, not GetTickCount64(): that one moves in 10-16ms
 * steps, and deadlines are as close as 1ms.  The counter includes suspended time.
 */

#include <stdint.h>
#include <utils/time_utils.h>

#if defined(_WIN32)
#    include <windows.h>

static int64_t perf_counter_scaled(int64_t units_per_sec) {
    LARGE_INTEGER count;
    LARGE_INTEGER freq;

    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&freq);

    /* split so the multiply cannot overflow. */
    return (count.QuadPart / freq.QuadPart) * units_per_sec + ((count.QuadPart % freq.QuadPart) * units_per_sec) / freq.QuadPart;
}

int64_t time_ms(void) { return perf_counter_scaled(1000); }

int64_t time_us(void) { return perf_counter_scaled(1000000); }

#else
#    include <time.h>

#    if defined(__linux__) && defined(CLOCK_BOOTTIME)
#        define TIME_UTILS_CLOCK CLOCK_BOOTTIME
#    else
#        define TIME_UTILS_CLOCK CLOCK_MONOTONIC
#    endif

int64_t time_ms(void) {
    struct timespec ts;

    clock_gettime(TIME_UTILS_CLOCK, &ts);

    return ((int64_t)ts.tv_sec * 1000) + ((int64_t)ts.tv_nsec / 1000000);
}

int64_t time_us(void) {
    struct timespec ts;

    clock_gettime(TIME_UTILS_CLOCK, &ts);

    return ((int64_t)ts.tv_sec * 1000000) + ((int64_t)ts.tv_nsec / 1000);
}

#endif
