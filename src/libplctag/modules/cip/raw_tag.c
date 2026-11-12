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

/* The @raw tag body, shared by the CIP families.  See raw_tag.h. */

#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/cip/raw_tag.h>
#include <libplctag/modules/cip/tag.h>
#include <platform.h>
#include <utils/debug.h>




tag_byte_order_t cip_raw_tag_byte_order = {.is_allocated = 0,

                                           .int16_order = {0, 1},
                                           .int32_order = {0, 1, 2, 3},
                                           .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                           .float32_order = {0, 1, 2, 3},
                                           .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                           .str_is_defined = 0};


static int raw_tag_tickler(plc_tag_p raw_tag);
static int raw_tag_encode_write(cip_tag_p tag, uint8_t *dest, int32_t capacity, tag_op_plan_t *plan);
static int raw_tag_decode_write(cip_tag_p tag, uint8_t *reply, int32_t reply_size);


static struct tag_vtable_t raw_tag_vtable = {
    .abort = cip_data_tag_abort,
    .status = cip_tag_status,
    .tickler = raw_tag_tickler,
    .write = cip_tag_write_start,
    .wake_plc = cip_tag_wake_plc, /* plc_tag_write() queues the tag with it */
    .activate = cip_tag_activate,
    .attribs = cip_tag_attribs,
};

static const cip_tag_codec_t raw_tag_codec = {.encode_write = raw_tag_encode_write, .decode_write = raw_tag_decode_write};


int cip_raw_tag_setup(cip_tag_p tag, debug_module_t debug_module) {
    return cip_setup_special_tag(tag, CIP_TYPE_TAG_RAW, &cip_raw_tag_byte_order, &raw_tag_vtable, &raw_tag_codec, debug_module);
}


static int raw_tag_tickler(plc_tag_p raw_tag) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    /* a raw tag has no target until the app writes one, so a connection that is up is its create. */
    if(tag->create_read_status == PLCTAG_STATUS_PENDING) {
        int32_t conn_status = PLCTAG_CONN_STATUS_DOWN;

        if(cip_tag_get_connection_status((plc_tag_p)tag, &conn_status) == PLCTAG_STATUS_OK
           && conn_status == PLCTAG_CONN_STATUS_UP) {
            cip_tag_finish_create(tag, PLCTAG_STATUS_OK);
        }
    }

    return tag->status;
}


/*
 * Check on a write in flight.  Must be called with the tag mutex locked.
 *
 * The connection hands back the CIP response with the EIP and CPF framing already
 * stripped, so nothing here depends on whether the request went out connected or
 * unconnected.
 */
static int raw_tag_decode_write(cip_tag_p tag, uint8_t *reply, int32_t reply_size) {
    int rc = PLCTAG_STATUS_OK;
    int data_size = 0;
    debug_module_t dbg = tag->debug_module;

    pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Starting.");

    data_size = reply_size;

    if(data_size <= 0) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Response carries no CIP data!");
        byte_buffer_set_len(&tag->buf, 0);
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* hand the whole CIP response back to the caller. */
    rc = byte_buffer_set_len(&tag->buf, data_size);

    if(rc == PLCTAG_STATUS_OK) {
        mem_copy(byte_buffer_data(tag->buf), reply, data_size);
    } else {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data buffer!");
        byte_buffer_set_len(&tag->buf, 0);
    }

    pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


/*
 * The caller supplies the entire CIP request in the tag's data buffer, so the request is
 * that buffer verbatim and the connection adds the EIP and CPF framing.  That framing was
 * the only thing that ever differed between the connected and unconnected forms of this
 * function.
 */
static int raw_tag_encode_write(cip_tag_p tag, uint8_t *dest, int32_t capacity, tag_op_plan_t *plan) {
    int32_t payload_size = byte_buffer_len(tag->buf);
    int32_t max_payload = 0;
    debug_module_t dbg = tag->debug_module;

    pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Starting.");

    if(payload_size <= 0) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Raw tag has no CIP request to send!");
        return PLCTAG_ERR_NO_DATA;
    }

    max_payload = cip_conn_max_cip_payload(cip_tag_conn(tag));

    if(payload_size > max_payload) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Request of %d bytes exceeds the %d bytes this connection can carry!",
               payload_size, max_payload);
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(payload_size > capacity) {
        pdebug(dbg, DEBUG_WARN | DEBUG_RATE_LIMIT, tag->tag_id, "Request of %d bytes does not fit in %d bytes!", payload_size,
               capacity);
        return PLCTAG_ERR_TOO_SMALL;
    }

    mem_copy(dest, byte_buffer_data(tag->buf), payload_size);

    plan->req_size = payload_size;
    plan->reply_size = 0;
    plan->packable = (tag->allow_packing != 0);

    pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}
