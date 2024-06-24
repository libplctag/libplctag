/***************************************************************************
 *   Copyright (C) 2024 by Kyle Hayes                                      *
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
#include <platform.h>
#include <lib/libplctag.h>
#include <lib/tag.h>
#include <omron/defs.h>
#include <omron/omron_common.h>
#include <omron/cip.h>
#include <omron/tag.h>
#include <omron/conn.h>
#include <omron/eip_cip.h>  /* for the Logix decode types. */
#include <omron/omron_udt_tag.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/vector.h>



/* UDT tag functions. */
static int udt_tag_read_start(omron_tag_p tag);
static int udt_tag_tickler(omron_tag_p tag);
//static int listing_tag_write_start(omron_tag_p tag);
static int udt_tag_check_read_metadata_status_connected(omron_tag_p tag);
static int udt_tag_build_read_metadata_request_connected(omron_tag_p tag);
static int udt_tag_check_read_fields_status_connected(omron_tag_p tag);
static int udt_tag_build_read_fields_request_connected(omron_tag_p tag);



/* define the vtable for udt tag type. */
static struct tag_vtable_t udt_tag_vtable = {
    (tag_vtable_func)omron_tag_abort, /* shared */
    (tag_vtable_func)udt_tag_read_start,
    (tag_vtable_func)omron_tag_status, /* shared */
    (tag_vtable_func)udt_tag_tickler,
    (tag_vtable_func)NULL, /* write */
    (tag_vtable_func)NULL, /* wake_plc */

    /* attribute accessors */
    omron_get_int_attrib,
    omron_set_int_attrib,

    omron_get_byte_array_attrib
};


/* strings are zero terminated. */
static tag_byte_order_t omron_udt_tag_byte_order = {
    .is_allocated = 0,

    .int16_order = {0,1},
    .int32_order = {0,1,2,3},
    .int64_order = {0,1,2,3,4,5,6,7},
    .float32_order = {0,1,2,3},
    .float64_order = {0,1,2,3,4,5,6,7},

    .str_is_defined = 1,
    .str_is_counted = 0,
    .str_is_fixed_length = 0,
    .str_is_zero_terminated = 1,
    .str_is_byte_swapped = 0,

    .str_count_word_bytes = 0,
    .str_max_capacity = 0,
    .str_total_length = 0,
    .str_pad_bytes = 0
};



/*************************************************************************
 **************************** UDT Functions ******************************
 ************************************************************************/


/*
 * Handle UDT tag set up.
 */

int omron_setup_udt_tag(omron_tag_p tag, const char *name)
{
    int rc = PLCTAG_STATUS_OK;
    const char *tag_id_str = name + str_length("@udt/");
    int tag_id = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* decode the UDT ID */
    rc = str_to_int(tag_id_str, &tag_id);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Badly formatted or missing UDT id in UDT string %s!", name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(tag_id < 0 || tag_id > 4095) {
        pdebug(DEBUG_WARN, "UDT ID must be between 0 and 4095 but was %d!", tag_id);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /*fill in the blanks. */
    tag->udt_id = (uint16_t)(unsigned int)tag_id;
    tag->special_tag = 1;
    tag->elem_type = OMRON_TYPE_TAG_UDT;
    tag->elem_count = 1;
    tag->elem_size = 1;

    tag->byte_order = &omron_udt_tag_byte_order;

    tag->vtable = &udt_tag_vtable;

    pdebug(DEBUG_INFO, "Done. Found UDT tag name %s.", name);

    return rc;
}



/*
 * udt_tag_read_start
 *
 * This function must be called only from within one thread, or while
 * the tag's mutex is locked.
 *
 * The function starts the process of getting UDT data from the PLC.
 */

