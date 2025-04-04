/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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

#include <platform.h>
#include <stdbool.h>
#include <stdint.h>



#if defined(__STDC_NO_ATOMICS__) || !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 11)

#define ATOMIC_INT_STATIC_INIT {0}

#ifdef WIN32
typedef volatile uint16_t atomic_bool;
#else
typedef volatile bool atomic_bool;
#endif
typedef volatile int32_t atomic_int32_t;
typedef volatile int64_t atomic_int64_t;


#else /* C11 atomics are supported. */

#include <stdatomic.h>

typedef _Atomic(bool) atomic_bool;
typedef _Atomic(int32_t) atomic_int32_t;
typedef _Atomic(int64_t) atomic_int64_t;

#endif

extern void atomic_init_bool(atomic_bool *a, bool new_val);
extern bool atomic_get_bool(atomic_bool *a);
extern bool atomic_set_bool(atomic_bool *a, bool new_val);
extern bool atomic_compare_and_set_bool(atomic_bool *a, bool old_val, bool new_val);

extern void atomic_init_int32(atomic_int32_t *a, int32_t new_val);
extern int32_t atomic_get_int32(atomic_int32_t *a);
extern int32_t atomic_set_int32(atomic_int32_t *a, int32_t new_val);
extern int32_t atomic_add_int32(atomic_int32_t *a, int32_t other);
extern int32_t atomic_compare_and_set_int32(atomic_int32_t *a, int32_t old_val, int32_t new_val);

extern void atomic_init_int64(atomic_int64_t *a, int64_t new_val);
extern int64_t atomic_get_int64(atomic_int64_t *a);
extern int64_t atomic_set_int64(atomic_int64_t *a, int64_t new_val);
extern int64_t atomic_add_int64(atomic_int64_t *a, int64_t other);
extern int64_t atomic_compare_and_set_int64(atomic_int64_t *a, int64_t old_val, int64_t new_val);
