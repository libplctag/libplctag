/***************************************************************************
 *   Copyright (C) 2018 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/


#include <util/atomic_int.h>
#include <util/debug.h>


void atomic_init(atomic_int *a, int new_val)
{
    a->lock = LOCK_INIT;
    a->val = new_val;
}



int atomic_get(atomic_int *a)
{
    int val = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    spin_block(&a->lock) {
        val = a->val;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return val;
}



int atomic_set(atomic_int *a, int new_val)
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



int atomic_add(atomic_int *a, int other)
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
