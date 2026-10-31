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

/* The one connection-status tag implementation.  See connection_tag.h. */

#include <libplctag/api/libplctag.h>
#include <libplctag/lib/conn_watch.h>
#include <libplctag/lib/connection_tag.h>
#include <libplctag/lib/tag.h>
#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/rc.h>
#include <utils/vector.h>


static int connection_tag_abort(plc_tag_p tag);
static int connection_tag_status(plc_tag_p tag);
static int connection_tag_activate(plc_tag_p tag);
static void connection_tag_deliver(connection_tag_p tag);
static int32_t connection_get_connection_status(plc_tag_p tag, int32_t *result);
static void connection_tag_destructor(void *ptr);
static const char *conn_status_name(int32_t conn_status);


/* A connection tag carries no PLC data, so its only runtime attribute is the link state. */
static const attr_def_t connection_tag_attribs[] = {
    {.name = "connection_status",
     .type = ATTR_TYPE_INT,
     .description = "The state of the connection this tag monitors, as a plc_tag_conn_status_t.",
     .get_int = connection_get_connection_status},

    {.name = NULL},
};


static struct tag_vtable_t connection_tag_vtable = {
    .abort = connection_tag_abort,     /* not used */
    .read = NULL,                      /* not used */
    .status = connection_tag_status,   /* returns the last connection status value */
    .tickler = NULL,                   /* the connection's handler delivers events instead */
    .write = NULL,                     /* not used */
    .wake_plc = NULL,                  /* not used */
    .tag_data_written = NULL,          /* not used */
    .activate = connection_tag_activate,
    .attribs = connection_tag_attribs,
};


plc_tag_p connection_tag_create(attr attribs, const connection_tag_args_t *args,
                                void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                void *userdata) {
    pdebug(args->debug_module, DEBUG_DETAIL, 0, "Starting.");

    connection_tag_p tag = (connection_tag_p)rc_alloc(sizeof(connection_tag_t), connection_tag_destructor);
    if(!tag) { return NULL; }

    tag->vtable = &connection_tag_vtable;
    tag->protocol_type = args->protocol_type;
    tag->debug_module = args->debug_module;
    tag->last_conn_state = PLCTAG_CONN_STATUS_DOWN;
    tag->io_events = attr_get_int(attribs, "io_events", 1);

    int32_t rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(args->debug_module, DEBUG_WARN, 0, "Unable to initialize generic tag parts!");
        rc_dec(tag);
        return (plc_tag_p)NULL;
    }

    if(!args->conn) {
        pdebug(args->debug_module, DEBUG_WARN, 0, "Unable to find or create connection, error %s!",
               plc_tag_decode_error(args->conn_rc));
        tag->status = (int8_t)args->conn_rc;
        tag_created((plc_tag_p)tag, args->conn_rc);
        return (plc_tag_p)tag;
    }

    tag->conn = args->conn;
    tag->watch = args->watch;
    tag->conn_mutex = args->conn_mutex;

    if(args->conn_is_new) {
        /*
         * We just created this connection, so we know its exact birth state
         * without racing its handler thread for a snapshot: it may already have
         * run through CONNECTING -> UP by the time we get here (a fast local
         * connection can do that before this thread is scheduled again), and
         * reading "current" values then would silently skip every transition
         * that already happened.
         */
        tag->ring_read_idx = 0;
        tag->last_conn_state = PLCTAG_CONN_STATUS_DOWN;
    } else {
        /*
         * Joining a connection that already existed: read both values as one
         * snapshot under the connection's own mutex, so we cannot pair a read
         * index from before a transition with a status from after it.
         */
        critical_block(args->conn_mutex) {
            tag->ring_read_idx = conn_watch_read_idx(tag->watch);
            tag->last_conn_state = atomic_get_int32(&tag->watch->status);
        }
    }

    pdebug(args->debug_module, DEBUG_DETAIL, 0, "Done with initial connection status=%s.",
           conn_status_name(tag->last_conn_state));

    return (plc_tag_p)tag;
}


/*
 * Watch from here, once the tag has its id: the handler delivers events from the moment the
 * tag is on the list.  The library holds the API mutex across this call, so the handler, which
 * delivers under it, can only queue connection events behind CREATED.  A connection already up
 * when the tag joins it reports that state straight after CREATED.
 */
static int connection_tag_activate(plc_tag_p raw_tag) {
    connection_tag_p tag = (connection_tag_p)raw_tag;
    int32_t rc = PLCTAG_STATUS_OK;

    /* no connection: the create already failed. */
    if(!tag->conn) { return tag->status; }

    rc = conn_watch_add_watcher(tag->watch, tag->conn_mutex, raw_tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(tag->debug_module, DEBUG_WARN, tag->tag_id, "Unable to watch the connection, error %s!", plc_tag_decode_error(rc));
        tag->status = (int8_t)rc;
        tag_created(raw_tag, rc);
        return rc;
    }

    tag_created(raw_tag, PLCTAG_STATUS_OK);

    if(tag->last_conn_state != PLCTAG_CONN_STATUS_DOWN) {
        tag_event_push(raw_tag, tag->last_conn_state + PLCTAG_EVENT_CONN_STATUS_OFFSET, PLCTAG_STATUS_OK);
    }

    return PLCTAG_STATUS_OK;
}


