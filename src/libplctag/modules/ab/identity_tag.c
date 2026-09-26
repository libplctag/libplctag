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

/*
 * size of the fixed-size fields preceding the CIP response in an identity reply:
 * interface_handle (one uint32_le) followed by timeout, item_count, NAI type, NAI length,
 * UDI type, and UDI length (six uint16_le fields).
 */
#define IDENTITY_RESPONSE_HEADER_SIZE ((int)(sizeof(uint32_le) + 6 * sizeof(uint16_le)))

/******************************************************************
 ******************* identity tag functions ***********************
 ******************************************************************/

/* identity tag functions */
static int identity_tag_read_start(ab_tag_p tag);
static int identity_tag_tickler(ab_tag_p tag);
static int identity_tag_check_read_status_connected(ab_tag_p tag);
static int identity_tag_check_read_status_unconnected(ab_tag_p tag);
static int identity_tag_build_read_request_connected(ab_tag_p tag);
static int identity_tag_build_read_request_unconnected(ab_tag_p tag);


/* define the vtable for identity tag type. */
static struct tag_vtable_t identity_tag_vtable = {.abort = (tag_vtable_func)ab_tag_abort_request,
                                                  .read = (tag_vtable_func)identity_tag_read_start,
                                                  .status = (tag_vtable_func)ab_tag_status,
                                                  .tickler = (tag_vtable_func)identity_tag_tickler,
                                                  .write = (tag_vtable_func)NULL,
                                                  .wake_plc = (tag_vtable_func)NULL,

                                                  /* attribute accessors */
                                                  .attribs = ab_attribs};


