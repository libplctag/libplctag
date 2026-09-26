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

#include <inttypes.h>
#include <stdlib.h>
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/lib/tag_registry.h>
#include <libplctag/api/libplctag.h>
#include <platform.h>
#include <utils/debug.h>
#include <utils/atomic_utils.h>
#include <utils/hashtable.h>
#include <utils/vector.h>
#include <utils/rc.h>


/*
 * The tag registry: the tag hashtable, its lookup mutex, and the tag tickler
 * thread and its condition variable.  Allocated with rc_alloc() so its lifetime
 * can be shared between "the registry is running" (current_registry holds the
 * genesis reference) and "a tag that outlived shutdown still needs it"
 * (tag->instance holds one per tag).
 */
struct tag_registry_t {
    hashtable_p tags;
    mutex_p tag_lookup_mutex;
    cond_p tag_tickler_wait;
    thread_p tag_tickler_thread;
};

static THREAD_LOCAL vector_p active_tags = NULL;

/* next_tag_id deliberately lives outside tag_registry_t and is never reset by
 * teardown: ids must not be reused across a shutdown/restart cycle, or a stale
 * handle held by the application would resolve to an unrelated tag. */
static volatile int32_t next_tag_id = 10; /* MAGIC */

/* Bound on the search for a free id, so a saturated table fails instead of spinning. */
#define MAX_TAG_MAP_ATTEMPTS (50)

static int tag_id_inc(int id);
static THREAD_FUNC(tag_tickler_func);

plc_tag_p lookup_tag(int32_t tag_id) {
    plc_tag_p tag = NULL;
    tag_registry_p inst = tag_registry_acquire();

    if(!inst) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag_id, "Library not running, returning NULL for tag lookup.");
        return NULL;
    }

    critical_block(inst->tag_lookup_mutex) {
        tag = hashtable_get(inst->tags, (int64_t)tag_id);

        if(tag && tag->tag_id == tag_id) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, tag_id, "Found tag %p with id %d.", tag, tag->tag_id);
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "rc_inc: Acquiring reference to tag %" PRId32 ".", tag->tag_id);
            tag = rc_inc(tag);
        } else {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Tag with ID %d not found.", tag_id);

            tag = NULL;
        }
    }

    rc_dec(inst);

    return tag;
}


int tag_id_inc(int id) {
    if(id <= 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_ERROR, 0, "Incoming ID is not valid! Got %d", id);
        /* try to correct. */
        id = (TAG_ID_MASK / 2);
    }

    id = (id + 1) & TAG_ID_MASK;

    if(id == 0) { id = 1; /* skip zero intentionally! Can't return an ID of zero because it looks like a NULL pointer */ }

    return id;
}


int add_tag_lookup(plc_tag_p tag) {
    int rc = PLCTAG_ERR_NOT_FOUND;
    int new_id = 0;

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, 0, "Starting.");

    critical_block(tag->instance->tag_lookup_mutex) {
        int attempts = 0;

        /* only get this when we hold the mutex. */
        new_id = next_tag_id;

        do {
            new_id = tag_id_inc(new_id);

            if(new_id <= 0) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "ID %d is illegal!", new_id);
                attempts = MAX_TAG_MAP_ATTEMPTS;
                break;
            }

            pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, 0, "Trying new ID %d.", new_id);

            if(!hashtable_get(tag->instance->tags, (int64_t)new_id)) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, 0, "Found unused ID %d", new_id);
                break;
            }

            attempts++;
        } while(attempts < MAX_TAG_MAP_ATTEMPTS);

        if(attempts < MAX_TAG_MAP_ATTEMPTS) {
            /* Set tag->tag_id before the tag is published into the hashtable (in
             * the same tag_lookup_mutex critical section as the hashtable_put()
             * below), not after: any thread that can observe this tag at all had
             * to acquire this same mutex to do so (tag_tickler_func()'s scan), so
             * by the time it later reads tag->tag_id -- even outside this lock,
             * even in a branch that never takes tag->api_mutex -- that read is
             * ordered after this write by the mutex's release/acquire pair. This
             * closes the race where the write used to happen later, under
             * api_mutex alone, after the tag was already hashtable-visible under
             * this lock. */
            tag->tag_id = new_id;
            rc = hashtable_put(tag->instance->tags, (int64_t)new_id, tag);
        } else {
            rc = PLCTAG_ERR_NO_RESOURCES;
        }

        next_tag_id = new_id;
    }

    if(rc != PLCTAG_STATUS_OK) { new_id = rc; }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, 0, "Done.");

    return new_id;
}


#define INITIAL_TAG_TABLE_SIZE (201)
/* The library-scoped instance: the tag hashtable, its lookup mutex, and the tag
 * tickler thread/condvar. Allocated with rc_alloc() so its lifetime can be shared
 * safely between "the library is running" (current_instance holds the genesis
 * reference) and "a tag that outlives shutdown still needs it"
 * (tag->instance holds one per tag). */

