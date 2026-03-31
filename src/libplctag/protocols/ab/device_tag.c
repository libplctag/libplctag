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

#include "device_tag.h"
#include <libplctag/lib/tag.h>
#include <libplctag/lib/libplctag.h>
#include "session.h"
#include <utils/atomic_utils.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <platform.h>
#include <utils/rc.h>
#include <inttypes.h>

typedef struct ab_device_tag_s {
    TAG_BASE_STRUCT;
    ab_session_p session;
    int32_t use_connected_msg;    /* if true, raise a PLCTAG_EVENT_CONNECTION_CHANGED_STATE event when the tag is created with the
                                     initial connection state */
    int32_t allow_packing;        /* if true, allow this tag to be packed with other tags in the same request */
    int32_t last_conn_state;      /* previous value; used to detect changes */
    int32_t io_events;            /* 1 = fire READ/WRITE events for session IO, 0 = suppress */
    int32_t status_ring_read_idx; /* index of the last ring buffer entry this tag has processed */
} ab_device_tag_t;

typedef ab_device_tag_t *ab_device_tag_p;


static int device_tag_abort(plc_tag_p tag);
static int device_tag_read(plc_tag_p tag);
static int device_tag_status(plc_tag_p tag);
static int device_tag_tickler(plc_tag_p tag);
static int device_tag_write(plc_tag_p tag);
static int device_get_int_attrib(plc_tag_p tag, const char *attrib_name, int default_value);
static void ab_device_tag_destructor(void *ptr);
static const char *conn_status_name(int32_t conn_status);

static struct tag_vtable_t device_tag_vtable = {
    .abort = device_tag_abort,               /* return PLCTAG_ERR_NOT_IMPLEMENTED */
    .read = device_tag_read,                 /* returns PLCTAG_ERR_NOT_IMPLEMENTED */
    .status = device_tag_status,             /* returns last connection status value */
    .tickler = device_tag_tickler,           /* polls connection_status, raises events */
    .write = device_tag_write,               /* returns PLCTAG_ERR_NOT_IMPLEMENTED */
    .wake_plc = NULL,                        /* not used */
    .get_int_attrib = device_get_int_attrib, /* get connection status attribute */
    .set_int_attrib = NULL,                  /* not used */
};


extern plc_tag_p ab_device_tag_create(attr attribs,
                                      void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                      void *userdata) {
    pdebug(DEBUG_MODULE_AB_DEVICE, DEBUG_DETAIL, 0, "Starting.");

    ab_device_tag_p tag = (ab_device_tag_p)rc_alloc(sizeof(ab_device_tag_t), ab_device_tag_destructor);
    if(!tag) { return NULL; }

    tag->last_conn_state = PLCTAG_CONN_STATUS_DOWN;
    tag->io_events = attr_get_int(attribs, "io_events", 1); /* FIXME does not do anything yet. */
    tag->use_connected_msg = attr_get_int(attribs, "use_connected_msg", 1);
    tag->allow_packing = attr_get_int(attribs, "allow_packing", 1);

    /* set the vtable to the device tag vtable. */
    tag->vtable = &device_tag_vtable;

    /* set up the generic parts. */
    int32_t rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_DEVICE, DEBUG_WARN, 0, "Unable to initialize generic tag parts!");
        pdebug(DEBUG_MODULE_AB_DEVICE, DEBUG_DETAIL, 0, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return (plc_tag_p)NULL;
    }

    /* get a session */
    if(session_find_or_create(&tag->session, attribs) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_DEVICE, DEBUG_INFO, 0, "Unable to create session!");
        tag->status = PLCTAG_ERR_BAD_GATEWAY;
        return (plc_tag_p)tag;
    }

    pdebug(DEBUG_MODULE_AB_DEVICE, DEBUG_DETAIL, 0, "using session=%p", tag->session);

    /* start at the current write index so we only see future state changes */
    tag->status_ring_read_idx = atomic_get_int32(&tag->session->conn_status_ring_write_idx);
    tag->last_conn_state = atomic_get_int32(&tag->session->connection_status);

    pdebug(DEBUG_MODULE_AB_DEVICE, DEBUG_DETAIL, 0, "Done with initial connection status=%s.",
           conn_status_name(tag->last_conn_state));

    return (plc_tag_p)tag;
}