int udt_tag_read_start(omron_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    if(tag->write_in_progress) {
        pdebug(DEBUG_WARN, "A write is in progress on a UDT tag!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->read_in_progress) {
        pdebug(DEBUG_WARN, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* mark the tag read in progress */
    tag->read_in_progress = 1;

    /* set up the state for the requests (there are two!) */
    tag->udt_get_fields = 0;
    tag->offset = 0;

    /* build the new request */
    rc = udt_tag_build_read_metadata_request_connected(tag);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to build read request!");

        tag->read_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}



int udt_tag_tickler(omron_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW,"Starting.");

    if (tag->read_in_progress) {
        if(tag->elem_type == OMRON_TYPE_TAG_RAW) {
            pdebug(DEBUG_WARN, "Something started a read on a raw tag.  This is not supported!");
            tag->read_in_progress = 0;
            tag->read_in_flight = 0;
        }

        if(tag->udt_get_fields) {
            rc = udt_tag_check_read_fields_status_connected(tag);
        } else {
            rc = udt_tag_check_read_metadata_status_connected(tag);
        }

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->read_in_progress) {
            pdebug(DEBUG_DETAIL, "Read complete.");
            tag->read_complete = 1;
        }

        pdebug(DEBUG_SPEW,"Done.  Read in progress.");

        return rc;
    }

    pdebug(DEBUG_SPEW, "Done.  No operation in progress.");

    return tag->status;
}









/*
 * udt_tag_check_read_metadata_status_connected
 *
 * This routine checks for any outstanding tag udt requests.  It will
 * terminate when there is no data in the response and the error is not "more data".
 *
 * This is not thread-safe!  It should be called with the tag mutex
 * locked!
 */

static int udt_tag_check_read_metadata_status_connected(omron_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp* cip_resp;
    uint8_t* data;
    uint8_t* data_end;
    int partial_data = 0;
    omron_request_p request = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_ERROR,"Null tag pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* guard against the request being deleted out from underneath us. */
    request = rc_inc(tag->req);
    rc = omron_check_read_reqest_status(tag, request);
    if(rc != PLCTAG_STATUS_OK)  {
        pdebug(DEBUG_DETAIL, "Read request status is not OK.");
        rc_dec(request);
        return rc;
    }

    /* the request reference is still valid. */

    /* point to the data */
    cip_resp = (eip_cip_co_resp*)(request->data);

    /* point to the start of the data */
    data = (request->data) + sizeof(eip_cip_co_resp);

    /* point the end of the data */
    data_end = (request->data + le2h16(cip_resp->encap_length) + sizeof(eip_encap));

    /* check the status */
    do {
        ptrdiff_t payload_size = (data_end - data);

        if (le2h16(cip_resp->encap_command) != OMRON_EIP_CONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", cip_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (le2h32(cip_resp->encap_status) != OMRON_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(cip_resp->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if (cip_resp->reply_service != (OMRON_EIP_CMD_CIP_GET_ATTR_LIST | OMRON_EIP_CMD_CIP_OK) ) {
            pdebug(DEBUG_WARN, "CIP response reply service unexpected: %d", cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (cip_resp->status != OMRON_CIP_STATUS_OK && cip_resp->status != OMRON_CIP_STATUS_FRAG) {
            pdebug(DEBUG_WARN, "CIP read failed with status: 0x%x %s", cip_resp->status, cip.decode_cip_error_short((uint8_t *)&cip_resp->status));
            pdebug(DEBUG_INFO, cip.decode_cip_error_long((uint8_t *)&cip_resp->status));
            rc = cip.decode_cip_error_code((uint8_t *)&cip_resp->status);
            break;
        }

        /* check to see if this is a partial response. */
        partial_data = (cip_resp->status == OMRON_CIP_STATUS_FRAG);

        /*
         * check to see if there is any data to process.  If this is a packed
         * response, there might not be.
         */
        if(payload_size > 0 && !partial_data) {
            uint8_t *new_buffer = NULL;
            int new_size = 14; /* MAGIC, size of the header below */
            uint32_le tmp_u32;
            uint16_le tmp_u16;
            uint8_t *payload = (uint8_t *)(cip_resp + 1);

            /*
             * We are going to build a 14-byte fake header in the buffer:
             *
             * Bytes   Meaning
             * 0-1     16-bit UDT ID
             * 2-5     32-bit UDT member description size, in 32-bit words.
             * 6-9     32-bit UDT instance size, in bytes.
             * 10-11   16-bit UDT number of members (fields).
             * 12-13   16-bit UDT handle/type.
             */

            pdebug(DEBUG_DETAIL, "Increasing tag buffer size to %d bytes.", new_size); /* MAGIC */

            new_buffer = (uint8_t*)mem_realloc(tag->data, new_size);
            if(!new_buffer) {
                pdebug(DEBUG_WARN, "Unable to reallocate tag data memory!");
                rc = PLCTAG_ERR_NO_MEM;
                break;
            }

            tag->data = new_buffer;
            tag->size = new_size;
            tag->elem_count = 1;
            tag->elem_size = new_size;

            /* fill in the data. */

            /* put in the UDT ID */
            tmp_u16 = h2le16(tag->udt_id);
            mem_copy(tag->data + 0, &tmp_u16, (int)(unsigned int)(sizeof(tmp_u16)));

            /* copy in the UDT member description size in 32-bit words */
            mem_copy(tag->data + 2, payload + 6, (int)(unsigned int)(sizeof(tmp_u32)));

            /* copy in the UDT instance size in bytes */
            mem_copy(tag->data + 6, payload + 14, (int)(unsigned int)(sizeof(tmp_u32)));

            /* copy in the UDT number of members */
            mem_copy(tag->data + 10, payload + 22, (int)(unsigned int)(sizeof(tmp_u16)));

            /* copy in the UDT number of members */
            mem_copy(tag->data + 12, payload + 28, (int)(unsigned int)(sizeof(tmp_u16)));

            pdebug(DEBUG_DETAIL, "current size %d", tag->size);
            pdebug_dump_bytes(DEBUG_DETAIL, tag->data, tag->size);
        } else {
            pdebug(DEBUG_DETAIL, "Response returned no data and no error.");
        }

        /* set the return code */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request */
    request->abort_request = 1;
    tag->req = rc_dec(request);

    /*
     * huh?  Yes, we do it a second time because we already had
     * a reference and got another at the top of this function.
     * So we need to remove it twice.   Once for the capture above,
     * and once for the original reference.
     */

    rc_dec(request);

    /* are we actually done? */
    if (rc == PLCTAG_STATUS_OK) {
        /* keep going if we are not done yet. */
        if (partial_data) {
            /* call read start again to try again.  The data returned might be zero bytes if this is a packed result */
            pdebug(DEBUG_DETAIL, "calling udt_tag_build_read_metadata_request_connected() to try again.");
            rc = udt_tag_build_read_metadata_request_connected(tag);
        } else {
            /* done! */
            pdebug(DEBUG_DETAIL, "Done reading udt metadata!");

            tag->elem_count = 1;
            tag->offset = 0;
            tag->udt_get_fields = 1;

            pdebug(DEBUG_DETAIL, "calling udt_tag_build_read_fields_request_connected() to get field data.");
            rc = udt_tag_build_read_fields_request_connected(tag);
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        /* error ! */
        pdebug(DEBUG_WARN, "Error received: %s!", plc_tag_decode_error(rc));

        tag->offset = 0;
        tag->udt_get_fields = 0;

        /* clean up everything. */
        omron_tag_abort(tag);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}




int udt_tag_build_read_metadata_request_connected(omron_tag_p tag)
{
    eip_cip_co_req* cip = NULL;
    //tag_list_req *list_req = NULL;
    omron_request_p req = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t *data_start = NULL;
    uint8_t *data = NULL;
    uint16_le tmp_u16 = UINT16_LE_INIT(0);

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = conn_create_request(tag->conn, tag->tag_id, &req);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    /* point the request struct at the buffer */
    cip = (eip_cip_co_req*)(req->data);

    /* point to the end of the struct */
    data_start = data = (uint8_t*)(cip + 1);

    /*
     * set up the embedded CIP UDT metadata request packet
        uint8_t request_service;    OMRON_EIP_CMD_CIP_GET_ATTR_LIST=0x03
        uint8_t request_path_size;  3 word = 6 bytes
        uint8_t request_path[6];        0x20    get class
                                        0x6C    UDT class
                                        0x25    get instance (16-bit)
                                        0x00    padding
                                        0x00    instance byte 0
                                        0x00    instance byte 1
        uint16_le instance_id;      NOTE! this is the last two bytes above for convenience!
        uint16_le num_attributes;   0x04    number of attributes to get
        uint16_le requested_attributes[4];      0x04    attribute #4 - Number of 32-bit words in the template definition.
                                                0x05    attribute #5 - Number of bytes in the structure on the wire.
                                                0x02    attribute #2 - Number of structure members.
                                                0x01    attribute #1 - Handle/type of structure.
    */

    *data = OMRON_EIP_CMD_CIP_GET_ATTR_LIST;
    data++;

    /* request path size, in 16-bit words */
    *data = (uint8_t)(3); /* size in words of routing header + routing and instance ID. */
    data++;

    /* add in the routing header . */

    /* first the fixed part. */
    data[0] = 0x20; /* class type */
    data[1] = 0x6C; /* UDT class */
    data[2] = 0x25; /* 16-bit instance ID type */
    data[3] = 0x00; /* padding */
    data += 4;

    /* now the instance ID */
    tmp_u16 = h2le16((uint16_t)tag->udt_id);
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* set up the request itself.  We are asking for a number of attributes. */

    /* set up the request attributes, first the number of attributes. */
    tmp_u16 = h2le16((uint16_t)4);  /* MAGIC, we have four attributes we want. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* first attribute: symbol type */
    tmp_u16 = h2le16((uint16_t)0x04);  /* MAGIC, Total field definition size in 32-bit words. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* second attribute: base type size in bytes */
    tmp_u16 = h2le16((uint16_t)0x05);  /* MAGIC, struct size in bytes. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* third attribute: tag array dimensions */
    tmp_u16 = h2le16((uint16_t)0x02);  /* MAGIC, number of structure members. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* fourth attribute: symbol/tag name */
    tmp_u16 = h2le16((uint16_t)0x01);  /* MAGIC, struct type/handle. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(OMRON_EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Connected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(OMRON_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(OMRON_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length = h2le16((uint16_t)((int)(data - data_start) + (int)sizeof(cip->cpf_conn_seq_num)));

    /* set the size of the request */
    req->request_size = (int)((int)sizeof(*cip) + (int)(data - data_start));

    req->allow_packing = tag->allow_packing;

    /* add the request to the conn's list. */
    rc = conn_add_request(tag->conn, req);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to conn! rc=%d", rc);
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}








/*
 * udt_tag_check_read_fields_status_connected
 *
 * This routine checks for any outstanding tag udt field data requests.  It will
 * terminate when there is no data in the response and the error is not "more data".
 *
 * This is not thread-safe!  It should be called with the tag mutex
 * locked!
 */

int udt_tag_check_read_fields_status_connected(omron_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp* cip_resp;
    uint8_t* data;
    uint8_t* data_end;
    int partial_data = 0;
    omron_request_p request = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_ERROR,"Null tag pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* guard against the request being deleted out from underneath us. */
    request = rc_inc(tag->req);
    rc = omron_check_read_reqest_status(tag, request);
    if(rc != PLCTAG_STATUS_OK)  {
        pdebug(DEBUG_DETAIL, "Read request status is not OK.");
        rc_dec(request);
        return rc;
    }

    /* the request reference is still valid. */

    /* point to the data */
    cip_resp = (eip_cip_co_resp*)(request->data);

    /* point to the start of the data */
    data = (request->data) + sizeof(eip_cip_co_resp);

    /* point the end of the data */
    data_end = (request->data + le2h16(cip_resp->encap_length) + sizeof(eip_encap));

    /* check the status */
    do {
        ptrdiff_t payload_size = (data_end - data);

        if (le2h16(cip_resp->encap_command) != OMRON_EIP_CONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", cip_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (le2h32(cip_resp->encap_status) != OMRON_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(cip_resp->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if (cip_resp->reply_service != (OMRON_EIP_CMD_CIP_READ | OMRON_EIP_CMD_CIP_OK) ) {
            pdebug(DEBUG_WARN, "CIP response reply service unexpected: %d", cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (cip_resp->status != OMRON_CIP_STATUS_OK && cip_resp->status != OMRON_CIP_STATUS_FRAG) {
            pdebug(DEBUG_WARN, "CIP read failed with status: 0x%x %s", cip_resp->status, cip.decode_cip_error_short((uint8_t *)&cip_resp->status));
            pdebug(DEBUG_INFO, cip.decode_cip_error_long((uint8_t *)&cip_resp->status));
            rc = cip.decode_cip_error_code((uint8_t *)&cip_resp->status);
            break;
        }

        /* check to see if this is a partial response. */
        partial_data = (cip_resp->status == OMRON_CIP_STATUS_FRAG);

        /*
         * check to see if there is any data to process.  If this is a packed
         * response, there might not be.
         */
        if(payload_size > 0) {
            uint8_t *new_buffer = NULL;
            int new_size = (int)(tag->size) + (int)payload_size;

            pdebug(DEBUG_DETAIL, "Increasing tag buffer size to %d bytes.", new_size);

            new_buffer = (uint8_t*)mem_realloc(tag->data, new_size);
            if(!new_buffer) {
                pdebug(DEBUG_WARN, "Unable to reallocate tag data memory!");
                rc = PLCTAG_ERR_NO_MEM;
                break;
            }

            /* copy the data into the tag's data buffer. */
            mem_copy(new_buffer + tag->offset + 14, data, (int)payload_size); /* MAGIC, offset plus the header. */

            tag->data = new_buffer;
            tag->size = new_size;
            tag->elem_size = new_size;

            tag->offset += (int)payload_size;

            pdebug(DEBUG_DETAIL, "payload of %d (%x) bytes resulting in current offset %d", (int)payload_size, (int)payload_size, tag->offset);
        } else {
            pdebug(DEBUG_DETAIL, "Response returned no data and no error.");
        }

        /* set the return code */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request */
    request->abort_request = 1;
    tag->req = rc_dec(request);

    /*
     * huh?  Yes, we do it a second time because we already had
     * a reference and got another at the top of this function.
     * So we need to remove it twice.   Once for the capture above,
     * and once for the original reference.
     */

    rc_dec(request);

    /* are we actually done? */
    if (rc == PLCTAG_STATUS_OK) {
        /* keep going if we are not done yet. */
        if (partial_data) {
            /* call read start again to try again.  The data returned might be zero bytes if this is a packed result */
            pdebug(DEBUG_DETAIL, "calling udt_tag_build_read_metadata_request_connected() to try again.");
            rc = udt_tag_build_read_fields_request_connected(tag);
        } else {
            /* done! */
            pdebug(DEBUG_DETAIL, "Done reading udt field data.  Tag buffer contains:");
            pdebug_dump_bytes(DEBUG_DETAIL, tag->data, tag->size);

            tag->elem_count = 1;

            /* this read is done. */
            tag->udt_get_fields = 0;
            tag->read_in_progress = 0;
            tag->offset = 0;
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        /* error ! */
        pdebug(DEBUG_WARN, "Error received: %s!", plc_tag_decode_error(rc));

        tag->offset = 0;
        tag->udt_get_fields = 0;

        /* clean up everything. */
        omron_tag_abort(tag);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}




int udt_tag_build_read_fields_request_connected(omron_tag_p tag)
{
    eip_cip_co_req* cip = NULL;
    //tag_list_req *list_req = NULL;
    omron_request_p req = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t *data_start = NULL;
    uint8_t *data = NULL;
    uint16_le tmp_u16 = UINT16_LE_INIT(0);
    uint32_le tmp_u32 = UINT32_LE_INIT(0);
    uint32_t total_size = 0;
    uint32_t neg_4 = (~(uint32_t)4) + 1; /* twos-complement */

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = conn_create_request(tag->conn, tag->tag_id, &req);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    /* calculate the total size we need to get. */
    mem_copy(&tmp_u32, tag->data + 2, (int)(unsigned int)(sizeof(tmp_u32)));
    total_size = (4 * le2h32(tmp_u32)) - 23; /* formula according to the docs. */

    pdebug(DEBUG_DETAIL, "Calculating total size of request, %d to %d.", (int)(unsigned int)total_size, (int)(unsigned int)((total_size + (uint32_t)3) & (uint32_t)neg_4));

    /* make the total size a multiple of 4 bytes.  Round up. */
    total_size = (total_size + 3) & (uint32_t)neg_4;

    /* point the request struct at the buffer */
    cip = (eip_cip_co_req*)(req->data);

    /* point to the end of the struct */
    data_start = data = (uint8_t*)(cip + 1);

    /*
     * set up the embedded CIP UDT metadata request packet
        uint8_t request_service;        OMRON_EIP_CMD_CIP_READ=0x4C
        uint8_t request_path_size;      3 word = 6 bytes
        uint8_t request_path[6];        0x20    get class
                                        0x6C    UDT class
                                        0x25    get instance (16-bit)
                                        0x00    padding
                                        0x00    instance byte 0
                                        0x00    instance byte 1
        uint32_t offset;                Byte offset in ongoing requests.
        uint16_t total_size;            Total size of request in bytes.
    */

    *data = OMRON_EIP_CMD_CIP_READ;
    data++;

    /* request path size, in 16-bit words */
    *data = (uint8_t)(3); /* size in words of routing header + routing and instance ID. */
    data++;

    /* add in the routing header . */

    /* first the fixed part. */
    data[0] = 0x20; /* class type */
    data[1] = 0x6C; /* UDT class */
    data[2] = 0x25; /* 16-bit instance ID type */
    data[3] = 0x00; /* padding */
    data += 4;

    /* now the instance ID */
    tmp_u16 = h2le16((uint16_t)tag->udt_id);
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* set the offset */
    tmp_u32 = h2le32((uint32_t)(tag->offset));
    mem_copy(data, &tmp_u32, (int)(unsigned int)sizeof(tmp_u32));
    data += sizeof(tmp_u32);

    /* set the total size */
    pdebug(DEBUG_DETAIL, "Total size %d less offset %d gives %d bytes for the request.", total_size, tag->offset, ((int)(unsigned int)total_size - tag->offset));
    tmp_u16 = h2le16((uint16_t)(total_size - (uint16_t)(unsigned int)tag->offset));
    mem_copy(data, &tmp_u16, (int)(unsigned int)sizeof(tmp_u16));
    data += sizeof(tmp_u16);

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(OMRON_EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Connected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(OMRON_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(OMRON_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length = h2le16((uint16_t)((int)(data - data_start) + (int)sizeof(cip->cpf_conn_seq_num)));

    /* set the size of the request */
    req->request_size = (int)((int)sizeof(*cip) + (int)(data - data_start));

    req->allow_packing = tag->allow_packing;

    /* add the request to the conn's list. */
    rc = conn_add_request(tag->conn, req);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to conn! rc=%d", rc);
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}
