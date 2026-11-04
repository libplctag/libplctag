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
#include <utils/hash.h>
#include <utils/hashtable.h>
#include <utils/rc.h>


/*
 * The tag registry: the reuse map and the mutex that serialises creates and removes.  Allocated
 * with rc_alloc() so its lifetime can be shared between "the registry is running"
 * (current_registry holds the genesis reference) and "a tag that outlived shutdown still needs
 * it" (tag->instance holds one per tag).  Tags themselves live in tag_slots[] below.
 */
struct tag_registry_t {
    /* hash64() of the attribute string -> tag, for tags created with find_or_create_tag=1. */
    hashtable_p reuse;
    mutex_p tag_lookup_mutex;

    /*
     * Set under tag_lookup_mutex by tag_registry_destroy_all_tags() before it starts
     * sweeping, and checked by add_tag_lookup() under the same mutex.  Closing the gate in
     * tag_registry_close() is not enough on its own: a create that passed the gate before
     * it closed is still running and will publish its tag when it finishes, and if that
     * lands after the sweep has already walked that bucket the tag is never destroyed.
     *
     * Serialising both on this mutex leaves only two orderings, and both are safe.  An
     * insert that takes the mutex first completes before the flag is set, so the tag is in
     * the table before iteration begins and the sweep finds it.  An insert that takes the
     * mutex afterwards sees the flag and is refused, so the tag is never published at all.
     */
    bool shutting_down;
};

/* next_tag_id deliberately lives outside tag_registry_t and is never reset by
 * teardown: ids must not be reused across a shutdown/restart cycle, or a stale
 * handle held by the application would resolve to an unrelated tag. */
static volatile int32_t next_tag_id = 10; /* MAGIC */

/*
 * Tag id -> tag.  A tag lives in slot (id & TAG_SLOT_MASK), so a lookup is one slot and one
 * spinlock, and lookups of different tags do not contend.  Creates pick an id whose slot is
 * free.  Writers take tag_lookup_mutex, then the slot lock; lookup_tag() takes only the slot
 * lock.  A tag in a slot holds the registry's reference, so rc_inc() under the slot lock
 * cannot see a freed tag.
 *
 * The table is fixed, so TAG_SLOT_COUNT is the most tags open at once.  CMake sets it from the
 * TAG_SLOT_COUNT cache variable.
 */
#ifndef TAG_SLOT_COUNT
#    define TAG_SLOT_COUNT (262144)
#endif

#if(TAG_SLOT_COUNT & (TAG_SLOT_COUNT - 1)) || TAG_SLOT_COUNT > (TAG_ID_MASK + 1)
#    error "TAG_SLOT_COUNT must be a power of two no larger than the tag id space."
#endif

#define TAG_SLOT_MASK (TAG_SLOT_COUNT - 1)

typedef struct {
    int32_t tag_id;
    lock_t lock;
    plc_tag_p tag;
} tag_slot_t;

static tag_slot_t tag_slots[TAG_SLOT_COUNT];


static int tag_id_inc(int id);
static int64_t reuse_hash(const char *reuse_str);
static void forget_reusable_unsafe(tag_registry_p registry, plc_tag_p tag);
static plc_tag_p take_slot_unsafe(int32_t tag_id, plc_tag_p tag);

/*
 * No registry reference here: shutdown clears lib_active before it sweeps the slots, and a tag
 * still in a slot is alive.
 */
