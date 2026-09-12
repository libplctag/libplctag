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
 * Clocks and sleeping.
 *
 * This replaces the time functions that used to live in the platform shims
 * (src/platform/posix/platform.[ch] and src/platform/windows/platform.[ch]),
 * plus the two private microsecond clocks that had been hand-rolled inside
 * utils/debug.c and protocols/mb/modbus.c.
 *
 * Both clocks are wall-clock time against the Unix epoch, not a monotonic
 * counter: they step when the system clock is set and they can go backwards.
 * That is deliberate -- utils/nap.h computes absolute deadlines against
 * time_ms() and hands them to a condition variable using the same realtime
 * base, so the two must agree.  Do not swap either for a monotonic source
 * without changing nap.c to match.
 *
 * Neither clock logs.  utils/debug.c calls time_us() while formatting a log
 * line, so a pdebug() in either one recurses.
 */

#include <stdint.h>



/* milliseconds since the Unix epoch. */
extern int64_t time_ms(void);

/* microseconds since the Unix epoch, for interval measurement. */
extern int64_t time_us(void);

/*
 * Sleeps for at least ms milliseconds, resuming if interrupted by a signal.
 * Returns PLCTAG_STATUS_OK, or PLCTAG_ERR_BAD_PARAM for a negative time.
 */
extern int32_t sleep_ms(int32_t ms);
