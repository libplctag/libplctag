/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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
#include <ab2/plc5.h>
#include <ab2/common_defs.h>
#include <ab2/df1.h>
#include <ab2/pccc_eip_plc.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/plc.h>
#include <util/string.h>


typedef struct {
    struct plc_tag_t base_tag;

    uint16_t elem_size;
    uint16_t elem_count;

    /* data type info */
    df1_file_t data_file_type;
    int data_file_num;
    int data_file_elem;
    int data_file_sub_elem;

    /* plc and request info */
    plc_p plc;
    struct plc_request_s request;

    uint16_t tsn; /* transfer sequence number of the most recent request. */
    uint16_t trans_offset;

} ab2_plc5_tag_t;
typedef ab2_plc5_tag_t *ab2_plc5_tag_p;


#define PLC5_RANGE_READ_FUNC ((uint8_t)(0x01))
#define PLC5_RANGE_WRITE_FUNC ((uint8_t)(0x00))

#define PLC5_WORD_RANGE_READ_MAX_PAYLOAD (240)
#define PLC5_WORD_RANGE_WRITE_MAX_PAYLOAD (240)

static void plc5_tag_destroy(void *tag_arg);

static int plc5_tag_abort(plc_tag_p tag);
static int plc5_tag_read(plc_tag_p tag);
static int plc5_tag_status(plc_tag_p tag);
static int plc5_tag_tickler(plc_tag_p tag);
static int plc5_tag_write(plc_tag_p tag);
static int plc5_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value);
static int plc5_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value);


tag_byte_order_t plc5_tag_byte_order = {
    .is_allocated = 0,

    .int16_order = {0,1},
    .int32_order = {0,1,2,3},
    .int64_order = {0,1,2,3,4,5,6,7},
    .float32_order = {2,3,0,1}, /* yes, it is that weird. */
    .float64_order = {0,1,2,3,4,5,6,7},

    .str_is_defined = 1,
    .str_is_counted = 1,
    .str_is_fixed_length = 1,
    .str_is_zero_terminated = 0,
    .str_is_byte_swapped = 1,

    .str_count_word_bytes = 2,
    .str_max_capacity = 82,
    .str_total_length = 84,
    .str_pad_bytes = 0
};



/* vtable for PLC-5 tags */
static struct tag_vtable_t plc5_vtable = {
    plc5_tag_abort,
    plc5_tag_read,
    plc5_tag_status,
    plc5_tag_tickler,
    plc5_tag_write,

    /* attribute accessors */
    plc5_get_int_attrib,
    plc5_set_int_attrib
};


static int build_read_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);
static int handle_read_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);
static int build_write_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);
static int handle_write_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);

static int encode_plc5_logical_address(uint8_t *buffer, int buffer_capacity, int *offset, int data_file_num, int data_file_elem, int data_file_sub_elem);

