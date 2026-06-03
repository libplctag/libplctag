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

#include <libplctag/protocols/enip/enip.h>
#include <libplctag/protocols/enip/tag.h>
#include <libplctag/protocols/enip/enip_cip.h>
#include <utils/debug.h>
#include <utils/rc.h>
#include <utils/attr.h>
#include <string.h>


/*
 * ENIP Tag vtable and tag-creation implementation.
 *
 * STATUS: KEEP vtable functions as-is; enip_protocol_tag_create needs Phase 6 changes.
 *
 * Phase 0: Change DEBUG_MODULE_LIB to DEBUG_MODULE_ENIP in enip_tag_destructor.
 *          Change return types from int to int32_t on all vtable functions (plan §0).
 *
 * Phase 6: In enip_protocol_tag_create, add:
 *   - Extract byte_order from attribs ("@byte_order" attribute).
 *   - Allocate and zero tag->data (tag->size bytes) using mem_alloc.
 *   - Set tag->metadata_required = (plc=ControlLogix/CompactLogix) ? true : false.
 *
 * Phase 6: In enip_tag_read and enip_tag_write:
 *   - Update tag->op_time = time_ms() after setting op_state.
 *   - Signal conn->wake so the handler thread wakes immediately.
 *
 * Phase 6: In enip_tag_abort:
 *   - Also remove the tag from conn->pending_requests if it is there.
 */

/* Phase 0: change return type int -> int32_t; correct as-is otherwise. */
static int32_t enip_tag_abort(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    tag->op_state = ENIP_TAG_OP_ERROR;
    tag->read_in_flight = 0;
    tag->write_in_flight = 0;

    return PLCTAG_STATUS_OK;
}


/* Phase 0: change return type int -> int32_t.
 * Phase 6: add tag->op_time = time_ms(); signal conn->wake. */
static int32_t enip_tag_read(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    tag->op_state = ENIP_TAG_OP_REQUEST;
    tag->read_in_flight = 1;
    tag->read_complete = 0;

    return PLCTAG_STATUS_PENDING;
}


/* Phase 0: change return type int -> int32_t.  Correct as-is otherwise. */
static int32_t enip_tag_status(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    if(tag->status != PLCTAG_STATUS_OK) { return tag->status; }

    if(tag->read_in_flight || tag->write_in_flight) { return PLCTAG_STATUS_PENDING; }

    return PLCTAG_STATUS_OK;
}


/* Phase 0: change return type int -> int32_t.  Correct as-is otherwise. */
static int32_t enip_tag_tickler(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    return tag->status;
}


/* Phase 0: change return type int -> int32_t.
 * Phase 6: add tag->op_time = time_ms(); signal conn->wake. */
static int32_t enip_tag_write(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    tag->op_state = ENIP_TAG_OP_REQUEST;
    tag->write_in_flight = 1;
    tag->write_complete = 0;

    return PLCTAG_STATUS_PENDING;
}


/* Phase 6: implement by calling cond_signal(conn->wake) to wake the handler thread. */
static int32_t enip_tag_wake_plc(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    return PLCTAG_STATUS_PENDING;
}


/* Phase 0: change return type int -> int32_t.  Correct as-is otherwise. */
static int32_t enip_tag_data_written(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    return PLCTAG_STATUS_OK;
}


/* Phase 0: change DEBUG_MODULE_LIB to DEBUG_MODULE_ENIP.
 * Phase 6: also remove the tag from conn->active_tags and conn->pending_requests. */
static void enip_tag_destructor(void *ptr) {
    enip_tag_t *tag = (enip_tag_t *)ptr;

    if(!tag) { return; }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, tag->tag_id, "ENIP tag destructor.");
}


static struct tag_vtable_t enip_tag_vtable = {
    .abort = enip_tag_abort,
    .read = enip_tag_read,
    .status = enip_tag_status,
    .tickler = enip_tag_tickler,
    .write = enip_tag_write,
    .wake_plc = enip_tag_wake_plc,
    .tag_data_written = enip_tag_data_written,
};


/* Phase 2: correct tag-path encoding and struct layout.  Mostly correct as-is.
 * Phase 6: add three things after plc_tag_generic_init_tag:
 *   (1) allocate tag->data = mem_alloc(size) where size comes from attribs or defaults.
 *   (2) add a conn pointer field to enip_tag_t and store the result of
 *       find_or_create_connection (called from enip_tag_create, not here).
 *   (3) tag->op_time should be initialized to time_ms() so the scheduler can sort it. */
plc_tag_p enip_protocol_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                   void *userdata, plc_tag_p src_tag) {
    const char *tag_path_str = attr_get_str(attribs, "name", "");

    /* Encode tag path to temporary stack buffer first */
    uint8_t temp_encoded_path[256];
    size_t encoded_len = enip_cip_encode_tag_path(tag_path_str, temp_encoded_path, sizeof(temp_encoded_path));

    if(encoded_len == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to encode path for tag '%s'", tag_path_str);
        return NULL;
    }

    /* Extract first symbolic segment (root tag name) from tag path
     * Format: "TagName[index].member[index]..." → extract "TagName"
     */
    char tag_name_buf[256];
    size_t tag_name_len = 0;
    for(size_t i = 0; tag_path_str[i] && tag_path_str[i] != '[' && tag_path_str[i] != '.'; i++) {
        if(i < sizeof(tag_name_buf) - 1) {
            tag_name_buf[i] = tag_path_str[i];
            tag_name_len++;
        }
    }
    tag_name_buf[tag_name_len] = '\0';

    if(tag_name_len == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Invalid tag path '%s' - no root tag name", tag_path_str);
        return NULL;
    }

    /* Allocate tag structure plus space for:
     * 1. Encoded tag path
     * 2. Root tag name string (null-terminated)
     */
    size_t tag_name_alloc = tag_name_len + 1; /* +1 for null terminator */
    size_t total_size = sizeof(enip_tag_t) + encoded_len + tag_name_alloc;
    enip_tag_t *tag = (enip_tag_t *)rc_alloc(total_size, enip_tag_destructor);
    int rc;

    (void)src_tag;

    if(!tag) { return NULL; }

    /* Encoded path is stored right after the tag structure */
    uint8_t *encoded_path_buf = (uint8_t *)((char *)tag + sizeof(enip_tag_t));

    /* Copy encoded path from stack buffer to allocated space */
    memcpy(encoded_path_buf, temp_encoded_path, encoded_len);

    /* Tag name string is stored right after encoded path */
    char *tag_name_storage = (char *)(encoded_path_buf + encoded_len);
    memcpy(tag_name_storage, tag_name_buf, tag_name_alloc);

    /* Store encoded path pointer and size in tag */
    tag->encoded_tag_path = encoded_path_buf;
    tag->encoded_tag_path_len = encoded_len;

    /* Store root tag name and initialize instance ID (to be filled by Phase-1 metadata) */
    tag->tag_name = tag_name_storage;
    tag->tag_instance_id = 0; /* Will be set when Phase-1 metadata is fetched */

    tag->vtable = &enip_tag_vtable;
    tag->protocol_type = TAG_PROTOCOL_ENIP;
    tag->metadata_required = 1;

    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        tag->status = (int8_t)rc;
        rc_dec(tag);
        return NULL;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, tag->tag_id,
           "ENIP tag created: root_name='%s' full_path='%s' (encoded_len=%zu, instance_id=0)", tag->tag_name, tag_path_str,
           encoded_len);

    tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, PLCTAG_STATUS_OK);

    return (plc_tag_p)tag;
}
