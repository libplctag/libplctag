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

#include <stdlib.h>
#include <string.h>

#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/handle_system.h>

#define DEBUG_DETAIL DEBUG_DETAIL
#define DEBUG_INFO DEBUG_INFO
#define DEBUG_WARN DEBUG_WARN

/* Metadata header (embedded before user data) */
typedef struct {
    mutex_p mutex;                  /* Protection for this slot */
    atomic_int32_t refcount;        /* External references */
    int8_t destroying;              /* Destruction in progress */
    handle_destructor_f destructor; /* User cleanup function */
} handle_header_t;

/* Array slot - tracks current handle value and data pointer */
typedef struct {
    handle_t handle; /* Current handle value (generation included) */
    void *data;      /* Pointer to user data (NULL if free) */
} handle_slot_t;

/* Global handle array management */
typedef struct {
    handle_slot_t *slots;        /* Dynamic array of slots */
    uint64_t num_slots;          /* Current capacity */
    uint64_t next_free;          /* Hint: first potentially free slot */
    mutex_p array_mutex;         /* Protects array resizing and slot allocation */
    atomic_int32_t active_count; /* Count of allocated slots */
} handle_array_t;

static handle_array_t handle_array = {0};
static mutex_p handle_array_init_mutex = NULL;
static cond_p handle_cleanup_cond = NULL; /* Signaled when last handle destroyed */


int handle_system_init(void) {
    if(handle_array_init_mutex == NULL) { handle_array_init_mutex = mutex_create(); }
    if(handle_cleanup_cond == NULL) { handle_cleanup_cond = cond_create(); }
    return PLCTAG_STATUS_OK;
}


handle_t handle_alloc(size_t data_size, handle_destructor_f destructor) {
    handle_header_t *header;
    void *user_data;
    uint64_t index;
    uint16_t gen;
    handle_slot_t *slot;

    if(!handle_array_init_mutex) { handle_system_init(); }

    /* Allocate header + data as single block */
    header = (handle_header_t *)mem_alloc(sizeof(handle_header_t) + data_size);
    if(!header) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, "Failed to allocate handle");
        return HANDLE_INVALID;
    }

    /* Initialize header */
    header->refcount = 1; /* Caller owns initial reference */
    header->destroying = 0;
    header->destructor = destructor;
    header->mutex = mutex_create();

    /* User data comes immediately after header */
    user_data = (void *)(header + 1);
    memset(user_data, 0, data_size);

    /* Find or create slot in array */
    critical_block(handle_array_init_mutex) {
        /* Initialize array if needed */
        if(!handle_array.slots) {
            handle_array.num_slots = 256;
            handle_array.slots = (handle_slot_t *)mem_alloc(handle_array.num_slots * sizeof(handle_slot_t));
            if(!handle_array.slots) {
                pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, "Failed to allocate handle array");
                mem_free(header);
                return HANDLE_INVALID;
            }
            memset(handle_array.slots, 0, handle_array.num_slots * sizeof(handle_slot_t));
            handle_array.next_free = 0;
        }

        /* Find next free slot */
        index = handle_array.next_free;
        while(index < handle_array.num_slots && handle_array.slots[index].data != NULL) { index++; }

        /* Grow array if at capacity */
        if(index >= handle_array.num_slots) {
            uint64_t new_size = handle_array.num_slots * 2;
            handle_slot_t *new_slots = (handle_slot_t *)mem_alloc(new_size * sizeof(handle_slot_t));
            if(!new_slots) {
                pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, "Failed to grow handle array");
                mem_free(header);
                return HANDLE_INVALID;
            }
            memcpy(new_slots, handle_array.slots, handle_array.num_slots * sizeof(handle_slot_t));
            memset(new_slots + handle_array.num_slots, 0, (new_size - handle_array.num_slots) * sizeof(handle_slot_t));
            mem_free(handle_array.slots);
            handle_array.slots = new_slots;
            handle_array.num_slots = new_size;
        }

        /* Get next generation from existing handle at this index, or start at 0 */
        gen = (index < handle_array.num_slots) ? handle_gen(handle_array.slots[index].handle) : 0;

        /* Create new handle with same index but potentially updated generation */
        handle_t new_handle = handle_make(index, gen);

        /* Store in slot */
        slot = &handle_array.slots[index];
        slot->handle = new_handle;
        slot->data = user_data;

        handle_array.next_free = index + 1;
        atomic_add_int32(&handle_array.active_count, 1);

        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Handle allocated: index=%llu gen=%u active=%d", index, gen,
               atomic_get_int32(&handle_array.active_count));
    }

    return slot->handle;
}


