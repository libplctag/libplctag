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

#include <libplctag/api/libplctag.h>
#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>
#include <utils/rc.h>


/*
 * This is a rc struct that we use to make sure that we are able to align
 * the remaining part of the allocated block.
 */

struct refcount_t {
    atomic_int32_t count;
    const char *function_name;
    int line_num;
    rc_cleanup_func cleanup_func;

    /* the cleanup queue's link, so queueing cannot allocate or fail.  Under cleanup_lock. */
    struct refcount_t *next_cleanup;

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


/*
 * Cleanup thread state.  cleanup_lock is static and holds no resource, so it is never freed and a
 * late rc_dec() during or after shutdown always finds it valid (review U8).  The library may be
 * unloaded and reloaded in-process, so nothing here may need an explicit free to stay valid.
 *
 * Under cleanup_lock: cleanup_running, the queue, and every signal of cleanup_cond.  The
 * condition is created before cleanup_running is set and destroyed after it is cleared, so no
 * one can signal it while it does not exist.
 */
static lock_t cleanup_lock = LOCK_INIT;
static bool cleanup_running = false;
static refcount_p cleanup_head = NULL;
static refcount_p cleanup_tail = NULL;
static cond_p cleanup_cond = NULL;
static thread_p cleanup_thread = NULL;

static void refcount_cleanup(refcount_p rc);
static refcount_p cleanup_pop(void);
static bool cleanup_is_running(void);

static THREAD_FUNC(refcount_cleanup_thread_func);


/*
 * rc_alloc
 *
 * Create a reference counted control for the requested data size.  Return a strong
 * reference to the data.
 */
// void *rc_alloc_impl(const char *func, int line_num, int data_size, int extra_arg_count, rc_cleanup_func cleaner_func, ...)
void *rc_alloc_impl(const char *func, int line_num, int data_size, rc_cleanup_func cleaner_func) {
    refcount_p rc = NULL;
    // cleanup_p cleanup = NULL;
    // va_list extra_args;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Starting, called from %s:%d", func, line_num);

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Allocating %d-byte refcount struct", (int)sizeof(struct refcount_t));

    rc = mem_alloc((int)sizeof(struct refcount_t) + data_size);
    if(!rc) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Unable to allocate refcount struct!");
        return NULL;
    }

    /* start with a reference count. */
    atomic_set_int32(&rc->count, 1);

    rc->cleanup_func = cleaner_func;

    /* store where we were called from for later. */
    rc->function_name = func;
    rc->line_num = line_num;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Done");

    /* return the original address if successful otherwise NULL. */

    /* DEBUG */
    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Returning memory pointer %p", (char *)(rc + 1));

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

void *rc_inc_impl(const char *func, int line_num, void *data) {
    int old_count = 0;
    refcount_p rc = NULL;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Starting, called from %s:%d for %p", func, line_num, data);

    if(!data) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Invalid pointer passed from %s:%d!", func, line_num);
        return NULL;
    }

    /* get the refcount structure. */
    rc = ((refcount_p)data) - 1;

    /* Increment the reference count atomically */
    do {
        old_count = atomic_get_int32(&rc->count);

        if(old_count <= 0) { return NULL; }
    } while(atomic_compare_and_set_int32(&rc->count, old_count, old_count + 1) != old_count);

    /* return the data pointer. */
    return data;
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

void *rc_dec_impl(const char *func, int line_num, void *data) {
    int32_t old_count = 0;
    refcount_p rc = NULL;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, 0, "Starting, called from %s:%d for %p", func, line_num, data);

    if(!data) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Null reference passed from %s:%d!", func, line_num);
        return NULL;
    }

    /* get the refcount structure. */
    rc = ((refcount_p)data) - 1;

    old_count = atomic_add_int32(&rc->count, -1); /* returns OLD value */
    int count = old_count - 1;
    if(count == 0) {
        /* queue cleanup */
        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Queueing cleanup due to call at %s:%d for object%p allocated in %s:%d.", func,
               line_num, data, rc->function_name, rc->line_num);

        bool queued = false;

        /*
         * Destructors run on the cleanup thread, not the caller's.  Once that thread is stopped,
         * they run here instead.
         */
        spin_block(&cleanup_lock) {
            if(cleanup_running) {
                rc->next_cleanup = NULL;

                if(cleanup_tail) {
                    cleanup_tail->next_cleanup = rc;
                } else {
                    cleanup_head = rc;
                }

                cleanup_tail = rc;
                cond_signal(cleanup_cond);
                queued = true;
            }
        }

        if(!queued) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Cleanup thread not running, performing immediate cleanup.");
            refcount_cleanup(rc);
        }
    }

    return NULL;
}


