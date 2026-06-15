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
 * Tag vtable and create path (design doc §9, §11.2, §14.7).
 *
 * Create primes the tag via OPEN_PROBE (+ OPEN_BULK for elem_count > 1,
 * §11.2/§11.3). Read and write are both windowed the same way for
 * elem_count > 1 (§11.5).
 */

#include <inttypes.h>
#include <string.h>

#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/enip/enip_cip.h>
#include <libplctag/protocols/enip/enip_session.h>
#include <libplctag/protocols/enip/enip_tag.h>
#include <platform.h>
#include <utils/arena.h>
#include <utils/attr.h>
#include <utils/bytes.h>
#include <utils/debug.h>
#include <utils/rc.h>

static int32_t enip_tag_abort(plc_tag_p tag);
static int32_t enip_tag_read(plc_tag_p tag);
static int32_t enip_tag_write(plc_tag_p tag);
static int32_t enip_tag_status(plc_tag_p tag);
static int32_t enip_tag_data_written(plc_tag_p tag);
static void enip_tag_destructor(void *tag_arg);

/* CIP types are little-endian on the wire (design doc §9). */
static tag_byte_order_t enip_tag_byte_order = {.is_allocated = 0,

                                                .int16_order = {0, 1},
                                                .int32_order = {0, 1, 2, 3},
                                                .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                                .float32_order = {0, 1, 2, 3},
                                                .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                                .str_is_defined = 1,
                                                .str_is_counted = 1,
                                                .str_is_fixed_length = 1,
                                                .str_is_zero_terminated = 0,
                                                .str_is_byte_swapped = 0,

                                                .str_pad_to_multiple_bytes = 1,
                                                .str_count_word_bytes = 4,
                                                .str_max_capacity = 82,
                                                .str_total_length = 88,
                                                .str_pad_bytes = 2};

struct tag_vtable_t enip_tag_vtable = {
    .abort = enip_tag_abort,
    .read = enip_tag_read,
    .status = enip_tag_status,
    .tickler = NULL,
    .write = enip_tag_write,

    .wake_plc = NULL,
    .tag_data_written = enip_tag_data_written,

    .get_int_attrib = NULL,
    .set_int_attrib = NULL,
    .get_byte_array_attrib = NULL,
};

/* ============================================================================
 * Vtable functions
 * ============================================================================ */

static int32_t enip_tag_abort(plc_tag_p tag) {
    enip_tag_p t = (enip_tag_p)tag;

    if(!t->conn) { return PLCTAG_STATUS_OK; }

    return enip_session_unschedule(t->conn, t);
}

static int32_t enip_tag_read(plc_tag_p tag) {
    enip_tag_p t = (enip_tag_p)tag;

    if(!t->conn) { return PLCTAG_ERR_BAD_GATEWAY; }

    if(!t->ready) {
        /* still opening (or open failed) -- report the create-time status. */
        return (t->status == (int8_t)PLCTAG_STATUS_OK) ? PLCTAG_STATUS_PENDING : t->status;
    }

    t->read_complete = 0;

    if(t->elem_count > 1) { t->read_off = 0; }

    tag_raise_event(tag, PLCTAG_EVENT_READ_STARTED, (int8_t)PLCTAG_STATUS_OK);

    return enip_session_schedule(t->conn, t, ENIP_OP_READ, time_ms());
}

static int32_t enip_tag_write(plc_tag_p tag) {
    enip_tag_p t = (enip_tag_p)tag;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, t->tag_id,
           "write entry: ready=%d scheduled=%d op=%d elem_count=%" PRIu32 " write_window_elems=%" PRIu32 " read_off=%" PRIu32,
           (int)t->ready, (int)t->scheduled, (int)t->op, t->elem_count, t->write_window_elems, t->read_off);

    if(!t->conn) { return PLCTAG_ERR_BAD_GATEWAY; }

    if(!t->ready) {
        /* still opening (or open failed) -- report the create-time status. */
        return (t->status == (int8_t)PLCTAG_STATUS_OK) ? PLCTAG_STATUS_PENDING : t->status;
    }

    t->write_complete = 0;

    if(t->elem_count > 1) { t->read_off = 0; }

    tag_raise_event(tag, PLCTAG_EVENT_WRITE_STARTED, (int8_t)PLCTAG_STATUS_OK);

    return enip_session_schedule(t->conn, t, ENIP_OP_WRITE, time_ms());
}

