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
 * The connection API used by enip_tag.c (design doc §14.2).
 *
 * enip_connection_t is opaque here; its full definition lives in
 * enip_session.c.  enip_tag_t is forward-declared so this header can be
 * included without enip_tag.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <utils/attr.h>

typedef struct enip_connection_t enip_connection_t;

typedef struct enip_tag_t enip_tag_t;
typedef enip_tag_t *enip_tag_p;

/* alloc + start the IO thread; returns a connection with one ref held by the
 * caller (no explicit destroy -- rc_dec drops the last ref). If is_new_out is
 * non-NULL it is set true when a fresh connection was created, false when an
 * existing one was reused (used by @connection tags to decide fresh vs late-join). */
extern enip_connection_t *enip_session_create(attr attribs, bool *is_new_out);

/* §13.3/13.6: under sched_mutex set op/op_time; if not already scheduled,
 * insert sorted; then socket_wake(c->sock). Returns PLCTAG_STATUS_PENDING.
 * Caller holds tag->api_mutex. */
extern int32_t enip_session_schedule(enip_connection_t *c, enip_tag_p tag, uint8_t op, int64_t op_time);

/* abort path (§8/§13.5): under sched_mutex, if scheduled && != in_flight unlink
 * and return OK; else if == in_flight set abort_requested; else OK (idle).
 * Caller holds tag->api_mutex. */
extern int32_t enip_session_unschedule(enip_connection_t *c, enip_tag_p tag);

/* tag destructor: under sched_mutex remove from list if linked. Never called
 * while the tag is in_flight (the rc pin prevents the destructor from running). */
extern void enip_session_tag_detach(enip_connection_t *c, enip_tag_p tag);

/* negotiated CIP payload capacity, for window math (§11.3). */
extern size_t enip_session_max_cip(enip_connection_t *c);

/* connection_status / connection_inactivity_timeout_ms tag attributes. */
extern int enip_session_get_status(enip_connection_t *c);
extern int enip_session_get_inactivity_timeout(enip_connection_t *c);
extern int enip_session_set_inactivity_timeout(enip_connection_t *c, int new_value);

/* connection-status event ring, drained by @connection tags. Current write
 * index is the late-join snapshot point; next_conn_status advances *read_idx by
 * one entry and returns its status, or false when caught up. */
extern int32_t enip_session_conn_status_idx(enip_connection_t *c);
extern bool enip_session_next_conn_status(enip_connection_t *c, int32_t *read_idx, int32_t *status_out);

/* Cached CIP Identity payload (raw Get_Attributes_All response), queried once
 * during bring-up. Returns false until that query completes. */
extern bool enip_session_get_identity(enip_connection_t *c, uint8_t **data_out, uint16_t *len_out);

/* registry lifecycle (called once from enip_init()/enip_teardown()). */
extern int32_t enip_session_module_init(void);
extern void enip_session_module_teardown(void);
