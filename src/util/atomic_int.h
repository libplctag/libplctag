/***************************************************************************
 *   Copyright (C) 2021 by Kyle Hayes                                      *
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


#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <lib/libplctag.h>
#include <util/debug.h>
#include <util/platform.h>
// #include <util/lock.h>

#define ATOMIC_INT_STATIC_INIT(n) {n}
#define ATOMIC_BOOL_STATIC_INIT(b) {b}

#ifdef PLATFORM_IS_WINDOWS

typedef struct { long val; } atomic_int;
typedef struct { unsigned long val; } atomic_uint;
typedef struct { unsigned long val; } atomic_bool;

static inline int atomic_int_load(atomic_int *atomic_val)
{
    return (int)InterlockedOr((volatile long*)atomic_val, (long)0);
}

static inline int atomic_int_store(atomic_int *atomic_val, int new_val)
{
    long old_val = 0;
    long result_val = (long)new_val;

    /* lame.  We need to loop until CAS works. */
    do {
        old_val = (long)InterlockedCompareExchange((volatile long *)atomic_val, (long)new_val, (long)old_val);

        if (old_val != (long)new_val) {
            result_val = old_val;
        }
    } while (old_val != (long)new_val);

    return result_val;
}

static inline int atomic_int_add(atomic_int *atomic_val, int addend)
{
    return (int)InterlockedExchangeAdd((volatile long *)atomic_val, (long)addend);
}

static inline long atomic_int_sub(atomic_int* atomic_val, int subend)
{
    return (int)InterlockedExchangeAdd((volatile long*)atomic_val, (long)(-subend));
}



/* unsigned int */

static inline unsigned int atomic_uint_load(atomic_uint *atomic_val)
{
    return (unsigned int)InterlockedOr((volatile long *)atomic_val, (long)0);
}

static inline unsigned int atomic_uint_store(atomic_uint *atomic_val, unsigned int new_val)
{
    unsigned int old_val = 0;
    unsigned int result_val = new_val;

    /* lame.  We need to loop until CAS works. */
    do {
        old_val = (unsigned int)InterlockedCompareExchange((volatile long *)atomic_val, (long)new_val, (long)old_val);

        if (old_val != new_val) {
            result_val = old_val;
        }
    } while (old_val != new_val);

    return result_val;
}

static inline unsigned int atomic_uint_add(atomic_uint *atomic_val, unsigned int addend)
{
    return (unsigned int)InterlockedExchangeAdd((volatile long *)atomic_val, (long)addend);
}

static inline unsigned int atomic_uint_sub(atomic_uint *atomic_val, unsigned int subend)
{
    return (unsigned int)InterlockedExchangeAdd((volatile long *)atomic_val, (-(long)subend));
}



/* bool */

static inline bool atomic_bool_load(atomic_bool* atomic_val)
{
    unsigned int tmp = atomic_uint_load((atomic_uint *)atomic_val);
    return (tmp ? true : false);
}

static inline bool atomic_bool_store(atomic_bool* atomic_val, bool new_bool_val)
{
    return (atomic_uint_store((atomic_uint *)atomic_val, (new_bool_val ? 1 : 0)) ? true : false);
}

#else

/* Only GCC and Clang! */

/*
 * NOTE: People are still using the older versions of GCC for many
 * embedded platforms.  Keep using the old __sync_XXX() functions for now.
 */


typedef struct { int val; } atomic_int;
typedef struct { unsigned int val; } atomic_uint;
typedef struct { int8_t val; } atomic_bool;




/* int32 */

static inline int atomic_int_load(atomic_int *atomic_val)
{
    return  __sync_fetch_and_or((int *)atomic_val, 0);
}

static inline int atomic_int_store(atomic_int *atomic_val, int new_val)
{
    int old_val = 0;
    int result_val = new_val;

    /* lame.  We need to loop until CAS works. */
    do {
        old_val = __sync_val_compare_and_swap((int *)atomic_val, old_val, new_val);

        if(old_val != new_val) {
            result_val = old_val;
        }
    } while(old_val != new_val);

    return result_val;
}

static inline int atomic_int_add(atomic_int *atomic_val, int addend)
{
    return __sync_fetch_and_add((int *)atomic_val, addend);
}

static inline int atomic_int_sub(atomic_int *atomic_val, int subend)
{
    return __sync_fetch_and_sub((int *)atomic_val, subend);
}





/* uint32 */

static inline unsigned int atomic_uint_load(atomic_uint *atomic_val)
{
    return  __sync_fetch_and_or((unsigned int *)atomic_val, 0);
}

static inline unsigned int atomic_uint_store(atomic_uint *atomic_val, unsigned int new_val)
{
    unsigned int old_val = 0;
    unsigned int result_val = new_val;

    /* lame.  We need to loop until CAS works. */
    do {
        old_val = __sync_val_compare_and_swap((unsigned int *)atomic_val, old_val, new_val);

        if(old_val != new_val) {
            result_val = old_val;
        }
    } while(old_val != new_val);

    return result_val;
}

static inline unsigned int atomic_uint_add(atomic_uint *atomic_val, unsigned int addend)
{
    return __sync_fetch_and_add((unsigned int *)atomic_val, addend);
}

static inline unsigned int atomic_uint_sub(atomic_uint *atomic_val, unsigned int subend)
{
    return __sync_fetch_and_sub((unsigned int *)atomic_val, subend);
}



/* bool */

static inline bool atomic_bool_load(atomic_bool *atomic_val)
{
    uint8_t tmp = __sync_fetch_and_or((uint8_t *)atomic_val, 0);
    return (tmp == 0 ? false : true);
}

static inline bool atomic_bool_store(atomic_bool *atomic_val, bool new_bool_val)
{
    uint8_t old_val = 0;
    uint8_t new_val = (new_bool_val == true ? 1 : 0);
    uint8_t result_val = new_val;

    /* We need to loop until CAS works. */
    do {
        old_val = __sync_val_compare_and_swap((uint8_t*)atomic_val, old_val, new_val);

        if (old_val != new_val) {
            result_val = old_val;
        }
    } while (old_val != new_val);

    return (result_val ? true : false);
}


#endif

