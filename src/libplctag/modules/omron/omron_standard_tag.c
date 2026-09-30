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
#include <libplctag/modules/omron/cip.h>
#include <libplctag/modules/omron/conn.h>
#include <libplctag/modules/omron/defs.h>
#include <libplctag/modules/omron/omron_common.h>
#include <libplctag/modules/omron/omron_standard_tag.h>
#include <libplctag/modules/omron/tag.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/vector.h>


static int build_read_request(omron_tag_p tag, int byte_offset);
static int build_write_request(omron_tag_p tag, int byte_offset);
static int build_write_bit_request(omron_tag_p tag);
static int check_read_status(omron_tag_p tag);
static int check_write_status(omron_tag_p tag);
static int calculate_write_data_per_packet(omron_tag_p tag);

static int tag_read_start(omron_tag_p tag);
static int tag_tickler(omron_tag_p tag);
static int tag_write_start(omron_tag_p tag);

/* define the exported vtable for this tag type. */
struct tag_vtable_t omron_standard_tag_vtable = {
    .abort = (tag_vtable_func)omron_tag_abort,
    .read = (tag_vtable_func)tag_read_start,
    .status = (tag_vtable_func)omron_tag_status,
    .tickler = (tag_vtable_func)tag_tickler,
    .write = (tag_vtable_func)tag_write_start,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .attribs = omron_attribs,
};


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


/*************************************************************************
 **************************** API Functions ******************************
 ************************************************************************/


int tag_tickler(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = omron_check_request_status(tag);
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

        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Done.  Read in progress.");

        return rc;
    }

    if(tag->write_in_progress) {
        rc = check_write_status(tag);

        tag->status = (int8_t)rc;

        /* if the operation completed, make a note so that the callback will be called. */
        if(!tag->write_in_progress) { tag->write_complete = 1; }

        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Done. Write in progress.");

        return rc;
    }

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Done.  No operation in progress.");

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

/*
 * True when the tag's elements are packed at their own lengths rather than laid out on a
 * fixed stride.
 *
 * An Omron string is a two-byte count, that many characters and a zero terminator, with no
 * padding out to a capacity, so an array of them has no element size: reaching element N
 * means walking the count words of the N before it.  That is what the library's string layer
 * already does -- omron_njnx_tag_byte_order sets str_is_fixed_length to zero, and
 * get_string_total_length_unsafe() walks rather than strides.  Dividing a reply by the
 * element count would invent a stride that is not there.
 */
static bool tag_elements_are_variable(omron_tag_p tag) {
    return tag->elem_type == CIP_TYPE_STRING || tag->elem_type == CIP_TYPE_SHORT_STRING;
}


/*
 * How many elements a read request should ask for.
 *
 * A pre-read before a write exists only to learn the tag's type and element size; its data is
 * discarded and it does not chase the remaining fragments.  Ask for a single element so that
 * the reply is one element rather than however much fits in a packet.
 */
static uint16_t read_request_elem_count(omron_tag_p tag) {
    return (uint16_t)(tag->pre_write_read ? 1 : tag->elem_count);
}


int tag_read_start(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* mark the tag read in progress */
    tag->read_in_progress = 1;

    /* i is the index of the first new request */
    rc = build_read_request(tag, tag->offset);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Unable to build read request!");

        tag->read_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Done.");

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

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
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
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id,
               "No read has completed yet, doing pre-read to get type information.");

        tag->pre_write_read = 1;
        tag->write_in_progress = 0; /* temporarily mask this off */

        return tag_read_start(tag);
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Unable to calculate write sizes!");
        tag->write_in_progress = 0;

        return rc;
    }

    rc = build_write_request(tag, tag->offset);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Unable to build write request!");
        tag->write_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * Build the CIP read request.
 *
 * The request is the CIP message alone; the connection adds the EIP encapsulation and the
 * CPF framing, and that framing was the only thing that ever separated the connected form of
 * this function from the unconnected one.
 *
 * byte_offset is unused: an NJ/NX does not support the fragmented read service, so a read
 * always asks for the whole tag from the start and the PLC either answers it or does not.
 */
