
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
 * Spin lock implementation.
 *
 * There is deliberately no #ifdef in this file.  The test-and-set comes from
 * utils/atomic_utils.h and the yield from utils/thread.h, and both of those
 * already handle the platform differences.
 */

#include <stdbool.h>
#include <stdint.h>

#include <utils/atomic_utils.h>
#include <utils/spinlock.h>
#include <utils/thread.h>


/*
 * How many times to spin before yielding.
 *
 * A bare spin only pays off when the holder is running on another core and
 * will release within a few cycles.  If it is descheduled, or blocked in a
 * syscall, or the process is being serialized onto one core -- an
 * oversubscribed machine, or running under Valgrind, which lets exactly one
 * thread run at a time -- then spinning burns the waiter's whole timeslice
 * and actively delays the holder it is waiting on.  Yielding hands the CPU to
 * the holder instead, so the worst case degrades to "slow" rather than to a
 * livelock that scales with the thread count.
 */
#define SPIN_COUNT_BEFORE_YIELD (100)


/*
 * atomic_set_bool() returns the value that was there before, so the lock was
 * ours only if we found it clear.
 *
 * IMPORTANT: none of the functions in this file may call pdebug().  debug.c
 * guards its logger callback with a spin lock precisely because these are
 * logging-free; a pdebug() here re-enters pdebug_impl() from inside the lock
 * it is already trying to take and never completes.  See the comment on
 * logger_callback_lock in debug.c.
 */
extern bool lock_acquire_try(lock_t *lock) { return !atomic_set_bool(lock, true); }


extern void lock_acquire(lock_t *lock) {
    int32_t spins = 0;

    while(!lock_acquire_try(lock)) {
        if(++spins >= SPIN_COUNT_BEFORE_YIELD) {
            thread_yield();
            spins = 0;
        }
    }
}


extern void lock_release(lock_t *lock) { atomic_set_bool(lock, false); }