plc_tag_p lookup_tag(int32_t tag_id) {
    tag_slot_t *slot = &tag_slots[tag_id & TAG_SLOT_MASK];
    plc_tag_p tag = NULL;

    if(!atomic_get_bool(&lib_active)) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag_id, "Library not running, returning NULL for tag lookup.");
        return NULL;
    }

    spin_block(&slot->lock) {
        if(slot->tag && slot->tag_id == tag_id) { tag = rc_inc(slot->tag); }
    }

    if(!tag) { pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Tag with ID %d not found.", tag_id); }

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


/* The hashtable treats key 0 as an empty slot. */
int64_t reuse_hash(const char *reuse_str) {
    int64_t key = (int64_t)hash64((const uint8_t *)reuse_str, (size_t)(unsigned int)str_length(reuse_str));

    return (key ? key : 1);
}


/*
 * Empty the slot for tag_id and return what was in it.  With tag set, only if it holds that
 * tag.  The caller holds tag_lookup_mutex.
 */
plc_tag_p take_slot_unsafe(int32_t tag_id, plc_tag_p tag) {
    tag_slot_t *slot = &tag_slots[tag_id & TAG_SLOT_MASK];
    plc_tag_p found = NULL;

    spin_block(&slot->lock) {
        if(slot->tag && slot->tag_id == tag_id && (!tag || slot->tag == tag)) {
            found = slot->tag;
            slot->tag = NULL;
            slot->tag_id = 0;
        }
    }

    return found;
}


/* Take the tag out of the reuse map, if it is there.  The caller holds tag_lookup_mutex. */
void forget_reusable_unsafe(tag_registry_p registry, plc_tag_p tag) {
    if(!tag->reuse_key) { return; }

    hashtable_remove(registry->reuse, reuse_hash(tag->reuse_key));
    mem_free(tag->reuse_key);
    tag->reuse_key = NULL;
}


/* A new reference to the reusable tag created with exactly this string, or NULL. */
plc_tag_p tag_registry_find_reusable(tag_registry_p registry, const char *reuse_str) {
    plc_tag_p tag = NULL;

    critical_block(registry->tag_lookup_mutex) {
        tag = hashtable_get(registry->reuse, reuse_hash(reuse_str));

        tag = ((tag && str_cmp(tag->reuse_key, reuse_str) == 0) ? rc_inc(tag) : NULL);
    }

    return tag;
}


/* Stop handing out this tag to creates.  Holders keep it. */
void tag_registry_forget_reusable(plc_tag_p tag) {
    critical_block(tag->instance->tag_lookup_mutex) { forget_reusable_unsafe(tag->instance, tag); }
}


/*
 * Publish the tag.  With reuse_str, also enter it in the reuse map.  If another thread already
 * entered a tag with the same string, publish nothing: *existing gets a reference to that tag
 * and its id is returned.
 */
int add_tag_lookup(plc_tag_p tag, const char *reuse_str, plc_tag_p *existing) {
    int rc = PLCTAG_ERR_NOT_FOUND;
    int new_id = 0;

    *existing = NULL;

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, 0, "Starting.");

    critical_block(tag->instance->tag_lookup_mutex) {
        /*
         * The library is being torn down and the tag table has already been swept, so
         * publishing here would leave a tag that nothing will ever destroy.  See the
         * comment on tag_registry_t.shutting_down.
         */
        if(tag->instance->shutting_down) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Library is shutting down, refusing to register the tag.");
            rc = PLCTAG_ERR_NOT_ALLOWED;
            break;
        }

        if(reuse_str) {
            plc_tag_p other = hashtable_get(tag->instance->reuse, reuse_hash(reuse_str));

            if(other && str_cmp(other->reuse_key, reuse_str) == 0 && (*existing = rc_inc(other))) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, other->tag_id, "Lost insert race, using the existing tag.");
                rc = PLCTAG_STATUS_OK;
                new_id = other->tag_id;
                break;
            }

            /* ponytail: two strings with one 64-bit hash; the second is not shared.  Never seen yet. */
            if(other) { reuse_str = NULL; }
        }

        /* only get this when we hold the mutex.  Every slot is tried once before giving up. */
        new_id = next_tag_id;
        rc = PLCTAG_ERR_NO_RESOURCES;

        for(int32_t attempts = 0; attempts < TAG_SLOT_COUNT && rc != PLCTAG_STATUS_OK; attempts++) {
            tag_slot_t *slot = NULL;

            new_id = tag_id_inc(new_id);
            slot = &tag_slots[new_id & TAG_SLOT_MASK];

            /* tag_id is set before the tag is visible, so a lookup that finds it reads the right id. */
            spin_block(&slot->lock) {
                if(!slot->tag) {
                    tag->tag_id = new_id;
                    slot->tag_id = new_id;
                    slot->tag = tag;
                    rc = PLCTAG_STATUS_OK;
                }
            }
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "All %d tag slots are in use!", TAG_SLOT_COUNT);
        } else if(reuse_str && (tag->reuse_key = str_dup(reuse_str))) {
            /* a tag that cannot be entered is still a working tag, only not shared. */
            if(hashtable_put(tag->instance->reuse, reuse_hash(reuse_str), tag) != PLCTAG_STATUS_OK) {
                mem_free(tag->reuse_key);
                tag->reuse_key = NULL;
            }
        }

        next_tag_id = new_id;
    }

    if(rc != PLCTAG_STATUS_OK) { new_id = rc; }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, 0, "Done.");

    return new_id;
}


