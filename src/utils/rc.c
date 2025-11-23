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

#include <libplctag/lib/libplctag.h>
#include <platform.h>
#include <utils/debug.h>
#include <utils/rc.h>
#include <utils/vector.h>


//~ #ifndef container_of
//~ #define container_of(ptr, type, member) ((type *)((char *)(1 ? (ptr) : &((type *)0)->member) - offsetof(type, member)))
//~ #endif


/*
 * Handle clean up functions.
 */

// typedef struct cleanup_t *cleanup_p;
//
// struct cleanup_t {
//     cleanup_p next;
//     const char *function_name;
//     int line_num;
//     int extra_arg_count;
//     void **extra_args;
//     rc_cleanup_func cleanup_func;
//     void *dummy[]; /* force alignment */
// };


/*
 * This is a rc struct that we use to make sure that we are able to align
 * the remaining part of the allocated block.
 */

struct refcount_t {
    lock_t lock;
    int count;
    const char *function_name;
    int line_num;
    // cleanup_p cleaners;
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


/* Global cleanup thread state */
static mutex_p cleanup_mutex = NULL;
static cond_p cleanup_cond = NULL;
static vector_p cleanup_queue = NULL;
static thread_p cleanup_thread = NULL;
static volatile int cleanup_thread_running = 0;

static void refcount_cleanup(refcount_p rc);

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

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, "Starting, called from %s:%d", func, line_num);

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, "Allocating %d-byte refcount struct", (int)sizeof(struct refcount_t));

    rc = mem_alloc((int)sizeof(struct refcount_t) + data_size);
    if(!rc) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, "Unable to allocate refcount struct!");
        return NULL;
    }

    rc->count = 1; /* start with a reference count. */
    rc->lock = LOCK_INIT;

    rc->cleanup_func = cleaner_func;

    /* store where we were called from for later. */
    rc->function_name = func;
    rc->line_num = line_num;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, "Done");

    /* return the original address if successful otherwise NULL. */

    /* DEBUG */
    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Returning memory pointer %p", (char *)(rc + 1));

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
    int count = 0;
    refcount_p rc = NULL;
    char *result = NULL;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, "Starting, called from %s:%d for %p", func, line_num, data);

    if(!data) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, "Invalid pointer passed from %s:%d!", func, line_num);
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
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, "Invalid ref count (%d) from call at %s line %d!  Unable to take strong reference.", count, func,
               line_num);
    } else {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, "Ref count is %d for %p.", count, data);
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

void *rc_dec_impl(const char *func, int line_num, void *data) {
    int count = 0;
    int invalid = 0;
    refcount_p rc = NULL;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, "Starting, called from %s:%d for %p", func, line_num, data);

    if(!data) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, "Null reference passed from %s:%d!", func, line_num);
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
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, "Reference has invalid count %d!", count);
    } else {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_SPEW, "Ref count is %d for %p.", count, data);

        /* clean up only if count is zero. */
        if(rc && count <= 0) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Queueing cleanup due to call at %s:%d for object%p allocated in %s:%d.", func, line_num, data, rc->function_name, rc->line_num);

            /*
             * Queue the cleanup instead of doing it immediately.
             * This ensures cleanup happens in a separate thread, not in the caller's thread.
             */
            if(cleanup_thread_running && cleanup_mutex && cleanup_queue) {
                int vec_len = 0;
                critical_block(cleanup_mutex) {
                    vec_len = vector_length(cleanup_queue);
                    if(vector_insert(cleanup_queue, vec_len, rc) == PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Cleanup queued, signaling cleanup thread.");
                        cond_signal(cleanup_cond);
                    } else {
                        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, "Unable to queue cleanup, falling back to immediate cleanup!");
                        /* Fallback: clean up immediately if we can't queue */
                        refcount_cleanup(rc);
                    }
                }
            } else {
                pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Cleanup thread not running, performing immediate cleanup.");
                /* Cleanup thread not available, clean up immediately */
                refcount_cleanup(rc);
            }
        }
    }

    return NULL;
}


void refcount_cleanup(refcount_p rc) {
    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, "Starting");
    if(!rc) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, "Refcount is NULL!");
        return;
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Deferred destruction of %p, allocated in function %s at line %d", (void*)(rc + 1), rc->function_name, rc->line_num);

    /* call the clean up function */
    if(rc->cleanup_func) {
        rc->cleanup_func((void *)(rc + 1));
    }

    /* finally done. */
    mem_free(rc);

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, "Done.");
}


