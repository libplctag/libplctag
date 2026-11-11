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
#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/lib/tag_driver.h>
#include <libplctag/lib/tag_registry.h>
#include <libplctag/lib/tag_schedule.h>
#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>
#include <utils/rc.h>
#include <utils/vector.h>


/*
 * most tags one tickle pass looks at, so a pass holds the connection mutex for a bounded time.
 * Above one packet's worth (about 180 small reads), so that auto-sync packets can fill.
 */
#define TAG_DRIVER_TICKLE_BATCH (512)

/*
 * A tickle pass stops after this many queued requests: about one packet's worth.  Under
 * overload the front of the list is a backlog of them, and starting more auto-sync ops behind
 * a full packet only queues them later.
 */
#define TAG_DRIVER_TICKLE_QUEUED (200)

/* when an idle tag, with nothing due, is looked at again. */
#define TAG_DRIVER_IDLE_OFFSET_MS (60 * 60 * 1000)

/* how soon a create that its kind's tickler finishes (@raw, a deferred CIP tag) is looked at again. */
#define TAG_DRIVER_CREATE_RECHECK_MS (100)

static void tickle_tags_unsafe(tag_driver_t *driver, plc_tag_p *tags, int32_t count, int64_t now);
static void tickle_tag(plc_tag_p tag, int64_t now);
static void arm_unsafe(tag_driver_t *driver, plc_tag_p tag, int64_t now);
static void notify_later_unsafe(tag_driver_t *driver, plc_tag_p tag);
static bool needs_no_connection(plc_tag_p tag);


int32_t tag_driver_init(tag_driver_t *driver) {
    driver->active = (tag_list_t){0};
    driver->notify = vector_create(10, 10);

    if(!driver->notify) {
        tag_driver_destroy(driver);
        return PLCTAG_ERR_NO_MEM;
    }

    return PLCTAG_STATUS_OK;
}


void tag_driver_destroy(tag_driver_t *driver) {
    if(driver->notify) {
        vector_destroy(driver->notify);
        driver->notify = NULL;
    }
}


void tag_driver_queue_unsafe(tag_driver_t *driver, plc_tag_p tag, int64_t when) {
    tag_schedule_insert(&driver->active, tag, when);
}


void tag_driver_forget_unsafe(tag_driver_t *driver, plc_tag_p tag) { tag_schedule_remove(&driver->active, tag); }


/* a due destroy or idle re-arm needs no connection, so it does not count. */
bool tag_driver_has_due_unsafe(tag_driver_t *driver, int64_t now) {
    for(plc_tag_p tag = tag_schedule_first(&driver->active); tag && tag->op_time <= now; tag = tag_schedule_next(tag)) {
        if(!needs_no_connection(tag)) { return true; }
    }

    return false;
}


/* a tag on the wire waits for its reply, not a time; there are at most a packet's worth. */
int64_t tag_driver_next_time_unsafe(tag_driver_t *driver) {
    plc_tag_p first = tag_schedule_first(&driver->active);

    while(first && (tag_op_state(first) == TAG_OP_READ_RESPONSE || tag_op_state(first) == TAG_OP_WRITE_RESPONSE)) {
        first = tag_schedule_next(first);
    }

    return first ? first->op_time : 0;
}


/*
 * Stops after a batch of queued requests as well.  No packet holds more, and a due tag behind
 * them would only queue behind them, so walking a long backlog on every packet buys nothing.
 * The build walk that follows stops at a full packet too.
 */
void tag_driver_tickle_due_unsafe(tag_driver_t *driver, int64_t now) {
    plc_tag_p due[TAG_DRIVER_TICKLE_BATCH];
    int32_t count = 0;
    int32_t queued = 0;

    for(plc_tag_p tag = tag_schedule_first(&driver->active);
        tag && tag->op_time <= now && count < TAG_DRIVER_TICKLE_BATCH && queued < TAG_DRIVER_TICKLE_QUEUED;
        tag = tag_schedule_next(tag)) {
        if(!tag_op_busy(tag)) {
            due[count++] = tag;
        } else {
            queued++;
        }
    }

    tickle_tags_unsafe(driver, due, count, now);
}


void tag_driver_expire_due_unsafe(tag_driver_t *driver, int64_t now) {
    plc_tag_p due[TAG_DRIVER_TICKLE_BATCH];
    int32_t count = 0;

    for(plc_tag_p tag = tag_schedule_first(&driver->active); tag && tag->op_time <= now && count < TAG_DRIVER_TICKLE_BATCH;
        tag = tag_schedule_next(tag)) {
        if(needs_no_connection(tag)) { due[count++] = tag; }
    }

    tickle_tags_unsafe(driver, due, count, now);
}


