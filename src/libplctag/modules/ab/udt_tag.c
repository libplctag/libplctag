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

/* byte offset of the UDT handle/type attribute within a Get_Attributes_List response payload */
#define UDT_METADATA_HANDLE_OFFSET (28)

/* UDT tag functions. */
static int udt_tag_read_start(ab_tag_p tag);
static int udt_tag_tickler(ab_tag_p tag);
// static int listing_tag_write_start(ab_tag_p tag);
static int udt_tag_check_read_metadata_status_connected(ab_tag_p tag);
static int udt_tag_build_read_metadata_request_connected(ab_tag_p tag);
static int udt_tag_check_read_fields_status_connected(ab_tag_p tag);
static int udt_tag_build_read_fields_request_connected(ab_tag_p tag);

/* define the vtable for udt tag type. */
static struct tag_vtable_t udt_tag_vtable = {
    .abort = (tag_vtable_func)ab_tag_abort_request,
    .read = (tag_vtable_func)udt_tag_read_start,
    .status = (tag_vtable_func)ab_tag_status,
    .tickler = (tag_vtable_func)udt_tag_tickler,
    .write = NULL,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .attribs = ab_attribs,
};

static tag_byte_order_t udt_tag_logix_byte_order = {.is_allocated = 0,

                                                    .int16_order = {0, 1},
                                                    .int32_order = {0, 1, 2, 3},
                                                    .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                                    .float32_order = {0, 1, 2, 3},
                                                    .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                                    .str_is_defined = 1,
                                                    .str_is_counted = 0,
                                                    .str_is_fixed_length = 0,
                                                    .str_is_zero_terminated = 1,
                                                    .str_is_byte_swapped = 0,

                                                    .str_pad_to_multiple_bytes = 1,
                                                    .str_count_word_bytes = 0,
                                                    .str_max_capacity = 0,
                                                    .str_total_length = 0,
                                                    .str_pad_bytes = 0};

/******************************************************************
 ******************* UDT listing functions ************************
 ******************************************************************/


/*
 * Handle UDT tag set up.
 */

int setup_udt_tag(ab_tag_p tag, const char *name) {
    int rc = PLCTAG_STATUS_OK;
    const char *tag_id_str = name + str_length("@udt/");
    int tag_id = 0;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Starting.");

    /* decode the UDT ID */
    rc = str_to_int(tag_id_str, &tag_id);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Badly formatted or missing UDT id in UDT string %s!",
               name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(tag_id < 0 || tag_id > 4095) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "UDT ID must be between 0 and 4095 but was %d!", tag_id);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /*fill in the blanks. */
    tag->udt_id = (uint16_t)(unsigned int)tag_id;
    tag->special_tag = 1;
    tag->elem_type = CIP_TYPE_TAG_UDT;
    tag->elem_count = 1;
    tag->elem_size = 1;

    tag->byte_order = &udt_tag_logix_byte_order;

    tag->vtable = &udt_tag_vtable;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done. Found UDT tag name %s.", name);

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

