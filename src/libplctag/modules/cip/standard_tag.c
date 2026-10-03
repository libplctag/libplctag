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

/*
 * The read/write engine for ordinary (symbolic, named) CIP tags.
 *
 * One implementation for every CIP family.  The services are the same wire requests
 * everywhere -- Read Tag, Read Tag Fragmented, Write Tag, Write Tag Fragmented and
 * Read-Modify-Write -- so what used to be two files differed only in which of them the
 * family implements.  That is a branch, not a second engine.
 *
 * OMRON NJ/NX is the one family here that has no fragmented services: it answers a whole
 * tag or it refuses, and it never returns a partial status.  tag_supports_fragments()
 * names that, and the three places it matters read it.
 */

#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/cip/error_codes.h>
#include <libplctag/modules/cip/path.h>
#include <libplctag/modules/cip/plc_type.h>
#include <libplctag/modules/cip/services.h>
#include <libplctag/modules/cip/standard_tag.h>
#include <libplctag/modules/cip/tag.h>
#include <libplctag/modules/cip/wire.h>
#include <platform.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <utils/byteorder.h>
#include <utils/debug.h>


static int build_read_request(cip_tag_p tag, int byte_offset);
static int build_write_request(cip_tag_p tag, int byte_offset);
static int build_write_bit_request(cip_tag_p tag);
static int check_read_status(cip_tag_p tag);
static int check_write_status(cip_tag_p tag);
static int calculate_write_data_per_packet(cip_tag_p tag);


static bool tag_supports_fragments(cip_tag_p tag);
static bool tag_elements_are_variable(cip_tag_p tag);
static uint16_t read_request_elem_count(cip_tag_p tag);
static debug_module_t tag_debug_module(cip_tag_p tag);


/*
 * Which debug module this tag's log lines belong to.
 *
 * The module IDs are public (PLCTAG_MODULE_* in libplctag.h) and callers filter on them,
 * so merging the two files must not merge their logging: an OMRON tag still logs under
 * OMRON_STANDARD_TAG and a Rockwell tag still logs under AB_EIP_CIP.
 */
static debug_module_t tag_debug_module(cip_tag_p tag) {
    return (tag->plc_type == CIP_PLC_OMRON_NJNX) ? DEBUG_MODULE_OMRON_STANDARD_TAG : DEBUG_MODULE_AB_EIP_CIP;
}


/*
 * True when the family implements the fragmented read and write services.
 *
 * OMRON NJ/NX does not.  It has no 0x52 or 0x53 service and never answers with status
 * 0x06, so a transfer that will not fit in one request cannot be split and has to be
 * refused up front.
 */
static bool tag_supports_fragments(cip_tag_p tag) { return tag->plc_type != CIP_PLC_OMRON_NJNX; }


/*
 * True when the tag's elements are packed at their own lengths rather than laid out on a
 * fixed stride.
 *
 * A CIP STRING or SHORT_STRING is a count word followed by exactly that many characters, so
 * an array of them has no element size: reaching element N means walking the counts of the N
 * before it, which is what the library's string layer does when str_is_fixed_length is zero.
 * Dividing a reply by the element count would invent a stride that is not there.
 *
 * The type can arrive two ways: declared up front with elem_type, or discovered from the type
 * code the PLC returns with the data.  Both are checked, because a Micro800 string tag is
 * usually created without an elem_type at all.
 *
 * Note that a Logix STRING is not one of these: it is a UDT of a fixed 88 bytes and comes
 * back as an abbreviated struct, not as a 0xD0 or 0xDA type code.
 */
static bool tag_elements_are_variable(cip_tag_p tag) {
    if(tag->elem_type == CIP_TYPE_STRING || tag->elem_type == CIP_TYPE_SHORT_STRING) { return true; }

    if(tag->encoded_type_info_size > 0
       && (tag->encoded_type_info[0] == CIP_DATA_STRING || tag->encoded_type_info[0] == CIP_DATA_SHORT_STRING)) {
        return true;
    }

    return false;
}


/*
 * How many elements a read request should ask for.
 *
 * A pre-read before a write exists only to learn the tag's type and element size;
 * check_read_status() throws its data away and does not chase the remaining fragments.
 * Ask for a single element.  The reply carries the encoded type either way, and one
 * element is the only thing that tells us the element size directly: a full read derives
 * it as size/elem_count, which comes out wrong -- zero, for an array whose first fragment
 * is smaller than elem_count bytes.
 */
