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


#include <limits.h>
#include <util/debug.h>
#include <util/lock.h>

typedef struct { lock_t lock; volatile int val; } atomic_int;

#define ATOMIC_INT_STATIC_INIT(n) {0, n}

static inline int atomic_int_get(atomic_int *a)
{
    int val = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    spin_block(&a->lock) {
        val = a->val;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return val;
}



static inline int atomic_int_set(atomic_int *a, int new_val)
{
    int old_val = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    spin_block(&a->lock) {
        old_val = a->val;
        a->val = new_val;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return old_val;
}



static inline int atomic_int_add(atomic_int *a, int other)
{
    int old_val = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    spin_block(&a->lock) {
        old_val = a->val;
        a->val += other;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return old_val;
}


static inline int atomic_int_and(atomic_int *a, int other)
{
    int old_val = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    spin_block(&a->lock) {
        old_val = a->val;
        a->val &= other;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return old_val;
}



static inline int atomic_int_or(atomic_int *a, int other)
{
    int old_val = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    spin_block(&a->lock) {
        old_val = a->val;
        a->val |= other;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return old_val;
}



static inline int atomic_int_xor(atomic_int *a, int other)
{
    int old_val = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    spin_block(&a->lock) {
        old_val = a->val;
        a->val ^= other;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return old_val;
}


/* atomic bit manipulation */

static inline int atomic_int_get_bit(atomic_int *a, int bit)
{
    int val = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    spin_block(&a->lock) {
        if(bit >= 0 && bit < (((int)(unsigned int)sizeof(int)*CHAR_BIT))) {
            val = (a->val & (1 << bit)) ? 1 : 0;
        } else {
            val = 0;
        }
    }

    pdebug(DEBUG_SPEW, "Done.");

    return val;
}

static inline int atomic_int_set_bit(atomic_int *a, int bit, int new_val)
{
    int old_val = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    spin_block(&a->lock) {
        if(bit >= 0 && bit < (((int)(unsigned int)sizeof(int)*CHAR_BIT))) {
            /* save old value */
            old_val = (a->val & (1 << bit)) ? 1 : 0;

            if(new_val) {
                a->val |= (1 << bit);
            } else {
                a->val &= ~(1 << bit);
            }
        } else {
            old_val = 0;
        }
    }

    pdebug(DEBUG_SPEW, "Done.");

    return old_val;
}



/* convenience for single bools */

typedef atomic_int atomic_bool;

#ifndef bool
#define bool int
#endif

#define ATOMIC_BOOL_STATIC_INIT(n) {0, n}


static inline int atomic_bool_get(atomic_bool *ab)
{
    int val = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    spin_block(&ab->lock) {
        val = (ab->val ? 1 : 0);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return val;
}


static inline int atomic_bool_set(atomic_bool *ab, bool new_val)
{
    int old_val = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    spin_block(&ab->lock) {
        old_val = (ab->val ? 1 : 0);

        ab->val = (new_val ? 1 : 0);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return old_val;
}