void tag_driver_tickle_creating_unsafe(tag_driver_t *driver) {
    plc_tag_p waiting[TAG_DRIVER_TICKLE_BATCH];
    int32_t count = 0;

    for(plc_tag_p tag = tag_schedule_first(&driver->active); tag && count < TAG_DRIVER_TICKLE_BATCH; tag = tag_schedule_next(tag)) {
        if(!tag->created && !tag_op_busy(tag)) { waiting[count++] = tag; }
    }

    tickle_tags_unsafe(driver, waiting, count, time_ms());
}


void tag_driver_done_unsafe(tag_driver_t *driver, plc_tag_p tag, int64_t now) {
    arm_unsafe(driver, tag, now);
    if(tag->event_count > 0) { notify_later_unsafe(driver, tag); }
}


/* a created tag that is IDLE or DESTROY: its tickle only re-arms or destroys it. */
static bool needs_no_connection(plc_tag_p tag) {
    tag_op_state_t state = tag_op_state(tag);

    return tag->created && (state == TAG_OP_IDLE || state == TAG_OP_DESTROY);
}


/*
 * Only the handler thread adds to or drains the list.  Dropping the reference can be the tag's
 * last; its destructor then runs on the rc cleanup thread.
 */
void tag_driver_notify(tag_driver_t *driver) {
    /* by index, then emptied once: removing from the front would shift the rest every time. */
    for(int32_t i = 0; i < vector_length(driver->notify); i++) {
        plc_tag_p tag = vector_get(driver->notify, i);

        plc_tag_generic_handle_event_callbacks(tag);

        /* the tickle took it out of the registry; the registry's reference is ours to drop. */
        if(tag->auto_destroyed) {
            tag->auto_destroyed = false;
            destroy_tag_common(tag);
        }

        rc_dec(tag);
    }

    vector_reset(driver->notify);
}


/* a tag whose API mutex is busy stays on the list for next time. */
static void tickle_tags_unsafe(tag_driver_t *driver, plc_tag_p *tags, int32_t count, int64_t now) {
    for(int32_t i = 0; i < count; i++) {
        plc_tag_p tag = tags[i];

        if(mutex_try_lock(&tag->api_mutex) != PLCTAG_STATUS_OK) { continue; }

        tickle_tag(tag, now);

        if(tag->vtable->tickler) { tag->vtable->tickler(tag); }

        if(!tag_op_busy(tag)) { arm_unsafe(driver, tag, now); }

        if(tag->event_count > 0 || tag->auto_destroyed) { notify_later_unsafe(driver, tag); }

        mutex_unlock(&tag->api_mutex);
    }
}


