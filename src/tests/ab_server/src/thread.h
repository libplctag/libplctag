/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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

#pragma once

#include "compat.h"

#if IS_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>

    #include <processthreadsapi.h>
#endif

/* Derived from PLCTAG_STATUS_OK et al. */
typedef enum {
    THREAD_STATUS_OK            = 0,
    THREAD_ERR_NULL_PTR         = -25,
    THREAD_ERR_THREAD_CREATE    = -30,
    THREAD_ERR_THREAD_JOIN      = -31
} thread_err_t;

typedef struct thread_t *thread_p;
#if IS_WINDOWS
#define thread_func_t LPTHREAD_START_ROUTINE
#define NO_RETURN __declspec(noreturn)
#else
typedef void *(*thread_func_t)(void *arg);
#define NO_RETURN __attribute__((noreturn))
#endif
extern int thread_create(thread_p *t, thread_func_t func, int stacksize, void *arg);
NO_RETURN extern void thread_stop(void);
extern void thread_kill(thread_p t);
extern int thread_join(thread_p t);
extern int thread_detach(void);
extern int thread_destroy(thread_p *t);

#if IS_WINDOWS
#define THREAD_FUNC(func) DWORD __stdcall func(LPVOID arg)
#define THREAD_RETURN(val) return (DWORD)val;

#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_FUNC(func) void *func(void *arg)
#define THREAD_RETURN(val) return (void *)val;

#define THREAD_LOCAL __thread
#endif
