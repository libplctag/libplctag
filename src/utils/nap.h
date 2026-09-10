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
 * Interruptible sleep.
 *
 * A nap is a thread's own sleep timer that somebody else can cut short.  The
 * waiter sleeps until its deadline; anyone who gives it work interrupts the
 * nap so it wakes now instead.
 *
 * This replaces the "condition variable" section that used to live in the
 * platform shims (src/platform/posix/platform.h and
 * src/platform/windows/platform.h).  Only the names and the home of the
 * declarations changed; the primitive behaves exactly as it always did.
 *
 * NOTE: this is not a condition variable, despite what it used to be called.
 * It carries no data and takes no caller-supplied mutex.  It is an auto-reset
 * event owning its own mutex:
 *
 *   - nap_interrupt() posts a pending interrupt and wakes one waiter.  The
 *     interrupt stays pending until a waiter takes it, so work handed over
 *     just before the waiter goes to sleep is not lost.
 *   - nap_wait() returns as soon as an interrupt is pending and takes it on
 *     the way out.  Spurious wake ups are handled internally.  Timing out is
 *     the normal path, not an error: callers sleep to a deadline and treat
 *     PLCTAG_ERR_TIMEOUT as "nothing happened, go around again".
 *   - nap_clear() discards a pending interrupt without waiting.  Use it
 *     before a fresh wait so a stale interrupt does not end it immediately.
 *
 * One interrupt at a time and one waiter released per interrupt.  Many
 * interrupts arriving during a single nap collapse into one wake up, which
 * suits the state machines that use this: they re-read all of their state
 * when they wake regardless of how many things changed.
 *
 * There is no broadcast.  Code that needs a real condition variable paired
 * with a caller-held mutex uses compat_cond_* in the test utilities.
 *
 * nap_wait() requires a positive timeout; zero or negative returns
 * PLCTAG_ERR_TIMEOUT without sleeping.
 *
 * The wait, interrupt and clear entry points are macros so that the calling
 * function and line number reach the log without every caller passing them.
 */

#include <stdint.h>

#include <libplctag/lib/libplctag.h>


typedef struct nap_t *nap_p;

extern int32_t nap_create(nap_p *n);
extern int32_t nap_destroy(nap_p *n);

extern int32_t nap_wait_impl(const char *func, int32_t line_num, nap_p n, int32_t timeout_ms);
extern int32_t nap_interrupt_impl(const char *func, int32_t line_num, nap_p n);
extern int32_t nap_clear_impl(const char *func, int32_t line_num, nap_p n);

/*
 * NOTE: utils/mutex.h supplies the __func__ shim needed by MSVC.  Both headers
 * arrive together through platform.h.
 */
#include <utils/mutex.h>

#define nap_wait(n, t) nap_wait_impl(__func__, __LINE__, n, t)
#define nap_interrupt(n) nap_interrupt_impl(__func__, __LINE__, n)
#define nap_clear(n) nap_clear_impl(__func__, __LINE__, n)