plc_tag_p ab2_plc5_tag_create(attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = NULL;
    const char *tag_name = NULL;
    int tmp = 0;

    pdebug(DEBUG_INFO, "Starting.");

    tag = (ab2_plc5_tag_p)base_tag_create(sizeof(*tag), (void (*)(void*))plc5_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_WARN, "Unable to allocate new PLC/5 tag!");
        return NULL;
    }

    /* parse the PLC-5 tag name */
    tag_name = attr_get_str(attribs, "name", NULL);
    if(!tag_name) {
        pdebug(DEBUG_WARN, "Data file name and offset missing!");
        rc_dec(tag);
        return NULL;
    }

    rc = df1_parse_logical_address(tag_name, &(tag->data_file_type),&(tag->data_file_num), &(tag->data_file_elem), &(tag->data_file_sub_elem));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Malformed data file name!");
        rc_dec(tag);
        return NULL;
    }

    /* set up the tag data buffer. This involves getting the element size and element count. */
    tmp = df1_element_size(tag->data_file_type);
    if(tmp < 0) {
        pdebug(DEBUG_WARN, "Unsupported or unknown data file type, got error %s converting data file type to element size!", plc_tag_decode_error(tag->elem_size));
        rc_dec(tag);
        return NULL;
    }

    /* see if the user overrode the element size */
    tag->elem_size = (uint16_t)attr_get_int(attribs, "elem_size", tmp);

    if(tag->elem_size == 0) {
        pdebug(DEBUG_WARN, "Data file type unsupported or unknown, unable to determine element size automatically!");
        rc_dec(tag);
        return NULL;
    }

    tmp = attr_get_int(attribs, "elem_count", 1);
    if(tmp < 1) {
        pdebug(DEBUG_WARN, "Element count must be greater than zero or missing and will default to one!");
        rc_dec(tag);
        return NULL;
    }

    tag->elem_count = (uint16_t)tmp;

    tag->base_tag.size = tag->elem_count * tag->elem_size;

    if(tag->base_tag.size <= 0) {
        pdebug(DEBUG_WARN, "Tag size must be a positive number of bytes!");
        rc_dec(tag);
        return NULL;
    }

    tag->base_tag.data = mem_alloc(tag->base_tag.size);
    if(!tag->base_tag.data) {
        pdebug(DEBUG_WARN, "Unable to allocate internal tag data buffer!");
        rc_dec(tag);
        return NULL;
    }

    tag->plc = pccc_eip_plc_get(attribs);
    if(!tag->plc) {
        pdebug(DEBUG_WARN, "Unable to get PLC!");
        rc_dec(tag);
        return NULL;
    }

    /* set the vtable for base functions. */
    tag->base_tag.vtable = &plc5_vtable;

    /* set up the byte order */
    tag->base_tag.byte_order = &plc5_tag_byte_order;

    pdebug(DEBUG_INFO, "Done.");

    return (plc_tag_p)tag;
}


/* helper functions. */
void plc5_tag_destroy(void *tag_arg)
{
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer passed to destructor!");
        return;
    }

    /* get rid of any outstanding timers and events. */

    /* unlink the protocol layers. */
    tag->plc = rc_dec(tag->plc);

    /* delete the base tag parts. */
    base_tag_destroy((plc_tag_p)tag);

    pdebug(DEBUG_INFO, "Done.");
}