int setup_identity_tag(ab_tag_p tag) {
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


int identity_tag_read_start(ab_tag_p tag) {
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

    /* build the request based on connection type */
    if(tag->use_connected_msg) {
        rc = identity_tag_build_read_request_connected(tag);
    } else {
        rc = identity_tag_build_read_request_unconnected(tag);
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to build read request!");
        tag->read_in_progress = 0;
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int identity_tag_tickler(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Write attempted on identity tag. Not supported!");

        ab_tag_abort_request(tag);
        tag->write_complete = 1;

        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(tag->read_in_progress) {
        if(tag->use_connected_msg) {
            rc = identity_tag_check_read_status_connected(tag);
        } else {
            rc = identity_tag_check_read_status_unconnected(tag);
        }

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


int identity_tag_build_read_request_connected(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;
    eip_cip_co_req *cip = NULL;
    uint8_t *data = NULL;
    uint8_t *data_start = NULL;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to get new request. Error %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    cip = (eip_cip_co_req *)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_co_req);
    data_start = data;

    /*
     * Build CIP Get_Attributes_All request to Identity Object
     * Service: 0x01 (Get_Attributes_All)
     * Class: 0x01 (Identity Object)
     * Instance: 0x01
     */

    /* Service code: Get_Attributes_All */
    *data = 0x01;
    data++;

    /* Request path size in words (class + instance = 2 words = 4 bytes) */
    *data = 0x02;
    data++;

    /* Class segment: 0x20 (8-bit class) + 0x01 (Identity class) */
    *data = 0x20;
    data++;
    *data = 0x01;
    data++;

    /* Instance segment: 0x24 (8-bit instance) + 0x01 (instance 1) */
    *data = 0x24;
    data++;
    *data = 0x01;
    data++;

    /* now fill in the static part of the request */

    /* encap fields */
    cip->encap_command = h2le16(AB_EIP_CONNECTED_SEND);

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout */

    /* Common Packet Format fields for connected send */
    cip->cpf_item_count = h2le16(2);
    cip->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);
    cip->cpf_cai_item_length = h2le16(4);
    cip->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);
    cip->cpf_cdi_item_length = h2le16((uint16_t)((int)(data - data_start) + (int)sizeof(cip->cpf_conn_seq_num)));

    /* set the size of the request */
    req->request_size = (int)((int)sizeof(*cip) + (int)(data - data_start));

    req->allow_packing = 0; /* identity requests should not be packed */

    /* save the request for later */
    critical_block(tag->api_mutex) { tag->req = req; }

    /* add the request to the session's list */
    rc = session_add_request(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


int identity_tag_build_read_request_unconnected(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;
    eip_encap *hdr = NULL;
    uint8_t *data = NULL;
    uint8_t *cpf_items = NULL;
    uint8_t *data_item = NULL;
    uint8_t *cip_request_start = NULL;
    int need_unconnected_send = 0;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting.");

    /* Determine if we need Unconnected Send (routing required) */
    need_unconnected_send = (tag->session->conn_path_size > 0);

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to get new request. Error %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    hdr = (eip_encap *)(req->data);
    data = (req->data) + sizeof(eip_encap);

    /* Set the EIP command */
    hdr->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND);

    /* Get the CPF items area - right after the header */
    cpf_items = data;

    /* Build CPF header (interface handle and timeout) */
    data = cpf_items;
    uint32_t interface_handle = 0;
    mem_copy(data, &interface_handle, 4);
    data += 4;
    uint16_t timeout = 0;
    mem_copy(data, &timeout, 2);
    data += 2;

    /* Set item count = 2 (Null Address Item + Unconnected Data Item) */
    uint16_le item_count = h2le16(2);
    mem_copy(data, &item_count, 2);
    data += 2;

    /* Null Address Item */
    uint16_le nai_type = h2le16(AB_EIP_ITEM_NAI);
    mem_copy(data, &nai_type, 2);
    data += 2;
    uint16_le nai_length = h2le16(0);
    mem_copy(data, &nai_length, 2);
    data += 2;

    /* Unconnected Data Item */
    data_item = data;
    uint16_le udi_type = h2le16(AB_EIP_ITEM_UDI);
    mem_copy(data, &udi_type, 2);
    data += 2;
    uint8_t *length_ptr = data;
    uint16_le udi_length_placeholder = h2le16(0); /* placeholder, will update later */
    mem_copy(data, &udi_length_placeholder, 2);
    data += 2;

    /* If routing is needed, wrap in Unconnected Send */
    if(need_unconnected_send) {
        /* Unconnected Send service */
        uint8_t uc_service = AB_EIP_CMD_UNCONNECTED_SEND;
        mem_copy(data, &uc_service, 1);
        data++;

        /* Path to Connection Manager (class 0x06, instance 0x01) */
        uint8_t cm_path_size = 0x02;
        mem_copy(data, &cm_path_size, 1);
        data++;
        uint8_t cm_class_seg = 0x20;
        mem_copy(data, &cm_class_seg, 1);
        data++;
        uint8_t cm_class_id = 0x06;
        mem_copy(data, &cm_class_id, 1);
        data++;
        uint8_t cm_instance_seg = 0x24;
        mem_copy(data, &cm_instance_seg, 1);
        data++;
        uint8_t cm_instance_id = 0x01;
        mem_copy(data, &cm_instance_id, 1);
        data++;

        /* Timeout */
        uint8_t secs_per_tick = 0x01;
        mem_copy(data, &secs_per_tick, 1);
        data++;
        uint8_t timeout_ticks = 0xFA;
        mem_copy(data, &timeout_ticks, 1);
        data++;

        /* Embedded message length placeholder */
        uint8_t *embed_length_ptr = data;
        uint16_le embed_length_placeholder = h2le16(0);
        mem_copy(data, &embed_length_placeholder, 2);
        data += 2;

        /* Remember start of embedded CIP request */
        cip_request_start = data;

        /* CIP request: Service (Get_Attributes_All) */
        uint8_t service = 0x01;
        mem_copy(data, &service, 1);
        data++;

        /* Path size in words: class + instance */
        uint8_t path_size = 0x02;
        mem_copy(data, &path_size, 1);
        data++;

        /* Class segment */
        uint8_t class_seg = 0x20;
        mem_copy(data, &class_seg, 1);
        data++;
        uint8_t class_id = 0x01;
        mem_copy(data, &class_id, 1);
        data++;

        /* Instance segment */
        uint8_t instance_seg = 0x24;
        mem_copy(data, &instance_seg, 1);
        data++;
        uint8_t instance_id = 0x01;
        mem_copy(data, &instance_id, 1);
        data++;

        /* Update embedded message length (CIP request only) */
        uint16_t embed_length = (uint16_t)(data - cip_request_start);
        uint16_le embed_length_le = h2le16(embed_length);
        mem_copy(embed_length_ptr, &embed_length_le, 2);

        /* Add routing information for the Unconnected Send (after the CIP request) */
        uint8_t conn_path_size_words = (uint8_t)((tag->session->conn_path_size) / 2);
        mem_copy(data, &conn_path_size_words, 1);
        data++;
        uint8_t conn_path_reserved = 0;
        mem_copy(data, &conn_path_reserved, 1);
        data++;
        mem_copy(data, tag->session->conn_path, tag->session->conn_path_size);
        data += tag->session->conn_path_size;
    } else {
        /* Direct CIP request without Unconnected Send wrapper */
        uint8_t service = 0x01;
        mem_copy(data, &service, 1);
        data++;
        uint8_t path_size = 0x02;
        mem_copy(data, &path_size, 1);
        data++;
        uint8_t class_seg = 0x20;
        mem_copy(data, &class_seg, 1);
        data++;
        uint8_t class_id = 0x01;
        mem_copy(data, &class_id, 1);
        data++;
        uint8_t instance_seg = 0x24;
        mem_copy(data, &instance_seg, 1);
        data++;
        uint8_t instance_id = 0x01;
        mem_copy(data, &instance_id, 1);
        data++;
    }

    /* Update the UDI length */
    uint16_t udi_data_length = (uint16_t)(data - (data_item + 4));
    uint16_le udi_data_length_le = h2le16(udi_data_length);
    mem_copy(length_ptr, &udi_data_length_le, 2);

    /* Set the EIP header length */
    uint16_t cpf_length = (uint16_t)(data - cpf_items);
    hdr->encap_length = h2le16(cpf_length);

    /* Set the total request size */
    req->request_size = (int)(sizeof(eip_encap) + cpf_length);

    req->allow_packing = 0;

    /* save the request for later */
    critical_block(tag->api_mutex) { tag->req = req; }

    /* add the request to the session's list */
    rc = session_add_request(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


int identity_tag_check_read_status_connected(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp *cip_resp;
    uint8_t *data_start = NULL;
    uint8_t *data_end = NULL;
    int data_size = 0;
    uint8_t *tag_data_buffer = NULL;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* if we got here, there is a response and status is OK */

    /* point to the response */
    cip_resp = (eip_cip_co_resp *)(tag->req->data);

    /* check the CIP response service code */
    if(cip_resp->reply_service != (0x01 | AB_EIP_CMD_CIP_OK)) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "CIP response service unexpected: 0x%02x",
               cip_resp->reply_service);
        rc = PLCTAG_ERR_BAD_DATA;
        ab_tag_abort_request(tag);
        return rc;
    }

    /* check the status */
    if(cip_resp->status != AB_CIP_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "CIP status is not OK: 0x%02x", cip_resp->status);
        rc = PLCTAG_ERR_REMOTE_ERR;
        ab_tag_abort_request(tag);
        return rc;
    }

    /* copy the response data into the tag buffer, including the CIP response header */
    data_start = (uint8_t *)(&cip_resp->reply_service);
    data_end = tag->req->data + (tag->req->request_size);

    if((intptr_t)data_end < (intptr_t)data_start) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Response is shorter than the CIP response header!");
        ab_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_SMALL;
    }

    data_size = (int)(unsigned int)(data_end - data_start);

    /* allocate/reallocate the tag data buffer */
    tag_data_buffer = mem_realloc(tag->data, data_size);

    if(tag_data_buffer) {
        tag->data = tag_data_buffer;
        tag->size = data_size;
        mem_copy(tag->data, data_start, data_size);

        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Copied %d bytes of identity data.", data_size);
    } else {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data buffer!");
        rc = PLCTAG_ERR_NO_MEM;
    }

    /* clean up the request */
    ab_tag_abort_request(tag);

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


int identity_tag_check_read_status_unconnected(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    eip_encap *hdr = NULL;
    uint8_t *data = NULL;
    // uint8_t *cpf_items = NULL;
    // uint8_t *udi_data = NULL;
    uint16_t cpf_item_count = 0;
    uint16_t item_type = 0;
    uint16_t item_length = 0;
    uint8_t *cip_response = NULL;
    uint8_t reply_service = 0;
    uint8_t cip_status = 0;
    int data_size = 0;
    uint8_t *tag_data_buffer = NULL;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* point to the response */
    hdr = (eip_encap *)(tag->req->data);
    data = (uint8_t *)(hdr + 1);

    uint8_t *data_end = tag->req->data + tag->req->request_size;

    /* Parse the response:
     * EIP header (28 bytes)
     * CPF header: interface_handle (4) + timeout (2) + item_count (2)
     * Item 1: Null Address Item (type + length = 4 bytes, no data)
     * Item 2: UDI (type + length + CIP response data)
     */

    if((data_end - data) < IDENTITY_RESPONSE_HEADER_SIZE) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Identity response is too short for the CPF and item headers!");
        rc = PLCTAG_ERR_TOO_SMALL;
        ab_tag_abort_request(tag);
        return rc;
    }

    // cpf_items = data;
    data += 4; /* skip interface handle */
    data += 2; /* skip timeout */

    uint16_le cpf_item_count_le;
    mem_copy(&cpf_item_count_le, data, 2);
    cpf_item_count = le2h16(cpf_item_count_le);
    data += 2;

    if(cpf_item_count != 2) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unexpected CPF item count: %d", cpf_item_count);
        rc = PLCTAG_ERR_BAD_DATA;
        ab_tag_abort_request(tag);
        return rc;
    }

    /* Skip Null Address Item (type + length only, no data) */
    uint16_le item_type_le;
    mem_copy(&item_type_le, data, 2);
    item_type = le2h16(item_type_le);
    data += 2;
    uint16_le item_length_le;
    mem_copy(&item_length_le, data, 2);
    item_length = le2h16(item_length_le);
    data += 2;

    if(item_type != AB_EIP_ITEM_NAI || item_length != 0) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Invalid Null Address Item: type=0x%04x, length=%d",
               item_type, item_length);
        rc = PLCTAG_ERR_BAD_DATA;
        ab_tag_abort_request(tag);
        return rc;
    }

    /* Parse UDI (Unconnected Data Item) */
    uint16_le udi_type_le;
    mem_copy(&udi_type_le, data, 2);
    item_type = le2h16(udi_type_le);
    data += 2;
    uint16_le udi_length_le;
    mem_copy(&udi_length_le, data, 2);
    item_length = le2h16(udi_length_le);
    data += 2;

    if(item_type != AB_EIP_ITEM_UDI) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Invalid UDI item type: 0x%04x", item_type);
        rc = PLCTAG_ERR_BAD_DATA;
        ab_tag_abort_request(tag);
        return rc;
    }

    /* UDI data starts here - this is the CIP response */
    cip_response = data;

    if((intptr_t)cip_response >= (intptr_t)data_end) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Identity response is too short for the CIP reply service byte!");
        rc = PLCTAG_ERR_TOO_SMALL;
        ab_tag_abort_request(tag);
        return rc;
    }

    /* Extract CIP response fields */
    reply_service = *cip_response;
    cip_response++;

    /* Check if this is an Unconnected Send response (0x52 | 0x80 = 0xD2) */
    if(reply_service == (AB_EIP_CMD_UNCONNECTED_SEND | AB_EIP_CMD_CIP_OK)) {
        /* Skip reserved byte */
        cip_response++;

        if((intptr_t)cip_response >= (intptr_t)data_end) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
                   "Identity response is too short for the Unconnected Send status byte!");
            rc = PLCTAG_ERR_TOO_SMALL;
            ab_tag_abort_request(tag);
            return rc;
        }

        /* Extract Unconnected Send status */
        cip_status = *cip_response;
        cip_response++;

        if(cip_status != AB_CIP_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unconnected Send CIP status is not OK: 0x%02x",
                   cip_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            ab_tag_abort_request(tag);
            return rc;
        }

        /* Skip extended status size (1 byte) */
        cip_response++;

        if((intptr_t)cip_response >= (intptr_t)data_end) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
                   "Identity response is too short for the embedded CIP reply service byte!");
            rc = PLCTAG_ERR_TOO_SMALL;
            ab_tag_abort_request(tag);
            return rc;
        }

        /* Now we should have the actual Get_Attributes_All response embedded */
        reply_service = *cip_response;
        cip_response++;
    }

    /* Check for Get_Attributes_All response (0x01 service with success bit 0x80) */
    if(reply_service != (0x01 | AB_EIP_CMD_CIP_OK)) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "CIP response service unexpected: 0x%02x (expected 0x%02x)", reply_service, (0x01 | AB_EIP_CMD_CIP_OK));
        rc = PLCTAG_ERR_BAD_DATA;
        ab_tag_abort_request(tag);
        return rc;
    }

    /* Skip reserved byte */
    cip_response++;

    if((intptr_t)cip_response >= (intptr_t)data_end) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Identity response is too short for the CIP status byte!");
        rc = PLCTAG_ERR_TOO_SMALL;
        ab_tag_abort_request(tag);
        return rc;
    }

    /* Extract CIP status */
    cip_status = *cip_response;
    cip_response++;

    if(cip_status != AB_CIP_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "CIP status is not OK: 0x%02x", cip_status);
        rc = PLCTAG_ERR_REMOTE_ERR;
        ab_tag_abort_request(tag);
        return rc;
    }

    /* Skip extended status size (1 byte) */
    cip_response++;

    if((intptr_t)cip_response > (intptr_t)data_end) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Identity response is truncated!");
        rc = PLCTAG_ERR_TOO_SMALL;
        ab_tag_abort_request(tag);
        return rc;
    }

    /* Calculate data size: remaining bytes after all CIP headers */
    data_size = item_length - (int)(cip_response - data);

    if(data_size < 0 || data_size > (int)(data_end - cip_response)) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Invalid response data size: %d", data_size);
        rc = PLCTAG_ERR_BAD_DATA;
        ab_tag_abort_request(tag);
        return rc;
    }

    /* allocate/reallocate the tag data buffer */
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
    ab_tag_abort_request(tag);

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}