static int device_tag_abort(plc_tag_p tag) {
    (void)tag;
    return PLCTAG_ERR_NOT_IMPLEMENTED;
}

static int device_tag_read(plc_tag_p tag) {
    (void)tag;
    return PLCTAG_ERR_NOT_IMPLEMENTED;
}

static int device_tag_status(plc_tag_p tag) {
    ab_device_tag_t *device_tag = (ab_device_tag_t *)tag;

    if(device_tag->vtable->tickler) { device_tag->vtable->tickler(tag); }

    return device_tag->status;
}

static int device_tag_tickler(plc_tag_p raw_tag) {
    ab_device_tag_p device_tag = (ab_device_tag_p)raw_tag;

    if(!device_tag->session) { return PLCTAG_STATUS_OK; }

    int32_t write_idx = atomic_get_int32(&device_tag->session->conn_status_ring_write_idx);
    int32_t read_idx = device_tag->status_ring_read_idx;

    /* drain all unread ring buffer entries; write_idx points to the last written slot */
    while(read_idx != write_idx) {
        read_idx = (read_idx + 1) & SESSION_CONN_STATUS_RING_SIZE_MASK;
        int32_t status = device_tag->session->conn_status_ring[read_idx].status;
        int32_t reason = device_tag->session->conn_status_ring[read_idx].reason;
        device_tag->last_conn_state = status;
        /* api_mutex is already held by the generic tickler so dispatch each entry directly */
        if(device_tag->callback) {
            device_tag->callback(device_tag->tag_id, PLCTAG_EVENT_CONNECTION_CHANGED_STATE, (int)reason, device_tag->userdata);
        }
    }

    device_tag->status_ring_read_idx = write_idx;

    return PLCTAG_STATUS_OK;
}

static int device_tag_write(plc_tag_p tag) {
    (void)tag;
    return PLCTAG_ERR_NOT_IMPLEMENTED;
}

static int device_get_int_attrib(plc_tag_p tag, const char *attrib_name, int default_value) {
    ab_device_tag_t *device_tag = (ab_device_tag_t *)tag;

    if(str_cmp(attrib_name, "connection_status") == 0) { return device_tag->last_conn_state; }

    return default_value;
}

static void ab_device_tag_destructor(void *ptr) {
    ab_device_tag_p tag = (ab_device_tag_p)ptr;
    (void)tag;

    if(tag->session) {
        pdebug(DEBUG_MODULE_AB_DEVICE, DEBUG_DETAIL, tag->tag_id, "Removing tag from session.");
        rc_dec(tag->session);
        tag->session = NULL;
    } else {
        pdebug(DEBUG_MODULE_AB_DEVICE, DEBUG_WARN, tag->tag_id, "No session pointer!");
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

    pdebug(DEBUG_MODULE_AB_DEVICE, DEBUG_INFO, tag->tag_id, "Finished releasing all tag resources.");

    pdebug(DEBUG_MODULE_AB_DEVICE, DEBUG_INFO, tag->tag_id, "Done");
}


static const char *conn_status_name(int32_t conn_status) {
    switch(conn_status) {
        case PLCTAG_CONN_STATUS_UP: return "UP";
        case PLCTAG_CONN_STATUS_DOWN: return "DOWN";
        case PLCTAG_CONN_STATUS_DISCONNECTING: return "DISCONNECTING";
        case PLCTAG_CONN_STATUS_CONNECTING: return "CONNECTING";
        case PLCTAG_CONN_STATUS_IDLE_WAIT: return "IDLE_WAIT";
        case PLCTAG_CONN_STATUS_ERR_WAIT: return "ERR_WAIT";
        default: return "UNKNOWN";
    }
}
