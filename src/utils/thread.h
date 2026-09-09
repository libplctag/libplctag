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
 * Portable thread creation and joining.
 *
 * This replaces the thread section that used to live in the platform shims
 * (src/platform/posix/platform.h and src/platform/windows/platform.h).  The
 * two shims had drifted apart: thread_create() named a different callback
 * type on each platform and thread_detach() was declared without a prototype
 * on Windows.  Both are spelled the same way here.
 *
 * Thread bodies must be declared with THREAD_FUNC() and must return with
 * THREAD_RETURN().  The underlying signature differs between Win32 and
 * pthreads and callers must never name it directly.
 *
 * Lifetime is create/join.  thread_join() waits for the thread, releases the
 * underlying OS resources and frees the handle, so there is no separate
 * destroy step.  A thread that is created must be joined.
 */

#include <stdint.h>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#    include <processthreadsapi.h>
#endif

#include <libplctag/lib/libplctag.h>


#ifdef _WIN32

#    define THREAD_FUNC(func) DWORD __stdcall func(LPVOID arg)
#    define THREAD_RETURN(val) return (DWORD)(val);

#    if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && (defined(__MINGW32__) || defined(__MINGW64__))
#        define THREAD_LOCAL _Thread_local
#    else
#        define THREAD_LOCAL __declspec(thread)
#    endif

typedef DWORD(__stdcall *thread_func_t)(LPVOID arg);

#else

#    define THREAD_FUNC(func) void *func(void *arg)
#    define THREAD_RETURN(val) return (void *)(val);

#    define THREAD_LOCAL __thread

typedef void *(*thread_func_t)(void *arg);

#endif


typedef struct thread_t *thread_p;


extern int32_t thread_create(thread_p *t, thread_func_t func, int32_t stacksize, void *arg);
extern int32_t thread_join(thread_p *t);
extern void thread_stop(void);
extern void thread_yield(void);