void refcount_cleanup(refcount_p rc) {
    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Starting");
    if(!rc) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Refcount is NULL!");
        return;
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Deferred destruction of %p, allocated in function %s at line %d", (void *)(rc + 1),
           rc->function_name, rc->line_num);

    /* call the clean up function */
    if(rc->cleanup_func) { rc->cleanup_func((void *)(rc + 1)); }

    /* finally done. */
    mem_free(rc);

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Done.");
}


/*
 * Cleanup thread function.  Runs every queued destructor, oldest first, until refcount_teardown()
 * stops it.  A destructor may queue more work; the inner loop runs that too.
 */
THREAD_FUNC(refcount_cleanup_thread_func) {
    refcount_p header = NULL;

    (void)arg; /* Unused parameter */
    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Cleanup thread starting.");

    do {
        cond_wait(cleanup_cond, 100); /* 100 millisecond timeout */

        while((header = cleanup_pop()) != NULL) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Processing cleanup for object %p allocated at %s:%d",
                   (void *)(header + 1), header->function_name, header->line_num);
            refcount_cleanup(header);
        }
    } while(cleanup_is_running());

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Cleanup thread exiting.");
    THREAD_RETURN(0);
}


/*
 * Start the refcount cleanup thread.
 * Called during library initialization.
 */
int refcount_startup(void) {
    int rc = PLCTAG_STATUS_OK;
    refcount_p header = NULL;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Starting refcount cleanup infrastructure.");

    rc = cond_create(&cleanup_cond);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "Unable to create cleanup condition variable!");
        return rc;
    }

    spin_block(&cleanup_lock) { cleanup_running = true; }

    rc = thread_create(&cleanup_thread, refcount_cleanup_thread_func, 32 * 1024, NULL);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "Unable to create cleanup thread!");

        spin_block(&cleanup_lock) { cleanup_running = false; }

        /* anything queued in the meantime still has to run. */
        while((header = cleanup_pop()) != NULL) { refcount_cleanup(header); }

        cond_destroy(&cleanup_cond);
        cleanup_cond = NULL;
        return rc;
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Refcount cleanup thread started successfully.");

    return PLCTAG_STATUS_OK;
}


/*
 * Stop the cleanup thread after it has run every queued destructor, then run any left over.
 * This is the one place shutdown waits for destructors, so it has no timeout: returning while a
 * destructor still runs would let an unload remove the code it is running (review U7).
 */
int refcount_teardown(void) {
    refcount_p header = NULL;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Shutting down refcount cleanup infrastructure.");

    /* from here on rc_dec() runs destructors itself and nothing signals cleanup_cond. */
    spin_block(&cleanup_lock) { cleanup_running = false; }

    if(cleanup_thread) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Waiting for cleanup thread to exit.");

        cond_signal(cleanup_cond);
        thread_join(cleanup_thread);
        thread_destroy(&cleanup_thread);
        cleanup_thread = NULL;
    } else {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Cleanup thread not running!");
    }

    /* anything queued after the thread's last pass. */
    while((header = cleanup_pop()) != NULL) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Force cleanup of queued refcount %p", (void *)header);
        refcount_cleanup(header);
    }

    if(cleanup_cond) {
        cond_destroy(&cleanup_cond);
        cleanup_cond = NULL;
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Refcount cleanup infrastructure shut down.");

    return PLCTAG_STATUS_OK;
}


/* take the oldest queued object, or NULL. */
static refcount_p cleanup_pop(void) {
    refcount_p header = NULL;

    spin_block(&cleanup_lock) {
        header = cleanup_head;

        if(header) {
            cleanup_head = header->next_cleanup;
            if(!cleanup_head) { cleanup_tail = NULL; }
        }
    }

    return header;
}


static bool cleanup_is_running(void) {
    bool running = false;

    spin_block(&cleanup_lock) { running = cleanup_running; }

    return running;
}
