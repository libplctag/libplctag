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
#include <libplctag/lib/lib_internal.h>
#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/lib/tag_registry.h>
#include <platform.h>
#include <utils/debug.h>
#include <utils/hashtable.h>
#include <utils/rc.h>

/* next_tag_id deliberately lives outside lib_instance_t and is never reset by
 * teardown: ids must not be reused across a shutdown/restart cycle, or a stale
 * handle held by the application would resolve to an unrelated tag. */
static volatile int32_t next_tag_id = 10; /* MAGIC */

/* Bound on the search for a free id, so a saturated table fails instead of spinning. */
#define MAX_TAG_MAP_ATTEMPTS (50)

static int tag_id_inc(int id);

plc_tag_p lookup_tag(int32_t tag_id) {
    plc_tag_p tag = NULL;
    lib_instance_p inst = lib_instance_acquire();

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
