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
#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/omron/conn.h>
#include <libplctag/protocols/omron/omron_common.h>
#include <libplctag/protocols/omron/tag.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/rc.h>

#include "omron_device_tag.h"

typedef struct omron_device_tag_s {
    TAG_BASE_STRUCT;
    omron_conn_p conn;
    int32_t last_conn_state;
    int32_t io_events;
    int32_t event_ring_read_idx;
} omron_device_tag_t;

typedef omron_device_tag_t *omron_device_tag_p;

static int omron_device_tag_abort(plc_tag_p tag);
static int omron_device_tag_status(plc_tag_p tag);
static int omron_device_tag_tickler(plc_tag_p tag);
static int omron_device_get_int_attrib(plc_tag_p tag, const char *attrib_name, int default_value);
static void omron_device_tag_destructor(void *ptr);

static struct tag_vtable_t omron_device_tag_vtable = {
    .abort = omron_device_tag_abort,
    .read = NULL,
    .status = omron_device_tag_status,
    .tickler = omron_device_tag_tickler,
    .write = NULL,
    .wake_plc = NULL,
    .tag_data_written = NULL,
    .get_int_attrib = omron_device_get_int_attrib,
    .set_int_attrib = NULL,
    .get_byte_array_attrib = NULL,
};


extern plc_tag_p omron_device_tag_create(attr attribs,
                                         void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                         void *userdata, plc_tag_p src_tag) {
    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, 0, "Starting.");

    omron_device_tag_p tag = (omron_device_tag_p)rc_alloc(sizeof(omron_device_tag_t), omron_device_tag_destructor);
    if(!tag) { return NULL; }

    tag->last_conn_state = PLCTAG_CONN_STATUS_DOWN;
    tag->io_events = attr_get_int(attribs, "io_events", 1);

    tag->vtable = &omron_device_tag_vtable;
    tag->protocol_type = TAG_PROTOCOL_OMRON_DEVICE;

    int rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, 0, "Unable to initialize generic tag parts!");
        rc_dec(tag);
        return NULL;
    }

    if(src_tag) {
        switch(src_tag->protocol_type) {
            case TAG_PROTOCOL_OMRON: tag->conn = rc_inc(((omron_tag_p)src_tag)->conn); break;

            case TAG_PROTOCOL_OMRON_DEVICE: tag->conn = rc_inc(((omron_device_tag_p)src_tag)->conn); break;

            default: tag->conn = NULL; break;
        }

        rc = tag->conn ? PLCTAG_STATUS_OK : PLCTAG_ERR_NOT_ALLOWED;
    } else {
        rc = conn_find_or_create(&tag->conn, attribs);
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, 0, "Unable to find or create conn, error %s!", plc_tag_decode_error(rc));
        tag->status = (int8_t)rc;
        tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)rc);
        return (plc_tag_p)tag;
    }

    tag->event_ring_read_idx = atomic_get_int32(&tag->conn->conn_event_ring_write_idx);
    tag->last_conn_state = atomic_get_int32(&tag->conn->connection_status);

    tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, PLCTAG_STATUS_OK);

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, 0, "Done. Initial connection status: %d.", tag->last_conn_state);
    return (plc_tag_p)tag;
}


static int omron_device_tag_abort(plc_tag_p tag) {
    (void)tag;
    return PLCTAG_STATUS_OK;
}

static int omron_device_tag_status(plc_tag_p raw_tag) {
    omron_device_tag_p tag = (omron_device_tag_p)raw_tag;

    if(tag->vtable->tickler) { tag->vtable->tickler(raw_tag); }

    return tag->status;
}

static int omron_device_tag_tickler(plc_tag_p raw_tag) {
    omron_device_tag_p tag = (omron_device_tag_p)raw_tag;

    if(!tag->conn) { return PLCTAG_STATUS_OK; }

    if(raw_tag->event_creation_complete) { return PLCTAG_STATUS_OK; }

    int32_t write_idx = atomic_get_int32(&tag->conn->conn_event_ring_write_idx);
    int32_t read_idx = tag->event_ring_read_idx;

    while(read_idx != write_idx) {
        read_idx = (read_idx + 1) & OMRON_CONN_EVENT_RING_MASK;
        int32_t event_type = tag->conn->conn_event_ring[read_idx].event_type;
        int32_t status = tag->conn->conn_event_ring[read_idx].status;

        if(tag->callback) {
            switch(event_type) {
                case TAG_CONN_EVENT_CONNECTION_CHANGED_STATE:
                    tag->last_conn_state = status;
                    tag->callback(tag->tag_id, status + PLCTAG_EVENT_CONN_STATUS_OFFSET, PLCTAG_STATUS_OK, tag->userdata);
                    break;

                case TAG_CONN_EVENT_SEND_REQUEST_STARTED:
                    if(tag->io_events) { tag->callback(tag->tag_id, PLCTAG_EVENT_WRITE_STARTED, status, tag->userdata); }
                    break;

                case TAG_CONN_EVENT_SEND_REQUEST_COMPLETED:
                    if(tag->io_events) { tag->callback(tag->tag_id, PLCTAG_EVENT_WRITE_COMPLETED, status, tag->userdata); }
                    break;

                case TAG_CONN_EVENT_RECEIVE_RESPONSE_STARTED:
                    if(tag->io_events) { tag->callback(tag->tag_id, PLCTAG_EVENT_READ_STARTED, status, tag->userdata); }
                    break;

                case TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED:
                    if(tag->io_events) { tag->callback(tag->tag_id, PLCTAG_EVENT_READ_COMPLETED, status, tag->userdata); }
                    break;

                default:
                    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "Unknown ring event type %d.", (int)event_type);
                    break;
            }
        }
    }

    tag->event_ring_read_idx = write_idx;
    return PLCTAG_STATUS_OK;
}

static int omron_device_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value) {
    omron_device_tag_p tag = (omron_device_tag_p)raw_tag;

    if(str_cmp_i(attrib_name, "connection_status") == 0) { return tag->last_conn_state; }

    return default_value;
}

static void omron_device_tag_destructor(void *ptr) {
    omron_device_tag_p tag = (omron_device_tag_p)ptr;

    if(tag->conn) {
        rc_dec(tag->conn);
        tag->conn = NULL;
    }

    if(tag->ext_mutex) {
        mutex_destroy(&(tag->ext_mutex));
        tag->ext_mutex = NULL;
    }

    if(tag->api_mutex) {
        mutex_destroy(&(tag->api_mutex));
        tag->api_mutex = NULL;
    }

    if(tag->tag_cond_wait) {
        cond_destroy(&(tag->tag_cond_wait));
        tag->tag_cond_wait = NULL;
    }

    if(tag->byte_order && tag->byte_order->is_allocated) {
        mem_free(tag->byte_order);
        tag->byte_order = NULL;
    }

    if(tag->data) {
        mem_free(tag->data);
        tag->data = NULL;
    }

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, tag->tag_id, "Done.");
}
