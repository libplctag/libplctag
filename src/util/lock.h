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

#pragma once

typedef int lock_t;

#define LOCK_INIT (0)

/* returns non-zero when lock acquired, zero when lock operation failed */
extern int lock_acquire_try(lock_t *lock);
extern int lock_acquire(lock_t *lock);
extern void lock_release(lock_t *lock);

#define spin_block(lock) \
for(int __sync_flag_nargle_lock_##__LINE__ = 1; __sync_flag_nargle_lock_##__LINE__ ; __sync_flag_nargle_lock_##__LINE__ = 0, lock_release(lock))  for(int __sync_rc_nargle_lock_##__LINE__ = lock_acquire(lock); __sync_rc_nargle_lock_##__LINE__ && __sync_flag_nargle_lock_##__LINE__ ; __sync_flag_nargle_lock_##__LINE__ = 0)