int handle_acquire(handle_t h, void **data_out) {
    uint64_t index = handle_index(h);
    uint16_t gen = handle_gen(h);
    handle_slot_t *slot;
    handle_header_t *header;

    if(!data_out) { return PLCTAG_ERR_NULL_PTR; }

    *data_out = NULL;

    if(!handle_array.slots || index >= handle_array.num_slots) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Handle acquire failed: invalid index %llu", index);
        return PLCTAG_ERR_NOT_FOUND;
    }

    slot = &handle_array.slots[index];

    /* Validate handle: check if handle matches current slot handle */
    if(slot->data == NULL || slot->handle != h) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Handle acquire failed: handle mismatch or free slot");
        return PLCTAG_ERR_NOT_FOUND; /* Slot is free or generation mismatch */
    }

    header = (handle_header_t *)slot->data - 1;

    /* Check destruction flag without lock first (fast path) */
    if(header->destroying) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Handle acquire failed: object being destroyed");
        return PLCTAG_ERR_OBJECT_FREED;
    }

    /* Acquire the slot's mutex */
    mutex_lock(header->mutex);

    /* Double-check destruction after acquiring lock */
    if(header->destroying) {
        mutex_unlock(header->mutex);
        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Handle acquire failed: object marked destroying after lock");
        return PLCTAG_ERR_OBJECT_FREED;
    }

    /* Increment refcount to track active users */
    atomic_add_int32(&header->refcount, 1);

    /* Return data pointer */
    *data_out = slot->data;

    /* Caller must call handle_release to unlock */

    return PLCTAG_STATUS_OK;
}


void handle_release(handle_t h) {
    uint64_t index = handle_index(h);
    handle_slot_t *slot;
    handle_header_t *header;

    if(!handle_array.slots || index >= handle_array.num_slots) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, "Handle release: invalid index %llu", index);
        return;
    }

    slot = &handle_array.slots[index];

    if(!slot->data || slot->handle != h) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, "Handle release: handle invalid or already freed");
        return; /* Handle is invalid */
    }

    header = (handle_header_t *)slot->data - 1;

    /* Decrement refcount */
    atomic_add_int32(&header->refcount, -1);

    /* Unlock */
    mutex_unlock(header->mutex);
}


int handle_destroy(handle_t h) {
    uint64_t index = handle_index(h);
    handle_slot_t *slot;
    handle_header_t *header;
    void *data;
    handle_destructor_f destructor;
    int wait_count = 0;

    if(!handle_array.slots || index >= handle_array.num_slots) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, "Handle destroy: invalid index %llu", index);
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(handle_array_init_mutex) {
        slot = &handle_array.slots[index];

        if(!slot->data || slot->handle != h) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, "Handle destroy: already freed or wrong generation");
            return PLCTAG_ERR_NOT_FOUND; /* Already freed or wrong generation */
        }

        header = (handle_header_t *)slot->data - 1;

        /* Mark as destroying to prevent new acquires */
        mutex_lock(header->mutex);
        header->destroying = 1;
        mutex_unlock(header->mutex);

        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Handle destroy: index=%llu marked destroying, refcount=%d", index,
               atomic_get_int32(&header->refcount));

        /* Wait for refcount to reach 1 (only the initial refcount remains) */
        while(atomic_get_int32(&header->refcount) > 1) {
            wait_count++;
            critical_block_exit(handle_array_init_mutex);
            thread_sleep(1);
            critical_block_re_enter(handle_array_init_mutex);

            if(wait_count > 10000) {
                pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, "Handle destroy: timeout waiting for refcount, count=%d",
                       atomic_get_int32(&header->refcount));
                break;
            }
        }

        /* Decrement the initial refcount */
        atomic_add_int32(&header->refcount, -1);

        /* Save destructor and data before clearing slot */
        data = slot->data;
        destructor = header->destructor;

        /* Increment generation to invalidate handle */
        uint16_t next_gen = (handle_gen(h) + 1) & HANDLE_GEN_MASK;
        slot->handle = handle_make(index, next_gen);
        slot->data = NULL;

        handle_array.next_free = (index < handle_array.next_free) ? index : handle_array.next_free;

        atomic_add_int32(&handle_array.active_count, -1);

        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Handle destroyed: index=%llu new_gen=%u active=%d", index, next_gen,
               atomic_get_int32(&handle_array.active_count));
    }

    /* Call destructor outside critical section */
    if(destructor && data) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Calling destructor for handle");
        destructor(data);
    }

    /* Free header + data */
    if(header) {
        if(header->mutex) { mutex_destroy(header->mutex); }
        mem_free(header);
    }

    /* Signal cleanup waiters if no more handles */
    if(atomic_get_int32(&handle_array.active_count) == 0) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, "All handles destroyed, signaling cleanup");
        cond_signal(handle_cleanup_cond);
    }

    return PLCTAG_STATUS_OK;
}


void handle_system_teardown(void) {
    int wait_count = 0;

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, "Handle system teardown starting");

    while(atomic_get_int32(&handle_array.active_count) > 0) {
        wait_count++;
        int rc = cond_wait(handle_cleanup_cond, 5000);
        if(rc == PLCTAG_ERR_TIMEOUT) {
            if(wait_count >= 3) {
                pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, "Timeout waiting for handles to cleanup, giving up (active=%d)",
                       atomic_get_int32(&handle_array.active_count));
                break;
            } else {
                pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Timeout waiting for handles, retrying... (active=%d)",
                       atomic_get_int32(&handle_array.active_count));
            }
        } else {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, "Handle cleanup signaled (active=%d)",
                   atomic_get_int32(&handle_array.active_count));
        }
    }

    if(handle_array.slots) {
        mem_free(handle_array.slots);
        handle_array.slots = NULL;
    }

    if(handle_array_init_mutex) {
        mutex_destroy(handle_array_init_mutex);
        handle_array_init_mutex = NULL;
    }

    if(handle_cleanup_cond) {
        cond_destroy(handle_cleanup_cond);
        handle_cleanup_cond = NULL;
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, "Handle system teardown complete");
}
