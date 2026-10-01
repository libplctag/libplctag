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
 * The part of a connection object that a connection-status tag observes.
 *
 * Embedded in cip_conn_t and modbus_plc_t so that one
 * connection-tag implementation can watch any of them.
 *
 * The ring is single-writer / multiple-reader: the connection's handler thread
 * is the only publisher, and each watching tag holds its own read index.
 */

#include <libplctag/lib/tag.h>
#include <stdbool.h>
#include <stdint.h>
#include <utils/atomic_utils.h>


#define CONN_EVENT_RING_SIZE (64)
#define CONN_EVENT_RING_MASK (CONN_EVENT_RING_SIZE - 1)


typedef struct {
    atomic_int32_t status; /* plc_tag_conn_status_t values */

    /*
     * ring_write_idx is the index of the last entry written, not the next free
     * slot.  A freshly initialized watch therefore holds one real entry at
     * index 0 describing the current state, so a tag created later can seed
     * itself from it.
     */
    tag_conn_event_t ring[CONN_EVENT_RING_SIZE];
    atomic_int32_t ring_write_idx;
} conn_event_ring_t;


/* seed the watch with its initial status.  Call before the handler thread starts. */
extern void conn_watch_init(conn_event_ring_t *watch, int32_t initial_status);

/*
 * Publish an event.  Single writer only: the connection's handler thread.
 * Repeating the last entry's event_type and status is dropped.
 */
extern void conn_watch_publish(conn_event_ring_t *watch, int32_t event_type, int32_t status);

/*
 * Consume the next event after *read_idx, advancing it.  Returns false when the
 * reader has caught up.  Each reader owns its own read_idx.
 */
extern bool conn_watch_next(conn_event_ring_t *watch, int32_t *read_idx, int32_t *event_type, int32_t *status);

/* index a newly created reader should start from to see only future events. */
extern int32_t conn_watch_read_idx(conn_event_ring_t *watch);