int build_read_request(omron_tag_p tag, int byte_offset) {
    omron_request_p req = NULL;
    uint8_t *data = NULL;
    int rc = PLCTAG_STATUS_OK;

    (void)byte_offset;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  Error %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    data = req->data;

    /*
     * The CIP Read Tag request:
     *
     *   uint8_t    service code
     *   uint8_t[]  the tag name, as an encoded path
     *   uint16_t   number of elements to read
     *
     * Omron fragmentation would need a data segment here; see the module notes.
     */
    *data = OMRON_EIP_CMD_CIP_READ;
    data++;

    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    *((uint16_le *)data) = h2le16(read_request_elem_count(tag));
    data += sizeof(uint16_le);

    /*
     * What this read will cost in the shared reply packet, so process_requests() can budget
     * it when bundling.  tag->size is what the previous read returned, which is the best
     * estimate of what this one will.  A first read has no previous size, so it cannot be
     * budgeted and must not be packed.
     */
    req->response_size = tag->size;
    req->first_read = tag->first_read;

    /* hand the finished request to the connection. */
    rc = cip_submit_payload(tag->session, req, (int)(data - req->data), tag->allow_packing);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Unable to submit the request, %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}


/*
 * Build the CIP Read-Modify-Write request that sets or clears a single bit.
 *
 * RMW takes a pair of masks the size of one element: the OR mask turns bits on and the AND
 * mask leaves them alone, so a bit is set by putting it in the OR mask and cleared by leaving
 * it out of the AND mask.
 */
int build_write_bit_request(omron_tag_p tag) {
    omron_request_p req = NULL;
    uint8_t *data = NULL;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Starting.");

    rc = calculate_write_data_per_packet(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_ERROR, tag->tag_id,
               "Unable to calculate valid write data per packet!.  rc=%s", plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < (tag->size * 2) + 2) { /* 2 masks plus a count word. */
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_ERROR, tag->tag_id, "Insufficient space to write bit masks!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  Error %s!",
               plc_tag_decode_error(rc));
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
    *data = OMRON_EIP_CMD_CIP_RMW;
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

        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "adding OR mask byte %d: %x", i, *data);

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

        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "adding AND mask byte %d: %x", i, *data);

        data++;
    }

    /* let the rest of the system know that the write is complete after this. */
    tag->offset = tag->size;

    /* hand the finished request to the connection. */
    rc = cip_submit_payload(tag->session, req, (int)(data - req->data), tag->allow_packing);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Unable to submit the request, %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}


/*
 * Build the CIP write request.
 */
int build_write_request(omron_tag_p tag, int byte_offset) {
    omron_request_p req = NULL;
    uint8_t *data = NULL;
    int rc = PLCTAG_STATUS_OK;
    int multiple_requests = 0;
    int write_size = 0;
    int str_pad_to_multiple_bytes = 1;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Starting.");

    if(tag->is_bit) { return build_write_bit_request(tag); }

    rc = calculate_write_data_per_packet(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_ERROR, tag->tag_id,
               "Unable to calculate valid write data per packet!.  rc=%s", plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < tag->size) { multiple_requests = 1; }

    if(multiple_requests && tag->plc_type == OMRON_PLC_OMRON_NJNX) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Tag too large for unfragmented request on Omron PLC!");
        return PLCTAG_ERR_TOO_LARGE;
    }

    /*
     * A write has to carry the tag's encoded type, so reject a tag that has none before
     * anything is allocated.  The type comes from the first read of the tag.
     */
    if(!tag->encoded_type_info_size) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Data type unsupported!");
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  Error %s!",
               plc_tag_decode_error(rc));
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
     *   uint32_t   byte offset, when the write is split across requests
     *   uint8_t[]  the data
     *
     * An NJ/NX is rejected above when the write does not fit one request, so the offset is
     * only ever reached by the other families.  Fragmenting it properly needs an Omron data
     * segment rather than a bare offset.
     */
    *data = OMRON_EIP_CMD_CIP_WRITE;
    data++;

    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    mem_copy(data, tag->encoded_type_info, tag->encoded_type_info_size);
    data += tag->encoded_type_info_size;

    *((uint16_le *)data) = h2le16((uint16_t)(tag->elem_count));
    data += sizeof(uint16_le);

    if(multiple_requests) {
        /* FIXME - this wants an Omron 0x80 data segment, not a plain byte offset. */
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
     * Pad the data out to a multiple of 1, 2 or 4 bytes.  On an NJ/NX padding a counted
     * string breaks it, because the padding makes the data longer than the count says, so
     * str_pad_to_multiple_bytes can turn this off.
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
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Unable to submit the request, %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}