int udt_tag_read_start(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "A write is in progress on a UDT tag!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* mark the tag read in progress */
    tag->read_in_progress = 1;

    /* set up the state for the requests (there are two!) */
    tag->udt_get_fields = 0;
    tag->offset = 0;

    /* build the new request */
    rc = udt_tag_build_read_metadata_request_connected(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to build read request!");

        tag->read_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int udt_tag_tickler(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Something started a write on a UDT tag.   This is not supported!");

        ab_tag_abort_request(tag);

        /* fire the event anyway. */
        tag->write_complete = 1;

        return PLCTAG_ERR_UNSUPPORTED;
    }

    if(tag->read_in_progress) {
        if(tag->udt_get_fields) {
            rc = udt_tag_check_read_fields_status_connected(tag);
        } else {
            rc = udt_tag_check_read_metadata_status_connected(tag);
        }

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->read_in_progress) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Read complete.");
            tag->read_complete = 1;
        } else {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done. Read in progress.");
        }

        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.  No operation in progress.");

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

int udt_tag_check_read_metadata_status_connected(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp *cip_resp;
    uint8_t *data;
    uint8_t *data_end;
    int partial_data = 0;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* if we got here then we have a response and it is valid. */

    /* point to the data */
    cip_resp = (eip_cip_co_resp *)(tag->req->data);

    /* point to the start of the data */
    data = (tag->req->data) + sizeof(eip_cip_co_resp);

    /* point the end of the data */
    data_end = tag->req->data + tag->req->request_size;

    /* check the status */
    do {
        ptrdiff_t payload_size = (data_end - data);

        if(cip_resp->reply_service != (AB_EIP_CMD_CIP_GET_ATTR_LIST | AB_EIP_CMD_CIP_OK)) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "CIP response reply service unexpected: %d",
                   cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(cip_resp->status != AB_CIP_STATUS_OK && cip_resp->status != AB_CIP_STATUS_FRAG) {
            size_t status_size = cip_error_data_size((uint8_t *)&cip_resp->status, data_end);

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "CIP read failed with status: 0x%x %s",
                   cip_resp->status, decode_cip_error_short((uint8_t *)&cip_resp->status, status_size));
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id,
                   decode_cip_error_long((uint8_t *)&cip_resp->status, status_size));
            rc = decode_cip_error_code((uint8_t *)&cip_resp->status, status_size);
            break;
        }

        /* check to see if this is a partial response. */
        partial_data = (cip_resp->status == AB_CIP_STATUS_FRAG);

        /*
         * check to see if there is any data to process.  If this is a packed
         * response, there might not be.
         */

        /* FIXME - it is not clear, but since we do not update tag->offset, a partial result
           will trigger a full retry.
       */
        if(payload_size > 0 && !partial_data) {
            /* we got the metadata, so the transfer made progress. */
            uint8_t *new_buffer = NULL;
            int new_size = 14; /* MAGIC, size of the header below */
            uint32_le tmp_u32;
            uint16_le tmp_u16;
            uint8_t *payload = (uint8_t *)(cip_resp + 1);
            int min_payload_size = UDT_METADATA_HANDLE_OFFSET + (int)sizeof(uint16_le);

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

            if(payload_size < min_payload_size) {
                pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
                       "UDT metadata response is too short, got %d bytes but need at least %d!", (int)payload_size,
                       min_payload_size);
                rc = PLCTAG_ERR_TOO_SMALL;
                break;
            }

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Increasing tag buffer size to %d bytes.",
                   new_size); /* MAGIC */

            new_buffer = (uint8_t *)mem_realloc(tag->data, new_size);
            if(!new_buffer) {
                pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data memory!");
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
            mem_copy(tag->data + 12, payload + UDT_METADATA_HANDLE_OFFSET, (int)(unsigned int)(sizeof(tmp_u16)));

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "current size %d", tag->size);
            pdebug_dump_bytes(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, tag->data, tag->size);
        } else if(partial_data) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Partial response, no data to process yet.");

            /* tag->offset is not advanced here, so a partial result retries the whole read. */
            tag->fragment_retry_count++;
        } else {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "UDT metadata response contained no data!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        /* set the return code */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* the old request is done */
    ab_tag_abort_request(tag);

    /* are we actually done? */
    if(rc == PLCTAG_STATUS_OK) {
        /* keep going if we are not done yet, unless we are getting nowhere. */
        if(partial_data && tag->fragment_retry_count > MAX_FRAGMENT_RETRIES) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
                   "Got %d partial responses in a row with no data.  The transfer is not making progress, giving up.",
                   tag->fragment_retry_count);
            rc = PLCTAG_ERR_PARTIAL;
        } else if(partial_data) {
            /* call read start again to try again.  The data returned might be zero bytes if this is a packed result */
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id,
                   "calling udt_tag_build_read_metadata_request_connected() to try again.");
            rc = udt_tag_build_read_metadata_request_connected(tag);
        } else {
            /* done!  Start the next transfer with a clean progress counter. */
            tag->fragment_retry_count = 0;

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Done reading udt metadata!");

            tag->elem_count = 1;
            tag->offset = 0;
            tag->udt_get_fields = 1;

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id,
                   "calling udt_tag_build_read_fields_request_connected() to get field data.");
            rc = udt_tag_build_read_fields_request_connected(tag);

            /* an OK from the builder means that we need to return PENDING because we just queued the new request*/
            if(rc == PLCTAG_STATUS_OK) { rc = PLCTAG_STATUS_PENDING; }
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc_is_error(rc)) {
        /* error ! */
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Error received: %s!", plc_tag_decode_error(rc));

        tag->offset = 0;
        tag->udt_get_fields = 0;

        /* clean up everything in case we managed to create a request before failing. */
        ab_tag_abort_request(tag);
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


int udt_tag_build_read_metadata_request_connected(ab_tag_p tag) {
    eip_cip_co_req *cip = NULL;
    // tag_list_req *list_req = NULL;
    ab_request_p req = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t *data_start = NULL;
    uint8_t *data = NULL;
    uint16_le tmp_u16 = UINT16_LE_INIT(0);

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    /* point the request struct at the buffer */
    cip = (eip_cip_co_req *)(req->data);

    /* point to the end of the struct */
    data_start = data = (uint8_t *)(cip + 1);

    /*
     * set up the embedded CIP UDT metadata request packet
        uint8_t request_service;    AB_EIP_CMD_CIP_GET_ATTR_LIST=0x03
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

    *data = AB_EIP_CMD_CIP_GET_ATTR_LIST;
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
    tmp_u16 = h2le16((uint16_t)4); /* MAGIC, we have four attributes we want. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* first attribute: symbol type */
    tmp_u16 = h2le16((uint16_t)0x04); /* MAGIC, Total field definition size in 32-bit words. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* second attribute: base type size in bytes */
    tmp_u16 = h2le16((uint16_t)0x05); /* MAGIC, struct size in bytes. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* third attribute: tag array dimensions */
    tmp_u16 = h2le16((uint16_t)0x02); /* MAGIC, number of structure members. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* fourth attribute: symbol/tag name */
    tmp_u16 = h2le16((uint16_t)0x01); /* MAGIC, struct type/handle. */
    mem_copy(data, &tmp_u16, (int)sizeof(tmp_u16));
    data += (int)sizeof(tmp_u16);

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(AB_EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Connected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI); /* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);             /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI); /* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length = h2le16((uint16_t)((int)(data - data_start) + (int)sizeof(cip->cpf_conn_seq_num)));

    /* Check if the payload size exceeds available space before setting request_size */
    int packet_payload_size = (int)(data - data_start) + (int)sizeof(cip->cpf_conn_seq_num);
    int available_payload = session_get_available_cip_payload_space(tag->session);

    if(packet_payload_size > available_payload) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Request payload (%d bytes) exceeds available space (%d bytes)!", packet_payload_size, available_payload);
        ab_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* set the size of the request */
    req->request_size = (int)((int)sizeof(*cip) + (int)(data - data_start));

    req->allow_packing = tag->allow_packing;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        ab_tag_abort_request(tag);
        return rc;
    }

    tag->read_in_progress = 1;

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done");

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

int udt_tag_check_read_fields_status_connected(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp *cip_resp;
    uint8_t *data;
    uint8_t *data_end;
    int partial_data = 0;

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* the request is there and valid */

    /* point to the data */
    cip_resp = (eip_cip_co_resp *)(tag->req->data);

    /* point to the start of the data */
    data = (tag->req->data) + sizeof(eip_cip_co_resp);

    /* point the end of the data */
    data_end = tag->req->data + tag->req->request_size;

    /* check the status */
    do {
        ptrdiff_t payload_size = (data_end - data);

        if(cip_resp->reply_service != (AB_EIP_CMD_CIP_READ | AB_EIP_CMD_CIP_OK)) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "CIP response reply service unexpected: %d",
                   cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(cip_resp->status != AB_CIP_STATUS_OK && cip_resp->status != AB_CIP_STATUS_FRAG) {
            size_t status_size = cip_error_data_size((uint8_t *)&cip_resp->status, data_end);

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "CIP read failed with status: 0x%x %s",
                   cip_resp->status, decode_cip_error_short((uint8_t *)&cip_resp->status, status_size));
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id,
                   decode_cip_error_long((uint8_t *)&cip_resp->status, status_size));
            rc = decode_cip_error_code((uint8_t *)&cip_resp->status, status_size);
            break;
        }

        /* check to see if this is a partial response. */
        partial_data = (cip_resp->status == AB_CIP_STATUS_FRAG);

        /*
         * check to see if there is any data to process.  If this is a packed
         * response, there might not be.
         */
        if(payload_size > 0) {
            /* we got data, so the transfer is moving again. */
            tag->fragment_retry_count = 0;

            uint8_t *new_buffer = NULL;
            int new_size = (int)(tag->size) + (int)payload_size;

            /* a PLC can keep returning fragments forever.  Do not grow without bound. */
            if(((ptrdiff_t)tag->size + payload_size) > (ptrdiff_t)AB_MAX_TAG_DATA_SIZE) {
                pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
                       "UDT field data size of %d bytes is larger than the maximum of %d bytes!",
                       (int)((ptrdiff_t)tag->size + payload_size), AB_MAX_TAG_DATA_SIZE);
                rc = PLCTAG_ERR_TOO_LARGE;
                break;
            }

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Increasing tag buffer size to %d bytes.",
                   new_size);

            new_buffer = (uint8_t *)mem_realloc(tag->data, new_size);
            if(!new_buffer) {
                pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data memory!");
                rc = PLCTAG_ERR_NO_MEM;
                break;
            }

            /* copy the data into the tag's data buffer. */
            mem_copy(new_buffer + tag->offset + 14, data, (int)payload_size); /* MAGIC, offset plus the header. */

            tag->data = new_buffer;
            tag->size = new_size;
            tag->elem_size = new_size;

            tag->offset += (int)payload_size;

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id,
                   "payload of %d (%x) bytes resulting in current offset %d", (int)payload_size, (int)payload_size, tag->offset);
        } else {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Response returned no data and no error.");

            /* no payload means no forward progress on a fragmented transfer. */
            tag->fragment_retry_count++;
        }
    } while(0);

    /* get rid of the old request. we are done with it. */
    ab_tag_abort_request_only(tag);

    /* are we actually done? */
    if(rc == PLCTAG_STATUS_OK) {
        /* keep going if we are not done yet, unless we are getting nowhere. */
        if(partial_data && tag->fragment_retry_count > MAX_FRAGMENT_RETRIES) {
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
                   "Got %d partial responses in a row with no data.  The transfer is not making progress, giving up.",
                   tag->fragment_retry_count);
            rc = PLCTAG_ERR_PARTIAL;
        } else if(partial_data) {
            /* call read start again to try again.  The data returned might be zero bytes if this is a packed result */
            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id,
                   "calling udt_tag_build_read_metadata_request_connected() to try again.");
            rc = udt_tag_build_read_fields_request_connected(tag);

            /* if we get OK, we need to return PENDING for the new request. */
            if(rc == PLCTAG_STATUS_OK) { rc = PLCTAG_STATUS_PENDING; }
        } else {
            /* done!  Start the next transfer with a clean progress counter. */
            tag->fragment_retry_count = 0;

            pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id,
                   "Done reading UDT field data.  Tag buffer contains:");
            pdebug_dump_bytes(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, tag->data, tag->size);

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
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id, "Error received: %s!", plc_tag_decode_error(rc));

        tag->offset = 0;
        tag->udt_get_fields = 0;

        /* clean up everything. */
        ab_tag_abort_request(tag);
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