/* an abort request, then automatic writes and reads.  Called with the tag's API mutex held. */
static void tickle_tag(plc_tag_p tag, int64_t now) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Tickling tag %d.", tag->tag_id);

    if(atomic_get_bool(&tag->abort_requested)) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Aborting ongoing operation if any!");

        tag_op_abort(tag);
        if(tag->vtable && tag->vtable->abort) { tag->vtable->abort(tag); }

        atomic_set_bool(&tag->abort_requested, false);

        return;
    }

    /* a DESTROY tag is only tickled at its due time: a wake makes it IDLE (tag_driver_queue_unsafe()). */
    if(tag_op_state(tag) == TAG_OP_DESTROY) {
        tag->auto_destroyed = remove_tag_lookup(tag);

        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Idle for %" PRId32 " s, %s.", tag->destroy_idle_sec,
               tag->auto_destroyed ? "destroying it" : "already being destroyed");

        return;
    }

    /* a dirty tag writes auto_sync_write_ms after it was first written to, ahead of any read. */
    if(tag->auto_sync_write_ms > 0 && tag->tag_is_dirty) {
        if(tag_op_is_read(tag)) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Aborting in-flight automatic read!");

            tag_op_abort(tag);
            if(tag->vtable && tag->vtable->abort) { tag->vtable->abort(tag); }
        }

        if(!tag->auto_sync_next_write) {
            tag->auto_sync_next_write = now + tag->auto_sync_write_ms;

            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Queueing up automatic write in %dms.", tag->auto_sync_write_ms);
        } else if(!tag_op_busy(tag) && tag->auto_sync_next_write <= now) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Triggering automatic write start.");

            tag->tag_is_dirty = 0;
            tag->auto_sync_next_write = 0;

            tag_op_start(tag, false);

            rc = (tag->vtable && tag->vtable->write) ? tag->vtable->write(tag) : PLCTAG_ERR_NOT_IMPLEMENTED;
            if(rc != PLCTAG_STATUS_PENDING) { tag_op_end(tag, rc); }
        }
    }

    if(tag->auto_sync_read_ms > 0) {
        if(tag->auto_sync_next_read < now) {
            if(!tag_op_busy(tag) && !tag->tag_is_dirty) {
                int64_t periods = 0;

                pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Triggering automatic read start.");

                tag_op_start(tag, true);

                rc = (tag->vtable && tag->vtable->read) ? tag->vtable->read(tag) : PLCTAG_ERR_NOT_IMPLEMENTED;
                if(rc != PLCTAG_STATUS_PENDING) { tag_op_end(tag, rc); }

                /*
                 * Skip to the next read time that is a whole multiple of the period, so that
                 * jitter does not slowly move the polling cycle.
                 */
                periods = (now - tag->auto_sync_next_read + (tag->auto_sync_read_ms - 1)) / tag->auto_sync_read_ms;

                if(periods > 1) {
                    pdebug(DEBUG_MODULE_LIB, DEBUG_WARN | DEBUG_RATE_LIMIT, tag->tag_id,
                           "Skipping %" PRId64 " periods of %" PRId32 "ms.", periods, tag->auto_sync_read_ms);
                }

                tag->auto_sync_next_read += (periods * tag->auto_sync_read_ms);

                pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Scheduling next read at time %" PRId64 ".",
                       tag->auto_sync_next_read);
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, tag->tag_id, "Unable to start auto read, op state %d, tag_is_dirty=%d!",
                       (int)tag_op_state(tag), tag->tag_is_dirty);
            }
        }
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Done.");
}


/*
 * Put a tag with no op in flight back on the list, in the state and at the time of what it
 * waits for next: SYNC_WAIT at its next auto-sync read or write, DESTROY at its idle destroy
 * time, otherwise IDLE an hour out.  A tag still being created is looked at again soon.  Only
 * an auto-destroyed tag comes off; its destroy follows in tag_driver_notify().
 *
 * tickle_tag() starts the op when the deadline comes, and it only starts a read whose time is
 * strictly past, so a deadline already past is set just after now rather than at it.
 */
static void arm_unsafe(tag_driver_t *driver, plc_tag_p tag, int64_t now) {
    tag_op_state_t state = TAG_OP_IDLE;
    int64_t when = 0;

    if(tag->auto_destroyed) {
        tag_schedule_remove(&driver->active, tag);
        return;
    }

    /* a dirty tag reads after its write. */
    if(tag->auto_sync_read_ms > 0 && !tag->tag_is_dirty) {
        when = (tag->auto_sync_next_read > now) ? tag->auto_sync_next_read : now + 1;
    }

    if(tag->auto_sync_write_ms > 0 && tag->tag_is_dirty) {
        int64_t write_time = (tag->auto_sync_next_write > now) ? tag->auto_sync_next_write : now + 1;

        if(!when || write_time < when) { when = write_time; }
    }

    if(when) { state = TAG_OP_SYNC_WAIT; }

    if(!tag->created && tag->vtable->tickler) {
        int64_t create_time = now + TAG_DRIVER_CREATE_RECHECK_MS;

        if(!when || create_time < when) { when = create_time; }
    }

    /* exclusive with auto-sync, so nothing else is due. */
    if(tag->destroy_idle_sec > 0 && tag->created) {
        state = TAG_OP_DESTROY;
        when = now + (int64_t)tag->destroy_idle_sec * 1000;
    }

    if(!when) { when = now + TAG_DRIVER_IDLE_OFFSET_MS; }

    tag_op_set(tag, state);
    tag_schedule_insert(&driver->active, tag, when);
}


/*
 * Callbacks run inside the tag's API mutex, which the handler may not wait for while it holds
 * its connection mutex.  So a tag with events is remembered here, with a reference that keeps
 * it alive, and tag_driver_notify() runs its callbacks later.  The tag is on the handler's
 * list, so its reference count cannot have reached zero unless its destructor is already
 * waiting on the connection mutex, and then rc_inc() refuses.
 */
static void notify_later_unsafe(tag_driver_t *driver, plc_tag_p tag) {
    plc_tag_p held = rc_inc(tag);

    if(held) { vector_set(driver->notify, vector_length(driver->notify), held); }
}
