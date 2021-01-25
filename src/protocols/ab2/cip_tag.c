/***************************************************************************
 *   Copyright (C) 2021 by Kyle Hayes                                      *
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

#include <stddef.h>
#include <lib/libplctag.h>
#include <lib/tag.h>
#include <ab2/df1.h>
#include <ab2/ab.h>
#include <ab2/cip_layer.h>
#include <ab2/cip_tag.h>
#include <ab2/cip_plc.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/plc.h>
#include <util/string.h>



#define CIP_CMD_MULTI            ((uint8_t)0x0A)
#define CIP_CMD_READ             ((uint8_t)0x4C)
#define CIP_CMD_WRITE            ((uint8_t)0x4D)
#define CIP_CMD_RMW              ((uint8_t)0x4E)
#define CIP_CMD_READ_FRAG        ((uint8_t)0x52)
#define CIP_CMD_WRITE_FRAG       ((uint8_t)0x53)
#define CIP_CMD_LIST_TAGS        ((uint8_t)0x55)

/* flag set when command is OK */
#define CIP_CMD_OK               ((uint8_t)0x80)

#define CIP_STATUS_OK                ((uint8_t)0x00)
#define CIP_STATUS_FRAG              ((uint8_t)0x06)

#define CIP_ERR_UNSUPPORTED_SERVICE  ((uint8_t)0x08)
#define CIP_ERR_PARTIAL_ERROR  ((uint8_t)0x1e)


/* vtable functions */
static int cip_tag_abort(plc_tag_p tag);
static int cip_tag_read(plc_tag_p tag);
static int cip_tag_status(plc_tag_p tag);
static int cip_tag_write(plc_tag_p tag);


/* vtable for PLC-5 tags */
struct tag_vtable_t cip_tag_vtable = {
    cip_tag_abort,
    cip_tag_read,
    cip_tag_status,
    /* plc5_tag_tickler */ NULL,
    cip_tag_write,

    /* attribute accessors */
    cip_get_int_attrib,
    cip_set_int_attrib
};


tag_byte_order_t cip_tag_byte_order = {
    .is_allocated = 0,

    .int16_order = {0,1},
    .int32_order = {0,1,2,3},
    .int64_order = {0,1,2,3,4,5,6,7},
    .float32_order = {0,1,2,3},
    .float64_order = {0,1,2,3,4,5,6,7},

    .str_is_defined = 1,
    .str_is_counted = 1,
    .str_is_fixed_length = 1,
    .str_is_zero_terminated = 0,
    .str_is_byte_swapped = 0,

    .str_count_word_bytes = 4,
    .str_max_capacity = 82,
    .str_total_length = 88,
    .str_pad_bytes = 2
};




static int cip_build_read_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);
static int cip_handle_read_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);
static int cip_build_write_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);
static int cip_handle_write_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);

static int raw_cip_build_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);
static int raw_cip_handle_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);




