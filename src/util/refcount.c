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


#include <lib/libplctag.h>
#include <platform.h>
#include <util/refcount.h>
#include <util/debug.h>



refcount refcount_init(int count, void *data, void (*delete_func)(void *data))
{
    refcount rc;

	pdebug(DEBUG_INFO, "Initializing refcount struct with count=%d", count);

    rc.count = count;
    rc.lock = LOCK_INIT;
    rc.data = data;
    rc.delete_func = delete_func;

    return rc;
}

/* must be called with a mutex held! */
int refcount_acquire(refcount *rc)
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

    pdebug(DEBUG_SPEW,"Ref count is now %d",count);

    return count;
}


int refcount_release(refcount *rc)
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
    
    if(rc->count < 0) {
		rc->count = 0;
	}
	
    count = rc->count;

    /* release the lock so that other things can get to it. */
    lock_release(&rc->lock);

    pdebug(DEBUG_SPEW,"Refcount is now %d", count);

    if(count <= 0) {
        pdebug(DEBUG_INFO,"Calling destructor function.");

        rc->delete_func(rc->data);
    }

    return count;
}

int refcount_get_count(refcount *rc)
{
    int count;

    if(!rc) {
        return PLCTAG_ERR_NULL_PTR;
    }

    /* loop until we get the lock */
    while (!lock_acquire(&rc->lock)) {
        ; /* do nothing, just spin */
    }

    count = rc->count;

    /* release the lock so that other things can get to it. */
    lock_release(&rc->lock);

	return count;
}

