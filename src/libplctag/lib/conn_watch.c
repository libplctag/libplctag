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

/* Shared connection-event ring.  See conn_watch.h for the concurrency contract. */

#include <libplctag/lib/conn_watch.h>
#include <libplctag/lib/tag.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>


void conn_watch_init(conn_watch_t *watch, int32_t initial_status) {
    mem_set(watch, 0, (int)sizeof(*watch));

    atomic_init_int32(&watch->status, initial_status);

    /*
     * Seed slot 0 with the current state rather than leaving the ring empty.
     * A connection tag created after this point starts its read index at the
     * write index, so it never replays this entry; one created before the
     * handler thread publishes anything still has a real state to read.
     */
    watch->ring[0].event_type = initial_status + PLCTAG_EVENT_CONN_STATUS_OFFSET;
    watch->ring[0].status = PLCTAG_STATUS_OK;
    atomic_init_int32(&watch->ring_write_idx, 0);
}


void conn_watch_publish(conn_watch_t *watch, int32_t event_type, int32_t status) {
    int32_t write_idx = atomic_get_int32(&watch->ring_write_idx);

    /* drop a repeat of the last entry */
    if(watch->ring[write_idx].event_type == event_type && watch->ring[write_idx].status == status) { return; }

    write_idx = (write_idx + 1) & CONN_EVENT_RING_MASK;

    /* fill the slot before publishing its index */
    watch->ring[write_idx].event_type = event_type;
    watch->ring[write_idx].status = status;

    /* the atomic store is the release point; readers cannot see the slot before it */
    atomic_set_int32(&watch->ring_write_idx, write_idx);

    plc_tag_tickler_wake();
}


bool conn_watch_next(conn_watch_t *watch, int32_t *read_idx, int32_t *event_type, int32_t *status) {
    /* the atomic load is the acquire point, pairing with the store in conn_watch_publish() */
    int32_t write_idx = atomic_get_int32(&watch->ring_write_idx);

    if(*read_idx == write_idx) { return false; }

    *read_idx = (*read_idx + 1) & CONN_EVENT_RING_MASK;
    *event_type = watch->ring[*read_idx].event_type;
    *status = watch->ring[*read_idx].status;

    return true;
}


int32_t conn_watch_read_idx(conn_watch_t *watch) { return atomic_get_int32(&watch->ring_write_idx); }
