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
#include <libplctag/modules/ab/ab_common.h>
#include <libplctag/modules/ab/cip.h>
#include <libplctag/modules/ab/pccc.h>
#include <libplctag/modules/ab/session.h>
#include <libplctag/modules/cip/error_codes.h>
#include <libplctag/modules/cip/services.h>
#include <libplctag/modules/cip/tag.h>
#include <libplctag/modules/cip/wire.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/byteorder.h>
#include <utils/debug.h>
#include <utils/vector.h>

/*
 * size of the fixed-size fields preceding the CIP response in an identity reply:
 * interface_handle (one uint32_le) followed by timeout, item_count, NAI type, NAI length,
 * UDI type, and UDI length (six uint16_le fields).
 */
/* CIP Get_Attributes_All, the service that reads the whole Identity object. */
#define AB_CIP_GET_ATTRIBUTES_ALL ((uint8_t)0x01)

/******************************************************************
 ******************* identity tag functions ***********************
 ******************************************************************/

/* identity tag functions */
static int identity_tag_read_start(cip_tag_p tag);
static int identity_tag_tickler(cip_tag_p tag);
static int identity_tag_check_read_status_unconnected(cip_tag_p tag);
static int identity_tag_build_read_request_unconnected(cip_tag_p tag);


/* define the vtable for identity tag type. */
static struct tag_vtable_t identity_tag_vtable = {.abort = (tag_vtable_func)cip_tag_abort_request,
                                                  .read = (tag_vtable_func)identity_tag_read_start,
                                                  .status = (tag_vtable_func)ab_tag_status,
                                                  .tickler = (tag_vtable_func)identity_tag_tickler,
                                                  .write = (tag_vtable_func)NULL,
                                                  .wake_plc = (tag_vtable_func)NULL,

                                                  /* attribute accessors */
                                                  .attribs = ab_attribs};


int setup_identity_tag(cip_tag_p tag) {
    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Starting.");

    /* set up identity tag */
    tag->special_tag = 1;
    tag->elem_type = CIP_TYPE_TAG_IDENTITY;
    tag->elem_count = 1;
    tag->elem_size = 1;

    tag->byte_order = &logix_tag_byte_order;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Setting vtable to %p.", &identity_tag_vtable);

    tag->vtable = &identity_tag_vtable;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


int identity_tag_read_start(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "A write is in progress on an identity tag!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Read operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* mark the tag read in progress */
    tag->read_in_progress = 1;

    /*
     * An identity tag is always unconnected.  ab_common.c only recognises "@identity" under
     * the generic PLC type, and that type sets use_connected_msg to zero unconditionally, so
     * there is no configuration that reaches a connected identity request.
     */
    rc = identity_tag_build_read_request_unconnected(tag);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to build read request!");
        tag->read_in_progress = 0;
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int identity_tag_tickler(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = cip_check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Write attempted on identity tag. Not supported!");

        cip_tag_abort_request(tag);
        tag->write_complete = 1;

        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(tag->read_in_progress) {
        rc = identity_tag_check_read_status_unconnected(tag);

        tag->status = (int8_t)rc;

        if(!tag->read_in_progress) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Read complete.");
            tag->read_complete = 1;
        } else {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Read in progress.");
        }

        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "No operation in progress.");

    return tag->status;
}


/*
 * Build the request that reads the Identity object.
 *
 * The CIP message is the same either way; what differs is how it is addressed.  With a
 * connection path the request has to be routed onward, so the transport wraps it in an
 * Unconnected Send and appends the path.  Without one it is addressed to the device at the
 * gateway itself and the CPF carries it directly.  Both framings live in the transport now,
 * so all this has to do is say which.
 */
int identity_tag_build_read_request_unconnected(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    cip_request_p req = NULL;
    uint8_t *data = NULL;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to get new request. Error %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    data = req->data;

    /*
     * Get_Attributes_All against the Identity object:
     *
     *   uint8_t  0x01  service, Get_Attributes_All
     *   uint8_t  0x02  path size in 16-bit words
     *   uint8_t  0x20  class segment
     *   uint8_t  0x01  Identity object
     *   uint8_t  0x24  instance segment
     *   uint8_t  0x01  instance 1
     */
    *data = 0x01;
    data++;
    *data = 0x02;
    data++;
    *data = 0x20;
    data++;
    *data = 0x01;
    data++;
    *data = 0x24;
    data++;
    *data = 0x01;
    data++;

    if(tag->session->conn_path_size > 0) {
        rc = cip_submit_payload(tag->session, req, (int)(data - req->data), false);
    } else {
        rc = cip_submit_unrouted_payload(tag->session, req, (int)(data - req->data));
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to submit the request, %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    /* save the request for later */
    critical_block(tag->api_mutex) { tag->req = req; }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


int identity_tag_check_read_status_unconnected(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    uint8_t *cip_response = tag->req->data;
    uint8_t *data_end = tag->req->data + tag->req->request_size;
    uint8_t reply_service = 0;
    uint8_t cip_status = 0;
    int data_size = 0;
    uint8_t *tag_data_buffer = NULL;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    /*
     * The connection hands back the CIP reply with the EIP and CPF framing already checked
     * and stripped, so this starts at the reply service byte.  What is left to unpick is that
     * a routed request comes back inside an Unconnected Send reply and an unrouted one does
     * not, so there may be one four-byte CIP reply header here or two.
     */
    if((data_end - cip_response) < 4) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Identity response is too short for a CIP reply!");
        cip_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_SMALL;
    }

    reply_service = cip_response[0];

    if(reply_service == (uint8_t)(CIP_EIP_CMD_UNCONNECTED_SEND | CIP_SVC_REPLY)) {
        cip_status = cip_response[2];

        if(cip_status != CIP_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unconnected Send CIP status is not OK: 0x%02x",
                   cip_status);
            cip_tag_abort_request(tag);
            return PLCTAG_ERR_REMOTE_ERR;
        }

        /* step over the Unconnected Send reply header to the embedded one. */
        cip_response += 4;

        if((data_end - cip_response) < 4) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
                   "Identity response is too short for the embedded CIP reply!");
            cip_tag_abort_request(tag);
            return PLCTAG_ERR_TOO_SMALL;
        }

        reply_service = cip_response[0];
    }

    if(reply_service != (uint8_t)(AB_CIP_GET_ATTRIBUTES_ALL | CIP_SVC_REPLY)) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "CIP response service unexpected: 0x%02x (expected 0x%02x)", reply_service,
               (unsigned int)(AB_CIP_GET_ATTRIBUTES_ALL | CIP_SVC_REPLY));
        cip_tag_abort_request(tag);
        return PLCTAG_ERR_BAD_DATA;
    }

    cip_status = cip_response[2];

    if(cip_status != CIP_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "CIP status is not OK: 0x%02x", cip_status);
        cip_tag_abort_request(tag);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* the identity data follows the four byte CIP reply header. */
    cip_response += 4;
    data_size = (int)(data_end - cip_response);

    if(data_size <= 0) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Identity response carries no data!");
        cip_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_SMALL;
    }

    tag_data_buffer = mem_realloc(tag->data, data_size);

    if(tag_data_buffer) {
        tag->data = tag_data_buffer;
        tag->size = data_size;
        mem_copy(tag->data, cip_response, data_size);

        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Copied %d bytes of identity data.", data_size);
    } else {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data buffer!");
        rc = PLCTAG_ERR_NO_MEM;
    }

    /* clean up the request */
    cip_tag_abort_request(tag);

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}
