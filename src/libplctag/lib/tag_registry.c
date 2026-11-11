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
#include <libplctag/lib/tag_registry.h>
#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <platform.h>
#include <utils/debug.h>
#include <utils/atomic_utils.h>
#include <utils/hash.h>
#include <utils/hashtable.h>
#include <utils/rc.h>


/*
 * The tag registry: a static table of entries, one per possible slot, and the reuse map.
 *
 * A tag lives at tag_table[tag_id % PLCTAG_MAX_TAGS].  Each entry has its own spinlock,
 * so a lookup writes nothing but that one lock: threads contend only when they use the same
 * tag at the same time.  The table is BSS, so it is valid memory before startup, after
 * shutdown and across restarts; nothing is allocated or freed for it.
 *
 * A lookup must take the entry lock, not just look at it.  A remover clears the entry under
 * the lock and only then drops the entry's reference, so a reader holding the lock either
 * takes its own reference first or finds the entry empty.
 */

/* any size up to TAG_ID_MASK.  Override for small targets. */
#ifndef PLCTAG_MAX_TAGS
#    define PLCTAG_MAX_TAGS (262144)
#endif

#define TAG_SLOT(tag_id) ((size_t)((uint32_t)(tag_id) % (uint32_t)PLCTAG_MAX_TAGS))

typedef struct {
    lock_t lock;    /* guards tag_id and tag */
    int32_t tag_id; /* == tag->tag_id while tag is set */
    plc_tag_p tag;  /* holds the registry's reference; NULL when free */
} tag_entry_t;

static tag_entry_t tag_table[PLCTAG_MAX_TAGS];

/*
 * Never reset, so ids are not reused across a shutdown/restart cycle: a stale handle held by
 * the application must not resolve to an unrelated tag.
 */
static atomic_int32_t next_tag_id = 10; /* MAGIC */

/*
 * True between tag_registry_publish() and tag_registry_close().  A create checks it under the
 * entry lock it publishes into, and the sweep clears it before it takes any entry lock.  For
 * any one entry, either the create's hold comes first and the sweep finds the tag, or the
 * sweep's comes first, the flag is already clear, and the create is refused.
 */
static atomic_bool registry_open = ATOMIC_BOOL_STATIC_INIT;

/* hash64() of the attribute string -> tag, for tags created with find_or_create_tag=1. */
static lock_t reuse_lock = LOCK_INIT;
static hashtable_p reuse = NULL;

atomic_bool lib_active = ATOMIC_BOOL_STATIC_INIT;


static int32_t tag_id_inc(void);
static int64_t reuse_hash(const char *reuse_str);
static void forget_reusable(plc_tag_p tag);
static plc_tag_p take_tag(int32_t tag_id, plc_tag_p tag);
static int32_t publish_tag(plc_tag_p tag);