#define INITIAL_TAG_TABLE_SIZE (201)
/* The library-scoped instance: the reuse map and the create/remove mutex. Allocated with rc_alloc() so its lifetime can be shared
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
 * either publishing it (tag_registry_publish(), once cip_conn_startup()/mb_init()
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


/* rc_alloc() destructor for tag_registry_t. */
static void tag_registry_destructor(void *arg) {
    tag_registry_p inst = (tag_registry_p)arg;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Starting.");

    if(inst->tag_lookup_mutex) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "About to destroy tag lookup mutex.");
        mutex_destroy(&inst->tag_lookup_mutex);
        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Tag lookup mutex destroyed.");
    }

    if(inst->reuse) { hashtable_destroy(inst->reuse); }

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

    /* rc_alloc() does not promise zeroed memory, so do not rely on it for the gate. */
    inst->shutting_down = false;

    if((inst->reuse = hashtable_create(INITIAL_TAG_TABLE_SIZE)) == NULL) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_ERROR, 0, "Unable to create tag reuse hashtable!");
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

    building_instance = inst;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/* Drops the last ("genesis") reference to the instance plc_tag_shutdown() staged
 * here after clearing current_instance. By this
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
    if(!registry) { return; }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Closing all tags.");

    /*
     * Shut the door before walking the table, so no create still in flight can publish a
     * tag into a slot this sweep has already passed.
     */
    critical_block(registry->tag_lookup_mutex) { registry->shutting_down = true; }

    for(int32_t i = 0; i < TAG_SLOT_COUNT; i++) {
        plc_tag_p tag = NULL;

        critical_block(registry->tag_lookup_mutex) {
            /*
             * Take it out of the table in the same critical section that found it.  The
             * remover owns DESTROYED, and a create still running checks with
             * remove_tag_lookup() whether shutdown got here first.  The slot's reference
             * becomes ours; this returns NULL if the tag's count already reached zero.
             */
            tag = take_slot_unsafe(tag_slots[i].tag_id, NULL);

            if(tag) {
                forget_reusable_unsafe(registry, tag);
                tag = rc_inc(tag);
            }
        }

        /* do this outside the mutex. */
        if(tag) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Destroying tag %" PRId32 ".", tag->tag_id);

            /*
             * Run the common destroy tail here, using the registry we already hold, rather
             * than going through the public plc_tag_destroy(): its tag_registry_acquire()
             * would correctly (and unhelpfully) fail, since the gate is deliberately already
             * closed to stop any *new* tag creation.
             */
            destroy_tag_common(tag);

            pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
            rc_dec(tag);
        }
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "All tags closed.");
}


/* Remove a tag from the registry.  The caller still holds its own reference. */
bool remove_tag_lookup(plc_tag_p tag) {
    plc_tag_p removed = NULL;

    if(!tag || !tag->instance) { return false; }

    critical_block(tag->instance->tag_lookup_mutex) {
        removed = take_slot_unsafe(tag->tag_id, tag);
        if(removed) { forget_reusable_unsafe(tag->instance, tag); }
    }

    return removed == tag;
}


/* Remove a tag by id and return it, or NULL if no such tag is registered. */
plc_tag_p remove_tag_lookup_by_id(tag_registry_p registry, int32_t tag_id) {
    plc_tag_p tag = NULL;

    if(!registry) { return NULL; }

    critical_block(registry->tag_lookup_mutex) {
        tag = take_slot_unsafe(tag_id, NULL);
        if(tag) { forget_reusable_unsafe(registry, tag); }
    }

    return tag;
}
