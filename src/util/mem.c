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


#include <stdlib.h>
#include <string.h>
#include <lib/libplctag.h>
#include <util/debug.h>
#include <util/lock.h>
#include <util/mem.h>
#include <util/platform.h>



/*
 * mem_alloc_impl
 *
 * This is a wrapper around the platform's memory allocation routine.
 * It will zero out memory before returning it.
 *
 * It will return NULL on failure.
 */
void *mem_alloc_impl(const char *func, int line_num, int size)
{
    pdebug(DEBUG_DETAIL, "Starting, called from %s:%d.", func, line_num);

    if(size <= 0) {
        pdebug(DEBUG_WARN, "Allocation size must be greater than zero bytes!");
        return NULL;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return calloc(INT_TO_SIZE_T(size), 1);
}



/*
 * mem_realloc_impl
 *
 * This is a wrapper around the platform's memory re-allocation routine.
 *
 * It will return NULL on failure.
 */
void *mem_realloc_impl(const char *func, int line_num, void *orig, int size)
{
    pdebug(DEBUG_DETAIL, "Starting, called from %s:%d.", func, line_num);

    if(size <= 0) {
        pdebug(DEBUG_WARN, "New allocation size must be greater than zero bytes!");
        return NULL;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return realloc(orig, INT_TO_SIZE_T(size));
}



/*
 * mem_free_impl
 *
 * Free the allocated memory passed in.  If the passed pointer is
 * null, do nothing.
 */
void mem_free_impl(const char *func, int line_num, const void *mem)
{
    pdebug(DEBUG_DETAIL, "Starting, called from %s:%d.", func, line_num);

    if(mem) {
        /* casting is required because free() does not take a const pointer. */
        free((void *)mem);

        pdebug(DEBUG_DETAIL, "Done.");
    } else {
        pdebug(DEBUG_INFO, "Null pointer passed at %s:%d.", func, line_num);
    }
}




/*
 * mem_set
 *
 * set memory to the passed argument.
 */
void mem_set(void *dest, int c, int size)
{
    if(!dest) {
        pdebug(DEBUG_WARN, "Destination pointer is NULL!");
        return;
    }

    if(size <= 0) {
        pdebug(DEBUG_WARN, "Size to set must be a positive number!");
        return;
    }

    memset(dest, c, INT_TO_SIZE_T(size));
}





/*
 * mem_copy
 *
 * copy memory from one pointer to another for the passed number of bytes.
 *
 * Memory cannot overlap!
 */
void mem_copy(void *dest, void *src, int size)
{
    if(!dest) {
        pdebug(DEBUG_WARN, "Destination pointer is NULL!");
        return;
    }

    if(!src) {
        pdebug(DEBUG_WARN, "Source pointer is NULL!");
        return;
    }

    if(size < 0) {
        pdebug(DEBUG_WARN, "Size to copy must be a positive number!");
        return;
    }

    if(size == 0) {
        /* nothing to do. */
        return;
    }

    memcpy(dest, src, INT_TO_SIZE_T(size));
}



/*
 * mem_move
 *
 * move memory from one pointer to another for the passed number of bytes.
 *
 * Memory can overlap.
 */
void mem_move(void *dest, void *src, int size)
{
    if(!dest) {
        pdebug(DEBUG_WARN, "Destination pointer is NULL!");
        return;
    }

    if(!src) {
        pdebug(DEBUG_WARN, "Source pointer is NULL!");
        return;
    }

    if(size < 0) {
        pdebug(DEBUG_WARN, "Size to move must be a positive number!");
        return;
    }

    if(size == 0) {
        /* nothing to do. */
        pdebug(DEBUG_INFO, "Number of bytes to move is zero.");
        return;
    }

    memmove(dest, src, INT_TO_SIZE_T(size));
}



int mem_cmp(void *src1, int src1_size, void *src2, int src2_size)
{
    if(!src1 || src1_size <= 0) {
        if(!src2 || src2_size <= 0) {
            /* both are NULL or zero length, but that is "equal" for our purposes. */
            return 0;
        } else {
            /* first one is "less" than second. */
            return -1;
        }
    } else {
        if(!src2 || src2_size <= 0) {
            /* first is "greater" than second */
            return 1;
        } else {
            /* both pointers are non-NULL and the lengths are positive. */

            /* short circuit the comparison if the blocks are different lengths */
            if(src1_size != src2_size) {
                return (src1_size - src2_size);
            }

            return memcmp(src1, src2, INT_TO_SIZE_T(src1_size));
        }
    }
}







/*
 * This is a rc struct that we use to make sure that we are able to align
 * the remaining part of the allocated block.
 */

struct refcount_t {
    lock_t lock;
    int count;
    const char *function_name;
    int line_num;
    //cleanup_p cleaners;
    rc_cleanup_func cleanup_func;

    /* FIXME - needed for alignment, this is a hack! */
    union {
        uint8_t dummy_u8;
        uint16_t dummy_u16;
        uint32_t dummy_u32;
        uint64_t dummy_u64;
        double dummy_double;
        void *dummy_ptr;
        void (*dummy_func)(void);
    } dummy_align[];
};


typedef struct refcount_t *refcount_p;

static void refcount_cleanup(refcount_p rc);
//static cleanup_p cleanup_entry_create(const char *func, int line_num, rc_cleanup_func cleaner, int extra_arg_count, va_list extra_args);
//static void cleanup_entry_destroy(cleanup_p entry);



/*
 * rc_alloc
 *
 * Create a reference counted control for the requested data size.  Return a strong
 * reference to the data.
 */
//void *rc_alloc_impl(const char *func, int line_num, int data_size, int extra_arg_count, rc_cleanup_func cleaner_func, ...)
void *rc_alloc_impl(const char *func, int line_num, int data_size, rc_cleanup_func cleaner_func)
{
    refcount_p rc = NULL;
    //cleanup_p cleanup = NULL;
    //va_list extra_args;

    pdebug(DEBUG_INFO,"Starting, called from %s:%d",func, line_num);

    pdebug(DEBUG_SPEW,"Allocating %d-byte refcount struct",(int)sizeof(struct refcount_t));

    rc = mem_alloc((int)sizeof(struct refcount_t) + data_size);
    if(!rc) {
        pdebug(DEBUG_WARN,"Unable to allocate refcount struct!");
        return NULL;
    }

    rc->count = 1;  /* start with a reference count. */
    rc->lock = LOCK_INIT;

    rc->cleanup_func = cleaner_func;

    /* store where we were called from for later. */
    rc->function_name = func;
    rc->line_num = line_num;

    pdebug(DEBUG_INFO, "Done");

    /* return the original address if successful otherwise NULL. */

    /* DEBUG */
    pdebug(DEBUG_DETAIL,"Returning memory pointer %p",(char *)(rc + 1));

    return (char *)(rc + 1);
}








/*
 * Increments the ref count if the reference is valid.
 *
 * It returns the original poiner if the passed pointer was valid.  It returns
 * NULL if the passed pointer was invalid.
 *
 * This is for usage like:
 * my_struct->some_field_ref = rc_inc(ref);
 */

void *rc_inc_impl(const char *func, int line_num, void *data)
{
    int count = 0;
    refcount_p rc = NULL;
    char *result = NULL;

    pdebug(DEBUG_SPEW,"Starting, called from %s:%d for %p",func, line_num, data);

    if(!data) {
        pdebug(DEBUG_SPEW,"Invalid pointer passed from %s:%d!", func, line_num);
        return result;
    }

    /* get the refcount structure. */
    rc = ((refcount_p)data) - 1;

    /* spin until we have ownership */
    spin_block(&rc->lock) {
        if(rc->count > 0) {
            rc->count++;
            count = rc->count;
            result = data;
        } else {
            count = rc->count;
            result = NULL;
        }
    }

    if(!result) {
        pdebug(DEBUG_SPEW,"Invalid ref count (%d) from call at %s line %d!  Unable to take strong reference.", count, func, line_num);
    } else {
        pdebug(DEBUG_SPEW,"Ref count is %d for %p.", count, data);
    }

    /* return the result pointer. */
    return result;
}



/*
 * Decrement the ref count.
 *
 * This is for usage like:
 * my_struct->some_field = rc_dec(rc_obj);
 *
 * Note that the final clean up function _MUST_ free the data pointer
 * passed to it.   It must clean up anything referenced by that data,
 * and the block itself using mem_free() or the appropriate function;
 */

void *rc_dec_impl(const char *func, int line_num, void *data)
{
    int count = 0;
    int invalid = 0;
    refcount_p rc = NULL;

    pdebug(DEBUG_SPEW,"Starting, called from %s:%d for %p",func, line_num, data);

    if(!data) {
        pdebug(DEBUG_SPEW,"Null reference passed from %s:%d!", func, line_num);
        return NULL;
    }

    /* get the refcount structure. */
    rc = ((refcount_p)data) - 1;

    /* do this sorta atomically */
    spin_block(&rc->lock) {
        if(rc->count > 0) {
            rc->count--;
            count = rc->count;
        } else {
            count = rc->count;
            invalid = 1;
        }
    }

    if(invalid) {
        pdebug(DEBUG_WARN,"Reference has invalid count %d!", count);
    } else {
        pdebug(DEBUG_SPEW,"Ref count is %d for %p.", count, data);

        /* clean up only if count is zero. */
        if(rc && count <= 0) {
            pdebug(DEBUG_DETAIL,"Calling cleanup functions due to call at %s:%d for %p.", func, line_num, data);

            refcount_cleanup(rc);
        }
    }

    return NULL;
}




void refcount_cleanup(refcount_p rc)
{
    pdebug(DEBUG_INFO,"Starting");
    if(!rc) {
        pdebug(DEBUG_WARN,"Refcount is NULL!");
        return;
    }

    /* call the clean up function */
    rc->cleanup_func((void *)(rc+1));

    /* finally done. */
    mem_free(rc);

    pdebug(DEBUG_INFO,"Done.");
}