int plc5_tag_abort(plc_tag_p tag_arg)
{
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    plc_stop_request(tag->plc, &(tag->request));

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int plc5_tag_read(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_start_request(tag->plc, &(tag->request), tag, build_read_request_callback, handle_read_response_callback);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to start read request!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int plc5_tag_status(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;

    pdebug(DEBUG_SPEW, "Starting.");

    rc = tag->base_tag.status;

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


int plc5_tag_tickler(plc_tag_p tag)
{
    (void)tag;

    pdebug(DEBUG_SPEW, "Starting.");

    pdebug(DEBUG_SPEW, "Done.");

    return PLCTAG_STATUS_OK;
}


int plc5_tag_write(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_start_request(tag->plc, &(tag->request), (void *)tag, build_write_request_callback, handle_write_response_callback);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to start read request!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int plc5_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value)
{
    int res = default_value;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)raw_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* assume we have a match. */
    tag->base_tag.status = PLCTAG_STATUS_OK;

    /* match the attribute. */
    if(str_cmp_i(attrib_name, "elem_size") == 0) {
        res = tag->elem_size;
    } else if(str_cmp_i(attrib_name, "elem_count") == 0) {
        res = tag->elem_count;
    } else if(str_cmp_i(attrib_name, "idle_timeout_ms") == 0) {
        res = plc_get_idle_timeout(tag->plc);
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        tag->base_tag.status = PLCTAG_ERR_UNSUPPORTED;
        return default_value;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return res;
}


int plc5_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)raw_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(str_cmp_i(attrib_name, "idle_timeout_ms") == 0) {
        rc = plc_set_idle_timeout(tag->plc, new_value);
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        rc = PLCTAG_ERR_UNSUPPORTED;
        tag->base_tag.status = (int8_t)rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



int build_read_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)context;
    int req_off = *payload_start;
    int max_trans_size = 0;
    int num_elems = 0;
    int trans_size = 0;

    (void)buffer_capacity;
    (void)req_id;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* encode the request. */

    do {
        /* PCCC command type byte */
        TRY_SET_BYTE(buffer, *payload_end, req_off, DF1_TYPED_CMD);

        /* status, always zero */
        TRY_SET_BYTE(buffer, *payload_end, req_off, 0);

        /* TSN - 16-bit value */
        rc = (uint16_t)pccc_eip_plc_get_tsn(tag->plc, &(tag->tsn));
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to get TSN!");
            break;
        }
        TRY_SET_U16_LE(buffer, *payload_end, req_off, tag->tsn);

        /* PLC5 read function. */
        TRY_SET_BYTE(buffer, *payload_end, req_off, PLC5_RANGE_READ_FUNC);

        /* offset of the transfer in words */
        TRY_SET_U16_LE(buffer, *payload_end, req_off, tag->trans_offset/2);

        /* total transfer size in words. */
        TRY_SET_U16_LE(buffer, *payload_end, req_off, tag->base_tag.size/2);

        /* set the logical PLC-5 address. */
        rc = encode_plc5_logical_address(buffer, *payload_end, &req_off, tag->data_file_num, tag->data_file_elem, tag->data_file_sub_elem);
        if(rc != PLCTAG_STATUS_OK) break;

        /* max transfer size in bytes. */

        /* how much is available? */
        max_trans_size = (int)(tag->base_tag.size - (int32_t)(uint32_t)tag->trans_offset);

        pdebug(DEBUG_DETAIL, "Available data size %d.", max_trans_size);

        /* clamp the transfer down to the maximum payload. */
        if(max_trans_size > PLC5_WORD_RANGE_READ_MAX_PAYLOAD)  {
            pdebug(DEBUG_DETAIL, "Clamping maximum size to maximum payload.");
            max_trans_size = PLC5_WORD_RANGE_READ_MAX_PAYLOAD;
        }
        pdebug(DEBUG_DETAIL, "Maximum allowed transfer size %d.", max_trans_size);

        /* The transfer must be a multiple of the element size. */
        num_elems = max_trans_size / tag->elem_size;

        pdebug(DEBUG_DETAIL, "Number of elements possible to transfer %d.", num_elems);

        trans_size = num_elems * tag->elem_size;

        pdebug(DEBUG_DETAIL, "Actual bytes to transfer %d.", trans_size);

        TRY_SET_BYTE(buffer, *payload_end, req_off, trans_size);

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


int handle_read_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)context;
    int resp_data_size = 0;
    int offset = *payload_start;
    int payload_size = *payload_end - *payload_start;

    (void)buffer_capacity;

    pdebug(DEBUG_DETAIL, "Starting for %" REQ_ID_FMT ".", req_id);

    do {
        uint8_t df1_cmd_response = 0;
        uint8_t cmd_status;
        uint16_t tsn;

        pdebug(DEBUG_DETAIL, "Read response packet:");
        pdebug_dump_bytes(DEBUG_DETAIL, buffer + *payload_start, payload_size);

        if(payload_size < 4) {
            pdebug(DEBUG_WARN, "Unexpectedly short PCCC response!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        /* check the response */
        TRY_GET_BYTE(buffer, buffer_capacity, offset, df1_cmd_response);
        TRY_GET_BYTE(buffer, buffer_capacity, offset, cmd_status); /* TODO check order! */
        TRY_GET_U16_LE(buffer, buffer_capacity, offset, tsn);

        if(df1_cmd_response != (DF1_TYPED_CMD | DF1_CMD_OK)) {
            pdebug(DEBUG_WARN, "Unexpected PCCC packet response type %d!", (int)(unsigned int)df1_cmd_response);
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        if(cmd_status != 0) {
            uint16_t extended_status = 0;

            pdebug(DEBUG_WARN, "Received error response %s!", df1_decode_error(cmd_status, extended_status));
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /*
        * copy the data.
        *
        * Note that we start at byte 4.  Bytes 0 and 1 are the CMD and
        * STS bytes, respectively, then we have the TSN.
        */
        resp_data_size = *payload_end - offset;

        for(int i=0; i < resp_data_size; i++) {
            TRY_GET_BYTE(buffer, buffer_capacity, offset, tag->base_tag.data[tag->trans_offset + i]);
        }

        tag->trans_offset += (uint16_t)(unsigned int)resp_data_size;

        /* do we have more work to do? */
        if(tag->trans_offset < tag->base_tag.size) {
            pdebug(DEBUG_DETAIL, "Starting new read request for remaining data.");
            rc = plc_start_request(tag->plc, &(tag->request), tag, build_read_request_callback, handle_read_response_callback);
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


int build_write_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)context;
    int req_off = *payload_start;
    int encoded_file_start = 0;
    int max_trans_size = 0;
    int num_elems = 0;
    int trans_size = 0;

    (void)buffer_capacity;
    (void)req_id;

    pdebug(DEBUG_DETAIL, "Starting for request %" REQ_ID_FMT ".", req_id);

    /* encode the request. */

    do {
        /* PCCC command type byte */
        TRY_SET_BYTE(buffer, *payload_end, req_off, DF1_TYPED_CMD);

        /* status, always zero */
        TRY_SET_BYTE(buffer, *payload_end, req_off, 0);

        /* TSN - 16-bit value */
        rc = pccc_eip_plc_get_tsn(tag->plc, &(tag->tsn));
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to get TSN for request, error %s!", plc_tag_decode_error(rc));
            break;
        }

        TRY_SET_U16_LE(buffer, *payload_end, req_off, tag->tsn);

        /* PLC5 read function. */
        TRY_SET_BYTE(buffer, *payload_end, req_off, PLC5_RANGE_WRITE_FUNC);

        /* offset of the transfer in words */
        TRY_SET_U16_LE(buffer, *payload_end, req_off, tag->trans_offset/2);

        /* total transfer size in words. */
        TRY_SET_U16_LE(buffer, *payload_end, req_off, tag->base_tag.size/2);

        /* set the logical PLC-5 address. */
        encoded_file_start = req_off;
        rc = encode_plc5_logical_address(buffer, *payload_end, &req_off, tag->data_file_num, tag->data_file_elem, tag->data_file_sub_elem);
        if(rc != PLCTAG_STATUS_OK) break;

        /* max transfer size. */
        /* TODO - is this correct logic?   What about the TSN? */
        max_trans_size = PLC5_WORD_RANGE_WRITE_MAX_PAYLOAD - (req_off - encoded_file_start);

        pdebug(DEBUG_DETAIL, "Maximum transfer size %d.", max_trans_size);

        /* size of the transfer in bytes. */
        if((tag->base_tag.size - (int32_t)(uint32_t)tag->trans_offset) < max_trans_size) {
            max_trans_size = (int)(tag->base_tag.size - (int32_t)(uint32_t)tag->trans_offset);
        }

        if(*payload_end - req_off > max_trans_size) {
            pdebug(DEBUG_DETAIL, "Write won't fit in remaining space, truncate it.");
            max_trans_size = *payload_end - req_off;
        }

        pdebug(DEBUG_DETAIL, "Available transfer data size %d.", max_trans_size);

        /* The transfer must be a multiple of the element size. */
        num_elems = max_trans_size / tag->elem_size;

        pdebug(DEBUG_DETAIL, "Number of elements possible to transfer %d.", num_elems);

        trans_size = num_elems * tag->elem_size;

        pdebug(DEBUG_DETAIL, "Actual bytes to transfer %d.", trans_size);

        /* copy the data. */
        for(int i=0; i < trans_size; i++) {
            buffer[req_off + i] = tag->base_tag.data[tag->trans_offset + i];
        }
        req_off += trans_size;

        if(rc != PLCTAG_STATUS_OK) break;

        /* update the amount transfered. */
        tag->trans_offset += (uint16_t)(unsigned int)trans_size;

        pdebug(DEBUG_DETAIL, "Write request packet:");
        pdebug_dump_bytes(DEBUG_DETAIL, buffer + *payload_start, req_off - *payload_start);

        /* we are done, mark the packet space as used. */
        *payload_start = *payload_end = req_off;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build read request, got error %s!", plc_tag_decode_error(rc));
        tag->base_tag.status = (int8_t)rc;
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int handle_write_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)context;
    int payload_size = *payload_end - *payload_start;
    int offset = *payload_start;

    pdebug(DEBUG_DETAIL, "Starting for request %" REQ_ID_FMT ".", req_id);

    do {
        uint8_t df1_cmd_response = 0;
        uint8_t cmd_status;
        uint16_t tsn;

        pdebug(DEBUG_DETAIL, "Write response packet:");
        pdebug_dump_bytes(DEBUG_DETAIL, buffer + *payload_start, payload_size);

        if(payload_size < 4) {
            pdebug(DEBUG_WARN, "Unexpectedly short PCCC response!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        /* check the response */
        TRY_GET_BYTE(buffer, buffer_capacity, offset, df1_cmd_response);
        TRY_GET_BYTE(buffer, buffer_capacity, offset, cmd_status); /* TODO check order! */
        TRY_GET_U16_LE(buffer, buffer_capacity, offset, tsn);

        if(df1_cmd_response != (DF1_TYPED_CMD | DF1_CMD_OK)) {
            pdebug(DEBUG_WARN, "Unexpected PCCC packet response type %d!", (int)(unsigned int)df1_cmd_response);
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        if(cmd_status != 0) {
            uint16_t extended_status = 0;

            pdebug(DEBUG_WARN, "Received error response %s!", df1_decode_error(cmd_status, extended_status));
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* do we have more work to do? */
        if(tag->trans_offset < tag->base_tag.size) {
            pdebug(DEBUG_DETAIL, "Starting new write request for remaining data.");
            rc = plc_start_request(tag->plc, &(tag->request), tag, build_write_request_callback, handle_write_response_callback);
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


int encode_plc5_logical_address(uint8_t *buffer, int buffer_capacity, int *offset, int data_file_num, int data_file_elem, int data_file_sub_elem)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    do {
        /*
         * do the required levels.  Remember we start at the low bit!
         *
         * 0x0E = 0b1110 = levels 1, 2, and 3.  3 = subelement.
         * 0x06 = 0b0110 = levels 1, and 2.
         */
        if(data_file_sub_elem > 0) {
            TRY_SET_BYTE(buffer, buffer_capacity, *offset, 0x0E);
            //if(*offset < buffer_capacity) { buffer[*offset] = (uint8_t)(unsigned int)(0x0E); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++);
        } else {
            TRY_SET_BYTE(buffer, buffer_capacity, *offset, 0x06);
        }

        /* add in the data file number. */
        if(data_file_num <= 0xFE) {
            TRY_SET_BYTE(buffer, buffer_capacity, *offset, data_file_num);
        } else {
            TRY_SET_BYTE(buffer, buffer_capacity, *offset, 0xFF);
            TRY_SET_U16_LE(buffer, buffer_capacity, *offset, data_file_num);
        }

        /* add in the element number */
        if(data_file_elem <= 0xFE) {
            TRY_SET_BYTE(buffer, buffer_capacity, *offset, data_file_elem);
        } else {
            TRY_SET_BYTE(buffer, buffer_capacity, *offset, 0xFF);
            TRY_SET_U16_LE(buffer, buffer_capacity, *offset, data_file_elem);
        }

        /* check to see if we need to put in a subelement. */
        if(data_file_sub_elem >= 0) {
            if(data_file_sub_elem <= 0xFE) {
                TRY_SET_BYTE(buffer, buffer_capacity, *offset, data_file_sub_elem);
            } else {
                TRY_SET_BYTE(buffer, buffer_capacity, *offset, 0xFF);
                TRY_SET_U16_LE(buffer, buffer_capacity, *offset, data_file_sub_elem);
            }
        }
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, while building encoded data file tag!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_DETAIL,"Done.");

    return rc;
}


