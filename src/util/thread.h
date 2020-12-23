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

#include <util/platform.h>

typedef struct thread_t *thread_p;
typedef void *(*thread_func_t)(void *arg);
extern int thread_create(thread_p *t, thread_func_t func, int stacksize, void *arg);

#ifdef PLATFORM_IS_POSIX
    extern void thread_stop(void) __attribute__((noreturn));
#else
    extern void thread_stop(void)
#endif

extern void thread_kill(thread_p t);
extern int thread_join(thread_p t);
extern int thread_detach();
extern int thread_destroy(thread_p *t);

#ifdef PLATFORM_IS_WINDOWS
    #define THREAD_FUNC(func) DWORD __stdcall func(LPVOID arg)
    #define THREAD_RETURN(val) return (DWORD)val;
    #define THREAD_LOCAL __declspec(thread)
#else
    #define THREAD_FUNC(func) void *func(void *arg)
    #define THREAD_RETURN(val) return (void *)val;
    #define THREAD_LOCAL __thread
#endif

