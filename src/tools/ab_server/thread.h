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

#if IS_WINDOWS
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>

#    include <processthreadsapi.h>
#endif

/* Functions return PLCTAG_STATUS_OK or a PLCTAG_ERR_* code from libplctag.h. */
typedef struct thread_t *thread_p;
#if IS_WINDOWS
#    define thread_func_t LPTHREAD_START_ROUTINE
#else
typedef void *(*thread_func_t)(void *arg);
#endif
extern int32_t thread_create(thread_p *t, thread_func_t func, void *arg);
extern int32_t thread_join(thread_p t);
extern int32_t thread_detach(void);
extern int32_t thread_destroy(thread_p *t);

#if IS_WINDOWS
#    define THREAD_FUNC(func) DWORD __stdcall func(LPVOID arg)
#    define THREAD_RETURN(val) return (DWORD)val;
#else
#    define THREAD_FUNC(func) void *func(void *arg)
#    define THREAD_RETURN(val) return (void *)val;
#endif
