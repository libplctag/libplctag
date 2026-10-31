#pragma once

/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *   Author Heath Raftery                                                  *
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

#include <stdint.h>
#include "compat.h"

/* Functions return PLCTAG_STATUS_OK or a PLCTAG_ERR_* code from libplctag.h. */
typedef struct mutex_t *mutex_p;
extern int32_t mutex_create(mutex_p *m);

extern int32_t mutex_lock_impl(const char *func, int32_t line_num, mutex_p m);
extern int32_t mutex_unlock_impl(const char *func, int32_t line_num, mutex_p m);

#if defined(IS_WINDOWS) && defined(IS_MSVC)
/* MinGW on Windows does not need this. */
#    define __func__ __FUNCTION__
#endif

#define mutex_lock(m) mutex_lock_impl(__func__, __LINE__, m)
#define mutex_unlock(m) mutex_unlock_impl(__func__, __LINE__, m)

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
 * Do not use return, goto or continue inside the block: they skip the unlock.
 * break leaves the block and unlocks, but does not leave any surrounding loop.
 * The lock result is compared to 0, which is PLCTAG_STATUS_OK.
 */
#define CRITICAL_CAT2(a, b) a##b
#define CRITICAL_CAT(a, b) CRITICAL_CAT2(a, b)
#define LINE_ID(base) CRITICAL_CAT(base, __LINE__)

#define critical_block(lock)                                                    \
    for(int32_t LINE_ID(__sync_flag_nargle_) = 1; LINE_ID(__sync_flag_nargle_); \
        LINE_ID(__sync_flag_nargle_) = 0, mutex_unlock(lock))                   \
        for(int32_t LINE_ID(__sync_rc_nargle_) = mutex_lock(lock);              \
            LINE_ID(__sync_rc_nargle_) == 0 && LINE_ID(__sync_flag_nargle_); LINE_ID(__sync_flag_nargle_) = 0)
