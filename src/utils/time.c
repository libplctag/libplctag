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
 * Clocks and sleeping.  See utils/time.h.
 *
 * The argument checking and logging that both platforms share live in the
 * public functions; the platform primitives are the static wrappers at the
 * bottom of this file, under one #ifdef.
 *
 * time_ms() and time_us() must not log -- utils/debug.c calls time_us() while
 * building a log line.
 */

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#else
#    include <errno.h>
#    include <sys/time.h>
#    include <time.h>
#endif

#include <stdbool.h>
#include <stdint.h>

#include <libplctag/lib/libplctag.h>
#include <utils/debug.h>
#include <utils/time.h>


/* the platform seam, defined at the bottom of this file. */
static int64_t platform_time_us(void);
static int32_t platform_sleep_ms(int32_t ms);


/*
 * time_ms
 *
 * Milliseconds since the Unix epoch.  Derived from the microsecond clock so
 * that the two can never disagree about what time it is.
 */
extern int64_t time_ms(void) { return platform_time_us() / 1000; }


/*
 * time_us
 *
 * Microseconds since the Unix epoch.
 */
extern int64_t time_us(void) { return platform_time_us(); }


/*
 * sleep_ms
 *
 * Sleep the passed number of milliseconds.
 *
 * The negative check is here rather than in the platform wrapper so that both
 * platforms refuse identically.  Windows used to pass a negative straight into
 * Sleep((DWORD)ms), which sleeps for about 49 days.
 */
extern int32_t sleep_ms(int32_t ms) {
    if(ms < 0) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "called with negative time %d!", ms);
        return PLCTAG_ERR_BAD_PARAM;
    }

    return platform_sleep_ms(ms);
}


/***************************************************************************
 ************************** Platform Wrappers ******************************
 **************************************************************************/

#ifdef _WIN32

/*
 * GetSystemTimePreciseAsFileTime() is Windows 8 and later and is accurate to
 * well under a microsecond.  The older GetSystemTimeAsFileTime() ticks at the
 * scheduler interval, about 15.6ms by default, which is useless for the
 * interval measurement time_us() exists to do.
 */
static int64_t platform_time_us(void) {
    FILETIME ft;
    ULARGE_INTEGER uli;

    GetSystemTimePreciseAsFileTime(&ft);

    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    /*
     * FILETIME counts 100ns intervals from Jan 1, 1601.  Divide to microseconds
     * and subtract the 11644473600 seconds between that epoch and Jan 1, 1970.
     */
    return (int64_t)((uli.QuadPart / 10) - 11644473600000000LL);
}


/* Sleep() cannot fail and cannot be interrupted by a signal. */
static int32_t platform_sleep_ms(int32_t ms) {
    Sleep((DWORD)ms);

    return PLCTAG_STATUS_OK;
}

#else

static int64_t platform_time_us(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);

    return ((int64_t)tv.tv_sec * 1000000) + (int64_t)tv.tv_usec;
}


/*
 * nanosleep() returns early with EINTR when a signal arrives, leaving the time
 * still to run in the remainder.  Feed that back in so the caller gets the full
 * sleep it asked for.
 */
static int32_t platform_sleep_ms(int32_t ms) {
    struct timespec wait_time;
    struct timespec remainder;
    bool done = true;
    int32_t rc = PLCTAG_STATUS_OK;

    wait_time.tv_sec = ms / 1000;
    wait_time.tv_nsec = ((long)ms % 1000) * 1000000; /* convert to nanoseconds */

    do {
        rc = nanosleep(&wait_time, &remainder);

        if(rc < 0 && errno == EINTR) {
            /* we were interrupted, keep going. */
            wait_time = remainder;
            done = false;
        } else {
            done = true;

            if(rc < 0) {
                /* error condition. */
                rc = PLCTAG_ERR_BAD_REPLY;
            }
        }
    } while(!done);

    return rc;
}

#endif
