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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Helpers the test programs need that the library does not provide.
 *
 * Everything the library already has -- timing, sleeping, atomics, mutexes,
 * string handling -- is used directly from src/utils.  This header pulls in the
 * ones the tests reach for most often so that a test needs one include, not five.
 */

#include <stdbool.h>
#include <stdint.h>

#include <utils/atomic_utils.h>
#include <utils/mutex.h>
#include <utils/str.h>
#include <utils/time.h>


/* catch terminate/interrupt signals/events. */
extern int test_set_interrupt_handler(void (*handler)(void));


/*
 * Poll a TCP port on host until something accepts a connection or timeout_ms
 * elapses.  Used instead of a fixed sleep after spawning a simulator process:
 * a fixed sleep either wastes time on a fast/idle runner or comes up short on
 * a slow/contended one, while this returns as soon as the server is actually
 * ready and only waits as long as it takes on a slow runner.
 */
extern bool test_wait_for_listener(const char *host, uint16_t port, uint32_t timeout_ms);


/* Number of logical CPUs available to this process, or 1 if it cannot be determined. */
extern int test_cpu_count(void);

#ifdef __cplusplus
}
#endif