/*
 * Cleanup thread function.
 *
 * Waits on the cleanup condition variable and processes cleanup queue entries
 * one at a time. This ensures that destructors don't run in arbitrary user threads
 * but in a dedicated cleanup thread, avoiding complex thread synchronization issues.
 * 
 * the deferred cleanup vector contains pointers to the refcount header.
 */
THREAD_FUNC(refcount_cleanup_thread_func) {
    (void)arg;  /* Unused parameter */
    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, "Cleanup thread starting.");

    while(cleanup_thread_running) {
        refcount_p header = NULL;

        cond_wait(cleanup_cond, 100); /* 100 millisecond timeout */

        do {
            critical_block(cleanup_mutex) {
                header = vector_remove(cleanup_queue, 0);
            }

            if(header) {
                pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Processing cleanup for object %p allocated at %s:%d", (void*)(header + 1), header->function_name, header->line_num);
                refcount_cleanup(header);
            }
        } while(header != NULL);
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, "Cleanup thread exiting.");
    THREAD_RETURN(0);
}


/*
 * Start the refcount cleanup thread.
 * Called during library initialization.
 */
int refcount_startup(void) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, "Starting refcount cleanup infrastructure.");

    /* Create the cleanup mutex */
    rc = mutex_create(&cleanup_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, "Unable to create cleanup mutex!");
        return rc;
    }

    /* Create the cleanup condition variable */
    rc = cond_create(&cleanup_cond);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, "Unable to create cleanup condition variable!");
        mutex_destroy(&cleanup_mutex);
        cleanup_mutex = NULL;
        return rc;
    }

    /* Create the cleanup queue */
    cleanup_queue = vector_create(16, 512);
    if(!cleanup_queue) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, "Unable to create cleanup queue!");
        cond_destroy(&cleanup_cond);
        cleanup_cond = NULL;
        mutex_destroy(&cleanup_mutex);
        cleanup_mutex = NULL;
        return PLCTAG_ERR_NO_MEM;
    }

    /* Start the cleanup thread */
    cleanup_thread_running = 1;
    rc = thread_create(&cleanup_thread, refcount_cleanup_thread_func, 32 * 1024, NULL);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, "Unable to create cleanup thread!");
        cleanup_thread_running = 0;
        vector_destroy(cleanup_queue);
        cleanup_queue = NULL;
        cond_destroy(&cleanup_cond);
        cleanup_cond = NULL;
        mutex_destroy(&cleanup_mutex);
        cleanup_mutex = NULL;
        return rc;
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, "Refcount cleanup thread started successfully.");

    return PLCTAG_STATUS_OK;
}


/*
 * Shutdown the refcount cleanup thread.
 * Called during library teardown.
 *
 * This waits for the cleanup queue to drain before returning.
 */
int refcount_teardown(void) {
    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, "Shutting down refcount cleanup infrastructure.");

    /* Wait for the cleanup thread to finish */
    if(cleanup_thread) {
        /* Signal the cleanup thread to exit */
        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Signaling cleanup thread to exit.");        
        cleanup_thread_running = 0;
        if(cleanup_cond) {
            cond_signal(cleanup_cond);
        }

        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Waiting for cleanup thread to exit.");
        
        thread_join(cleanup_thread);
        thread_destroy(&cleanup_thread);
        cleanup_thread = NULL;
    } else {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, "Cleanup thread not running!");
    }

    /* drain any remaining queue entries */
    if(cleanup_queue) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Draining any remaining cleanup queue entries.");

        refcount_p header = NULL;
        
        do {
            critical_block(cleanup_mutex) { header = vector_remove(cleanup_queue, 0); }

            if(header) {
                pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Force cleanup of queued refcount %p", (void *)header);
                refcount_cleanup(header);
            }
        } while(header != NULL);

        vector_destroy(cleanup_queue);
        cleanup_queue = NULL;
    }

    /* Clean up synchronization primitives */
    if(cleanup_cond) {
        cond_destroy(&cleanup_cond);
        cleanup_cond = NULL;
    }

    if(cleanup_mutex) {
        mutex_destroy(&cleanup_mutex);
        cleanup_mutex = NULL;
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, "Refcount cleanup infrastructure shut down.");

    return PLCTAG_STATUS_OK;
}

