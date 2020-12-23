/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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


#include <util/debug.h>
#include <util/lock.h>
#include <util/platform.h>

#ifdef PLATFORM_IS_WINDOWS
    #include <winnt.h>

    #define ATOMIC_UNLOCK_VAL ((LONG)(0))
    #define ATOMIC_LOCK_VAL ((LONG)(1))
#else
    #define ATOMIC_UNLOCK_VAL ((unsigned int)0)
    #define ATOMIC_LOCK_VAL ((unsigned int)1)
#endif

/*
 * lock_acquire
 *
 * Tries to write a non-zero value into the lock atomically.
 *
 * Returns non-zero on success.
 *
 * Warning: do not pass null pointers!
 */

int lock_acquire_try(lock_t *lock)
{
#ifdef PLATFORM_IS_WINDOWS
    LONG rc = InterlockedExchange(lock, ATOMIC_LOCK_VAL);
#else
    int rc = __sync_lock_test_and_set((int*)lock, ATOMIC_LOCK_VAL);
#endif

    if(rc != ATOMIC_LOCK_VAL) {
        return 1;
    } else {
        return 0;
    }
}


int lock_acquire(lock_t *lock)
{
    while(!lock_acquire_try(lock)) ;

    return 1;
}


void lock_release(lock_t *lock)
{
#ifdef PLATFORM_IS_WINDOWS
    InterlockedExchange(lock, ATOMIC_UNLOCK_VAL);
#else
    __sync_lock_release((int*)lock);
#endif
}


