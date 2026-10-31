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

#include "utils.h"
#include <utils/byte_slice.h>
#include "compat.h"
#include "err.h"
#include "plc.h"
#include "log.h"
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#if IS_WINDOWS
#    include <windows.h>
#else
#    include <sys/time.h>
#    include <unistd.h>
#endif


/*
 * This contains the utilities used by the test harness.
 */


#if IS_WINDOWS

int32_t util_sleep_ms(int32_t ms) {
    if(ms <= 0) { return 1; }

    Sleep((DWORD)ms);
    return 1;
}

#else

int32_t util_sleep_ms(int32_t ms) {
    struct timespec req;
    struct timespec rem = {0};

    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000;

    return nanosleep(&req, &rem);
}

#endif


/*
 * time_ms
 *
 * Return the current epoch time in milliseconds.
 */

#if IS_WINDOWS
int64_t util_time_us(void) {
    FILETIME ft;
    int64_t res;

    GetSystemTimeAsFileTime(&ft);

    /* calculate time as 100ns increments since Jan 1, 1601. */
    res = (int64_t)(ft.dwLowDateTime) + ((int64_t)(ft.dwHighDateTime) << 32);

    /* get time in microseconds */
    res = res / 10;

    return res;
}

#else


int64_t util_time_us(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);

    return ((int64_t)tv.tv_sec * 1000000) + (int64_t)tv.tv_usec;
}

#endif

#if IS_WINDOWS
void system_yield(void) { SwitchToThread(); }
#else
#    include <sched.h>
void system_yield(void) { sched_yield(); }
#endif

#define COLUMNS (size_t)(10)

/* hex rows; each row starts with its offset in the parent buffer */
void log_byte_slice_impl_func(const char *func, int32_t line, log_level_t lvl, byte_slice_t s) {
    const uint8_t *data = byte_slice_get_data(s);
    int32_t len = byte_slice_len(s);
    char row_buf[300]; /* MAGIC */

    if(byte_slice_has_error(s)) {
        log_impl(func, line, lvl, "Slice has error %d at offset %d", byte_slice_get_error(s), s.start);
        return;
    }

    for(int32_t row = 0; row < len; row += (int32_t)COLUMNS) {
        size_t used = (size_t)snprintf(row_buf, sizeof(row_buf), "%05d", s.start + row);

        for(int32_t column = row; column < len && column < row + (int32_t)COLUMNS; column++) {
            used += (size_t)snprintf(&row_buf[used], sizeof(row_buf) - used, " %02x", data[column]);
        }

        /* use the caller's function and line number */
        log_impl(func, line, lvl, "%s", row_buf);
    }
}