/* Guards current_instance and building_instance/shutting_down_instance below. Must
 * be a spinlock, not a real mutex: it needs no runtime construction, so it is valid
 * before any other library state exists and after all of it is gone. */
static lock_t instance_lock = LOCK_INIT;

/* The single published instance. NULL means "not running" -- this is the entire
 * gate tag_registry_acquire() checks. Set by tag_registry_publish(), cleared by
 * plc_tag_shutdown(). */
static tag_registry_p current_instance = NULL;

/* Staging for an instance between tag_registry_init() building it and initialize_modules()
 * either publishing it (tag_registry_publish(), once ab_init()/mb_init()/omron_init()
 * have also succeeded) or discarding it (tag_registry_discard_pending(), if one of
 * those fails). Not protected by instance_lock: only initialize_modules() touches
 * it, and only while it is the sole thread inside the library's startup CAS. */
static tag_registry_p building_instance = NULL;

/* Staging for the instance between plc_tag_shutdown() clearing current_instance
 * and tag_registry_teardown() (called later, via destroy_modules()) dropping the last
 * ("genesis") reference. Not protected by instance_lock for the same reason as
 * building_instance: only the single thread running shutdown ever touches it. */
static tag_registry_p shutting_down_instance = NULL;

atomic_bool lib_active = false;

#define TAG_TICKLER_TIMEOUT_MS (100)
#define TAG_TICKLER_TIMEOUT_MIN_MS (10)
static int64_t tag_tickler_wait_timeout_end = 0;


/* rc_alloc() destructor for tag_registry_t. By the time this runs, tag_tickler_thread
 * is already NULL: plc_tag_shutdown() joins and destroys it itself, before protocol
 * modules are torn down (the tickler calls into their vtables), which is well
 * before this destructor's reference (the last one) is ever dropped. */
static void tag_registry_destructor(void *arg) {
    tag_registry_p inst = (tag_registry_p)arg;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Starting.");

    if(inst->tag_tickler_wait) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "About to destroy tag tickler condition var.");
        cond_destroy(&inst->tag_tickler_wait);
        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Tag tickler condition var destroyed.");
    }

    if(inst->tag_lookup_mutex) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "About to destroy tag lookup mutex.");
        mutex_destroy(&inst->tag_lookup_mutex);
        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Tag lookup mutex destroyed.");
    }

    if(inst->tags) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "About to destroy tag hashtable.");
        hashtable_destroy(inst->tags);
        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Tag hashtable destroyed.");
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Done.");
}


tag_registry_p tag_registry_acquire(void) {
    tag_registry_p inst = NULL;

    spin_block(&instance_lock) {
        if(current_instance) { inst = rc_inc(current_instance); }
    }

    return inst;
}


void tag_registry_discard_pending(void) {
    if(building_instance) {
        rc_dec(building_instance);
        building_instance = NULL;
    }
}


void tag_registry_publish(void) {
    tag_registry_p inst = building_instance;

    building_instance = NULL;

    if(!inst) {
        /* Only reachable if tag_registry_init() never actually built an instance -- e.g. a
         * unit test that mocks tag_registry_init() itself out. Nothing to publish. */
        return;
    }

    spin_block(&instance_lock) { current_instance = inst; }

    atomic_set_bool(&lib_active, true);

    /*
     * Start the tickler last.  Its "keep running" condition is "there are tags, or the
     * library is RUNNING".  There are no tags yet here, and library_state does not reach
     * RUNNING until this function returns, so a thread started earlier exits on its first
     * check.
     */
    if(thread_create(&inst->tag_tickler_thread, tag_tickler_func, 32 * 1024, inst) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_ERROR, 0, "Unable to create tag tickler thread! Automatic tag operations will not run.");
    }
}


int tag_registry_init(void) {
    int rc = PLCTAG_STATUS_OK;
    tag_registry_p inst = NULL;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Starting.");

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Setting up global library data.");

    inst = rc_alloc(sizeof(struct tag_registry_t), tag_registry_destructor);
    if(!inst) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_ERROR, 0, "Unable to allocate library instance!");
        return PLCTAG_ERR_NO_MEM;
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Creating tag hashtable.");
    if((inst->tags = hashtable_create(INITIAL_TAG_TABLE_SIZE)) == NULL) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_ERROR, 0, "Unable to create tag hashtable!");
        rc_dec(inst);
        return PLCTAG_ERR_NO_MEM;
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Creating tag hashtable mutex.");
    rc = mutex_create(&(inst->tag_lookup_mutex));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_ERROR, 0, "Unable to create tag hashtable mutex!");
        rc_dec(inst);
        return rc;
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Creating tag condition variable.");
    rc = cond_create(&(inst->tag_tickler_wait));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_ERROR, 0, "Unable to create tag condition var!");
        rc_dec(inst);
        return rc;
    }

    building_instance = inst;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/* Drops the last ("genesis") reference to the instance plc_tag_shutdown() staged
 * here after clearing current_instance and joining the tickler thread. By this
 * point (called from destroy_modules(), after refcount_teardown() has already
 * drained the cleanup queue and stopped the RC thread) every tag's own reference
 * is already gone, so this rc_dec() is the last one: it runs
 * tag_registry_destructor() synchronously, inline, on the calling thread. */
