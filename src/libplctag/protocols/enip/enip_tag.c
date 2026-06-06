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
#include <libplctag/protocols/enip/enip_conn.h>
#include <libplctag/protocols/enip/tag.h>
#include <libplctag/protocols/enip/enip_cip.h>
#include <platform.h>
#include <utils/debug.h>
#include <utils/rc.h>
#include <utils/attr.h>
#include <utils/vector.h>
#include <string.h>


/*
 * ENIP Tag vtable and tag-creation implementation.
 *
 * STATUS: Phase 6. Metadata validity gate implemented.
 *
 * Phase 0: Change DEBUG_MODULE_LIB to DEBUG_MODULE_ENIP in enip_tag_destructor.
 *          Change return types from int to int32_t on all vtable functions (plan §0).
 *
 * Phase 6: Metadata validity predicate (plan §3.3):
 *   - Tag metadata usable iff: meta.state == ENIP_META_READY && meta.generation == conn->metadata_generation
 *   - Used by tag_status (PENDING until usable) and get_int_attrib (expose only when usable)
 */

/* Metadata validity gate (plan §3.3): true iff metadata is READY and current generation. */
static bool enip_metadata_usable(enip_tag_t *tag) {
    if(!tag || !tag->conn) { return false; }
    return (tag->meta.state == ENIP_META_READY && tag->meta.generation == tag->conn->metadata_generation);
}

/* Phase 0: change return type int -> int32_t; correct as-is otherwise. */
static int32_t enip_tag_abort(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    tag->op.op_state = ENIP_OP_DONE; /* terminal — engine will not re-issue */
    tag->op.kind = ENIP_OP_KIND_NONE;

    return PLCTAG_STATUS_OK;
}


static int32_t enip_tag_read(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    if(!tag->conn) { return PLCTAG_ERR_NULL_PTR; }

    tag->op.op_state = ENIP_OP_REQUEST;
    tag->op.op_time  = time_ms();
    tag->op.kind     = ENIP_OP_KIND_READ;
    tag->op.chunk_offset = 0;

    if(tag->conn->link.socket) {
        socket_wake(tag->conn->link.socket);
    }

    return PLCTAG_STATUS_PENDING;
}


static int32_t enip_tag_status(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    if(tag->status != PLCTAG_STATUS_OK) { return tag->status; }

    /* PENDING if metadata is not yet usable or operation is in flight (plan §3.3) */
    if(!enip_metadata_usable(tag)) {
        return PLCTAG_STATUS_PENDING;
    }

    if(tag->op.op_state == ENIP_OP_REQUEST || tag->op.op_state == ENIP_OP_INFLIGHT) {
        return PLCTAG_STATUS_PENDING;
    }

    return PLCTAG_STATUS_OK;
}


/* Phase 0: change return type int -> int32_t.  Correct as-is otherwise. */
static int32_t enip_tag_tickler(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    return tag->status;
}


static int32_t enip_tag_write(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    if(!tag->conn) { return PLCTAG_ERR_NULL_PTR; }

    tag->op.op_state = ENIP_OP_REQUEST;
    tag->op.op_time  = time_ms();
    tag->op.kind     = ENIP_OP_KIND_WRITE;
    tag->op.chunk_offset = 0;

    if(tag->conn->link.socket) {
        socket_wake(tag->conn->link.socket);
    }

    return PLCTAG_STATUS_PENDING;
}


static int32_t enip_tag_wake_plc(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    if(tag->conn && tag->conn->link.socket) {
        socket_wake(tag->conn->link.socket);
    }

    return PLCTAG_STATUS_OK;
}


static int32_t enip_tag_data_written(plc_tag_p p_tag) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    return PLCTAG_STATUS_OK;
}


static int32_t enip_tag_get_int_attrib(plc_tag_p p_tag, const char *attrib_name, int default_value) {
    enip_tag_t *tag = (enip_tag_t *)p_tag;

    if(!tag || !attrib_name) { return default_value; }

    /* Only expose metadata when usable (plan §3.3, §2.1) */
    if(!enip_metadata_usable(tag)) {
        return default_value;
    }

    if(str_cmp_i(attrib_name, "elem_size") == 0) {
        return (int32_t)tag->meta.elem_size;
    }

    if(str_cmp_i(attrib_name, "elem_count") == 0) {
        return (int32_t)tag->meta.elem_count;
    }

    if(str_cmp_i(attrib_name, "data_type") == 0) {
        return (int32_t)tag->meta.data_type;
    }

    return default_value;
}


static void enip_tag_destructor(void *ptr) {
    enip_tag_t *tag = (enip_tag_t *)ptr;

    if(!tag) { return; }

    if(tag->in_active_tags && tag->conn
       && tag->conn->active_tags && tag->conn->active_tags_mutex) {
        critical_block(tag->conn->active_tags_mutex) {
            int n = vector_length(tag->conn->active_tags);
            for(int i = 0; i < n; i++) {
                if(vector_get(tag->conn->active_tags, i) == tag) {
                    vector_remove(tag->conn->active_tags, i);
                    break;
                }
            }
            tag->in_active_tags = false;
        }
    }

    if(tag->data) { mem_free(tag->data); tag->data = NULL; }

    /* Release the connection back-pointer ref acquired at tag creation. */
    if(tag->conn) { rc_dec(tag->conn); tag->conn = NULL; }

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
    .get_int_attrib = enip_tag_get_int_attrib,
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

    tag->op.encoded_path     = encoded_path_buf;
    tag->op.encoded_path_len = (uint16_t)encoded_len;

    tag->tag_name          = tag_name_storage;
    tag->meta.instance_id  = 0;   /* set when phase-1 metadata is fetched */
    tag->meta.state        = ENIP_META_NONE;
    tag->meta.needs_metadata = true;

    tag->vtable = &enip_tag_vtable;
    tag->protocol_type = TAG_PROTOCOL_ENIP;

    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        tag->status = (int8_t)rc;
        rc_dec(tag);
        return NULL;
    }

    if(tag->meta.elem_size > 0 && tag->meta.elem_count > 0 && !tag->data) {
        tag->size = tag->meta.elem_count * tag->meta.elem_size;
        tag->data = (uint8_t *)mem_alloc(tag->size);
        if(!tag->data) {
            rc_dec(tag);
            return NULL;
        }
    }

    /* Wire up the connection back-pointer and add to active_tags (plan §3.1).
     * Uses registry to share connections by gateway+path. */
    enip_connection_t *conn = enip_registry_find_or_create(attribs);
    if(!conn) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to find or create connection for tag '%s'", tag_path_str);
        rc_dec(tag);
        return NULL;
    }

    tag->conn       = conn;  /* already rc_inc'd by registry */
    tag->op.op_time = time_ms();

    critical_block(conn->active_tags_mutex) {
        vector_insert(conn->active_tags, vector_length(conn->active_tags), tag);
        tag->in_active_tags = true;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, tag->tag_id,
           "ENIP tag created: root_name='%s' full_path='%s' (encoded_len=%zu)",
           tag->tag_name, tag_path_str, encoded_len);

    tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, PLCTAG_STATUS_OK);

    return (plc_tag_p)tag;
}