static int check_read_status(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    cip_header *cip_resp;
    uint8_t *data;
    uint8_t *data_end;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* the request reference is valid. */

    /* point to the data */
    cip_resp = (cip_header *)(tag->req->data);

    /* point to the start of the data */
    data = (tag->req->data) + sizeof(cip_header);

    /* point the end of the data */
    data_end = tag->req->data + tag->req->request_size;

    /* check the status */
    do {
        ptrdiff_t payload_size = 0;

        if(cip_resp->reply_service != (OMRON_EIP_CMD_CIP_READ | OMRON_EIP_CMD_CIP_OK)) {
            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "CIP response reply service unexpected: %d",
                   cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(cip_resp->status != OMRON_CIP_STATUS_OK) {
            size_t status_size = cip_error_data_size((uint8_t *)&cip_resp->status, data_end);

            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "CIP read failed with status: 0x%x %s",
                   cip_resp->status, CIP.decode_cip_error_short((uint8_t *)&cip_resp->status, status_size));
            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id,
                   CIP.decode_cip_error_long((uint8_t *)&cip_resp->status, status_size));

            rc = CIP.decode_cip_error_code((uint8_t *)&cip_resp->status, status_size);

            break;
        }

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
                pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "type byte = %d (0x%02x)", (int)*data,
                       (int)*data);

                if(CIP.lookup_encoded_type_size(*data, &type_length) == PLCTAG_STATUS_OK) {
                    /* found it and we got the type data size */

                    /* some types use the second byte to indicate how many bytes more are used. */
                    if(type_length == 0) {
                        if(payload_size < 2) {
                            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id,
                                   "Response too short to hold extended type length byte!");
                            rc = PLCTAG_ERR_TOO_SMALL;
                            break;
                        }

                        type_length = *(data + 1) + 2;
                    }

                    if(type_length <= 0 || type_length > (int)sizeof(tag->encoded_type_info) || type_length > (int)payload_size) {
                        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id,
                               "Type data length %d for type byte 0x%02x is out of range (max %d, available %d)!", type_length,
                               *data, (int)sizeof(tag->encoded_type_info), (int)payload_size);
                        rc = PLCTAG_ERR_TOO_LARGE;
                        break;
                    }

                    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Type data is %d bytes long.",
                           type_length);
                    pdebug_dump_bytes(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, data, type_length);

                    tag->encoded_type_info_size = type_length;
                    mem_copy(tag->encoded_type_info, data, tag->encoded_type_info_size);
                } else {
                    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id,
                           "Unsupported data type returned, type byte=0x%02x", *data);
                    rc = PLCTAG_ERR_UNSUPPORTED;
                    break;
                }
            }

            /* skip past the type data */
            data += (tag->encoded_type_info_size);

            if((intptr_t)data > (intptr_t)data_end) {
                pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id,
                       "Response too short to hold remembered type info of %d bytes!", tag->encoded_type_info_size);
                rc = PLCTAG_ERR_TOO_SMALL;
                break;
            }

            /* check payload size now that we have bumped past the data type info. */
            payload_size = (data_end - data);

            /*
             * The pre-read asked for a single element, so the payload is that element and its
             * length is the element size.  Nothing here extrapolates a whole-tag size from it:
             * tag->size belongs to the caller, who set it staging the write, and for a string
             * array there is no stride to extrapolate along in the first place.
             */
            if(tag->pre_write_read) {
                if(payload_size <= 0) {
                    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id,
                           "Pre-write read returned no element data!");
                    rc = PLCTAG_ERR_TOO_SMALL;
                    break;
                }

                if(tag_elements_are_variable(tag)) {
                    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id,
                           "Pre-write read of %d bytes; elements are variable length, so the element size is unchanged.",
                           (int)payload_size);
                } else if(tag->elem_size <= 0) {
                    tag->elem_size = (int)payload_size;

                    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id,
                           "Pre-write read: element size is %d bytes.", tag->elem_size);
                } else if(tag->elem_size != (int)payload_size) {
                    /* keep what the tag already believes; a mismatch means the stride is not uniform. */
                    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id,
                           "Pre-write read returned a %d byte element but the element size is %d bytes!",
                           (int)payload_size, tag->elem_size);
                }
            } else if(payload_size + tag->offset > tag->size) {
                /* the buffer size comes off the wire, so bound it whatever the PLC claims. */
                if((payload_size + tag->offset) > (ptrdiff_t)OMRON_MAX_TAG_DATA_SIZE) {
                    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id,
                           "Tag data size of %d bytes is larger than the maximum of %d bytes!", (int)(payload_size + tag->offset),
                           OMRON_MAX_TAG_DATA_SIZE);
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

                pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Increasing tag buffer size to %d bytes.",
                       tag->size);

                tag->data = (uint8_t *)mem_realloc(tag->data, tag->size);
                if(!tag->data) {
                    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Unable to reallocate tag data memory!");
                    rc = PLCTAG_ERR_NO_MEM;
                    break;
                }
            }

            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id, "Got %d bytes of data", (int)payload_size);

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
            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Response returned no data and no error.");
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

        /*
         * Every response that reaches here carried a status of zero, which on Omron means the
         * whole transfer.  There is no continuation to run: Omron reports a transfer it could
         * not finish as an error rather than with CIP's 0x06 partial status, and it implements
         * no fragmented read service to ask for the rest with in any case.
         */
        tag->offset = 0;

        /* if this is a pre-read for a write, then pass off to the write routine */
        if(tag->pre_write_read) {
            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Restarting write call now.");
            tag->pre_write_read = 0;
            rc = tag_write_start(tag);
        }
    }

    /* this is not an else clause because the above if could result in bad rc. */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        /* error ! */
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Error received!");

        /* clean up everything. */
        omron_tag_abort(tag);
    }

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


