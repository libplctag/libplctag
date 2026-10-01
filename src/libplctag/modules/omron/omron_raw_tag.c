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

#include <ctype.h>
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/tag.h>
#include <libplctag/modules/omron/cip.h>
#include <libplctag/modules/omron/conn.h>
#include <libplctag/modules/omron/defs.h>
#include <libplctag/modules/omron/omron_common.h>
#include <libplctag/modules/omron/omron_raw_tag.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/vector.h>

/* raw tag functions */
// static int raw_tag_read_start(cip_tag_p tag);
static int raw_tag_tickler(cip_tag_p tag);
static int raw_tag_write_start(cip_tag_p tag);
static int raw_tag_check_write_status(cip_tag_p tag);
static int raw_tag_build_write_request(cip_tag_p tag);


/* define the vtable for raw tag type. */
static struct tag_vtable_t omron_raw_tag_vtable = {
    .abort = (tag_vtable_func)omron_tag_abort,
    .read = NULL,
    .status = (tag_vtable_func)omron_tag_status,
    .tickler = (tag_vtable_func)raw_tag_tickler,
    .write = (tag_vtable_func)raw_tag_write_start,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .attribs = omron_attribs,
};

// tag_byte_order_t omron_tag_listing_byte_order = {
//     .is_allocated = 0,

//     .int16_order = {0,1},
//     .int32_order = {0,1,2,3},
//     .int64_order = {0,1,2,3,4,5,6,7},
//     .float32_order = {0,1,2,3},
//     .float64_order = {0,1,2,3,4,5,6,7},

//     .str_is_defined = 1,
//     .str_is_counted = 1,
//     .str_is_fixed_length = 0,
//     .str_is_zero_terminated = 0,
//     .str_is_byte_swapped = 0,

//     .str_count_word_bytes = 2,
//     .str_max_capacity = 0,
//     .str_total_length = 0,
//     .str_pad_bytes = 0
// };


/*************************************************************************
 ************************** Raw Tag Functions ****************************
 ************************************************************************/


int omron_setup_raw_tag(cip_tag_p tag) {
    pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_DETAIL, tag->tag_id, "Starting.");

    /* set up raw tag. */
    tag->special_tag = 1;
    tag->elem_type = CIP_TYPE_TAG_RAW;
    tag->elem_count = 1;
    tag->elem_size = 1;

    tag->byte_order = &omron_njnx_tag_byte_order;

    pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_DETAIL, tag->tag_id, "Setting vtable to %p.", &omron_raw_tag_vtable);

    tag->vtable = &omron_raw_tag_vtable;

    pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


int raw_tag_tickler(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = cip_check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_WARN, tag->tag_id, "Something started a read on a raw tag.  This is not supported!");
        tag->read_in_progress = 0;
        tag->read_in_flight = 0;

        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(tag->write_in_progress) {
        rc = raw_tag_check_write_status(tag);

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->write_in_progress) {
            pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_DETAIL, tag->tag_id, "Write complete.");
            tag->write_complete = 1;
        }

        pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_SPEW, tag->tag_id, "Done.");

        return rc;
    }

    pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_SPEW, tag->tag_id, "Done.  No operation in progress.");

    return tag->status;
}


/*
 * raw_tag_write_start
 *
 * This must be called from one thread alone, or while the tag mutex is
 * locked.
 *
 * The routine starts the process of writing to a tag.
 */

int raw_tag_write_start(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_WARN, tag->tag_id, "Raw tag found with a read in flight!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* the write is now in flight */
    tag->write_in_progress = 1;

    rc = raw_tag_build_write_request(tag);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_WARN, tag->tag_id, "Unable to build write request!");
        tag->write_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * raw_tag_check_write_status
 *
 * This routine must be called with the tag mutex locked.  It checks the current
 * status of a write operation.  If the write is done, it triggers the clean up.
 *
 * The connection hands back the CIP response with the EIP and CPF framing already
 * stripped, so nothing here depends on whether the request went out connected or
 * unconnected.
 */

static int raw_tag_check_write_status(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    int data_size = 0;
    uint8_t *tag_data_buffer = NULL;

    pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* write is done in one way or another. */
    tag->write_in_progress = 0;

    data_size = tag->req->request_size;

    if(data_size <= 0) {
        pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_WARN, tag->tag_id, "Response carries no CIP data!");
        omron_tag_abort(tag);
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* hand the whole CIP response back to the caller. */
    tag_data_buffer = mem_realloc(tag->data, data_size);

    if(tag_data_buffer) {
        tag->data = tag_data_buffer;
        tag->size = data_size;

        mem_copy(tag->data, tag->req->data, data_size);
    } else {
        pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data buffer!");
        rc = PLCTAG_ERR_NO_MEM;
    }

    /* clean up the request. */
    omron_tag_abort(tag);

    pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_INFO, tag->tag_id, "Done.");

    return rc;
}


/*
 * The caller supplies the entire CIP request in the tag's data buffer, so the request is
 * that buffer verbatim and the connection adds the EIP and CPF framing.  That framing was
 * the only thing that ever differed between the connected and unconnected forms of this
 * function.
 */

int raw_tag_build_write_request(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    cip_request_p req = NULL;
    int payload_size = tag->size;
    int max_payload = 0;

    pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_INFO, tag->tag_id, "Starting.");

    if(payload_size <= 0) {
        pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_WARN, tag->tag_id, "Raw tag has no CIP request to send!");
        return PLCTAG_ERR_NO_DATA;
    }

    max_payload = cip_conn_max_cip_payload(tag->session);

    if(payload_size > max_payload) {
        pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_WARN, tag->tag_id,
               "Request of %d bytes exceeds the %d bytes this connection can carry!", payload_size, max_payload);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    mem_copy(req->data, tag->data, payload_size);

    /* reset the tag size so that incoming data overwrites the old. */
    tag->size = 0;

    /* hand the finished request to the connection. */
    rc = cip_submit_payload(tag->session, req, payload_size, tag->allow_packing);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_WARN, tag->tag_id, "Unable to submit the request, %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_OMRON_RAW_TAG, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}
