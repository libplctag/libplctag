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
#include <libplctag/protocols/omron/omron_standard_tag.h>
#include <libplctag/protocols/omron/tag.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/vector.h>


static int build_read_request_connected(omron_tag_p tag, int byte_offset);
// static int build_tag_list_request_connected(omron_tag_p tag);
static int build_read_request_unconnected(omron_tag_p tag, int byte_offset);
static int build_write_request_connected(omron_tag_p tag, int byte_offset);
static int build_write_request_unconnected(omron_tag_p tag, int byte_offset);
static int build_write_bit_request_connected(omron_tag_p tag);
static int build_write_bit_request_unconnected(omron_tag_p tag);
static int check_read_status_connected(omron_tag_p tag);
static int check_read_status_unconnected(omron_tag_p tag);
static int check_write_status_connected(omron_tag_p tag);
static int check_write_status_unconnected(omron_tag_p tag);
static int calculate_write_data_per_packet(omron_tag_p tag);

static int tag_read_start(omron_tag_p tag);
static int tag_tickler(omron_tag_p tag);
static int tag_write_start(omron_tag_p tag);

/* define the exported vtable for this tag type. */
struct tag_vtable_t omron_standard_tag_vtable = {(tag_vtable_func)omron_tag_abort,                                   /* shared */
                                                 (tag_vtable_func)tag_read_start, (tag_vtable_func)omron_tag_status, /* shared */
                                                 (tag_vtable_func)tag_tickler, (tag_vtable_func)tag_write_start,
                                                 (tag_vtable_func)NULL, /* wake_plc */

                                                 /* attribute accessors */
                                                 omron_get_int_attrib, omron_set_int_attrib,

                                                 omron_get_byte_array_attrib};

// /* default string types used for ControlLogix-class PLCs. */
// tag_byte_order_t omron_udt_tag_byte_order = {
//     .is_allocated = 0,

//     .int16_order = {0,1},
//     .int32_order = {0,1,2,3},
//     .int64_order = {0,1,2,3,4,5,6,7},
//     .float32_order = {0,1,2,3},
//     .float64_order = {0,1,2,3,4,5,6,7},

//     .str_is_defined = 1,
//     .str_is_counted = 1,
//     .str_is_fixed_length = 1,
//     .str_is_zero_terminated = 0,
//     .str_is_byte_swapped = 0,

//     .str_pad_to_multiple_bytes = 1,
//     .str_count_word_bytes = 4,
//     .str_max_capacity = 82,
//     .str_total_length = 88,
//     .str_pad_bytes = 2
// };


/* default string types used for Omron-NJ/NX PLCs. */
tag_byte_order_t omron_njnx_tag_byte_order = {.is_allocated = 0,

                                              .int16_order = {0, 1},
                                              .int32_order = {0, 1, 2, 3},
                                              .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                              .float32_order = {0, 1, 2, 3},
                                              .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                              .str_is_defined = 1,
                                              .str_is_counted = 1,
                                              .str_is_fixed_length = 0,
                                              .str_is_zero_terminated = 1,
                                              .str_is_byte_swapped = 0,

                                              .str_pad_to_multiple_bytes = 1,
                                              .str_count_word_bytes = 2,
                                              .str_max_capacity = 0,
                                              .str_total_length = 0,
                                              .str_pad_bytes = 0};

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

//     .str_pad_to_multiple_bytes = 2,
//     .str_count_word_bytes = 2,
//     .str_max_capacity = 0,
//     .str_total_length = 0,
//     .str_pad_bytes = 0
// };


/*************************************************************************
 **************************** API Functions ******************************
 ************************************************************************/


int tag_tickler(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    rc = omron_check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->read_in_progress) {
        if(tag->use_connected_msg) {
            rc = check_read_status_connected(tag);
        } else {
            rc = check_read_status_unconnected(tag);
        }

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->read_in_progress) {
            /* done! */
            if(tag->first_read) {
                tag->first_read = 0;
                tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)rc);
            }

            tag->read_complete = 1;
        }

        pdebug(DEBUG_SPEW, "Done.  Read in progress.");

        return rc;
    }

    if(tag->write_in_progress) {
        if(tag->use_connected_msg) {
            rc = check_write_status_connected(tag);
        } else {
            rc = check_write_status_unconnected(tag);
        }

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->write_in_progress) { tag->write_complete = 1; }

        pdebug(DEBUG_SPEW, "Done. Write in progress.");

        return rc;
    }

    pdebug(DEBUG_SPEW, "Done.  No operation in progress.");

    return tag->status;
}


/*
 * tag_read_common_start
 *
 * This function must be called only from within one thread, or while
 * the tag's mutex is locked.
 *
 * The function starts the process of getting tag data from the PLC.
 */