static int check_write_status(omron_tag_p tag) {
    cip_header *cip_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Starting.");

    if(!tag) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_ERROR, tag->tag_id, "Null tag pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* the request reference is valid. */

    /* point to the data */
    cip_resp = (cip_header *)(tag->req->data);

    do {
        if(cip_resp->reply_service != (OMRON_EIP_CMD_CIP_WRITE | OMRON_EIP_CMD_CIP_OK)
           && cip_resp->reply_service != (OMRON_EIP_CMD_CIP_RMW | OMRON_EIP_CMD_CIP_OK)) {
            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "CIP response reply service unexpected: %d",
                   cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(cip_resp->status != OMRON_CIP_STATUS_OK) {
            size_t status_size = cip_error_data_size((uint8_t *)&cip_resp->status, tag->req->data + tag->req->request_size);

            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "CIP read failed with status: 0x%x %s",
                   cip_resp->status, CIP.decode_cip_error_short((uint8_t *)&cip_resp->status, status_size));
            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_INFO, tag->tag_id,
                   CIP.decode_cip_error_long((uint8_t *)&cip_resp->status, status_size));
            rc = CIP.decode_cip_error_code((uint8_t *)&cip_resp->status, status_size);
            break;
        }
    } while(0);

    /* clean up the request. */
    omron_tag_abort_request_only(tag);

    /* write is done in one way or another. */
    tag->write_in_progress = 0;

    if(rc == PLCTAG_STATUS_OK) {
        if(tag->offset < tag->size) {

            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Write not complete, triggering next round.");
            /* FIXME - the abort function above resets the offset */
            rc = tag_write_start(tag);
        } else {
            /* only clear this if we are done. */
            tag->offset = 0;
        }
    } else {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Write failed!");

        tag->offset = 0;
    }

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


