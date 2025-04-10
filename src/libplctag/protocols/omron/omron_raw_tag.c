/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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
#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/omron/cip.h>
#include <libplctag/protocols/omron/conn.h>
#include <libplctag/protocols/omron/defs.h>
#include <libplctag/protocols/omron/omron_common.h>
#include <libplctag/protocols/omron/omron_raw_tag.h>
#include <libplctag/protocols/omron/omron_standard_tag.h> /* for the Logix decode types. */
#include <libplctag/protocols/omron/tag.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/vector.h>

/* raw tag functions */
// static int raw_tag_read_start(omron_tag_p tag);
static int raw_tag_tickler(omron_tag_p tag);
static int raw_tag_write_start(omron_tag_p tag);
static int raw_tag_check_write_status_connected(omron_tag_p tag);
static int raw_tag_check_write_status_unconnected(omron_tag_p tag);
static int raw_tag_build_write_request_connected(omron_tag_p tag);
static int raw_tag_build_write_request_unconnected(omron_tag_p tag);


/* define the vtable for raw tag type. */
static struct tag_vtable_t omron_raw_tag_vtable = {(tag_vtable_func)omron_tag_abort,  /* shared */
                                                   (tag_vtable_func)NULL,             /* read */
                                                   (tag_vtable_func)omron_tag_status, /* shared */
                                                   (tag_vtable_func)raw_tag_tickler, (tag_vtable_func)raw_tag_write_start,
                                                   (tag_vtable_func)NULL, /* wake_plc */

                                                   /* attribute accessors */
                                                   omron_get_int_attrib, omron_set_int_attrib,

                                                   omron_get_byte_array_attrib};

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


int omron_setup_raw_tag(omron_tag_p tag) {
    pdebug(DEBUG_DETAIL, "Starting.");

    /* set up raw tag. */
    tag->special_tag = 1;
    tag->elem_type = OMRON_TYPE_TAG_RAW;
    tag->elem_count = 1;
    tag->elem_size = 1;

    tag->byte_order = &omron_njnx_tag_byte_order;

    pdebug(DEBUG_DETAIL, "Setting vtable to %p.", &omron_raw_tag_vtable);

    tag->vtable = &omron_raw_tag_vtable;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int raw_tag_tickler(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    rc = omron_check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->read_in_progress) {
        pdebug(DEBUG_WARN, "Something started a read on a raw tag.  This is not supported!");
        tag->read_in_progress = 0;
        tag->read_in_flight = 0;

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
            pdebug(DEBUG_DETAIL, "Write complete.");
            tag->write_complete = 1;
        }

        pdebug(DEBUG_SPEW, "Done.");

        return rc;
    }

    pdebug(DEBUG_SPEW, "Done.  No operation in progress.");

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

