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
#include <libplctag/modules/ab/defs.h>
#include <libplctag/modules/ab/eip_cip.h> /* for the Logix decode types. */
#include <libplctag/modules/ab/eip_cip_special.h>
#include <libplctag/modules/cip/error_codes.h>
#include <libplctag/modules/ab/session.h>
#include <libplctag/modules/ab/tag.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/vector.h>

/* raw tag functions */
// static int raw_tag_read_start(ab_tag_p tag);
static int raw_tag_tickler(ab_tag_p tag);
static int raw_tag_write_start(ab_tag_p tag);
static int raw_tag_check_write_status_connected(ab_tag_p tag);
static int raw_tag_check_write_status_unconnected(ab_tag_p tag);
static int raw_tag_build_write_request_connected(ab_tag_p tag);
static int raw_tag_build_write_request_unconnected(ab_tag_p tag);

/* define the vtable for raw tag type. */
static struct tag_vtable_t raw_tag_vtable = {
    .abort = (tag_vtable_func)ab_tag_abort_request,
    .read = NULL,
    .status = (tag_vtable_func)ab_tag_status,
    .tickler = (tag_vtable_func)raw_tag_tickler,
    .write = (tag_vtable_func)raw_tag_write_start,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .attribs = ab_attribs,
};

/*************************************************************************
 **************************** API Functions ******************************
 ************************************************************************/


/* Raw tag functions */