int tag_read_start(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_WARN, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* mark the tag read in progress */
    tag->read_in_progress = 1;

    /* i is the index of the first new request */
    if(tag->use_connected_msg) {
        // if(tag->tag_list) {
        //     rc = build_tag_list_request_connected(tag);
        // } else {
        rc = build_read_request_connected(tag, tag->offset);
        // }
    } else {
        rc = build_read_request_unconnected(tag, tag->offset);
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build read request!");

        tag->read_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * tag_write_common_start
 *
 * This must be called from one thread alone, or while the tag mutex is
 * locked.
 *
 * The routine starts the process of writing to a tag.
 */

int tag_write_start(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_WARN, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* the write is now in flight */
    tag->write_in_progress = 1;

    /*
     * if the tag has not been read yet, read it.
     *
     * This gets the type data and sets up the request
     * buffers.
     */

    if(tag->first_read) {
        pdebug(DEBUG_DETAIL, "No read has completed yet, doing pre-read to get type information.");

        tag->pre_write_read = 1;
        tag->write_in_progress = 0; /* temporarily mask this off */

        return tag_read_start(tag);
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to calculate write sizes!");
        tag->write_in_progress = 0;

        return rc;
    }

    if(tag->use_connected_msg) {
        rc = build_write_request_connected(tag, tag->offset);
    } else {
        rc = build_write_request_unconnected(tag, tag->offset);
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build write request!");
        tag->write_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int build_read_request_connected(omron_tag_p tag, int byte_offset) {
    eip_cip_co_req *cip = NULL;
    uint8_t *data = NULL;
    omron_request_p req = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t read_cmd = OMRON_EIP_CMD_CIP_READ;

    (void)byte_offset;

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = conn_create_request(tag->conn, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  Error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* point the request struct at the buffer */
    cip = (eip_cip_co_req *)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_co_req);

    /*
     * set up the embedded CIP read packet
     * The format is:
     *
     * uint8_t cmd
     * LLA formatted name
     * uint16_t # of elements to read
     */

    // embed_start = data;

    /* set up the CIP Read request */
    read_cmd = OMRON_EIP_CMD_CIP_READ;

    *data = read_cmd;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* add the count of elements to read. */
    *((uint16_le *)data) = h2le16((uint16_t)(tag->elem_count));
    data += sizeof(uint16_le);

    /* here is where we need to add the data segment that controls Omron fragmentation */

    // if (read_cmd == OMRON_EIP_CMD_CIP_READ_FRAG) {
    //     /* add the byte offset for this request */
    //     *((uint32_le*)data) = h2le32((uint32_t)byte_offset);
    //     data += sizeof(uint32_le);
    // }

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

    /* set the conn so that we know what conn the request is aiming at */
    // req->conn = tag->conn;

    req->allow_packing = tag->allow_packing;

    /* set the response size to the size of data from the previous read */
    req->response_size = tag->size;

    /* if this is the first read of the tag then we do not know the size of the response data and cannot use packing unless the
     * plc supports fragmented reads*/
    req->first_read = tag->first_read;
    req->supports_fragmented_read = tag->supports_fragmented_read;

    /* add the request to the conn's list. */
    rc = conn_add_request(tag->conn, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to add request to conn! Error %s!", plc_tag_decode_error(rc));
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to request of tag %" PRId32 ".", tag->tag_id);
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}


int build_read_request_unconnected(omron_tag_p tag, int byte_offset) {
    eip_cip_uc_req *cip;
    uint8_t *data;
    uint8_t *embed_start, *embed_end;
    omron_request_p req = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t read_cmd = OMRON_EIP_CMD_CIP_READ;
    uint16_le tmp_uint16_le;

    (void)byte_offset;

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = conn_create_request(tag->conn, tag->tag_id, &req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  Error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* point the request struct at the buffer */
    cip = (eip_cip_uc_req *)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_uc_req);

    /*
     * set up the embedded CIP read packet
     * The format is:
     *
     * uint8_t cmd
     * LLA formatted name
     * uint16_t # of elements to read
     */

    embed_start = data;

    /* set up the CIP Read request */
    read_cmd = OMRON_EIP_CMD_CIP_READ;

    *data = read_cmd;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* add the count of elements to read. */
    tmp_uint16_le = h2le16((uint16_t)(tag->elem_count));
    mem_copy(data, &tmp_uint16_le, (int)(unsigned int)sizeof(tmp_uint16_le));
    data += sizeof(tmp_uint16_le);

    /* add the byte offset for this request */

    /* here is where we put the data segment fragmentation information */

    /* mark the end of the embedded packet */
    embed_end = data;

    /* Now copy in the routing information for the embedded message */
    /*
     * routing information.  Format:
     *
     * uint8_t path_size in 16-bit words
     * uint8_t reserved/pad (zero)
     * uint8_t[...] path (padded to even number of bytes)
     */
    if(tag->conn->conn_path_size > 0) {
        *data = (tag->conn->conn_path_size) / 2; /* in 16-bit words */
        data++;
        *data = 0; /* reserved/pad */
        data++;
        mem_copy(data, tag->conn->conn_path, tag->conn->conn_path_size);
        data += tag->conn->conn_path_size;
    }

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(OMRON_EIP_UNCONNECTED_SEND); /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                     /* ALWAYS 2 */
    cip->cpf_nai_item_type = h2le16(OMRON_EIP_ITEM_NAI); /* ALWAYS 0 */
    cip->cpf_nai_item_length = h2le16(0);                /* ALWAYS 0 */
    cip->cpf_udi_item_type = h2le16(OMRON_EIP_ITEM_UDI); /* ALWAYS 0x00B2 - Unconnected Data Item */
    cip->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&cip->cm_service_code))); /* REQ: fill in with length of remaining data. */

    /* CM Service Request - Connection Manager */
    cip->cm_service_code = OMRON_EIP_CMD_UNCONNECTED_SEND; /* 0x52 Unconnected Send */
    cip->cm_req_path_size = 2;                             /* 2, size in 16-bit words of path, next field */
    cip->cm_req_path[0] = 0x20;                            /* class */
    cip->cm_req_path[1] = 0x06;                            /* Connection Manager */
    cip->cm_req_path[2] = 0x24;                            /* instance */
    cip->cm_req_path[3] = 0x01;                            /* instance 1 */

    /* Unconnected send needs timeout information */
    cip->secs_per_tick = OMRON_EIP_SECS_PER_TICK; /* seconds per tick */
    cip->timeout_ticks = OMRON_EIP_TIMEOUT_TICKS; /* timeout = src_secs_per_tick * src_timeout_ticks */

    /* size of embedded packet */
    cip->uc_cmd_length = h2le16((uint16_t)(embed_end - embed_start));

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));

    /* allow packing if the tag allows it. */
    req->allow_packing = tag->allow_packing;

    /* set the response size to the size of data from the previous read */
    req->response_size = tag->size;

    /* if this is the first read of the tag then we do not know the size of the response data and cannot use packing unless the
     * plc supports fragmented reads*/
    req->first_read = tag->first_read;
    req->supports_fragmented_read = tag->supports_fragmented_read;

    /* add the request to the conn's list. */
    rc = conn_add_request(tag->conn, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to add request to conn! Error %s!", plc_tag_decode_error(rc));
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to request of tag %" PRId32 ".", tag->tag_id);
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}


int build_write_bit_request_connected(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_req *cip = NULL;
    uint8_t *data = NULL;
    omron_request_p req = NULL;
    int i;

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = conn_create_request(tag->conn, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    rc = calculate_write_data_per_packet(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to calculate valid write data per packet!.  rc=%s", plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < (tag->size * 2) + 2) { /* 2 masks plus a count word. */
        pdebug(DEBUG_ERROR, "Insufficient space to write bit masks!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    cip = (eip_cip_co_req *)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_co_req);

    /*
     * set up the embedded CIP read packet
     * The format is:
     *
     * uint8_t cmd
     * LLA formatted name
     * uint16_t # size of a mask element
     * OR mask
     * AND mask
     */

    /*
     * set up the CIP Read-Modify-Write request type.
     */
    *data = OMRON_EIP_CMD_CIP_RMW;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* write an INT of the mask size. */
    *data = (uint8_t)(tag->elem_size & 0xFF);
    data++;
    *data = (uint8_t)((tag->elem_size >> 8) & 0xFF);
    data++;

    /* write the OR mask */
    for(i = 0; i < tag->elem_size; i++) {
        if((tag->bit / 8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            /* if the bit is set, then we want to mask it on. */
            if(tag->data[tag->bit / 8] & mask) {
                *data = mask;
            } else {
                *data = (uint8_t)0;
            }

            pdebug(DEBUG_DETAIL, "adding OR mask byte %d: %x", i, *data);

            data++;
        } else {
            /* this is not the data we care about. */
            *data = (uint8_t)0;

            pdebug(DEBUG_DETAIL, "adding OR mask byte %d: %x", i, *data);

            data++;
        }
    }

    /* write the AND mask */
    for(i = 0; i < tag->elem_size; i++) {
        if((tag->bit / 8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            /* if the bit is set, then we want to _not_ mask it off. */
            if(tag->data[tag->bit / 8] & mask) {
                *data = (uint8_t)0xFF;
            } else {
                *data = (uint8_t)(~mask);
            }

            pdebug(DEBUG_DETAIL, "adding AND mask byte %d: %x", i, *data);

            data++;
        } else {
            /* this is not the data we care about. */
            *data = (uint8_t)0xFF;

            pdebug(DEBUG_DETAIL, "adding AND mask byte %d: %x", i, *data);

            data++;
        }
    }

    /* let the rest of the system know that the write is complete after this. */
    tag->offset = tag->size;

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(OMRON_EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(OMRON_EIP_ITEM_CAI); /* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);             /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(OMRON_EIP_ITEM_CDI); /* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&cip->cpf_conn_seq_num))); /* REQ: fill in with length of remaining data. */

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));

    /* allow packing if the tag allows it. */
    req->allow_packing = tag->allow_packing;

    /* add the request to the session's list. */
    rc = conn_add_request(tag->conn, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
        omron_tag_abort_request(tag);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}


int build_write_bit_request_unconnected(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_uc_req *cip = NULL;
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    uint8_t *embed_end = NULL;
    omron_request_p req = NULL;
    int i = 0;

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = conn_create_request(tag->conn, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    rc = calculate_write_data_per_packet(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to calculate valid write data per packet!.  rc=%s", plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < (tag->size * 2) + 2) { /* 2 masks plus a count word. */
        pdebug(DEBUG_ERROR, "Insufficient space to write bit masks!");
        return PLCTAG_ERR_TOO_SMALL;
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
     * uint16_t # size of a mask element
     * OR mask
     * AND mask
     */

    /*
     * set up the CIP Read-Modify-Write request type.
     */
    *data = OMRON_EIP_CMD_CIP_RMW;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* write an INT of the mask size. */
    *data = (uint8_t)(tag->elem_size & 0xFF);
    data++;
    *data = (uint8_t)((tag->elem_size >> 8) & 0xFF);
    data++;

    /* write the OR mask */
    for(i = 0; i < tag->elem_size; i++) {
        if((tag->bit / 8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            /* if the bit is set, then we want to mask it on. */
            if(tag->data[tag->bit / 8] & mask) {
                *data = mask;
            } else {
                *data = (uint8_t)0;
            }

            pdebug(DEBUG_DETAIL, "adding OR mask byte %d: %x", i, *data);

            data++;
        } else {
            /* this is not the data we care about. */
            *data = (uint8_t)0;

            pdebug(DEBUG_DETAIL, "adding OR mask byte %d: %x", i, *data);

            data++;
        }
    }

    /* write the AND mask */
    for(i = 0; i < tag->elem_size; i++) {
        if((tag->bit / 8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            /* if the bit is set, then we want to _not_ mask it off. */
            if(tag->data[tag->bit / 8] & mask) {
                *data = (uint8_t)0xFF;
            } else {
                *data = (uint8_t)(~mask);
            }

            pdebug(DEBUG_DETAIL, "adding OR mask byte %d: %x", i, *data);

            data++;
        } else {
            /* this is not the data we care about. */
            *data = (uint8_t)0xFF;

            pdebug(DEBUG_DETAIL, "adding OR mask byte %d: %x", i, *data);

            data++;
        }
    }

    /* let the rest of the system know that the write is complete after this. */
    tag->offset = tag->size;

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
    data++;
    mem_copy(data, tag->conn->conn_path, tag->conn->conn_path_size);
    data += tag->conn->conn_path_size;

    /* now fill in the rest of the structure. */

    /* encap fields */
    cip->encap_command = h2le16(OMRON_EIP_UNCONNECTED_SEND); /* ALWAYS 0x006F Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_nai_item_type = h2le16(OMRON_EIP_ITEM_NAI); /* ALWAYS 0 */
    cip->cpf_nai_item_length = h2le16(0);             /* ALWAYS 0 */
    cip->cpf_udi_item_type = h2le16(OMRON_EIP_ITEM_UDI); /* ALWAYS 0x00B2 - Unconnected Data Item */
    cip->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&(cip->cm_service_code)))); /* REQ: fill in with length of remaining data. */

    /* CM Service Request - Connection Manager */
    cip->cm_service_code = OMRON_EIP_CMD_UNCONNECTED_SEND; /* 0x52 Unconnected Send */
    cip->cm_req_path_size = 2;                          /* 2, size in 16-bit words of path, next field */
    cip->cm_req_path[0] = 0x20;                         /* class */
    cip->cm_req_path[1] = 0x06;                         /* Connection Manager */
    cip->cm_req_path[2] = 0x24;                         /* instance */
    cip->cm_req_path[3] = 0x01;                         /* instance 1 */

    /* Unconnected send needs timeout information */
    cip->secs_per_tick = OMRON_EIP_SECS_PER_TICK; /* seconds per tick */
    cip->timeout_ticks = OMRON_EIP_TIMEOUT_TICKS; /* timeout = srd_secs_per_tick * src_timeout_ticks */

    /* size of embedded packet */
    cip->uc_cmd_length = h2le16((uint16_t)(embed_end - embed_start));

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));

    /* allow packing if the tag allows it. */
    req->allow_packing = tag->allow_packing;

    /* add the request to the session's list. */
    rc = conn_add_request(tag->conn, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
        omron_tag_abort_request(tag);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}



int build_write_request_connected(omron_tag_p tag, int byte_offset) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_req *cip = NULL;
    uint8_t *data = NULL;
    omron_request_p req = NULL;
    int multiple_requests = 0;
    int write_size = 0;
    int str_pad_to_multiple_bytes = 1;

    pdebug(DEBUG_INFO, "Starting.");

    if(tag->is_bit) { return build_write_bit_request_connected(tag); }

    /* get a request buffer */
    rc = conn_create_request(tag->conn, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  Error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = calculate_write_data_per_packet(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to calculate valid write data per packet!.  rc=%s", plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < tag->size) { multiple_requests = 1; }

    if(multiple_requests && tag->plc_type == OMRON_PLC_OMRON_NJNX) {
        pdebug(DEBUG_WARN, "Tag too large for unfragmented request on Omron PLC!");
        return PLCTAG_ERR_TOO_LARGE;
    }

    cip = (eip_cip_co_req *)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_co_req);

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
     * set up the CIP Read request type.
     * Different if more than one request.
     *
     * This handles a bug where attempting fragmented requests
     * does not appear to work with a single boolean.
     */
    *data = OMRON_EIP_CMD_CIP_WRITE;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* copy encoded type info */
    if(tag->encoded_type_info_size) {
        mem_copy(data, tag->encoded_type_info, tag->encoded_type_info_size);
        data += tag->encoded_type_info_size;
    } else {
        pdebug(DEBUG_WARN, "Data type unsupported!");
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* copy the item count, little endian */
    *((uint16_le *)data) = h2le16((uint16_t)(tag->elem_count));
    data += sizeof(uint16_le);

    if(multiple_requests) {
        // FIXME - add 0x80 data segment stuff here
        /* put in the byte offset */
        *((uint32_le *)data) = h2le32((uint32_t)(byte_offset));
        data += sizeof(uint32_le);
    }

    /* how much data to write? */
    write_size = tag->size - tag->offset;

    if(write_size > tag->write_data_per_packet) { write_size = tag->write_data_per_packet; }

    /* now copy the data to write */
    mem_copy(data, tag->data + tag->offset, write_size);
    data += write_size;
    tag->offset += write_size;

    /* need to pad data to multiple of either 1, 2 or 4 bytes */
    /* for some PLCs (OmronNJ), padding causes issues when writing counted strings as it creates a mismatch between
        the length of the string and the count integer, therefor this padding can be disabled using the str_pad_16_bits attribute
     */
    str_pad_to_multiple_bytes = (int)tag->byte_order->str_pad_to_multiple_bytes;
    if((str_pad_to_multiple_bytes == 2 || str_pad_to_multiple_bytes == 4) && write_size != 0) {
        if(write_size % str_pad_to_multiple_bytes != 0) {
            int pad_size = str_pad_to_multiple_bytes - (write_size % str_pad_to_multiple_bytes);
            for(int i = 0; i < pad_size; i++) {
                *data = 0;
                data++;
            }
        }
    }

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

    /* add the request to the conn's list. */
    rc = conn_add_request(tag->conn, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to add request to conn! Error %s!", plc_tag_decode_error(rc));
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to request of tag %" PRId32 ".", tag->tag_id);
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}


int build_write_request_unconnected(omron_tag_p tag, int byte_offset) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_uc_req *cip = NULL;
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    uint8_t *embed_end = NULL;
    omron_request_p req = NULL;
    int multiple_requests = 0;
    int write_size = 0;
    int str_pad_to_multiple_bytes = 1;

    pdebug(DEBUG_INFO, "Starting.");

    if(tag->is_bit) { return build_write_bit_request_unconnected(tag); }

    /* get a request buffer */
    rc = conn_create_request(tag->conn, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  Error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = calculate_write_data_per_packet(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to calculate valid write data per packet!.  rc=%s", plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < tag->size) { multiple_requests = 1; }

    if(multiple_requests && tag->plc_type == OMRON_PLC_OMRON_NJNX) {
        pdebug(DEBUG_WARN, "Tag too large for unfragmented request on Omron PLC!");
        return PLCTAG_ERR_TOO_LARGE;
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
     * set up the CIP Read request type.
     * Different if more than one request.
     *
     * This handles a bug where attempting fragmented requests
     * does not appear to work with a single boolean.
     */
    *data = OMRON_EIP_CMD_CIP_WRITE;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* copy encoded type info */
    if(tag->encoded_type_info_size) {
        mem_copy(data, tag->encoded_type_info, tag->encoded_type_info_size);
        data += tag->encoded_type_info_size;
    } else {
        pdebug(DEBUG_WARN, "Data type unsupported!");
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* copy the item count, little endian */
    *((uint16_le *)data) = h2le16((uint16_t)(tag->elem_count));
    data += sizeof(uint16_le);

    if(multiple_requests) {
        // FIXME - add 0x80 data segment stuff here.
        /* put in the byte offset */
        *((uint32_le *)data) = h2le32((uint32_t)byte_offset);
        data += sizeof(uint32_le);
    }

    /* how much data to write? */
    write_size = tag->size - tag->offset;

    if(write_size > tag->write_data_per_packet) { write_size = tag->write_data_per_packet; }

    /* now copy the data to write */
    mem_copy(data, tag->data + tag->offset, write_size);
    data += write_size;
    tag->offset += write_size;

    /* need to pad data to multiple of either 1, 2 or 4 bytes */
    /* for some PLCs (OmronNJ), padding causes issues when writing counted strings as it creates a mismatch between
        the length of the string and the count integer, therefor this padding can be disabled using the str_pad_16_bits attribute
     */
    str_pad_to_multiple_bytes = (int)tag->byte_order->str_pad_to_multiple_bytes;
    if((str_pad_to_multiple_bytes == 2 || str_pad_to_multiple_bytes == 4) && write_size != 0) {
        if(write_size % str_pad_to_multiple_bytes != 0) {
            int pad_size = str_pad_to_multiple_bytes - (write_size % str_pad_to_multiple_bytes);
            for(int i = 0; i < pad_size; i++) {
                *data = 0;
                data++;
            }
        }
    }


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
    data++;
    mem_copy(data, tag->conn->conn_path, tag->conn->conn_path_size);
    data += tag->conn->conn_path_size;

    /* now fill in the rest of the structure. */

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

    /* add the request to the conn's list. */
    rc = conn_add_request(tag->conn, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to add request to conn! Error %s!", plc_tag_decode_error(rc));
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to request of tag %" PRId32 ".", tag->tag_id);
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}


/*
 * check_read_status_connected
 *
 * This routine checks for any outstanding requests and copies in data
 * that has arrived.  At the end of the request, it will clean up the request
 * buffers.  This is not thread-safe!  It should be called with the tag mutex
 * locked!
 */

static int check_read_status_connected(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp *cip_resp;
    uint8_t *data;
    uint8_t *data_end;
    int partial_data = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    /* the request reference is valid. */

    /* point to the data */
    cip_resp = (eip_cip_co_resp *)(tag->req->data);

    /* point to the start of the data */
    data = (tag->req->data) + sizeof(eip_cip_co_resp);

    /* point the end of the data */
    data_end = (tag->req->data + le2h16(cip_resp->encap_length) + sizeof(eip_encap));

    /* check the status */
    do {
        ptrdiff_t payload_size = 0;

        if(cip_resp->reply_service != (OMRON_EIP_CMD_CIP_READ | OMRON_EIP_CMD_CIP_OK)) {
            pdebug(DEBUG_WARN, "CIP response reply service unexpected: %d", cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(cip_resp->status != OMRON_CIP_STATUS_OK && cip_resp->status != OMRON_CIP_STATUS_FRAG) {
            pdebug(DEBUG_WARN, "CIP read failed with status: 0x%x %s", cip_resp->status,
                   CIP.decode_cip_error_short((uint8_t *)&cip_resp->status));
            pdebug(DEBUG_INFO, CIP.decode_cip_error_long((uint8_t *)&cip_resp->status));

            rc = CIP.decode_cip_error_code((uint8_t *)&cip_resp->status);

            break;
        }

        /* check to see if this is a partial response. */
        partial_data = (cip_resp->status == OMRON_CIP_STATUS_FRAG);

        /*
         * check to see if there is any data to process.  If this is a packed
         * response, there might not be.
         */
        payload_size = (data_end - data);
        if(payload_size > 0) {
            /* skip the copy if we already have type data */
            if(tag->encoded_type_info_size == 0) {
                int type_length = 0;

                /* the first byte of the response is a type byte. */
                pdebug(DEBUG_DETAIL, "type byte = %d (0x%02x)", (int)*data, (int)*data);

                if(CIP.lookup_encoded_type_size(*data, &type_length) == PLCTAG_STATUS_OK) {
                    /* found it and we got the type data size */

                    /* some types use the second byte to indicate how many bytes more are used. */
                    if(type_length == 0) { type_length = *(data + 1) + 2; }

                    if(type_length <= 0) {
                        pdebug(DEBUG_WARN, "Unable to determine type data length for type byte 0x%02x!", *data);
                        rc = PLCTAG_ERR_UNSUPPORTED;
                        break;
                    }

                    pdebug(DEBUG_DETAIL, "Type data is %d bytes long.", type_length);
                    pdebug_dump_bytes(DEBUG_DETAIL, data, type_length);

                    tag->encoded_type_info_size = type_length;
                    mem_copy(tag->encoded_type_info, data, tag->encoded_type_info_size);
                } else {
                    pdebug(DEBUG_WARN, "Unsupported data type returned, type byte=0x%02x", *data);
                    rc = PLCTAG_ERR_UNSUPPORTED;
                    break;
                }
            }

            /* skip past the type data */
            data += (tag->encoded_type_info_size);

            /* check payload size now that we have bumped past the data type info. */
            payload_size = (data_end - data);

            /* copy the data into the tag and realloc if we need more space. */
            if(payload_size + tag->offset > tag->size) {
                tag->size = (int)payload_size + tag->offset;
                tag->elem_size = tag->size / tag->elem_count;

                pdebug(DEBUG_DETAIL, "Increasing tag buffer size to %d bytes.", tag->size);

                tag->data = (uint8_t *)mem_realloc(tag->data, tag->size);
                if(!tag->data) {
                    pdebug(DEBUG_WARN, "Unable to reallocate tag data memory!");
                    rc = PLCTAG_ERR_NO_MEM;
                    break;
                }
            }

            pdebug(DEBUG_INFO, "Got %d bytes of data", (int)payload_size);

            /*
             * copy the data, but only if this is not
             * a pre-read for a subsequent write!  We do not
             * want to overwrite the data the upstream has
             * put into the tag's data buffer.
             */
            if(!tag->pre_write_read) { mem_copy(tag->data + tag->offset, data, (int)(payload_size)); }

            /* bump the byte offset */
            tag->offset += (int)(payload_size);
        } else {
            pdebug(DEBUG_DETAIL, "Response returned no data and no error.");
        }

        /* set the return code */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request */
    omron_tag_abort(tag);

    /* are we actually done? */
    if(rc == PLCTAG_STATUS_OK) {
        /* this particular read is done. */
        tag->read_in_progress = 0;

        /* skip if we are doing a pre-write read. */
        if(!tag->pre_write_read && partial_data) {
            /* call read start again to get the next piece */
            pdebug(DEBUG_DETAIL, "calling tag_read_start() to get the next chunk.");
            /* FIXNE - the abort function above resets the offset. */
            rc = tag_read_start(tag);
        } else {
            tag->offset = 0;

            /* if this is a pre-read for a write, then pass off to the write routine */
            if(tag->pre_write_read) {
                pdebug(DEBUG_DETAIL, "Restarting write call now.");
                tag->pre_write_read = 0;
                rc = tag_write_start(tag);
            }
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        /* error ! */
        pdebug(DEBUG_WARN, "Error received!");

        /* clean up everything. */
        omron_tag_abort(tag);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


static int check_read_status_unconnected(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_uc_resp *cip_resp;
    uint8_t *data;
    uint8_t *data_end;
    int partial_data = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    /* the request reference is valid. */

    /* point to the data */
    cip_resp = (eip_cip_uc_resp *)(tag->req->data);

    /* point to the start of the data */
    data = (tag->req->data) + sizeof(eip_cip_uc_resp);

    /* point the end of the data */
    data_end = (tag->req->data + le2h16(cip_resp->encap_length) + sizeof(eip_encap));

    /* check the status */
    do {
        ptrdiff_t payload_size = 0;

        if(le2h16(cip_resp->encap_command) != OMRON_EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", cip_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(cip_resp->encap_status) != OMRON_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(cip_resp->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /*
         * TODO
         *
         * It probably should not be necessary to check for both as setting the type to anything other
         * than fragmented is error-prone.
         */

        if(cip_resp->reply_service != (OMRON_EIP_CMD_CIP_READ | OMRON_EIP_CMD_CIP_OK)) {
            pdebug(DEBUG_WARN, "CIP response reply service unexpected: %d", cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(cip_resp->status != OMRON_CIP_STATUS_OK && cip_resp->status != OMRON_CIP_STATUS_FRAG) {
            pdebug(DEBUG_WARN, "CIP read failed with status: 0x%x %s", cip_resp->status,
                   CIP.decode_cip_error_short((uint8_t *)&cip_resp->status));
            pdebug(DEBUG_INFO, CIP.decode_cip_error_long((uint8_t *)&cip_resp->status));

            rc = CIP.decode_cip_error_code((uint8_t *)&cip_resp->status);

            break;
        }

        /* check to see if this is a partial response. */
        partial_data = (cip_resp->status == OMRON_CIP_STATUS_FRAG);

        /*
         * check to see if there is any data to process.  If this is a packed
         * response, there might not be.
         */
        payload_size = (data_end - data);
        if(payload_size > 0) {
            /* skip the copy if we already have type data */
            if(tag->encoded_type_info_size == 0) {
                int type_length = 0;

                /* the first byte of the response is a type byte. */
                pdebug(DEBUG_DETAIL, "type byte = %d (0x%02x)", (int)*data, (int)*data);

                if(CIP.lookup_encoded_type_size(*data, &type_length) == PLCTAG_STATUS_OK) {
                    /* found it and we got the type data size */

                    /* some types use the second byte to indicate how many bytes more are used. */
                    if(type_length == 0) { type_length = *(data + 1) + 2; }

                    if(type_length <= 0) {
                        pdebug(DEBUG_WARN, "Unable to determine type data length for type byte 0x%02x!", *data);
                        rc = PLCTAG_ERR_UNSUPPORTED;
                        break;
                    }

                    pdebug(DEBUG_DETAIL, "Type data is %d bytes long.", type_length);
                    pdebug_dump_bytes(DEBUG_DETAIL, data, type_length);

                    tag->encoded_type_info_size = type_length;
                    mem_copy(tag->encoded_type_info, data, tag->encoded_type_info_size);
                } else {
                    pdebug(DEBUG_WARN, "Unsupported data type returned, type byte=0x%02x", *data);
                    rc = PLCTAG_ERR_UNSUPPORTED;
                    break;
                }
            }

            /* skip past the type data */
            data += (tag->encoded_type_info_size);

            /* check payload size now that we have bumped past the data type info. */
            payload_size = (data_end - data);

            /* copy the data into the tag and realloc if we need more space. */
            if(payload_size + tag->offset > tag->size) {
                tag->size = (int)payload_size + tag->offset;
                tag->elem_size = tag->size / tag->elem_count;

                pdebug(DEBUG_DETAIL, "Increasing tag buffer size to %d bytes.", tag->size);

                tag->data = (uint8_t *)mem_realloc(tag->data, tag->size);
                if(!tag->data) {
                    pdebug(DEBUG_WARN, "Unable to reallocate tag data memory!");
                    rc = PLCTAG_ERR_NO_MEM;
                    break;
                }
            }

            pdebug(DEBUG_INFO, "Got %d bytes of data", (int)payload_size);

            /*
             * copy the data, but only if this is not
             * a pre-read for a subsequent write!  We do not
             * want to overwrite the data the upstream has
             * put into the tag's data buffer.
             */
            if(!tag->pre_write_read) { mem_copy(tag->data + tag->offset, data, (int)payload_size); }

            /* bump the byte offset */
            tag->offset += (int)payload_size;
        } else {
            pdebug(DEBUG_DETAIL, "Response returned no data and no error.");
        }

        /* set the return code */
        rc = PLCTAG_STATUS_OK;
    } while(0);


    /* clean up the request */
    omron_tag_abort(tag);

    /* are we actually done? */
    if(rc == PLCTAG_STATUS_OK) {
        /* this read is done. */
        tag->read_in_progress = 0;

        /* skip if we are doing a pre-write read. */
        if(!tag->pre_write_read && partial_data) {
            /* call read start again to get the next piece */
            pdebug(DEBUG_DETAIL, "calling tag_read_start() to get the next chunk.");
            /* FIXME - the abort function above resets the offset! */
            rc = tag_read_start(tag);
        } else {
            tag->offset = 0;

            /* if this is a pre-read for a write, then pass off to the write routine */
            if(tag->pre_write_read) {
                pdebug(DEBUG_DETAIL, "Restarting write call now.");
                tag->pre_write_read = 0;
                rc = tag_write_start(tag);
            }
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        /* error ! */
        pdebug(DEBUG_WARN, "Error received!");

        /* clean up everything. */
        omron_tag_abort(tag);
    }

    /* release the referene to the request. */

    // FIXME - why is this different than the connected case?
    // rc_dec(request);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


/*
 * check_write_status_connected
 *
 * This routine must be called with the tag mutex locked.  It checks the current
 * status of a write operation.  If the write is done, it triggers the clean up.
 */

static int check_write_status_connected(omron_tag_p tag) {
    eip_cip_co_resp *cip_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_ERROR, "Null tag pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* the request reference is valid. */

    /* point to the data */
    cip_resp = (eip_cip_co_resp *)(tag->req->data);

    do {
        if(cip_resp->reply_service != (OMRON_EIP_CMD_CIP_WRITE | OMRON_EIP_CMD_CIP_OK)
           && cip_resp->reply_service != (OMRON_EIP_CMD_CIP_RMW | OMRON_EIP_CMD_CIP_OK)) {
            pdebug(DEBUG_WARN, "CIP response reply service unexpected: %d", cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(cip_resp->status != OMRON_CIP_STATUS_OK && cip_resp->status != OMRON_CIP_STATUS_FRAG) {
            pdebug(DEBUG_WARN, "CIP read failed with status: 0x%x %s", cip_resp->status,
                   CIP.decode_cip_error_short((uint8_t *)&cip_resp->status));
            pdebug(DEBUG_INFO, CIP.decode_cip_error_long((uint8_t *)&cip_resp->status));
            rc = CIP.decode_cip_error_code((uint8_t *)&cip_resp->status);
            break;
        }
    } while(0);

    /* clean up the request. */
    omron_tag_abort_request_only(tag);

    /* write is done in one way or another. */
    tag->write_in_progress = 0;

    if(rc == PLCTAG_STATUS_OK) {
        if(tag->offset < tag->size) {

            pdebug(DEBUG_DETAIL, "Write not complete, triggering next round.");
            /* FIXME - the abort function above resets the offset */
            rc = tag_write_start(tag);
        } else {
            /* only clear this if we are done. */
            tag->offset = 0;
        }
    } else {
        pdebug(DEBUG_WARN, "Write failed!");

        tag->offset = 0;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


static int check_write_status_unconnected(omron_tag_p tag) {
    eip_cip_uc_resp *cip_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    /* the request reference is valid. */

    /* point to the data */
    cip_resp = (eip_cip_uc_resp *)(tag->req->data);

    do {
        if(cip_resp->reply_service != (OMRON_EIP_CMD_CIP_WRITE | OMRON_EIP_CMD_CIP_OK)
           && cip_resp->reply_service != (OMRON_EIP_CMD_CIP_RMW | OMRON_EIP_CMD_CIP_OK)) {
            pdebug(DEBUG_WARN, "CIP response reply service unexpected: %d", cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }


        if(cip_resp->status != OMRON_CIP_STATUS_OK && cip_resp->status != OMRON_CIP_STATUS_FRAG) {
            pdebug(DEBUG_WARN, "CIP read failed with status: 0x%x %s", cip_resp->status,
                   CIP.decode_cip_error_short((uint8_t *)&cip_resp->status));
            pdebug(DEBUG_INFO, CIP.decode_cip_error_long((uint8_t *)&cip_resp->status));
            rc = CIP.decode_cip_error_code((uint8_t *)&cip_resp->status);
            break;
        }
    } while(0);

    /* clean up the request. */
    omron_tag_abort_request_only(tag);

    /* write is done in one way or another */
    tag->write_in_progress = 0;

    if(rc == PLCTAG_STATUS_OK) {
        if(tag->offset < tag->size) {

            pdebug(DEBUG_DETAIL, "Write not complete, triggering next round.");
            /* FIXME - the above abort function resets the offset */
            rc = tag_write_start(tag);
        } else {
            /* only clear this if we are done. */
            tag->offset = 0;
        }
    } else {
        pdebug(DEBUG_WARN, "Write failed!");
        tag->offset = 0;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}



int calculate_write_data_per_packet(omron_tag_p tag) {
    int overhead = 0;
    int data_per_packet = 0;
    int available_payload = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* if we are here, then we have all the type data etc. */
    available_payload = conn_get_available_cip_payload_space(tag->conn);

    if(tag->use_connected_msg) {
        pdebug(DEBUG_DETAIL, "Connected tag.");
        overhead = 1                             /* service request, one byte */
                   + tag->encoded_name_size      /* full encoded name */
                   + tag->encoded_type_info_size /* encoded type size */
                   + 2                           /* element count, 16-bit int */
                   + 4                           /* byte offset, 32-bit int */
                   + 8;                          /* MAGIC fudge factor */
    } else {
        pdebug(DEBUG_DETAIL, "Unconnected tag.");
        overhead = 1                                  /* service request, one byte */
                   + tag->encoded_name_size           /* full encoded name */
                   + tag->encoded_type_info_size      /* encoded type size */
                   + tag->conn->conn_path_size + 2 /* encoded device path size plus two bytes for length and padding */
                   + 2                                /* element count, 16-bit int */
                   + 4                                /* byte offset, 32-bit int */
                   + 8;                               /* MAGIC fudge factor */
    }

    /* make sure that overhead is an even number of bytes */
    if(overhead & 1) { overhead++; }

    pdebug(DEBUG_DETAIL, "Write overhead is %d bytes.", overhead);

    data_per_packet = available_payload - overhead;

    pdebug(DEBUG_DETAIL, "Write packet available payload is %d, write overhead is %d, and write data per packet is %d.",
           available_payload, overhead, data_per_packet);

    if(data_per_packet <= 0) {
        pdebug(DEBUG_WARN, "Unable to send request.  Packet overhead, %d bytes, is too large for available payload, %d bytes!",
               overhead, available_payload);
        return PLCTAG_ERR_TOO_LARGE;
    }

    int element_size = 0;
    int elements_per_packet = 0;

   /* if the tag size is less than 8 bytes, then use a multiple of the tag size.  Otherwise use
    8 bytes as the unit */
    if(tag->elem_size < 8) {
        elements_per_packet = data_per_packet / tag->elem_size;
        data_per_packet = elements_per_packet * tag->elem_size;
        element_size = tag->elem_size;
        pdebug(DEBUG_DETAIL, "Using tag size %d bytes for element size.", element_size);
    } else {
        /* round down to the nearest multiple of 8 bytes */
        elements_per_packet = data_per_packet / 8;
        data_per_packet = elements_per_packet * 8;
        element_size = 8;
        pdebug(DEBUG_DETAIL, "Using element size %d bytes.", element_size);
    }

    if(elements_per_packet < 1) {
        pdebug(DEBUG_WARN, "Unable to send request.  Available payload, %d bytes, is too small to write at least %d bytes!",
               available_payload, element_size);
        return PLCTAG_ERR_TOO_LARGE;
    }

    pdebug(DEBUG_DETAIL, "Write data per packet is %d bytes.", data_per_packet);

    tag->write_data_per_packet = data_per_packet;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}
