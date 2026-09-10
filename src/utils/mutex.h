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
 * Portable recursive mutexes.
 *
 * This replaces the mutex section that used to live in the platform shims
 * (src/platform/posix/platform.h and src/platform/windows/platform.h).  The
 * API is unchanged; only the home of the declarations moved.
 *
 * Mutexes are recursive on both platforms: the owning thread may lock the
 * same mutex more than once as long as it unlocks it the same number of
 * times.
 *
 * The lock, try-lock and unlock entry points are macros so that the calling
 * function and line number reach the log without every caller passing them.
 */

#include <stdint.h>

#include <libplctag/lib/libplctag.h>


typedef struct mutex_t *mutex_p;

extern int32_t mutex_create(mutex_p *m);
extern int32_t mutex_destroy(mutex_p *m);

extern int32_t mutex_lock_impl(const char *func, int32_t line_num, mutex_p m);
extern int32_t mutex_try_lock_impl(const char *func, int32_t line_num, mutex_p m);
extern int32_t mutex_unlock_impl(const char *func, int32_t line_num, mutex_p m);

#if defined(_WIN32) && defined(_MSC_VER)
/* MinGW on Windows does not need this. */
#    define __func__ __FUNCTION__
#endif

#define mutex_lock(m) mutex_lock_impl(__func__, __LINE__, m)
#define mutex_try_lock(m) mutex_try_lock_impl(__func__, __LINE__, m)
#define mutex_unlock(m) mutex_unlock_impl(__func__, __LINE__, m)


/* macros are evil */

/*
 * Use this one like this:
 *
 *     critical_block(my_mutex) {
 *         locked_data++;
 *         foo(locked_data);
 *     }
 *
 * The macro locks and unlocks for you.  Derived from ideas/code on StackOverflow.com.
 *
 * Do not use break, return, goto or continue inside the synchronized block if
 * you intend to have them apply to a loop outside the synchronized block.
 *
 * You can use break, but it will drop out of the inner for loop and correctly
 * unlock the mutex.  It will NOT break out of any surrounding loop outside the
 * synchronized block.
 *
 * If the lock cannot be taken the body is skipped and the mutex is NOT
 * unlocked.  The lock is taken in the outer loop's initializer so that the
 * outer condition can gate on the result; a for loop only runs its increment
 * after a completed iteration, so a failed lock never reaches the unlock.
 *
 * The two nested for loops exist so that break unlocks: the outer loop owns
 * the lock and unlock, the inner loop owns the body.  A break leaves only the
 * inner loop, so the outer increment still runs.
 *
 * return and goto still escape both loops and leak the lock.  Neither is used
 * inside a critical_block anywhere in the tree; keep it that way.
 *
 * Note that a token paste of ##__LINE__ does not expand __LINE__.  The two
 * levels of indirection through PLCTAG_CAT/PLCTAG_CAT2 are what make the
 * generated names actually carry the line number, which is what keeps nested
 * critical blocks from shadowing each other.
 */

#define PLCTAG_CAT2(a, b) a##b
#define PLCTAG_CAT(a, b) PLCTAG_CAT2(a, b)
#define LINE_ID(base) PLCTAG_CAT(base, __LINE__)

#define critical_block(lock)                                                                 \
    for(int32_t LINE_ID(__sync_rc_nargle_) = mutex_lock(lock), LINE_ID(__sync_flag_nargle_) = 1; \
        LINE_ID(__sync_rc_nargle_) == PLCTAG_STATUS_OK && LINE_ID(__sync_flag_nargle_);          \
        LINE_ID(__sync_flag_nargle_) = 0, mutex_unlock(lock))                                    \
        for(; LINE_ID(__sync_flag_nargle_); LINE_ID(__sync_flag_nargle_) = 0)
