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
#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/cip/error_codes.h>
#include <libplctag/modules/cip/identity_tag.h>
#include <libplctag/modules/cip/services.h>
#include <libplctag/modules/cip/tag.h>
#include <libplctag/modules/cip/wire.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/byteorder.h>
#include <utils/debug.h>
#include <utils/vector.h>

/******************************************************************
 ******************* identity tag functions ***********************
 ******************************************************************/

/* identity tag functions */


int cip_identity_tag_setup(cip_tag_p tag, tag_vtable_p vtable, debug_module_t debug_module) {
    /*
     * The Identity object's product name is a CIP SHORT_STRING.  This used to borrow the
     * Logix STRING UDT's byte order, whose four-byte count word made plc_tag_get_string()
     * read a length of hundreds of millions and fail the range check -- the name was only
     * ever readable by picking it apart a byte at a time.
     */
    /* with no path the request goes to the device at the gateway itself. */
    tag->unrouted = (tag->session->conn_path_size == 0);

    return cip_setup_special_tag(tag, CIP_TYPE_TAG_IDENTITY, &cip_short_string_byte_order, vtable, debug_module);
}


/* the request travels however the connection does, connected or not. */
int cip_identity_tag_read_start(cip_tag_p tag) {
    return cip_readonly_read_start(tag, tag->debug_module, cip_identity_tag_encode_read);
}


int cip_identity_tag_tickler(cip_tag_p tag) {
    return cip_readonly_tickler(tag, tag->debug_module, cip_identity_tag_decode_read);
}


/*
 * Encode the request that reads the Identity object.
 *
 * The CIP message is the same every way; what differs is how it is addressed.  A connected
 * connection carries it as it does any request.  Otherwise, with a connection path, the
 * transport wraps it in an Unconnected Send and appends the path, and without one it is
 * addressed to the device at the gateway itself, which tag->unrouted records.
 */
int cip_identity_tag_encode_read(cip_tag_p tag, uint8_t *dest, int32_t capacity, tag_op_plan_t *plan) {
    uint8_t *data = dest;

    pdebug(tag->debug_module, DEBUG_DETAIL, tag->tag_id, "Starting.");

    if(capacity < 6) {
        pdebug(tag->debug_module, DEBUG_WARN, tag->tag_id, "The 6 byte Identity request does not fit in %d bytes!", capacity);
        return PLCTAG_ERR_TOO_SMALL;
    }

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

    plan->req_size = (int32_t)(data - dest);
    plan->reply_size = 0;
    plan->packable = false;

    pdebug(tag->debug_module, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


int cip_identity_tag_decode_read(cip_tag_p tag, uint8_t *reply, int32_t reply_size) {
    int rc = PLCTAG_STATUS_OK;
    uint8_t *cip_response = reply;
    uint8_t *data_end = reply + reply_size;
    uint8_t reply_service = 0;
    uint8_t cip_status = 0;
    int data_size = 0;
    uint8_t *tag_data_buffer = NULL;

    pdebug(tag->debug_module, DEBUG_SPEW, tag->tag_id, "Starting.");

    /*
     * The connection hands back the CIP reply with the EIP and CPF framing already checked
     * and stripped, so this starts at the reply service byte.  What is left to unpick is that
     * a routed request comes back inside an Unconnected Send reply and an unrouted one does
     * not, so there may be one four-byte CIP reply header here or two.
     */
    if((data_end - cip_response) < 4) {
        pdebug(tag->debug_module, DEBUG_WARN, tag->tag_id, "Identity response is too short for a CIP reply!");
        cip_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_SMALL;
    }

    reply_service = cip_response[0];

    if(reply_service == (uint8_t)(CIP_EIP_CMD_UNCONNECTED_SEND | CIP_SVC_REPLY)) {
        cip_status = cip_response[2];

        if(cip_status != CIP_STATUS_OK) {
            pdebug(tag->debug_module, DEBUG_WARN, tag->tag_id, "Unconnected Send CIP status is not OK: 0x%02x",
                   cip_status);
            cip_tag_abort_request(tag);
            return PLCTAG_ERR_REMOTE_ERR;
        }

        /* step over the Unconnected Send reply header to the embedded one. */
        cip_response += 4;

        if((data_end - cip_response) < 4) {
            pdebug(tag->debug_module, DEBUG_WARN, tag->tag_id,
                   "Identity response is too short for the embedded CIP reply!");
            cip_tag_abort_request(tag);
            return PLCTAG_ERR_TOO_SMALL;
        }

        reply_service = cip_response[0];
    }

    if(reply_service != (uint8_t)(CIP_GET_ATTRIBUTES_ALL | CIP_SVC_REPLY)) {
        pdebug(tag->debug_module, DEBUG_WARN, tag->tag_id,
               "CIP response service unexpected: 0x%02x (expected 0x%02x)", reply_service,
               (unsigned int)(CIP_GET_ATTRIBUTES_ALL | CIP_SVC_REPLY));
        cip_tag_abort_request(tag);
        return PLCTAG_ERR_BAD_DATA;
    }

    cip_status = cip_response[2];

    if(cip_status != CIP_STATUS_OK) {
        pdebug(tag->debug_module, DEBUG_WARN, tag->tag_id, "CIP status is not OK: 0x%02x", cip_status);
        cip_tag_abort_request(tag);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* the identity data follows the four byte CIP reply header. */
    cip_response += 4;
    data_size = (int)(data_end - cip_response);

    if(data_size <= 0) {
        pdebug(tag->debug_module, DEBUG_WARN, tag->tag_id, "Identity response carries no data!");
        cip_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_SMALL;
    }

    tag_data_buffer = mem_realloc(tag->data, data_size);

    if(tag_data_buffer) {
        tag->data = tag_data_buffer;
        tag->size = data_size;
        mem_copy(tag->data, cip_response, data_size);

        pdebug(tag->debug_module, DEBUG_DETAIL, tag->tag_id, "Copied %d bytes of identity data.", data_size);
    } else {
        pdebug(tag->debug_module, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data buffer!");
        rc = PLCTAG_ERR_NO_MEM;
    }

    /* clean up the request */
    cip_tag_abort_request(tag);

    pdebug(tag->debug_module, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}
