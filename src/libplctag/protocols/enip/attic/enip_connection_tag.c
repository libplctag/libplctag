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
 * ENIP @connection Tag Implementation
 *
 * STATUS: MOSTLY CORRECT through Phase 5.  Phase 0 and Phase 6 changes needed.
 *
 * This file implements the vtable for the @connection diagnostic tag — the tag
 * returned when the user creates a tag with protocol=enip and a @connection path.
 * It surfaces per-connection metrics (queue_depth, latency) as readable attributes.
 *
 * Phase 0: change all int return types to int32_t (plan §0 coding standards).
 *          change DEBUG_MODULE_LIB to DEBUG_MODULE_ENIP in pdebug calls.
 *
 * Phase 6 (Engine): the tickler and get_int_attrib need access to the owning
 *   enip_connection_t.  Add a back-pointer field to enip_connection_tag_t (or
 *   look it up via a global connection list).  Then:
 *   - tickler: read conn->last_callback_latency_ms / conn->messages_sent etc.
 *     and store into tag->callback_latency_last_ms and tag->queue_depth.
 *   - get_int_attrib: add "state" attribute returning the DISCONNECTED/OPENING/READY enum.
 */

#include <libplctag/protocols/enip/client/enip.h>
#include <libplctag/protocols/enip/tag.h>
#include <utils/debug.h>
#include <utils/rc.h>


/* Phase 0: change return type from int to int32_t.  No functional change. */
static int32_t enip_connection_abort(plc_tag_p p_tag) {
    (void)p_tag;
    return PLCTAG_STATUS_OK;
}


/* Phase 0: change return type from int to int32_t.  No functional change. */
static int32_t enip_connection_status(plc_tag_p p_tag) {
    enip_connection_tag_t *tag = (enip_connection_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    return tag->status;
}


/* Phase 0: change return type from int to int32_t.
 * Phase 6: read latency/queue_depth from the owning enip_connection_t and store
 *          into tag->callback_latency_last_ms, tag->callback_latency_max_ms,
 *          and tag->queue_depth so callers can plc_tag_get_int_attrib them. */
static int32_t enip_connection_tickler(plc_tag_p p_tag) {
    enip_connection_tag_t *tag = (enip_connection_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    return PLCTAG_STATUS_OK;
}


/* Phase 0: change return type from int to int32_t.
 * Phase 6: add "state" attribute returning DISCONNECTED(0)/OPENING(1)/READY(2)
 *          by reading conn->state from the owning enip_connection_t. */
static int32_t enip_connection_get_int_attrib(plc_tag_p p_tag, const char *attrib_name, int default_value) {
    enip_connection_tag_t *tag = (enip_connection_tag_t *)p_tag;

    if(!tag || !attrib_name) { return default_value; }

    if(str_cmp_i(attrib_name, "queue_depth") == 0) { return tag->queue_depth; }

    if(str_cmp_i(attrib_name, "callback_latency_last_ms") == 0) { return tag->callback_latency_last_ms; }

    if(str_cmp_i(attrib_name, "callback_latency_max_ms") == 0) { return tag->callback_latency_max_ms; }

    return default_value;
}


/* Phase 0: change DEBUG_MODULE_LIB to DEBUG_MODULE_ENIP. No other changes. */
static void enip_connection_tag_destructor(void *ptr) {
    enip_connection_tag_t *tag = (enip_connection_tag_t *)ptr;

    if(!tag) { return; }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, tag->tag_id, "ENIP connection tag destructor.");
}


/* Phase 6: add set_int_attrib entry if needed for write-back; otherwise keep as-is. */
static struct tag_vtable_t enip_connection_tag_vtable = {
    .abort = enip_connection_abort,
    .status = enip_connection_status,
    .tickler = enip_connection_tickler,
    .get_int_attrib = enip_connection_get_int_attrib,
};


/* Phase 0: correct as-is.
 * Phase 6: after plc_tag_generic_init_tag, look up or create the owning
 *          enip_connection_t and store a back-pointer in the tag so tickler
 *          can read live metrics. */
plc_tag_p enip_connection_tag_create(attr attribs,
                                     void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                     void *userdata, plc_tag_p src_tag) {
    enip_connection_tag_t *tag = (enip_connection_tag_t *)rc_alloc(sizeof(enip_connection_tag_t), enip_connection_tag_destructor);
    int rc;

    (void)src_tag;

    if(!tag) { return NULL; }

    tag->vtable = &enip_connection_tag_vtable;
    tag->protocol_type = TAG_PROTOCOL_ENIP_CONNECTION;

    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        tag->status = (int8_t)rc;
        rc_dec(tag);
        return NULL;
    }

    tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, PLCTAG_STATUS_OK);

    return (plc_tag_p)tag;
}