static int32_t enip_tag_status(plc_tag_p tag) {
    enip_tag_p t = (enip_tag_p)tag;

    if(t->op != ENIP_OP_IDLE) { return PLCTAG_STATUS_PENDING; }

    return t->status;
}

/* Called under api_mutex from the data-setter functions when
 * auto_sync_write_ms > 0 (design doc §9/§14.8). Arms auto_sync_next_write
 * immediately so the generic tickler's next pass (within ~100ms) sees a
 * due time already set, instead of needing one tick just to start the
 * countdown. */
static int32_t enip_tag_data_written(plc_tag_p tag) {
    enip_tag_p t = (enip_tag_p)tag;

    int64_t fire_at = time_ms() + t->auto_sync_write_ms;

    if(t->auto_sync_next_write == 0 || t->auto_sync_next_write > fire_at) { t->auto_sync_next_write = fire_at; }

    return PLCTAG_STATUS_OK;
}

/* ============================================================================
 * Create / destroy
 * ============================================================================ */

static void enip_tag_destructor(void *tag_arg) {
    enip_tag_p t = (enip_tag_p)tag_arg;

    if(!t) { return; }

    if(t->conn) {
        enip_session_tag_detach(t->conn, t);
        t->conn = rc_dec(t->conn);
    }

    if(t->data) {
        mem_free(t->data);
        t->data = NULL;
    }

    if(t->api_mutex) { mutex_destroy(&t->api_mutex); }
    if(t->ext_mutex) { mutex_destroy(&t->ext_mutex); }
    if(t->tag_cond_wait) { cond_destroy(&t->tag_cond_wait); }
}

/* Allocate the tag with tag_name and the encoded CIP path packed into the
 * tail of the allocation (per enip_tag.h). */
static enip_tag_p create_tag_object(attr attribs) {
    const char *tag_name = attr_get_str(attribs, "name", NULL);

    if(!tag_name || str_length(tag_name) == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Missing required \"name\" attribute.");
        return NULL;
    }

    Arena scratch;
    if(arena_init(&scratch, (size_t)1024) != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to allocate scratch arena!");
        return NULL;
    }

    Bytes encoded = enip_cip_encode_path(&scratch, tag_name);
    if(bytes_is_null(encoded)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to encode tag path for \"%s\"!", tag_name);
        arena_free(&scratch);
        return NULL;
    }

    size_t name_len = (size_t)str_length(tag_name);
    size_t tail_size = name_len + 1 + encoded.len;

    enip_tag_p tag = (enip_tag_p)rc_alloc((int)(sizeof(struct enip_tag_t) + tail_size), enip_tag_destructor);
    if(!tag) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to allocate ENIP tag!");
        arena_free(&scratch);
        return NULL;
    }

    uint8_t *tail = (uint8_t *)(tag + 1);

    memcpy(tail, tag_name, name_len + 1);
    tag->tag_name = (char *)tail;

    memcpy(tail + name_len + 1, encoded.data, encoded.len);
    tag->path = bytes_from_buf(tail + name_len + 1, encoded.len);

    arena_free(&scratch);

    tag->vtable = &enip_tag_vtable;
    tag->byte_order = &enip_tag_byte_order;
    tag->elem_count = (uint32_t)attr_get_int(attribs, "elem_count", 1);

    return tag;
}

plc_tag_p enip_tag_create_impl(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                void *userdata, plc_tag_p src_tag) {
    int32_t rc;

    enip_tag_p tag = create_tag_object(attribs);
    if(!tag) { return NULL; }

    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to initialize generic tag parts!");
        rc_dec(tag);
        return NULL;
    }

    tag->protocol_type = TAG_PROTOCOL_ENIP;

    if(src_tag && src_tag->protocol_type == TAG_PROTOCOL_ENIP) {
        tag->conn = rc_inc(((enip_tag_p)src_tag)->conn);
    } else {
        tag->conn = enip_session_create(attribs);
    }

    if(!tag->conn) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to create or find a connection!");
        tag->status = (int8_t)PLCTAG_ERR_BAD_GATEWAY;
        tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)PLCTAG_ERR_BAD_GATEWAY);
        plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);
        return (plc_tag_p)tag;
    }

    tag->status = (int8_t)PLCTAG_STATUS_PENDING;

    enip_session_schedule(tag->conn, tag, ENIP_OP_OPEN_PROBE, time_ms());

    return (plc_tag_p)tag;
}
