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
 * A spin lock, for critical sections short enough that sleeping on a mutex
 * would cost more than spinning through them.
 *
 * This used to live in the platform shims, once per platform.  It does not
 * need to: utils/atomic_utils.h already carries the compiler and platform
 * split for the test-and-set, and thread_yield() carries it for the yield,
 * so there is nothing platform-specific left here.
 *
 * lock_acquire() cannot fail -- it spins until it holds the lock -- so there
 * is no error to check and spin_block() has no failure path.
 */

#include <stdbool.h>
#include <stdint.h>

#include <utils/atomic_utils.h>


typedef atomic_bool lock_t;

#define LOCK_INIT ATOMIC_BOOL_STATIC_INIT

#define PLCTAG_SPIN_CAT2(a, b) a##b
#define PLCTAG_SPIN_CAT(a, b) PLCTAG_SPIN_CAT2(a, b)
#define SPIN_LINE_ID(base) PLCTAG_SPIN_CAT(base, __LINE__)

/*
 * Use this like:
 *
 *     spin_block(&my_lock) {
 *         locked_data++;
 *     }
 *
 * The macro acquires and releases for you.
 *
 * Do not use return or goto inside the block -- both leave without releasing
 * the lock.  break is safe: it drops out of the inner loop and the outer one
 * still releases.  It will NOT break out of any loop surrounding the block.
 */
#define spin_block(lock)                                              \
    for(int32_t SPIN_LINE_ID(spin_flag_) = 1; SPIN_LINE_ID(spin_flag_); \
        SPIN_LINE_ID(spin_flag_) = 0, lock_release(lock))             \
        for(lock_acquire(lock); SPIN_LINE_ID(spin_flag_); SPIN_LINE_ID(spin_flag_) = 0)


/* returns true when the lock was acquired, false when it was already held */
extern bool lock_acquire_try(lock_t *lock);
extern void lock_acquire(lock_t *lock);
extern void lock_release(lock_t *lock);