static uint16_t read_request_elem_count(cip_tag_p tag) {
    return (uint16_t)(tag->pre_write_read ? 1 : tag->elem_count);
}


/*************************************************************************
 **************************** API Functions ******************************
 ************************************************************************/


int cip_standard_tag_tickler(plc_tag_p tag_arg) {
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = (cip_tag_p)tag_arg;
    debug_module_t dbg = tag_debug_module(tag);

    pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = cip_check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->read_in_progress) {
        rc = check_read_status(tag);

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

        pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Done with status %s.", plc_tag_decode_error(rc));

        return rc;
    }

    if(tag->write_in_progress) {
        rc = check_write_status(tag);

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->write_in_progress) { tag->write_complete = 1; }

        pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Done with status %s.", plc_tag_decode_error(rc));

        return rc;
    }

    pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Done.  No operation in progress.");

    return tag->status;
}


/*
 * cip_standard_tag_read_start
 *
 * This function must be called only from within one thread, or while
 * the tag's mutex is locked.
 *
 * The function starts the process of getting tag data from the PLC.
 */

int cip_standard_tag_read_start(plc_tag_p tag_arg) {
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = (cip_tag_p)tag_arg;
    debug_module_t dbg = tag_debug_module(tag);

    pdebug(dbg, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* mark the tag read in progress */
    tag->read_in_progress = 1;

    rc = build_read_request(tag, tag->offset);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Unable to build read request!");

        tag->read_in_progress = 0;

        return rc;
    }

    pdebug(dbg, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * cip_standard_tag_write_start
 *
 * This must be called from one thread alone, or while the tag mutex is
 * locked.
 *
 * The routine starts the process of writing to a tag.
 */

int cip_standard_tag_write_start(plc_tag_p tag_arg) {
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = (cip_tag_p)tag_arg;
    debug_module_t dbg = tag_debug_module(tag);

    pdebug(dbg, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
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
        pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "No read has completed yet, doing pre-read to get type information.");

        tag->pre_write_read = 1;
        tag->write_in_progress = 0; /* temporarily mask this off */

        return cip_standard_tag_read_start((plc_tag_p)tag);
    }

    rc = build_write_request(tag, tag->offset);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Unable to build write request!");
        tag->write_in_progress = 0;

        return rc;
    }

    pdebug(dbg, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * Build the CIP read request.
 *
 * The request is the CIP message alone; the connection adds the EIP encapsulation and the CPF
 * framing, and that framing was the only thing that ever separated the connected form of this
 * function from the unconnected one.
 */
int build_read_request(cip_tag_p tag, int byte_offset) {
    cip_request_p req = NULL;
    uint8_t *data = NULL;
    int rc = PLCTAG_STATUS_OK;
    debug_module_t dbg = tag_debug_module(tag);

    pdebug(dbg, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(dbg, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  Error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    data = req->data;

    /*
     * The CIP Read Tag request:
     *
     *   uint8_t    service code
     *   uint8_t[]  the tag name, as an encoded path
     *   uint16_t   number of elements to read
     *   uint32_t   byte offset into the tag's data, on the fragmented service only
     *
     * The fragmented service is used whatever the offset, because the plain Read Tag
     * service carries no offset and so cannot continue a transfer the PLC split across
     * replies.  A family without the fragmented service gets the plain one and never
     * splits a transfer.
     */
    if(tag_supports_fragments(tag)) {
        *data = CIP_SVC_READ_FRAG;
    } else {
        *data = CIP_SVC_READ;
    }
    data++;

    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    *((uint16_le *)data) = h2le16(read_request_elem_count(tag));
    data += sizeof(uint16_le);

    if(tag_supports_fragments(tag)) {
        *((uint32_le *)data) = h2le32((uint32_t)byte_offset);
        data += sizeof(uint32_le);
    }

    /*
     * What this read will cost in the shared reply packet, so process_requests() can budget it
     * when bundling.  tag->size is what the previous read returned, which is the best estimate
     * of what this one will.  A first read has no previous size, so it cannot be budgeted and
     * must not be packed.
     */
    req->response_size = tag->size;
    req->first_read = tag->first_read;

    /* hand the finished request to the connection. */
    rc = cip_submit_payload(tag->session, req, (int)(data - req->data), tag->allow_packing);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Unable to submit the request, %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(dbg, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}


/*
 * Build the CIP Read-Modify-Write request that sets or clears a single bit.
 *
 * RMW takes a pair of masks the size of one element: the OR mask turns bits on and the AND
 * mask leaves them alone, so a bit is set by putting it in the OR mask and cleared by leaving
 * it out of the AND mask.
 */
int build_write_bit_request(cip_tag_p tag) {
    cip_request_p req = NULL;
    uint8_t *data = NULL;
    int rc = PLCTAG_STATUS_OK;
    debug_module_t dbg = tag_debug_module(tag);

    pdebug(dbg, DEBUG_INFO, tag->tag_id, "Starting.");

    rc = calculate_write_data_per_packet(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(dbg, DEBUG_ERROR, tag->tag_id, "Unable to calculate valid write data per packet!.  rc=%s",
               plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < (tag->size * 2) + 2) { /* 2 masks plus a count word. */
        pdebug(dbg, DEBUG_ERROR, tag->tag_id, "Insufficient space to write bit masks!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(dbg, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  Error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    data = req->data;

    /*
     * The CIP Read-Modify-Write request:
     *
     *   uint8_t    service code
     *   uint8_t[]  the tag name, as an encoded path
     *   uint16_t   size of one mask in bytes
     *   uint8_t[]  OR mask
     *   uint8_t[]  AND mask
     */
    *data = CIP_SVC_RMW;
    data++;

    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    *data = (uint8_t)(tag->elem_size & 0xFF);
    data++;
    *data = (uint8_t)((tag->elem_size >> 8) & 0xFF);
    data++;

    /* the OR mask carries the bit only when it is being set. */
    for(int i = 0; i < tag->elem_size; i++) {
        if((tag->bit / 8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            *data = (tag->data[tag->bit / 8] & mask) ? mask : (uint8_t)0;
        } else {
            *data = (uint8_t)0;
        }

        pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "adding OR mask byte %d: %x", i, *data);

        data++;
    }

    /* the AND mask clears the bit only when it is being cleared; every other bit stays. */
    for(int i = 0; i < tag->elem_size; i++) {
        if((tag->bit / 8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            *data = (tag->data[tag->bit / 8] & mask) ? (uint8_t)0xFF : (uint8_t)(~mask);
        } else {
            *data = (uint8_t)0xFF;
        }

        pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "adding AND mask byte %d: %x", i, *data);

        data++;
    }

    /* let the rest of the system know that the write is complete after this. */
    tag->offset = tag->size;

    /* hand the finished request to the connection. */
    rc = cip_submit_payload(tag->session, req, (int)(data - req->data), tag->allow_packing);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Unable to submit the request, %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(dbg, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}


/*
 * Build the CIP write request, or the next fragment of one.
 */
int build_write_request(cip_tag_p tag, int byte_offset) {
    cip_request_p req = NULL;
    uint8_t *data = NULL;
    int rc = PLCTAG_STATUS_OK;
    int multiple_requests = 0;
    int write_size = 0;
    int str_pad_to_multiple_bytes = 1;
    debug_module_t dbg = tag_debug_module(tag);

    pdebug(dbg, DEBUG_INFO, tag->tag_id, "Starting.");

    if(tag->is_bit) { return build_write_bit_request(tag); }

    rc = calculate_write_data_per_packet(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(dbg, DEBUG_ERROR, tag->tag_id, "Unable to calculate valid write data per packet!.  rc=%s",
               plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < tag->size) { multiple_requests = 1; }

    /* a family with no fragmented write cannot split the transfer, so it has to refuse it. */
    if(multiple_requests && !tag_supports_fragments(tag)) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Tag too large for an unfragmented write on this PLC!");
        return PLCTAG_ERR_TOO_LARGE;
    }

    /*
     * A write has to carry the tag's encoded type, so reject a tag that has none before
     * anything is allocated.  The type comes from the first read of the tag.
     */
    if(!tag->encoded_type_info_size) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Data type unsupported!");
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(dbg, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  Error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    data = req->data;

    /*
     * The CIP Write Tag request:
     *
     *   uint8_t    service code
     *   uint8_t[]  the tag name, as an encoded path
     *   uint8_t[]  the encoded data type
     *   uint16_t   number of elements to write
     *   uint32_t   byte offset, on the fragmented service only
     *   uint8_t[]  the data
     *
     * The plain service is used when the whole tag fits in one request.  Fragmented writes of
     * a single boolean have been seen not to work, and a single boolean always fits, so this
     * is not merely an optimisation.
     */
    *data = (multiple_requests) ? CIP_SVC_WRITE_FRAG : CIP_SVC_WRITE;
    data++;

    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    mem_copy(data, tag->encoded_type_info, tag->encoded_type_info_size);
    data += tag->encoded_type_info_size;

    *((uint16_le *)data) = h2le16((uint16_t)(tag->elem_count));
    data += sizeof(uint16_le);

    if(multiple_requests) {
        *((uint32_le *)data) = h2le32((uint32_t)byte_offset);
        data += sizeof(uint32_le);
    }

    /* how much data to write? */
    write_size = tag->size - tag->offset;

    if(write_size > tag->write_data_per_packet) { write_size = tag->write_data_per_packet; }

    mem_copy(data, tag->data + tag->offset, write_size);
    data += write_size;
    tag->offset += write_size;

    /*
     * Pad the data out to a multiple of 1, 2 or 4 bytes.  On some PLCs (OmronNJ) padding a
     * counted string breaks it, because the padding makes the data longer than the count says,
     * so str_pad_to_multiple_bytes can turn this off.
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

    /* hand the finished request to the connection. */
    rc = cip_submit_payload(tag->session, req, (int)(data - req->data), tag->allow_packing);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Unable to submit the request, %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(dbg, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}


static int check_read_status(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    cip_header *cip_resp;
    uint8_t *data;
    uint8_t *data_end;
    int partial_data = 0;
    debug_module_t dbg = tag_debug_module(tag);

    pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* The request is valid. */

    /* point to the data */
    cip_resp = (cip_header *)(tag->req->data);

    /* point to the start of the data */
    data = (tag->req->data) + sizeof(cip_header);

    /* point the end of the data */
    data_end = tag->req->data + tag->req->request_size;

    do {
        ptrdiff_t payload_size = 0;

        /* check the status */
        if(cip_resp->reply_service != (CIP_SVC_READ_FRAG | CIP_SVC_REPLY)
           && cip_resp->reply_service != (CIP_SVC_READ | CIP_SVC_REPLY)) {
            pdebug(dbg, DEBUG_WARN, tag->tag_id, "CIP response reply service unexpected: %d", cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        /*
         * A partial status is only a status on a family that has the fragmented services.
         * Everywhere else it is an error like any other -- the PLC is saying the transfer
         * does not fit, and there is no second request that could finish it.
         */
        if(cip_resp->status != CIP_STATUS_OK
           && !(cip_resp->status == CIP_STATUS_FRAG && tag_supports_fragments(tag))) {
            size_t status_size = cip_error_data_size((uint8_t *)&cip_resp->status, data_end);

            pdebug(dbg, DEBUG_WARN, tag->tag_id, "CIP read failed with status: 0x%x %s", cip_resp->status,
                   decode_cip_error_short((uint8_t *)&cip_resp->status, status_size));
            pdebug(dbg, DEBUG_INFO, tag->tag_id, decode_cip_error_long((uint8_t *)&cip_resp->status, status_size));

            rc = decode_cip_error_code((uint8_t *)&cip_resp->status, status_size);

            break;
        }

        /* check to see if this is a partial response. */
        partial_data = (cip_resp->status == CIP_STATUS_FRAG && tag_supports_fragments(tag));

        /*
         * check to see if there is any data to process.  If this is a packed
         * response, there might not be.
         */
        payload_size = (data_end - data);
        if(payload_size > 0) {
            /* we got data, so the transfer is moving again. */
            tag->fragment_retry_count = 0;

            /* skip the copy if we already have type data */
            if(tag->encoded_type_info_size == 0) {
                int type_length = 0;

                /* the first byte of the response is a type byte. */
                pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "type byte = %d (0x%02x)", (int)*data, (int)*data);

                if(cip_lookup_encoded_type_size(*data, &type_length) == PLCTAG_STATUS_OK) {
                    /* found it and we got the type data size */

                    /* some types use the second byte to indicate how many bytes more are used. */
                    if(type_length == 0) {
                        if(payload_size < 2) {
                            pdebug(dbg, DEBUG_WARN, tag->tag_id, "Response too short to hold extended type length byte!");
                            rc = PLCTAG_ERR_TOO_SMALL;
                            break;
                        }

                        type_length = *(data + 1) + 2;
                    }

                    if(type_length <= 0 || type_length > (int)sizeof(tag->encoded_type_info)
                       || type_length > (int)payload_size) {
                        pdebug(dbg, DEBUG_WARN, tag->tag_id,
                               "Type data length %d for type byte 0x%02x is out of range (max %d, available %d)!", type_length,
                               *data, (int)sizeof(tag->encoded_type_info), (int)payload_size);
                        rc = PLCTAG_ERR_TOO_LARGE;
                        break;
                    }

                    pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Type data is %d bytes long.", type_length);
                    pdebug_dump_bytes(dbg, DEBUG_DETAIL, tag->tag_id, data, type_length);

                    tag->encoded_type_info_size = type_length;
                    mem_copy(tag->encoded_type_info, data, tag->encoded_type_info_size);
                } else {
                    pdebug(dbg, DEBUG_WARN, tag->tag_id, "Unsupported data type returned, type byte=0x%02x", *data);
                    rc = PLCTAG_ERR_UNSUPPORTED;
                    break;
                }
            }

            /* skip past the type data */
            data += (tag->encoded_type_info_size);

            if((intptr_t)data > (intptr_t)data_end) {
                pdebug(dbg, DEBUG_WARN, tag->tag_id, "Response too short to hold remembered type info of %d bytes!",
                       tag->encoded_type_info_size);
                rc = PLCTAG_ERR_TOO_SMALL;
                break;
            }

            /* check payload size now that we have bumped past the data type info. */
            payload_size = (data_end - data);

            /*
             * The pre-read asked for a single element, so the payload is that element and
             * its length is the element size.  The write that follows needs elem_size to
             * chunk itself, and deriving it the general way below -- reply length divided
             * by elem_count -- yields zero for any array whose first fragment is shorter
             * than elem_count bytes.
             *
             * That the payload is one element's worth only holds where elements are a
             * fixed stride.  It is true for the tags this function serves: they carry
             * logix_tag_byte_order, whose strings are a fixed 88 bytes.  It is not true
             * generally -- the tag listing, @udt and the Omron types all set
             * str_is_fixed_length to zero, and a listing entry's size varies with the
             * length of the tag name it carries.  Those types declare an element size of
             * one byte and build their own requests, so they never reach this code.
             *
             * Nothing here extrapolates a whole-tag size from the one element, because
             * that is the step that would be wrong if a variable-stride tag ever did
             * arrive: tag->size belongs to the caller, who set it staging the write.
             */
            if(tag->pre_write_read) {
                if(payload_size <= 0) {
                    pdebug(dbg, DEBUG_WARN, tag->tag_id, "Pre-write read returned no element data!");
                    rc = PLCTAG_ERR_TOO_SMALL;
                    break;
                }

                if(tag_elements_are_variable(tag)) {
                    pdebug(dbg, DEBUG_DETAIL, tag->tag_id,
                           "Pre-write read of %d bytes; elements are variable length, so the element size is unchanged.",
                           (int)payload_size);
                } else if(tag->elem_size <= 0) {
                    tag->elem_size = (int)payload_size;

                    pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Pre-write read: element size is %d bytes.", tag->elem_size);
                } else if(tag->elem_size != (int)payload_size) {
                    /* keep what the tag already believes; a mismatch means the stride is not uniform. */
                    pdebug(dbg, DEBUG_WARN, tag->tag_id,
                           "Pre-write read returned a %d byte element but the element size is %d bytes!", (int)payload_size,
                           tag->elem_size);
                }
            } else if(payload_size + tag->offset > tag->size) {
                /* a PLC can keep returning fragments forever.  Do not grow without bound. */
                if((payload_size + tag->offset) > (ptrdiff_t)CIP_MAX_TAG_DATA_SIZE) {
                    pdebug(dbg, DEBUG_WARN, tag->tag_id, "Tag data size of %d bytes is larger than the maximum of %d bytes!",
                           (int)(payload_size + tag->offset), CIP_MAX_TAG_DATA_SIZE);
                    rc = PLCTAG_ERR_TOO_LARGE;
                    break;
                }

                tag->size = (int)payload_size + tag->offset;

                /* only a fixed stride can be recovered by division; see tag_elements_are_variable(). */
                if(!tag_elements_are_variable(tag)) {
                    tag->elem_size = tag->size / tag->elem_count;
                } else if(tag->elem_size <= 0) {
                    /*
                     * There is no stride, so the element size is one byte -- the same answer the
                     * tag listing and @udt tags give.  It still has to be set to something
                     * usable: calculate_write_data_per_packet() divides by it.
                     */
                    tag->elem_size = 1;
                }

                pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Increasing tag buffer size to %d bytes.", tag->size);

                tag->data = (uint8_t *)mem_realloc(tag->data, tag->size);
                if(!tag->data) {
                    pdebug(dbg, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data memory!");
                    rc = PLCTAG_ERR_NO_MEM;
                    break;
                }
            }

            pdebug(dbg, DEBUG_INFO, tag->tag_id, "Got %d bytes of data", (int)payload_size);

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
            pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Response returned no data and no error.");

            /* no payload means no forward progress on a fragmented transfer. */
            tag->fragment_retry_count++;
        }

        /* set the return code */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request */
    cip_tag_abort_request_only(tag);

    /* are we actually done? */
    if(rc == PLCTAG_STATUS_OK) {
        /* skip if we are doing a pre-write read. */
        if(!tag->pre_write_read && partial_data && tag->fragment_retry_count > CIP_MAX_FRAGMENT_RETRIES) {
            pdebug(dbg, DEBUG_WARN, tag->tag_id,
                   "Got %d fragment responses in a row with no data.  The transfer is not making progress, giving up.",
                   tag->fragment_retry_count);
            rc = PLCTAG_ERR_PARTIAL;
        } else if(!tag->pre_write_read && partial_data) {
            /* call read start again to get the next piece */
            pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "calling cip_standard_tag_read_start() to get the next chunk.");
            rc = cip_standard_tag_read_start((plc_tag_p)tag);
        } else {
            tag->offset = 0;

            /* the transfer is over one way or the other, so start the next one clean. */
            tag->fragment_retry_count = 0;

            /* if this is a pre-read for a write, then pass off to the write routine */
            if(tag->pre_write_read) {
                pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Restarting write call now.");
                tag->pre_write_read = 0;
                rc = cip_standard_tag_write_start((plc_tag_p)tag);
            } else {
                pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Read complete.");
            }
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc != PLCTAG_STATUS_PENDING) {
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(dbg, DEBUG_WARN, tag->tag_id, "Error reading tag data! rc=%d %s", rc, plc_tag_decode_error(rc));
        }

        /* clean up everything if this tag is not pending. */
        cip_tag_abort_request(tag);
    }

    pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


static int check_write_status(cip_tag_p tag) {
    cip_header *cip_resp;
    int rc = PLCTAG_STATUS_OK;
    debug_module_t dbg = tag_debug_module(tag);

    pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* the request reference is valid. */

    /* point to the data */
    cip_resp = (cip_header *)(tag->req->data);

    do {
        if(cip_resp->reply_service != (CIP_SVC_WRITE_FRAG | CIP_SVC_REPLY)
           && cip_resp->reply_service != (CIP_SVC_WRITE | CIP_SVC_REPLY)
           && cip_resp->reply_service != (CIP_SVC_RMW | CIP_SVC_REPLY)) {
            pdebug(dbg, DEBUG_WARN, tag->tag_id, "CIP response reply service unexpected: %d", cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(cip_resp->status != CIP_STATUS_OK
           && !(cip_resp->status == CIP_STATUS_FRAG && tag_supports_fragments(tag))) {
            size_t status_size = cip_error_data_size((uint8_t *)&cip_resp->status, tag->req->data + tag->req->request_size);

            pdebug(dbg, DEBUG_WARN, tag->tag_id, "CIP write failed with status: 0x%x %s", cip_resp->status,
                   decode_cip_error_short((uint8_t *)&cip_resp->status, status_size));
            pdebug(dbg, DEBUG_INFO, tag->tag_id, decode_cip_error_long((uint8_t *)&cip_resp->status, status_size));
            rc = decode_cip_error_code((uint8_t *)&cip_resp->status, status_size);
            break;
        }
    } while(0);

    cip_tag_abort_request_only(tag);

    if(rc == PLCTAG_STATUS_OK) {
        if(tag->offset < tag->size) {
            pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Write not complete, triggering next round.");
            rc = cip_standard_tag_write_start((plc_tag_p)tag);
        } else {
            /* only clear this if we are done. */
            tag->offset = 0;
        }
    } else {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Write failed!");

        tag->offset = 0;
    }

    pdebug(dbg, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


int calculate_write_data_per_packet(cip_tag_p tag) {
    int overhead = 0;
    int data_per_packet = 0;
    int available_payload = 0;
    debug_module_t dbg = tag_debug_module(tag);

    pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Starting.");

    /* if we are here, then we have all the type data etc. */
    available_payload = session_get_available_cip_payload_space(tag->session);

    /* the CIP Write Tag request itself, which every family sends the same way. */
    overhead = 1                             /* service request, one byte */
               + tag->encoded_name_size      /* full encoded name */
               + tag->encoded_type_info_size /* encoded type size */
               + 2                           /* element count, 16-bit int */
               + 4                           /* byte offset, 32-bit int */
               + 8;                          /* MAGIC fudge factor */

    /*
     * An unconnected request is wrapped in an Unconnected Send by the connection, and that
     * wrapper has to come out of the same budget.  A connected request has no wrapper.
     *
     * This no longer asks which family the tag belongs to.  OMRON used to add its route
     * path here, which was wrong twice over: session_get_available_cip_payload_space() has
     * already subtracted the route path for an unconnected session, and this library only
     * ever opens connected sessions to an OMRON PLC, so there was no route path in the
     * packet at all.  It only made OMRON write packets smaller than they needed to be,
     * which is why it never surfaced as a failure.
     *
     * OMRON hardware does support unconnected messaging, identically to Rockwell; this
     * library has simply never implemented it.  When it does, this calculation already
     * covers it, because the question asked here is whether the request is connected and
     * not who built the PLC.
     */
    if(!tag->use_connected_msg) {
        pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Unconnected tag.");

        overhead += CIP_EIP_UC_SEND_OVERHEAD;
    } else {
        pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Connected tag.");
    }

    /* make sure that overhead is an even number of bytes */
    if(overhead & 1) { overhead++; }

    pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Write overhead is %d bytes.", overhead);

    data_per_packet = available_payload - overhead;

    pdebug(dbg, DEBUG_DETAIL, tag->tag_id,
           "Write packet available payload is %d, write overhead is %d, and write data per packet is %d.", available_payload,
           overhead, data_per_packet);

    if(data_per_packet <= 0) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id,
               "Unable to send request.  Packet overhead, %d bytes, is too large for available payload, %d bytes!", overhead,
               available_payload);
        return PLCTAG_ERR_TOO_LARGE;
    }

    int elements_per_packet = 0;
    int element_size = 0;

    /*
     * elem_size is derived from the PLC's response as tag->size / tag->elem_count, so a PLC
     * that returns fewer bytes than the tag has elements drives it to zero.  It is the
     * divisor below, so check it here.
     */
    if(tag->elem_size <= 0) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id, "Tag element size of %d bytes is not usable!", tag->elem_size);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * Round the chunk down to something we may legally split on.  An element may be split
     * internally, but never inside one of its atomic members, and the largest CIP atomic
     * is 8 bytes (LINT, LREAL) -- so anything bigger than that splits on 8, and anything
     * smaller splits on itself.
     */
    element_size = (tag->elem_size < CIP_MAX_ATOMIC_SIZE) ? tag->elem_size : CIP_MAX_ATOMIC_SIZE;

    elements_per_packet = data_per_packet / element_size;
    data_per_packet = elements_per_packet * element_size;

    pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Splitting on %d byte boundaries.", element_size);

    if(elements_per_packet < 1) {
        pdebug(dbg, DEBUG_WARN, tag->tag_id,
               "Unable to send request.  Available payload, %d bytes, is too small to write at least %d bytes!",
               available_payload, element_size);
        return PLCTAG_ERR_TOO_LARGE;
    }

    pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Write data per packet is %d bytes.", data_per_packet);

    tag->write_data_per_packet = data_per_packet;

    pdebug(dbg, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}
