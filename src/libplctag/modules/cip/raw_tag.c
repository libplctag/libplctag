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


static int raw_tag_check_write_status(cip_tag_p tag);
static int raw_tag_build_write_request(cip_tag_p tag);


tag_byte_order_t cip_raw_tag_byte_order = {.is_allocated = 0,

                                           .int16_order = {0, 1},
                                           .int32_order = {0, 1, 2, 3},
                                           .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                           .float32_order = {0, 1, 2, 3},
                                           .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                           .str_is_defined = 0};


int cip_raw_tag_setup(cip_tag_p tag, tag_vtable_p vtable, debug_module_t debug_module) {
    return cip_setup_special_tag(tag, CIP_TYPE_TAG_RAW, &cip_raw_tag_byte_order, vtable, debug_module);
}


int cip_raw_tag_tickler(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    debug_module_t dbg = tag->debug_module;

    pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = cip_check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->read_in_progress) {
        /*
         * A raw tag is write-only: the application supplies a whole CIP request and gets
         * the whole reply back, so there is nothing for a read to mean.
         *
         * Clear both flags and complete anyway.  Leaving read_in_flight set would block
         * auto-read for the life of the tag (see lib.c), and not setting read_complete
         * would leave a caller that asked for a read waiting out its whole timeout for an
         * answer that is never coming.
         */
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Something started a read on a raw tag.  This is not supported!");

        cip_tag_abort_request(tag);

        tag->read_in_flight = 0;
        tag->read_complete = 1;

        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(tag->write_in_progress) {
        rc = raw_tag_check_write_status(tag);

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->write_in_progress) {
            pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Write complete.");
            tag->write_complete = 1;
        } else {
            pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Write in progress.");
        }

        pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Done.");

        return rc;
    }

    pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Done.  No operation in progress.");

    return tag->status;
}


/*
 * Start writing a raw tag.
 *
 * This must be called from one thread alone, or while the tag mutex is locked.
 */
int cip_raw_tag_write_start(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    debug_module_t dbg = tag->debug_module;

    pdebug(dbg, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->read_in_progress) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Raw tag found with a read in flight!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->write_in_progress) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* the write is now in flight */
    tag->write_in_progress = 1;

    rc = raw_tag_build_write_request(tag);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Unable to build write request!");
        tag->write_in_progress = 0;

        return rc;
    }

    pdebug(dbg, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * Check on a write in flight.  Must be called with the tag mutex locked.
 *
 * The connection hands back the CIP response with the EIP and CPF framing already
 * stripped, so nothing here depends on whether the request went out connected or
 * unconnected.
 */
static int raw_tag_check_write_status(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    int data_size = 0;
    uint8_t *tag_data_buffer = NULL;
    debug_module_t dbg = tag->debug_module;

    pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* the write is over one way or another, whatever the response turns out to hold. */
    tag->write_in_progress = 0;

    data_size = tag->req->request_size;

    if(data_size <= 0) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Response carries no CIP data!");
        cip_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* hand the whole CIP response back to the caller. */
    tag_data_buffer = mem_realloc(tag->data, data_size);

    if(tag_data_buffer) {
        tag->data = tag_data_buffer;
        tag->size = data_size;

        mem_copy(tag->data, tag->req->data, data_size);
    } else {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data buffer!");
        rc = PLCTAG_ERR_NO_MEM;
    }

    /* clean up the request regardless. */
    cip_tag_abort_request(tag);

    pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


/*
 * The caller supplies the entire CIP request in the tag's data buffer, so the request is
 * that buffer verbatim and the connection adds the EIP and CPF framing.  That framing was
 * the only thing that ever differed between the connected and unconnected forms of this
 * function.
 */
static int raw_tag_build_write_request(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    cip_request_p req = NULL;
    int payload_size = tag->size;
    int max_payload = 0;
    debug_module_t dbg = tag->debug_module;

    pdebug(dbg, DEBUG_INFO, tag->tag_id, "Starting.");

    if(payload_size <= 0) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Raw tag has no CIP request to send!");
        return PLCTAG_ERR_NO_DATA;
    }

    max_payload = cip_conn_max_cip_payload(tag->session);

    if(payload_size > max_payload) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Request of %d bytes exceeds the %d bytes this connection can carry!",
               payload_size, max_payload);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(dbg, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  Error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    mem_copy(req->data, tag->data, payload_size);

    /* reset the tag size so that incoming data overwrites the old. */
    tag->size = 0;

    /*
     * Hand the finished request to the connection, and only then publish it on the tag.
     * cip_submit_payload() releases the request if it fails, so a tag that already held a
     * pointer to it would be left holding a dangling one.
     */
    rc = cip_submit_payload(tag->session, req, payload_size, tag->allow_packing);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Unable to submit the request, %s!", plc_tag_decode_error(rc));
        return rc;
    }

    tag->req = req;

    pdebug(dbg, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}