void tag_registry_teardown(void) {
    tag_registry_p inst = shutting_down_instance;

    shutting_down_instance = NULL;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Tearing down library.");

    if(inst) { rc_dec(inst); }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Library teardown complete.");
}


int plc_tag_tickler_wake_impl(const char *func, int line_num) {
    int rc = PLCTAG_STATUS_OK;
    tag_registry_p inst = tag_registry_acquire();

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, 0, "Starting. Called from %s:%d.", func, line_num);

    if(!inst) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Called from %s:%d when library is not running!", func, line_num);
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!inst->tag_tickler_wait) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Called from %s:%d when tag tickler condition var is NULL!", func, line_num);
        rc_dec(inst);
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = cond_signal(inst->tag_tickler_wait);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Error %s trying to signal condition variable in call from %s:%d",
               plc_tag_decode_error(rc), func, line_num);
    }

    rc_dec(inst);

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, 0, "Done. Called from %s:%d.", func, line_num);

    return rc;
}


THREAD_FUNC(tag_tickler_func) {
    tag_registry_p inst = (tag_registry_p)arg;

    if(!active_tags) { active_tags = vector_create(100, 100); }

    if(!active_tags) {
        /* ERROR! This is terminal.*/
        pdebug(DEBUG_MODULE_LIB, DEBUG_ERROR, 0, "Unable to create active tags vector for tag tickler thread!");

        /* shut down the app, can't call plc_tag_shutdown() */
        exit(1);
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Starting.");

    while(atomic_get_bool(&lib_active)) {
        int max_index = 0;
        int64_t timeout_wait_ms = TAG_TICKLER_TIMEOUT_MS;

        /* what is the maximum time we will wait until */
        tag_tickler_wait_timeout_end = time_ms() + timeout_wait_ms;

        critical_block(inst->tag_lookup_mutex) {
            max_index = hashtable_capacity(inst->tags);

            for(int i = 0; i < max_index; i++) {
                plc_tag_p tag = hashtable_get_index(inst->tags, i);

                if(tag && !atomic_get_bool(&tag->skip_tickler) && rc_inc(tag) != NULL) {
                    vector_insert(active_tags, vector_length(active_tags), tag);
                }
            }
        }

        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, 0, "Tickling %d active tags.", vector_length(active_tags));

        int num_active_tags = vector_length(active_tags);

        for(int tag_index = 0; tag_index < num_active_tags; tag_index++) {
            plc_tag_p tag = vector_get(active_tags, tag_index);

            /* try to hold the tag API mutex while all this goes on. */
            if(mutex_try_lock(tag->api_mutex) == PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "calling generic tag tickler.");

                plc_tag_generic_tickler(tag);

                /* call the tickler function if we can. */
                if(tag->vtable && tag->vtable->tickler) { tag->vtable->tickler(tag); }

                if(tag->read_complete) {
                    tag->read_complete = 0;
                    tag->read_in_flight = 0;

                    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Raising read complete event.");

                    // tag->event_read_complete = 1;
                    tag_raise_event(tag, PLCTAG_EVENT_READ_COMPLETED, tag->status);

                    /* wake immediately */
                    // plc_tag_tickler_wake();
                    cond_signal(tag->tag_cond_wait);
                }

                if(tag->write_complete) {
                    tag->write_complete = 0;
                    tag->write_in_flight = 0;
                    tag->auto_sync_next_write = 0;

                    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Raising write complete event.");

                    // tag->event_write_complete = 1;
                    tag_raise_event(tag, PLCTAG_EVENT_WRITE_COMPLETED, tag->status);

                    /* wake immediately */
                    //  plc_tag_tickler_wake();
                    cond_signal(tag->tag_cond_wait);
                }

                /* wake up earlier if the time until the next write wake up is sooner. */
                if(tag->auto_sync_next_write && tag->auto_sync_next_write < tag_tickler_wait_timeout_end) {
                    tag_tickler_wait_timeout_end = tag->auto_sync_next_write;
                }

                /* wake up earlier if the time until the next read wake up is sooner. */
                if(tag->auto_sync_next_read && tag->auto_sync_next_read < tag_tickler_wait_timeout_end) {
                    tag_tickler_wait_timeout_end = tag->auto_sync_next_read;
                }

                /* we are done with the tag API mutex now. */
                mutex_unlock(tag->api_mutex);

                /* call callbacks */
                plc_tag_generic_handle_event_callbacks(tag);
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Skipping tag as it is already locked.");
            }

            if(tag) { rc_dec(tag); }
        }

        /* clear the active tags vector */
        vector_reset(active_tags);

        if(inst->tag_tickler_wait) {
            int64_t time_to_wait = tag_tickler_wait_timeout_end - time_ms();
            int wait_rc = PLCTAG_STATUS_OK;

            if(time_to_wait < TAG_TICKLER_TIMEOUT_MIN_MS) { time_to_wait = TAG_TICKLER_TIMEOUT_MIN_MS; }

            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, 0, "Waiting for %" PRId64 "ms until next tickler wake up.", time_to_wait);

            wait_rc = cond_wait(inst->tag_tickler_wait, (int)time_to_wait);
            if(wait_rc == PLCTAG_ERR_TIMEOUT) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, 0, "Tag tickler thread timed out waiting for something to do.");
            }
        }
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Terminating.");

    vector_destroy(active_tags);
    active_tags = NULL;

    THREAD_RETURN(0);
}


