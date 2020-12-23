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

typedef struct mutex_t *mutex_p;
extern int mutex_create(mutex_p *m);
extern int mutex_destroy(mutex_p *m);

#define mutex_lock(m) mutex_lock_impl(__func__, __LINE__, m)
extern int mutex_lock_impl(const char *func, int line_num, mutex_p m);

#define mutex_try_lock(m) mutex_lock_impl(__func__, __LINE__, m)
extern int mutex_try_lock_impl(const char *func, int line_num, mutex_p m);

#define mutex_unlock(m) mutex_unlock_impl(__func__, __LINE__, m)
extern int mutex_unlock_impl(const char *func, int line_num, mutex_p m);


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
 */
#define critical_block(lock) \
for(int __sync_flag_nargle_##__LINE__ = 1; __sync_flag_nargle_##__LINE__ ; __sync_flag_nargle_##__LINE__ = 0, mutex_unlock(lock))  for(int __sync_rc_nargle_##__LINE__ = mutex_lock(lock); __sync_rc_nargle_##__LINE__ == PLCTAG_STATUS_OK && __sync_flag_nargle_##__LINE__ ; __sync_flag_nargle_##__LINE__ = 0)

