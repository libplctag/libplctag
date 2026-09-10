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
 * Portable condition variables.
 *
 * This replaces the condition variable section that used to live in the
 * platform shims (src/platform/posix/platform.h and
 * src/platform/windows/platform.h).  The API is unchanged; only the home of
 * the declarations moved.
 *
 * NOTE: this is not a POSIX condition variable.  It is an auto-reset event
 * that owns its own mutex:
 *
 *   - cond_signal() raises the flag and wakes one waiter.  The flag stays
 *     raised until a waiter consumes it, so a signal sent before anyone waits
 *     is not lost.
 *   - cond_wait() returns as soon as the flag is up and clears it on the way
 *     out.  Spurious wake ups are handled internally.
 *   - cond_clear() drops the flag without waiting.
 *
 * There is no broadcast and no caller-supplied mutex.  Code that needs a real
 * condition variable paired with a caller-held mutex uses compat_cond_* in the
 * test utilities.
 *
 * cond_wait() requires a positive timeout; zero or negative returns
 * PLCTAG_ERR_TIMEOUT without waiting.
 *
 * The wait, signal and clear entry points are macros so that the calling
 * function and line number reach the log without every caller passing them.
 */

#include <stdint.h>

#include <libplctag/lib/libplctag.h>


typedef struct cond_t *cond_p;

extern int32_t cond_create(cond_p *c);
extern int32_t cond_destroy(cond_p *c);

extern int32_t cond_wait_impl(const char *func, int32_t line_num, cond_p c, int32_t timeout_ms);
extern int32_t cond_signal_impl(const char *func, int32_t line_num, cond_p c);
extern int32_t cond_clear_impl(const char *func, int32_t line_num, cond_p c);

/*
 * NOTE: utils/mutex.h supplies the __func__ shim needed by MSVC.  Both headers
 * arrive together through platform.h.
 */
#include <utils/mutex.h>

#define cond_wait(c, t) cond_wait_impl(__func__, __LINE__, c, t)
#define cond_signal(c) cond_signal_impl(__func__, __LINE__, c)
#define cond_clear(c) cond_clear_impl(__func__, __LINE__, c)
