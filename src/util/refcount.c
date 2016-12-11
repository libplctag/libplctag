/***************************************************************************
 *   Copyright (C) 2016 by Kyle Hayes                                      *
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


#include <platform.h>
$include <util/refcount.h>


struct refcount {
    int count;
    lock_t lock;
    void *data;
    void (*delete_func)(void *data);
};


extern refcount refcount_init(int count, void *data, void (*delete_func)(void *data))
{
    refcount rc;

    rc.count = count;
    rc.lock = INIT_LOCK;
    rc.data = data;
    rc.delete_func = delete_func;

    return rc;
}

/* must be called with a mutex held! */
extern int refcount_acquire(refcount *rc)
{
    int count;

    if(!rc) {
        return PLCTAG_ERR_NULL_PTR;
    }

    /* loop until we get the lock */
    while (!lock_acquire(&rc->lock)) {
        ; /* do nothing, just spin */
    }

    rc->count++;
    count = rc->count;

    /* release the lock so that other things can get to it. */
    lock_release(&rc->lock);

    pdebug(DEBUG_INFO,"Ref count is now %d",count);

    return count;
}


extern int refcount_release(refcount *rc)
{
    int count;

    if(!rc) {
        return PLCTAG_ERR_NULL_PTR;
    }

    /* loop until we get the lock */
    while (!lock_acquire(&rc->lock)) {
        ; /* do nothing, just spin */
    }

    rc->count--;
    count = rc->count;

    /* release the lock so that other things can get to it. */
    lock_release(&rc->lock);

    pdebug(DEBUG_INFO,"Refcount is now %d", count);

    if(count <= 0) {
        pdebug(DEBUG_INFO,"Calling destructor function.");

        rc->delete_func(rc->data);

        /* do not return negatives, those are errors. */
        count = 0;
    }

    return count;
}