int setup_raw_tag(ab_tag_p tag) {
    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Starting.");

    /* set up raw tag. */
    tag->special_tag = 1;
    tag->elem_type = CIP_TYPE_TAG_RAW;
    tag->elem_count = 1;
    tag->elem_size = 1;

    tag->byte_order = &logix_tag_byte_order;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Setting vtable to %p.", &raw_tag_vtable);

    tag->vtable = &raw_tag_vtable;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


int raw_tag_tickler(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Something started a read on a raw tag.  This is not supported!");

        ab_tag_abort_request(tag);

        /* fire the event anyway */
        tag->read_complete = 1;

        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(tag->write_in_progress) {
        if(tag->use_connected_msg) {
            rc = raw_tag_check_write_status_connected(tag);
        } else {
            rc = raw_tag_check_write_status_unconnected(tag);
        }

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->write_in_progress) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Write complete.");
            tag->write_complete = 1;
        } else {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Write in progress.");
        }

        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.");

        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.  No operation in progress.");

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

int raw_tag_write_start(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Raw tag found with a read in flight!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* the write is now in flight */
    tag->write_in_progress = 1;

    if(tag->use_connected_msg) {
        rc = raw_tag_build_write_request_connected(tag);
    } else {
        rc = raw_tag_build_write_request_unconnected(tag);
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to build write request!");
        tag->write_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * raw_tag_check_write_status_connected
 *
 * This routine must be called with the tag mutex locked.  It checks the current
 * status of a write operation.  If the write is done, it triggers the clean up.
 */

int raw_tag_check_write_status_connected(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp *cip_resp;
    uint8_t *data_start = NULL;
    uint8_t *data_end = NULL;
    int data_size = 0;
    uint8_t *tag_data_buffer = NULL;


    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* if we got here, there is a response and status is OK. */

    /* point to the data */
    cip_resp = (eip_cip_co_resp *)(tag->req->data);

    /* copy the data into the tag. */
    data_start = (uint8_t *)(&cip_resp->reply_service);
    data_end = tag->req->data + (tag->req->request_size);

    if((intptr_t)data_end < (intptr_t)data_start) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Response is shorter than the CIP response header!");
        ab_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_SMALL;
    }

    data_size = (int)(unsigned int)(data_end - data_start);

    tag_data_buffer = mem_realloc(tag->data, data_size);

    if(tag_data_buffer) {
        tag->data = tag_data_buffer;
        tag->size = data_size;

        mem_copy(tag->data, data_start, data_size);
    } else {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data buffer!");
        rc = PLCTAG_ERR_NO_MEM;
    }

    /* clean up regardless */
    ab_tag_abort_request(tag);

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


/*
 * raw_tag_check_write_status_unconnected
 *
 * This routine must be called with the tag mutex locked.  It checks the current
 * status of a write operation.  If the write is done, it triggers the clean up.
 */

int raw_tag_check_write_status_unconnected(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_uc_resp *cip_resp = NULL;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* if we got here, there is a response and status is OK. */

    cip_resp = (eip_cip_uc_resp *)(tag->req->data);

    /* copy the data into the tag. */
    uint8_t *data_start = (uint8_t *)(&cip_resp->reply_service);
    uint8_t *data_end = tag->req->data + tag->req->request_size;

    if((intptr_t)data_end < (intptr_t)data_start) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Response is shorter than the CIP response header!");
        ab_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_SMALL;
    }

    int data_size = (int)(unsigned int)(data_end - data_start);
    uint8_t *tag_data_buffer = mem_realloc(tag->data, data_size);

    if(tag_data_buffer) {
        tag->data = tag_data_buffer;
        tag->size = data_size;

        mem_copy(tag->data, data_start, data_size);
    } else {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data buffer!");
        rc = PLCTAG_ERR_NO_MEM;
    }

    /* clean up the request. */
    ab_tag_abort_request(tag);

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


int raw_tag_build_write_request_connected(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_req *cip = NULL;
    uint8_t *data = NULL;
    size_t required_space = 0;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &tag->req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  Error %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    /* how much space do we need? */
    required_space = (size_t)tag->size + sizeof(*cip);

    if(required_space > (size_t)tag->req->request_capacity) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Amount to write, %zu bytes, exceeds request capacity %d bytes!", required_space, tag->req->request_capacity);
        return PLCTAG_ERR_TOO_LARGE;
    }

    cip = (eip_cip_co_req *)(tag->req->data);

    /* point to the end of the struct */
    data = (tag->req->data) + sizeof(eip_cip_co_req);

    /*
     * set up the embedded CIP request packet.  The user/client needs
     * to set up the entire CIP request.   We just copy it here.
     */

    /* copy the tag data into the request */
    mem_copy(data, tag->data, tag->size);
    data += tag->size;

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(AB_EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI); /* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);             /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI); /* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&cip->cpf_conn_seq_num))); /* REQ: fill in with length of remaining data. */

    /* Check if the payload size exceeds available space before setting request_size */
    int packet_payload_size = (int)(data - (uint8_t *)(&cip->cpf_conn_seq_num));
    int available_payload = session_get_available_cip_payload_space(tag->session);

    if(packet_payload_size > available_payload) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Request payload (%d bytes) exceeds available space (%d bytes)!", packet_payload_size, available_payload);
        ab_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* set the size of the request */
    tag->req->request_size = (int)(data - (tag->req->data));

    /* allow packing if the tag allows it. */
    tag->req->allow_packing = tag->allow_packing;

    /* reset the tag size so that incoming data overwrites the old. */
    tag->size = 0;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, tag->req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! Error %s",
               plc_tag_decode_error(rc));

        ab_tag_abort_request(tag);
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done");

    return rc;
}


int raw_tag_build_write_request_unconnected(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_uc_req *cip = NULL;
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    uint8_t *embed_end = NULL;
    size_t required_space = 0;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &tag->req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  Error %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    /* how much space do we need? */
    required_space = (size_t)tag->size + sizeof(eip_cip_uc_req) + (size_t)tag->session->conn_path_size;

    if(required_space > (size_t)tag->req->request_capacity) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Amount to write, %zu bytes, exceeds request capacity %d bytes!", required_space, tag->req->request_capacity);
        return PLCTAG_ERR_TOO_LARGE;
    }

    cip = (eip_cip_uc_req *)(tag->req->data);

    /* point to the end of the struct */
    data = (tag->req->data) + sizeof(eip_cip_uc_req);

    embed_start = data;

    /*
     * set up the embedded CIP read packet
     * The format is:
     *
     * uint8_t cmd
     * LLA formatted name
     * data type to write
     * uint16_t # of elements to write
     * data to write
     */

    /*
     * set up the embedded CIP request packet.  The user/client needs
     * to set up the entire CIP request.   We just copy it here.
     */

    /* copy the tag data into the request */
    mem_copy(data, tag->data, tag->size);
    data += tag->size;

    /* now we go back and fill in the fields of the static part */

    /* mark the end of the embedded packet */
    embed_end = data;

    /*
     * after the embedded packet, we need to tell the message router
     * how to get to the target device.
     */

    /* Now copy in the routing information for the embedded message */
    *data = (tag->session->conn_path_size) / 2; /* in 16-bit words */
    data++;
    *data = 0;
    data++; /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* encap fields */
    cip->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND); /* ALWAYS 0x006F Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI); /* ALWAYS 0 */
    cip->cpf_nai_item_length = h2le16(0);             /* ALWAYS 0 */
    cip->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI); /* ALWAYS 0x00B2 - Unconnected Data Item */
    cip->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&(cip->cm_service_code)))); /* REQ: fill in with length of remaining data. */

    /* CM Service Request - Connection Manager */
    cip->cm_service_code = AB_EIP_CMD_UNCONNECTED_SEND; /* 0x52 Unconnected Send */
    cip->cm_req_path_size = 2;                          /* 2, size in 16-bit words of path, next field */
    cip->cm_req_path[0] = 0x20;                         /* class */
    cip->cm_req_path[1] = 0x06;                         /* Connection Manager */
    cip->cm_req_path[2] = 0x24;                         /* instance */
    cip->cm_req_path[3] = 0x01;                         /* instance 1 */

    /* Unconnected send needs timeout information */
    cip->secs_per_tick = AB_EIP_SECS_PER_TICK; /* seconds per tick */
    cip->timeout_ticks = AB_EIP_TIMEOUT_TICKS; /* timeout = srd_secs_per_tick * src_timeout_ticks */

    /* size of embedded packet */
    cip->uc_cmd_length = h2le16((uint16_t)(embed_end - embed_start));

    /* Check if the payload size exceeds available space before setting request_size */
    int packet_payload_size = (int)(embed_end - embed_start);
    int available_payload = session_get_available_cip_payload_space(tag->session);

    if(packet_payload_size > available_payload) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Request payload (%d bytes) exceeds available space (%d bytes)!", packet_payload_size, available_payload);
        ab_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* set the size of the request */
    tag->req->request_size = (int)(data - (tag->req->data));

    /* allow packing if the tag allows it. */
    tag->req->allow_packing = tag->allow_packing;

    /* reset the tag size so that incoming data overwrites the old. */
    tag->size = 0;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, tag->req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! Error %s",
               plc_tag_decode_error(rc));
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}