int udt_tag_build_read_fields_request_connected(ab_tag_p tag) {
    eip_cip_co_req *cip = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t *data_start = NULL;
    uint8_t *data = NULL;
    uint16_le tmp_u16 = UINT16_LE_INIT(0);
    uint32_le tmp_u32 = UINT32_LE_INIT(0);
    uint32_t total_size = 0;
    uint32_t neg_4 = (~(uint32_t)4) + 1; /* twos-complement */

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &tag->req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%d", rc);
        ab_tag_abort_request(tag);
        return rc;
    }

    /* calculate the total size we need to get. */
    mem_copy(&tmp_u32, tag->data + 2, (int)(unsigned int)(sizeof(tmp_u32)));
    total_size = (4 * le2h32(tmp_u32)) - 23; /* formula according to the docs. */

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id, "Calculating total size of request, %d to %d.",
           (int)(unsigned int)total_size, (int)(unsigned int)((total_size + (uint32_t)3) & (uint32_t)neg_4));

    /* make the total size a multiple of 4 bytes.  Round up. */
    total_size = (total_size + 3) & (uint32_t)neg_4;

    /* point the request struct at the buffer */
    cip = (eip_cip_co_req *)(tag->req->data);

    /* point to the end of the struct */
    data_start = data = (uint8_t *)(cip + 1);

    /*
     * set up the embedded CIP UDT metadata request packet
        uint8_t request_service;        AB_EIP_CMD_CIP_READ=0x4C
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

    *data = AB_EIP_CMD_CIP_READ;
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
    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_DETAIL, tag->tag_id,
           "Total size %d less offset %d gives %d bytes for the request.", total_size, tag->offset,
           ((int)(unsigned int)total_size - tag->offset));
    tmp_u16 = h2le16((uint16_t)(total_size - (uint16_t)(unsigned int)tag->offset));
    mem_copy(data, &tmp_u16, (int)(unsigned int)sizeof(tmp_u16));
    data += sizeof(tmp_u16);

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(AB_EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Connected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI); /* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);             /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI); /* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length = h2le16((uint16_t)((int)(data - data_start) + (int)sizeof(cip->cpf_conn_seq_num)));

    /* Check if the payload size exceeds available space before setting request_size */
    int packet_payload_size = (int)(data - data_start) + (int)sizeof(cip->cpf_conn_seq_num);
    int available_payload = session_get_available_cip_payload_space(tag->session);

    if(packet_payload_size > available_payload) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_WARN, tag->tag_id,
               "Request payload (%d bytes) exceeds available space (%d bytes)!", packet_payload_size, available_payload);
        ab_tag_abort_request(tag);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* set the size of the request */
    tag->req->request_size = (int)((int)sizeof(*cip) + (int)(data - data_start));

    tag->req->allow_packing = tag->allow_packing;

    tag->read_in_progress = 1;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, tag->req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_CIP_SPECIAL, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}