int calculate_write_data_per_packet(omron_tag_p tag) {
    int overhead = 0;
    int data_per_packet = 0;
    int available_payload = 0;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Starting.");

    /* if we are here, then we have all the type data etc. */
    available_payload = session_get_available_cip_payload_space(tag->session);

    if(tag->use_connected_msg) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Connected tag.");
        overhead = 1                             /* service request, one byte */
                   + tag->encoded_name_size      /* full encoded name */
                   + tag->encoded_type_info_size /* encoded type size */
                   + 2                           /* element count, 16-bit int */
                   + 4                           /* byte offset, 32-bit int */
                   + 8;                          /* MAGIC fudge factor */
    } else {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Unconnected tag.");
        overhead = 1                                  /* service request, one byte */
                   + tag->encoded_name_size           /* full encoded name */
                   + tag->encoded_type_info_size      /* encoded type size */
                   + tag->session->conn_path_size + 2 /* encoded device path size plus two bytes for length and padding */
                   + 2                                /* element count, 16-bit int */
                   + 4                                /* byte offset, 32-bit int */
                   + 8;                               /* MAGIC fudge factor */
    }

    /* make sure that overhead is an even number of bytes */
    if(overhead & 1) { overhead++; }

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Write overhead is %d bytes.", overhead);

    data_per_packet = available_payload - overhead;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id,
           "Write packet available payload is %d, write overhead is %d, and write data per packet is %d.", available_payload,
           overhead, data_per_packet);

    if(data_per_packet <= 0) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id,
               "Unable to send request.  Packet overhead, %d bytes, is too large for available payload, %d bytes!", overhead,
               available_payload);
        return PLCTAG_ERR_TOO_LARGE;
    }

    int element_size = 0;
    int elements_per_packet = 0;

    /*
     * elem_size is derived from the PLC's response as tag->size / tag->elem_count, so a PLC
     * that returns fewer bytes than the tag has elements drives it to zero.  It is the
     * divisor below, so check it here.
     */
    if(tag->elem_size <= 0) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Tag element size of %d bytes is not usable!",
               tag->elem_size);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* if the tag size is less than 8 bytes, then use a multiple of the tag size.  Otherwise use
     8 bytes as the unit */
    if(tag->elem_size < 8) {
        elements_per_packet = data_per_packet / tag->elem_size;
        data_per_packet = elements_per_packet * tag->elem_size;
        element_size = tag->elem_size;
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Using tag size %d bytes for element size.",
               element_size);
    } else {
        /* round down to the nearest multiple of 8 bytes */
        elements_per_packet = data_per_packet / 8;
        data_per_packet = elements_per_packet * 8;
        element_size = 8;
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Using element size %d bytes.", element_size);
    }

    if(elements_per_packet < 1) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id,
               "Unable to send request.  Available payload, %d bytes, is too small to write at least %d bytes!",
               available_payload, element_size);
        return PLCTAG_ERR_TOO_LARGE;
    }

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Write data per packet is %d bytes.", data_per_packet);

    tag->write_data_per_packet = data_per_packet;

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}