static int connection_tag_abort(plc_tag_p tag) {
    (void)tag;
    return PLCTAG_STATUS_OK;
}


static int connection_tag_status(plc_tag_p raw_tag) {
    connection_tag_p tag = (connection_tag_p)raw_tag;

    /*
     * Do NOT deliver here.  connection_tag_deliver() must have exactly one
     * caller -- the connection's handler -- since it advances ring_read_idx
     * past whatever is in the ring on every call.  plc_tag_create_impl() calls
     * ->status() synchronously, in a tight loop, on the creating thread while
     * waiting for creation to finish; if that also delivered, it could mark ring
     * entries as seen before the handler delivered them, silently dropping the
     * earliest events such as CONNECTING.
     */
    return tag->status;
}


/*
 * Queue the connection ring's new events on the tag, behind whatever the tag already has.
 * Called with the tag's API mutex held, only by connection_tag_notify_watchers(), which
 * dispatches them.
 */
static void connection_tag_deliver(connection_tag_p tag) {
    int32_t read_idx = tag->ring_read_idx;
    int32_t event_type = 0;
    int32_t status = 0;

    if(!tag->conn) { return; }

    while(conn_watch_next(tag->watch, &read_idx, &event_type, &status)) {
        int event = 0;

        switch(event_type) {
            case TAG_CONN_EVENT_SEND_REQUEST_STARTED: event = (tag->io_events ? PLCTAG_EVENT_WRITE_STARTED : 0); break;
            case TAG_CONN_EVENT_SEND_REQUEST_COMPLETED: event = (tag->io_events ? PLCTAG_EVENT_WRITE_COMPLETED : 0); break;
            case TAG_CONN_EVENT_RECEIVE_RESPONSE_STARTED: event = (tag->io_events ? PLCTAG_EVENT_READ_STARTED : 0); break;
            case TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED: event = (tag->io_events ? PLCTAG_EVENT_READ_COMPLETED : 0); break;

            default:
                if(event_type >= PLCTAG_EVENT_CONN_STATUS_OFFSET) {
                    tag->last_conn_state = event_type - PLCTAG_EVENT_CONN_STATUS_OFFSET;
                    event = event_type;
                    status = PLCTAG_STATUS_OK;
                } else {
                    pdebug(tag->debug_module, DEBUG_WARN, tag->tag_id, "Unsupported ring event type %d.", (int)event_type);
                }
                break;
        }

        if(!event) { continue; }

        tag_event_push((plc_tag_p)tag, event, status);

        /* a busy connection can publish more in one pass than the tag's ring holds. */
        if(tag->event_count >= TAG_EVENT_RING_SIZE) { plc_tag_generic_handle_event_callbacks((plc_tag_p)tag); }
    }

    tag->ring_read_idx = read_idx;
}


void connection_tag_notify_watchers(conn_event_ring_t *watch, mutex_p conn_mutex) {
    /*
     * One tag at a time, each taken under the connection's mutex with a reference so it outlives
     * the delivery.  rc_inc() refuses a tag already being destroyed; its destructor is waiting on
     * the mutex to remove itself, so it is skipped.
     */
    for(int32_t index = 0;; index++) {
        connection_tag_p tag = NULL;
        bool more = false;

        critical_block(conn_mutex) {
            if(watch->watchers && index < vector_length(watch->watchers)) {
                more = true;
                tag = rc_inc(vector_get(watch->watchers, index));
            }
        }

        if(!more) { break; }

        if(!tag) { continue; }

        critical_block(tag->api_mutex) { connection_tag_deliver(tag); }

        plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);

        rc_dec(tag);
    }
}


static int32_t connection_get_connection_status(plc_tag_p raw_tag, int32_t *result) {
    connection_tag_p tag = (connection_tag_p)raw_tag;

    /*
     * Straight from the connection, as data tags report it.  last_conn_state only moves when
     * the handler delivers events, and it skips them all for a tag without a callback.
     */
    *result = (tag->watch ? atomic_get_int32(&tag->watch->status) : tag->last_conn_state);

    return PLCTAG_STATUS_OK;
}


static void connection_tag_destructor(void *ptr) {
    connection_tag_p tag = (connection_tag_p)ptr;

    if(tag->conn) {
        pdebug(tag->debug_module, DEBUG_DETAIL, tag->tag_id, "Releasing the connection.");

        /* stop being delivered to before anything is freed. */
        conn_watch_remove_watcher(tag->watch, tag->conn_mutex, (plc_tag_p)tag);

        rc_dec(tag->conn);
        tag->conn = NULL;
        tag->watch = NULL;
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

    if(tag->instance) {
        rc_dec(tag->instance);
        tag->instance = NULL;
    }

    pdebug(tag->debug_module, DEBUG_INFO, tag->tag_id, "Done.");
}


static const char *conn_status_name(int32_t conn_status) {
    if(conn_status >= PLCTAG_EVENT_CONN_STATUS_OFFSET) { conn_status -= PLCTAG_EVENT_CONN_STATUS_OFFSET; }

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