int cip_tag_abort(plc_tag_p tag_arg)
{
    cip_tag_p tag = (cip_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    plc_stop_request(tag->plc, &(tag->request));

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int cip_tag_read(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = (cip_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(tag->is_raw_tag == FALSE) {
        rc = plc_start_request(tag->plc, &(tag->request), tag, cip_build_read_request_callback, cip_handle_read_response_callback);
    } else {
        rc = plc_start_request(tag->plc, &(tag->request), tag, raw_cip_build_request_callback, raw_cip_handle_response_callback);
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to start read request!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int cip_tag_status(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = (cip_tag_p)tag_arg;

    pdebug(DEBUG_SPEW, "Starting.");

    rc = tag->base_tag.status;

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}



int cip_tag_write(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = (cip_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(tag->is_raw_tag == FALSE) {
        rc = plc_start_request(tag->plc, &(tag->request), tag, cip_build_write_request_callback, cip_handle_write_response_callback);
    } else {
        rc = plc_start_request(tag->plc, &(tag->request), tag, raw_cip_build_request_callback, raw_cip_handle_response_callback);
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to start write request!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int cip_build_read_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = (cip_tag_p)context;
    int req_off = *payload_start;
    // int max_trans_size = 0;
    // int num_elems = 0;
    // int trans_size = 0;

    (void)buffer_capacity;
    (void)req_id;

    pdebug(DEBUG_DETAIL, "Starting.");


    do {
        /* if this is the first read, then do not use the fragmented read command. */
        if(tag->trans_offset == 0) {
            TRY_SET_BYTE(buffer, *payload_end, req_off, CIP_CMD_READ);
        } else {
            TRY_SET_BYTE(buffer, *payload_end, req_off, CIP_CMD_READ_FRAG);
        }

        /* copy the encoded tag name. */
        for(int index=0; index < tag->encoded_name_length; index++) {
            TRY_SET_BYTE(buffer, *payload_end, req_off, tag->encoded_name[index]);
        }

        /* add the element count */
        TRY_SET_U16_LE(buffer, *payload_end, req_off, tag->elem_count);

        /* add the offset if we are doing a fragmented read. */
        if(tag->trans_offset != 0) {
            TRY_SET_U32_LE(buffer, *payload_end, req_off, tag->trans_offset);
        }

        *payload_end = req_off;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build read request, got error %s!", plc_tag_decode_error(rc));
        tag->base_tag.status = (int8_t)rc;
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Read request packet:");
    pdebug_dump_bytes(DEBUG_DETAIL, buffer + *payload_start, *payload_end - *payload_start);

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int cip_handle_read_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = (cip_tag_p)context;
    int resp_data_size = 0;
    int offset = *payload_start;
    // int payload_size = *payload_end - *payload_start;

    (void)buffer_capacity;

    pdebug(DEBUG_DETAIL, "Starting for %" REQ_ID_FMT ".", req_id);

    do {
        uint8_t service_response;
        uint8_t reserved;
        uint8_t service_status;
        uint8_t status_extra_words;

        TRY_GET_BYTE(buffer, *payload_end, offset, service_response);
        TRY_GET_BYTE(buffer, *payload_end, offset, reserved);
        TRY_GET_BYTE(buffer, *payload_end, offset, service_status);
        TRY_GET_BYTE(buffer, *payload_end, offset, status_extra_words);

        (void)reserved;

        if(service_response != (CIP_CMD_OK | CIP_CMD_READ) && service_response != (CIP_CMD_OK | CIP_CMD_READ_FRAG)) {
            pdebug(DEBUG_WARN, "Unexpected CIP service response type %" PRIu8 "!", service_response);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        /* check the error response */
        if(service_status != CIP_STATUS_OK && service_status != CIP_STATUS_FRAG) {
            uint16_t extended_err_status = 0;

            if(status_extra_words > 0) {
                TRY_GET_U16_LE(buffer, *payload_end, offset, extended_err_status);
            }

            pdebug(DEBUG_WARN, "Error response %s (%s) from the PLC!", cip_decode_error_short(service_status, extended_err_status), cip_decode_error_long(service_status, extended_err_status));
            rc = cip_decode_error_code(service_status, extended_err_status);
            break;
        }

        /* all good. Copy the data type and data. */

        /* get the data type. */
        TRY_GET_BYTE(buffer, *payload_end, offset, tag->tag_type_info[0]);

        if(tag->tag_type_info[0] < 0xA0) {
            pdebug(DEBUG_DETAIL, "Tag type is a standard atomic type.");

            /* two byte type */
            tag->tag_type_info_length = 2;

            /* get the next byte. */
            TRY_GET_BYTE(buffer, *payload_end, offset, tag->tag_type_info[1]);

            /* handle bit/boolean types specially */
            if(tag->tag_type_info[0] == 0xC1) {
                /* bit type */
                pdebug(DEBUG_DETAIL, "Tag is a boolean/bit type.");
                tag->base_tag.is_bit = 1;
                tag->base_tag.bit = tag->tag_type_info[1]; /* bit number */
            }
        } else {
            /* UDT, array etc. */
            pdebug(DEBUG_DETAIL, "Tag type is a UDT, array etc.");

            /* four bytes. */
            tag->tag_type_info_length = 4;

            /* get the remaining three bytes. */
            TRY_GET_BYTE(buffer, *payload_end, offset, tag->tag_type_info[1]);
            TRY_GET_BYTE(buffer, *payload_end, offset, tag->tag_type_info[2]);
            TRY_GET_BYTE(buffer, *payload_end, offset, tag->tag_type_info[3]);
        }

        resp_data_size = *payload_end - offset;

        /* check to see if the response fits. */
        if(((int32_t)tag->trans_offset + (int32_t)resp_data_size) > tag->base_tag.size) {
            uint8_t *new_data = NULL;
            int total_bytes = (int)(unsigned int)tag->trans_offset + resp_data_size;

            pdebug(DEBUG_INFO, "Expanding tag data buffer from %" PRId32 " to %d bytes.", tag->base_tag.size, total_bytes);

            new_data = mem_realloc(tag->base_tag.data, total_bytes);
            if(new_data != NULL) {
                tag->base_tag.data = new_data;
                tag->base_tag.size = total_bytes;
            } else {
                pdebug(DEBUG_WARN, "Unable to allocate memory for new tag data buffer!");
                rc = PLCTAG_ERR_NO_MEM;
                break;
            }
        }

        for(int i=0; i < resp_data_size; i++) {
            TRY_GET_BYTE(buffer, buffer_capacity, offset, tag->base_tag.data[tag->trans_offset + (uint32_t)i]);
        }

        tag->trans_offset += (uint16_t)(unsigned int)resp_data_size;

        /* do we have more work to do? */
        if(tag->trans_offset < (uint32_t)tag->base_tag.size) {
            pdebug(DEBUG_DETAIL, "Starting new read request for remaining data.");
            rc = plc_start_request(tag->plc, &(tag->request), tag, cip_build_read_request_callback, cip_handle_read_response_callback);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error queuing up next request!");
                break;
            }
        } else {
            /* done! */
            tag->trans_offset = 0;
            rc = PLCTAG_STATUS_OK;
            tag->base_tag.status = (int8_t)rc;
        }

        *payload_start = *payload_end;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, handling read response!", plc_tag_decode_error(rc));
        tag->base_tag.status = (int8_t)rc;
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int cip_build_write_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = (cip_tag_p)context;
    int req_off = *payload_start;
    // int max_trans_size = 0;
    // int num_elems = 0;
    int trans_size = 0;
    int max_payload = 0;
    bool use_frag = FALSE;

    (void)buffer_capacity;
    (void)req_id;

    pdebug(DEBUG_DETAIL, "Starting for request %" REQ_ID_FMT ".", req_id);

    /* encode the request. */

    do {
        int cmd_index = req_off;

        if(tag->trans_offset != 0) {
            /* we may still patch this. */
            TRY_SET_BYTE(buffer, *payload_end, req_off, CIP_CMD_WRITE);
        } else {
            TRY_SET_BYTE(buffer, *payload_end, req_off, CIP_CMD_WRITE_FRAG);
        }

        /* copy the encoded tag name. */
        for(int index=0; index < tag->encoded_name_length; index++) {
            TRY_SET_BYTE(buffer, *payload_end, req_off, tag->encoded_name[index]);
        }

        /* add the element count */
        TRY_SET_U16_LE(buffer, *payload_end, req_off, tag->elem_count);

        /* calculate the maximum possible payload. */
        max_payload = *payload_end - req_off;

        /* Clamp to 4-byte boundaries. */
        pdebug(DEBUG_DETAIL, "Clamping to 4-byte boundary.");

        max_payload = max_payload & (INT_MAX - 3);

        if((int32_t)max_payload < (tag->base_tag.size - (int32_t)tag->trans_offset)) {
            pdebug(DEBUG_DETAIL, "We will need to use fragmented writes.");

            use_frag = TRUE;
        }

        /* patch the command type and write out the offset. */
        if(use_frag == TRUE) {
            TRY_SET_BYTE(buffer, *payload_end, cmd_index, CIP_CMD_WRITE_FRAG);

            TRY_SET_U32_LE(buffer, *payload_end, req_off, tag->trans_offset);

            /* remove the overhed of the offset */
            max_payload -= 4;
        }

        /* do we still have enough room left to write something? */
        if(max_payload <= 0) {
            pdebug(DEBUG_INFO, "Insufficient space to write out request.");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        /* add the offset if we are doing a fragmented read. */
        if(use_frag) {
            TRY_SET_U32_LE(buffer, *payload_end, req_off, tag->trans_offset);
        }

        /* copy the write data. */
        for(int index=0; index < max_payload; index++) {
            TRY_SET_BYTE(buffer, *payload_end, req_off, tag->base_tag.data[tag->trans_offset + (uint32_t)index]);
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error copying data to the output buffer!");
            break;
        }

        req_off += max_payload;

        if(rc != PLCTAG_STATUS_OK) break;

        /* update the amount transfered. */
        tag->trans_offset += (uint16_t)(unsigned int)trans_size;

        pdebug(DEBUG_DETAIL, "Write request packet:");
        pdebug_dump_bytes(DEBUG_DETAIL, buffer + *payload_start, req_off - *payload_start);

        /* we are done, mark the packet space as used. */
        *payload_end = req_off;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build read request, got error %s!", plc_tag_decode_error(rc));
        tag->base_tag.status = (int8_t)rc;
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



int cip_handle_write_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = (cip_tag_p)context;
    // int payload_size = *payload_end - *payload_start;
    int offset = *payload_start;

    (void)buffer_capacity;

    pdebug(DEBUG_DETAIL, "Starting for request %" REQ_ID_FMT ".", req_id);

    do {
        uint8_t service_response;
        uint8_t reserved;
        uint8_t service_status;
        uint8_t status_extra_words;

        TRY_GET_BYTE(buffer, *payload_end, offset, service_response);
        TRY_GET_BYTE(buffer, *payload_end, offset, reserved);
        TRY_GET_BYTE(buffer, *payload_end, offset, service_status);
        TRY_GET_BYTE(buffer, *payload_end, offset, status_extra_words);

        (void)reserved;

        if(service_response != (CIP_CMD_OK | CIP_CMD_WRITE) && service_response != (CIP_CMD_OK | CIP_CMD_WRITE_FRAG)) {
            pdebug(DEBUG_WARN, "Unexpected CIP service response type %" PRIu8 "!", service_response);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        /* check the error response */
        if(service_status != CIP_STATUS_OK && service_status != CIP_STATUS_FRAG) {
            uint16_t extended_err_status = 0;

            if(status_extra_words > 0) {
                TRY_GET_U16_LE(buffer, *payload_end, offset, extended_err_status);
            }

            pdebug(DEBUG_WARN, "Error response %s (%s) from the PLC!", cip_decode_error_short(service_status, extended_err_status), cip_decode_error_long(service_status, extended_err_status));
            rc = cip_decode_error_code(service_status, extended_err_status);
            break;
        }

        /* do we have more work to do? */
        if(tag->trans_offset < (uint32_t)tag->base_tag.size) {
            pdebug(DEBUG_DETAIL, "Starting new write request for remaining data.");
            rc = plc_start_request(tag->plc, &(tag->request), tag, cip_build_write_request_callback, cip_handle_write_response_callback);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error, %s, queuing up next request!", plc_tag_decode_error(rc));
                break;
            }
        } else {
            /* done! */
            tag->trans_offset = 0;
            rc = PLCTAG_STATUS_OK;
            tag->base_tag.status = (int8_t)rc;
        }
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        tag->base_tag.status = (int8_t)rc;
        return rc;
    }

    /* Clear out the buffer.   This marks that we processed it all. */
    *payload_start = *payload_end;

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}




/* this just copies the tag's buffer into the output. */

int raw_cip_build_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = (cip_tag_p)context;
    int req_off = *payload_start;
    int max_trans_size = 0;
    // int num_elems = 0;
    // int trans_size = 0;

    (void)buffer_capacity;
    (void)req_id;

    pdebug(DEBUG_DETAIL, "Starting.");


    do {
        max_trans_size = *payload_end - *payload_start;

        if(max_trans_size < tag->base_tag.size) {
            pdebug(DEBUG_WARN, "Tag raw CIP command too large to fit!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        for(int index=0; index < (int)tag->base_tag.size; index++) {
            TRY_SET_BYTE(buffer, *payload_end, req_off, tag->base_tag.data[index]);
        }

        *payload_end = req_off;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build raw CIP request, got error %s!", plc_tag_decode_error(rc));
        tag->base_tag.status = (int8_t)rc;
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Raw CIP request packet:");
    pdebug_dump_bytes(DEBUG_DETAIL, buffer + *payload_start, *payload_end - *payload_start);

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int raw_cip_handle_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    cip_tag_p tag = (cip_tag_p)context;
    int resp_data_size = 0;
    int offset = *payload_start;

    (void)buffer_capacity;

    pdebug(DEBUG_DETAIL, "Starting for %" REQ_ID_FMT ".", req_id);

    do {
        resp_data_size = *payload_end - *payload_start;

        /* check to see if the response fits. */
        if(resp_data_size > tag->base_tag.size) {
            uint8_t *new_data = NULL;

            pdebug(DEBUG_INFO, "Expanding tag data buffer from %" PRId32 " to %d bytes.", tag->base_tag.size, resp_data_size);

            new_data = mem_realloc(tag->base_tag.data, resp_data_size);
            if(new_data != NULL) {
                tag->base_tag.data = new_data;
                tag->base_tag.size = resp_data_size;
            } else {
                pdebug(DEBUG_WARN, "Unable to allocate memory for new tag data buffer!");
                rc = PLCTAG_ERR_NO_MEM;
                break;
            }
        }

        for(int index=0; index < resp_data_size; index++) {
            TRY_GET_BYTE(buffer, buffer_capacity, offset, tag->base_tag.data[index]);
        }

        *payload_start = *payload_end;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, handling read response!", plc_tag_decode_error(rc));
        tag->base_tag.status = (int8_t)rc;
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}