plc_tag_p lookup_tag(int32_t tag_id) {
    tag_entry_t *entry = &tag_table[TAG_SLOT(tag_id)];
    plc_tag_p tag = NULL;

    if(tag_id <= 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Tag ID %" PRId32 " is not valid.", tag_id);
        return NULL;
    }

    spin_block(&entry->lock) {
        if(entry->tag && entry->tag_id == tag_id) { tag = rc_inc(entry->tag); }
    }

    if(!tag) { pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Tag with ID %" PRId32 " not found.", tag_id); }

    return tag;
}


/* The next id: 31 bits, never 0. */
static int32_t tag_id_inc(void) {
    int32_t old_id = 0;
    int32_t new_id = 0;

    do {
        old_id = atomic_get_int32(&next_tag_id);
        new_id = (int32_t)(((uint32_t)old_id + 1) & TAG_ID_MASK);

        /* plc_tag_create() returns an id or a negative error, and 0 would be neither. */
        if(new_id == 0) { new_id = 1; }
    } while(atomic_compare_and_set_int32(&next_tag_id, old_id, new_id) != old_id);

    return new_id;
}


/* The hashtable treats key 0 as an empty slot. */
static int64_t reuse_hash(const char *reuse_str) {
    int64_t key = (int64_t)hash64((const uint8_t *)reuse_str, (size_t)(unsigned int)str_length(reuse_str));

    return (key ? key : 1);
}


/* Empty the entry for tag_id and return what was in it.  With tag set, only if it holds that tag. */
static plc_tag_p take_tag(int32_t tag_id, plc_tag_p tag) {
    tag_entry_t *entry = &tag_table[TAG_SLOT(tag_id)];
    plc_tag_p found = NULL;

    if(tag_id <= 0) { return NULL; }

    spin_block(&entry->lock) {
        if(entry->tag && entry->tag_id == tag_id && (!tag || entry->tag == tag)) {
            found = entry->tag;
            entry->tag = NULL;
        }
    }

    if(found) { forget_reusable(found); }

    return found;
}


/* Take the tag out of the reuse map, if it is there. */
static void forget_reusable(plc_tag_p tag) {
    spin_block(&reuse_lock) {
        if(tag->reuse_key) {
            if(reuse) { hashtable_remove(reuse, reuse_hash(tag->reuse_key)); }
            mem_free(tag->reuse_key);
            tag->reuse_key = NULL;
        }
    }
}


/* A new reference to the reusable tag created with exactly this string, or NULL. */
plc_tag_p tag_registry_find_reusable(const char *reuse_str) {
    plc_tag_p tag = NULL;

    spin_block(&reuse_lock) {
        tag = (reuse ? hashtable_get(reuse, reuse_hash(reuse_str)) : NULL);
        tag = ((tag && str_cmp(tag->reuse_key, reuse_str) == 0) ? rc_inc(tag) : NULL);
    }

    return tag;
}


/* Stop handing out this tag to creates.  Holders keep it. */
void tag_registry_forget_reusable(plc_tag_p tag) { forget_reusable(tag); }


/* Give the tag a fresh id whose entry is free, and put it there.  Returns the id or an error. */
static int32_t publish_tag(plc_tag_p tag) {
    int32_t rc = PLCTAG_ERR_NO_RESOURCES;

    for(int32_t attempts = 0; attempts < PLCTAG_MAX_TAGS && rc == PLCTAG_ERR_NO_RESOURCES; attempts++) {
        int32_t id = tag_id_inc();
        tag_entry_t *entry = &tag_table[TAG_SLOT(id)];

        spin_block(&entry->lock) {
            if(!atomic_get_bool(&registry_open)) {
                rc = PLCTAG_ERR_NOT_ALLOWED;
            } else if(!entry->tag) {
                /* tag->tag_id is set before the unlock, so a reader that takes the lock sees it. */
                tag->tag_id = id;
                entry->tag_id = id;
                entry->tag = tag;
                rc = id;
            }
        }
    }

    return rc;
}


/*
 * Publish the tag.  With reuse_str, also enter it in the reuse map, holding reuse_lock
 * throughout so two creates with one string cannot both publish.  If another create already
 * entered the string, publish nothing: *existing gets a reference to that tag and its id is
 * returned.
 */
int add_tag_lookup(plc_tag_p tag, const char *reuse_str, plc_tag_p *existing) {
    int32_t rc = PLCTAG_STATUS_OK;

    *existing = NULL;

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, 0, "Starting.");

    if(!reuse_str) {
        rc = publish_tag(tag);
    } else {
        /* copied before the spinlock, so no other reusing create spins through an allocation (U4). */
        char *reuse_key = str_dup(reuse_str);

        spin_block(&reuse_lock) {
            plc_tag_p other = (reuse ? hashtable_get(reuse, reuse_hash(reuse_str)) : NULL);

            if(other && str_cmp(other->reuse_key, reuse_str) == 0 && (*existing = rc_inc(other))) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, other->tag_id, "Lost insert race, using the existing tag.");
                rc = other->tag_id;
                break;
            }

            /* ponytail: publish_tag() probes long only when the table is nearly full. */
            rc = publish_tag(tag);

            /*
             * ponytail: two strings with one 64-bit hash; the second is not shared.  Never seen yet.
             * A tag that cannot be entered is still a working tag, only not shared.
             * ponytail: hashtable_put() can grow the table under the spinlock; measure before moving it.
             */
            if(rc > 0 && !other && reuse && reuse_key && hashtable_put(reuse, reuse_hash(reuse_str), tag) == PLCTAG_STATUS_OK) {
                tag->reuse_key = reuse_key;
                reuse_key = NULL;
            }
        }

        if(reuse_key) { mem_free(reuse_key); }
    }

    if(rc == PLCTAG_ERR_NOT_ALLOWED) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Library is not running, refusing to register the tag.");
    } else if(rc == PLCTAG_ERR_NO_RESOURCES) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "All %d tag slots are in use!", PLCTAG_MAX_TAGS);
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


int tag_registry_init(void) {
    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Starting.");

    spin_block(&reuse_lock) {
        if(!reuse) { reuse = hashtable_create(1024); /* MAGIC */ }
    }

    if(!reuse) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_ERROR, 0, "Unable to create tag reuse hashtable!");
        return PLCTAG_ERR_NO_MEM;
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * Open the registry to creates.  initialize_modules() calls this once every module has
 * started, so a shutdown racing startup finds nothing to close.
 */
void tag_registry_publish(void) {
    atomic_set_bool(&registry_open, true);
    atomic_set_bool(&lib_active, true);
}


bool tag_registry_close(void) { return atomic_compare_and_set_bool(&registry_open, true, false) == true; }


/* Free the reuse map.  Tags still alive forget their keys against NULL, which does nothing. */
void tag_registry_teardown(void) {
    hashtable_p old = NULL;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Tearing down tag registry.");

    atomic_set_bool(&registry_open, false);

    spin_block(&reuse_lock) {
        old = reuse;
        reuse = NULL;
    }

    if(old) { hashtable_destroy(old); }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Done.");
}


/*
 * Destroy every tag still in the registry.
 *
 * Called from plc_tag_shutdown() after tag_registry_close(), so no create can publish into an
 * entry this sweep has already passed.  Tags are destroyed outside the entry lock: the
 * destructor runs protocol teardown, which takes other locks.
 */
void tag_registry_destroy_all_tags(void) {
    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "Closing all tags.");

    for(size_t i = 0; i < PLCTAG_MAX_TAGS; i++) {
        tag_entry_t *entry = &tag_table[i];
        plc_tag_p tag = NULL;

        /*
         * Take it out in the same critical section that found it.  The remover owns DESTROYED,
         * and a create still running checks with remove_tag_lookup() whether shutdown got
         * here first.  The entry's reference becomes ours, and destroy_tag_common() drops it.
         * The extra reference keeps the tag alive for the log line after that.
         */
        spin_block(&entry->lock) {
            tag = (entry->tag ? rc_inc(entry->tag) : NULL);
            entry->tag = NULL;
        }

        if(tag) {
            forget_reusable(tag);

            pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Destroying tag %" PRId32 ".", tag->tag_id);

            destroy_tag_common(tag);

            pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
            rc_dec(tag);
        }
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "All tags closed.");
}


/* Remove a tag from the registry.  The caller still holds its own reference. */
bool remove_tag_lookup(plc_tag_p tag) { return tag && take_tag(tag->tag_id, tag) == tag; }


/* Remove a tag by id and return it, or NULL if no such tag is registered. */
plc_tag_p remove_tag_lookup_by_id(int32_t tag_id) { return take_tag(tag_id, NULL); }