tag_registry_p tag_registry_close(void) {
    tag_registry_p registry = NULL;

    spin_block(&instance_lock) {
        registry = current_instance;
        current_instance = NULL;
    }

    return registry;
}


void tag_registry_stage_shutdown(tag_registry_p registry) { shutting_down_instance = registry; }


/*
 * Destroy every tag still in the registry.
 *
 * Called from plc_tag_shutdown() after the gate is closed, so no new tag can appear
 * while this runs.  The table is re-measured on each pass because destroying a tag can
 * shrink it.  Tags are destroyed outside the lookup mutex: the destructor runs protocol
 * teardown, which may take other locks.
 */
void tag_registry_destroy_all_tags(tag_registry_p registry) {
    int tag_table_entries = 0;

    if(!registry) { return; }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Closing all tags.");

    critical_block(registry->tag_lookup_mutex) { tag_table_entries = hashtable_capacity(registry->tags); }

    for(int i = 0; i < tag_table_entries; i++) {
        plc_tag_p tag = NULL;

        critical_block(registry->tag_lookup_mutex) {
            tag_table_entries = hashtable_capacity(registry->tags);

            if(i < tag_table_entries && tag_table_entries >= 0) {
                tag = hashtable_get_index(registry->tags, i);

                /* make sure the tag does not go away while we are using the pointer. */
                if(tag) {
                    /* this returns NULL if the existing ref-count is zero. */
                    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "rc_inc: Acquiring reference to tag %" PRId32 ".",
                           tag->tag_id);
                    tag = rc_inc(tag);
                }
            }
        }

        /* do this outside the mutex. */
        if(tag) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Destroying tag %" PRId32 ".", tag->tag_id);

            /*
             * Remove from the hashtable and run the common destroy tail here, using the
             * registry we already hold, rather than going through the public
             * plc_tag_destroy(): its tag_registry_acquire() would correctly (and
             * unhelpfully) fail, since the gate is deliberately already closed to stop
             * any *new* tag creation.
             */
            critical_block(registry->tag_lookup_mutex) { hashtable_remove(registry->tags, (int64_t)tag->tag_id); }
            destroy_tag_common(tag);

            pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
            rc_dec(tag);
        }
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "All tags closed.");
}


/* Wake the tickler thread and wait for it to exit. */
void tag_registry_stop_tickler(tag_registry_p registry) {
    if(!registry || !registry->tag_tickler_thread) { return; }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Waiting for tag tickler thread to exit.");
    cond_signal(registry->tag_tickler_wait);
    thread_join(registry->tag_tickler_thread);
    thread_destroy(&registry->tag_tickler_thread);
    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Tag tickler thread exited.");
}


/* Remove a tag from the registry.  The caller still holds its own reference. */
void remove_tag_lookup(plc_tag_p tag) {
    if(!tag || !tag->instance) { return; }

    critical_block(tag->instance->tag_lookup_mutex) { hashtable_remove(tag->instance->tags, (int64_t)tag->tag_id); }
}


/* Remove a tag by id and return it, or NULL if no such tag is registered. */
plc_tag_p remove_tag_lookup_by_id(tag_registry_p registry, int32_t tag_id) {
    plc_tag_p tag = NULL;

    if(!registry) { return NULL; }

    critical_block(registry->tag_lookup_mutex) { tag = hashtable_remove(registry->tags, (int64_t)tag_id); }

    return tag;
}