int raw_tag_write_start(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    if(tag->read_in_progress) {
        pdebug(DEBUG_WARN, "Raw tag found with a read in flight!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->write_in_progress) {
        pdebug(DEBUG_WARN, "Read or write operation already in flight!");
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
        pdebug(DEBUG_WARN, "Unable to build write request!");
        tag->write_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * raw_tag_check_write_status_connected
 *
 * This routine must be called with the tag mutex locked.  It checks the current
 * status of a write operation.  If the write is done, it triggers the clean up.
 */

static int raw_tag_check_write_status_connected(omron_tag_p tag) {
    eip_cip_co_resp *cip_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    /* the request reference is valid. */

    /* point to the data */
    cip_resp = (eip_cip_co_resp *)(tag->req->data);

    /* write is done in one way or another. */
    tag->write_in_progress = 0;

    if(rc == PLCTAG_STATUS_OK) {
        /* copy the data into the tag. */
        uint8_t *data_start = (uint8_t *)(&cip_resp->reply_service);
        uint8_t *data_end = tag->req->data + (tag->req->request_size);
        int data_size = (int)(unsigned int)(data_end - data_start);
        uint8_t *tag_data_buffer = mem_realloc(tag->data, data_size);

        if(tag_data_buffer) {
            tag->data = tag_data_buffer;
            tag->size = data_size;

            mem_copy(tag->data, data_start, data_size);
        } else {
            pdebug(DEBUG_WARN, "Unable to reallocate tag data buffer!");
            rc = PLCTAG_ERR_NO_MEM;
        }
    } else {
        pdebug(DEBUG_WARN, "Write failed!");

        tag->offset = 0;
    }

    /* clean up the request. */
    omron_tag_abort(tag);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


/*
 * raw_tag_check_write_status_unconnected
 *
 * This routine must be called with the tag mutex locked.  It checks the current
 * status of a write operation.  If the write is done, it triggers the clean up.
 */

static int raw_tag_check_write_status_unconnected(omron_tag_p tag) {
    eip_cip_uc_resp *cip_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /* the request reference is valid. */

    /* point to the data */
    cip_resp = (eip_cip_uc_resp *)(tag->req->data);

    /* write is done in one way or another. */
    tag->write_in_progress = 0;

    if(rc == PLCTAG_STATUS_OK) {
        /* copy the data into the tag. */
        uint8_t *data_start = (uint8_t *)(&cip_resp->reply_service);
        uint8_t *data_end = data_start + le2h16(cip_resp->cpf_udi_item_length);
        int data_size = (int)(unsigned int)(data_end - data_start);
        uint8_t *tag_data_buffer = mem_realloc(tag->data, data_size);

        if(tag_data_buffer) {
            tag->data = tag_data_buffer;
            tag->size = data_size;

            mem_copy(tag->data, data_start, data_size);
        } else {
            pdebug(DEBUG_WARN, "Unable to reallocate tag data buffer!");
            rc = PLCTAG_ERR_NO_MEM;
        }
    } else {
        pdebug(DEBUG_WARN, "Write failed!");

        tag->offset = 0;
    }

    /* clean up the request. */
    omron_tag_abort(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


int raw_tag_build_write_request_connected(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_req *cip = NULL;
    uint8_t *data = NULL;
    omron_request_p req = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = conn_create_request(tag->conn, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    if(tag->size > conn_get_max_payload(tag->conn)) {
        pdebug(DEBUG_WARN, "Amount to write exceeds negotiated conn size %d!", conn_get_max_payload(tag->conn));
        return PLCTAG_ERR_TOO_LARGE;
    }

    cip = (eip_cip_co_req *)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_co_req);

    /*
     * set up the embedded CIP request packet.  The user/client needs
     * to set up the entire CIP request.   We just copy it here.
     */

    /* copy the tag data into the request */
    mem_copy(data, tag->data, tag->size);
    data += tag->size;

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(OMRON_EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                     /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(OMRON_EIP_ITEM_CAI); /* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);                /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(OMRON_EIP_ITEM_CDI); /* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&cip->cpf_conn_seq_num))); /* REQ: fill in with length of remaining data. */

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));

    /* allow packing if the tag allows it. */
    req->allow_packing = tag->allow_packing;

    /* reset the tag size so that incoming data overwrites the old. */
    tag->size = 0;

    /* add the request to the conn's list. */
    rc = conn_add_request(tag->conn, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to conn! rc=%d", rc);
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to request of tag %" PRId32 ".", tag->tag_id);
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}


int raw_tag_build_write_request_unconnected(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_uc_req *cip = NULL;
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    uint8_t *embed_end = NULL;
    omron_request_p req = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = conn_create_request(tag->conn, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    cip = (eip_cip_uc_req *)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_uc_req);

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
    *data = (tag->conn->conn_path_size) / 2; /* in 16-bit words */
    data++;
    *data = 0;
    data++; /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* encap fields */
    cip->encap_command = h2le16(OMRON_EIP_UNCONNECTED_SEND); /* ALWAYS 0x006F Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                     /* ALWAYS 2 */
    cip->cpf_nai_item_type = h2le16(OMRON_EIP_ITEM_NAI); /* ALWAYS 0 */
    cip->cpf_nai_item_length = h2le16(0);                /* ALWAYS 0 */
    cip->cpf_udi_item_type = h2le16(OMRON_EIP_ITEM_UDI); /* ALWAYS 0x00B2 - Unconnected Data Item */
    cip->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&(cip->cm_service_code)))); /* REQ: fill in with length of remaining data. */

    /* CM Service Request - Connection Manager */
    cip->cm_service_code = OMRON_EIP_CMD_UNCONNECTED_SEND; /* 0x52 Unconnected Send */
    cip->cm_req_path_size = 2;                             /* 2, size in 16-bit words of path, next field */
    cip->cm_req_path[0] = 0x20;                            /* class */
    cip->cm_req_path[1] = 0x06;                            /* Connection Manager */
    cip->cm_req_path[2] = 0x24;                            /* instance */
    cip->cm_req_path[3] = 0x01;                            /* instance 1 */

    /* Unconnected send needs timeout information */
    cip->secs_per_tick = OMRON_EIP_SECS_PER_TICK; /* seconds per tick */
    cip->timeout_ticks = OMRON_EIP_TIMEOUT_TICKS; /* timeout = srd_secs_per_tick * src_timeout_ticks */

    /* size of embedded packet */
    cip->uc_cmd_length = h2le16((uint16_t)(embed_end - embed_start));

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));

    /* allow packing if the tag allows it. */
    req->allow_packing = tag->allow_packing;

    /* reset the tag size so that incoming data overwrites the old. */
    tag->size = 0;

    /* add the request to the conn's list. */
    rc = conn_add_request(tag->conn, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to conn! rc=%d", rc);
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to request of tag %" PRId32 ".", tag->tag_id);
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}
