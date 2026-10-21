#pragma once

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

/*
 * The tag side of a connection handler, the same for every protocol.
 *
 * A handler keeps its own connection state machine and wire format.  What it does with tags
 * once the connection can carry requests is here: the list of tags that need it, sorted by
 * when; starting auto-sync reads and writes and acting on aborts; putting a tag whose op ended
 * back on the list for its next deadline; and running callbacks once the handler has let go of
 * its connection mutex.  The handler calls these functions from its own states; nothing here
 * calls back into it.
 *
 * The functions named _unsafe are called with the handler's connection mutex held, which
 * guards the list.  They only try each tag's API mutex, so a tag an application thread holds
 * is skipped and stays due; that thread wakes the handler when it lets go
 * (tag_driver_api_released()).  tag_driver_notify() is called with the connection mutex
 * released.
 */

#include <libplctag/lib/tag.h>
#include <stdbool.h>
#include <stdint.h>
#include <utils/vector.h>


typedef struct {
    /*
     * Tags with an op or a deadline, sorted by op_time (lib/tag_schedule.h).  Raw pointers: a
     * tag takes itself off before it is freed (tag_driver_forget_unsafe()).
     */
    vector_p active;

    /* Tags owing callbacks, each holding a reference.  Only the handler thread touches it. */
    vector_p notify;
} tag_driver_t;


extern int32_t tag_driver_init(tag_driver_t *driver);
extern void tag_driver_destroy(tag_driver_t *driver);

/* put the tag on the list at when, or move it there; take it off. */
extern void tag_driver_queue_unsafe(tag_driver_t *driver, plc_tag_p tag, int64_t when);
extern void tag_driver_forget_unsafe(tag_driver_t *driver, plc_tag_p tag);

/* is a tag due now?  When is the first one due, 0 for none? */
extern bool tag_driver_has_due_unsafe(tag_driver_t *driver, int64_t now);
extern int64_t tag_driver_next_time_unsafe(tag_driver_t *driver);

/*
 * Run the library's housekeeping for the due tags with no op in flight: an abort request, an
 * auto-sync read or write whose time has come, and the tag kind's own tickler.  What it starts
 * stays on the list as an op; anything else is re-armed for its next deadline or taken off.
 */
extern void tag_driver_tickle_due_unsafe(tag_driver_t *driver, int64_t now);

/*
 * The same for the tags still waiting for their create, wherever they are on the list: for a
 * handler that cannot reach the state where it tickles due tags, whose answer they need.
 */
extern void tag_driver_tickle_creating_unsafe(tag_driver_t *driver);

/*
 * The tag's op has ended (tag_op_end()): put it back at its next auto-sync deadline, or take
 * it off, and run its callbacks at the next tag_driver_notify().  Called with the tag's API
 * mutex held too.
 */
extern void tag_driver_done_unsafe(tag_driver_t *driver, plc_tag_p tag, int64_t now);

/* run the callbacks for every tag that has events: connection mutex released, each tag's API mutex held. */
extern void tag_driver_notify(tag_driver_t *driver);

/*
 * The handler's try for a tag's API mutex.  On failure the tag stays marked, and the thread
 * holding the mutex wakes the handler as it lets go, in tag_driver_api_released(), rather than
 * the tag waiting for the handler's next tick.
 */
extern bool tag_driver_try_lock(plc_tag_p tag);

/* called by an API function after it lets go of the tag's API mutex: wake the handler if it is waiting for it. */
extern void tag_driver_api_released(plc_tag_p tag);

/*
 * A handler that must have a busy tag's API mutex retries with its own mutex released.  The
 * holder is nearly always an API call that lets go within microseconds, so the first retries
 * only yield; after these it sleeps 1ms between tries.
 */
#define TAG_DRIVER_LOCK_YIELDS (100) /* MAGIC */
