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
#include <ab2/plc5.h>
#include <ab2/common_defs.h>
#include <ab2/pccc.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/string.h>


typedef struct {
    struct plc_tag_t base_tag;

    uint16_t elem_size;
    uint16_t elem_count;

    /* data type info */
    pccc_file_t data_file_type;
    int data_file_num;
    int data_file_elem;
    int data_file_sub_elem;

    /* plc and request info */
    pccc_plc_p plc;
    struct pccc_plc_request_s request;
    uint16_t tsn; /* transfer sequence number */
    uint16_t trans_offset;

} ab2_plc5_tag_t;
typedef ab2_plc5_tag_t *ab2_plc5_tag_p;


#define PLC5_RANGE_READ_FUNC ((uint8_t)(0x01))
#define PLC5_RANGE_WRITE_FUNC ((uint8_t)(0x00))

#define PLC5_WORD_RANGE_READ_MAX_PAYLOAD (244)
#define PLC5_WORD_RANGE_WRITE_MAX_PAYLOAD (244)

static void plc5_tag_destroy(void *tag_arg);

static int plc5_tag_abort(plc_tag_p tag);
static int plc5_tag_read(plc_tag_p tag);
static int plc5_tag_status(plc_tag_p tag);
static int plc5_tag_tickler(plc_tag_p tag);
static int plc5_tag_write(plc_tag_p tag);
static int plc5_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value);
static int plc5_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value);


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


static slice_t build_read_request_callback(slice_t output_buffer, pccc_plc_p plc, void *tag);
static int handle_read_response_callback(slice_t input_buffer, pccc_plc_p plc, void *tag);
static slice_t build_write_request_callback(slice_t output_buffer, pccc_plc_p plc, void *tag);
static int handle_write_response_callback(slice_t input_buffer, pccc_plc_p plc, void *tag);
static int encode_plc5_logical_address(slice_t output, int *offset, int data_file_num, int data_file_elem, int data_file_sub_elem);

plc_tag_p ab2_plc5_tag_create(attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = NULL;
    pccc_plc_p plc = NULL;
    const char *tag_name = NULL;

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

    rc = pccc_plc_parse_logical_address(tag_name, &(tag->data_file_type),&(tag->data_file_num), &(tag->data_file_elem), &(tag->data_file_sub_elem));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Malformed data file name!");
        rc_dec(tag);
        return NULL;
    }

    plc = pccc_plc_get(attribs);
    if(!plc) {
        pdebug(DEBUG_WARN, "Unable to get PLC!");
        rc_dec(tag);
        return NULL;
    }

    pccc_plc_request_init(plc, &(tag->request));

    /* set the vtable for base functions. */
    tag->base_tag.vtable = &plc5_vtable;

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

    /* delete the base tag parts. */
    base_tag_destroy((plc_tag_p)tag);

    /* get rid of any outstanding timers and events. */


    pdebug(DEBUG_INFO, "Done.");
}


int plc5_tag_abort(plc_tag_p tag_arg)
{
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    pccc_plc_request_abort(tag->plc, &(tag->request));

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

    rc = pccc_plc_request_start(tag->plc, &(tag->request), tag, build_read_request_callback, handle_read_response_callback);
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

    pdebug(DEBUG_INFO, "Starting.");

    rc = tag->base_tag.status;

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


int plc5_tag_tickler(plc_tag_p tag)
{
    (void)tag;

    pdebug(DEBUG_INFO, "Starting.");

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_ERR_UNSUPPORTED;
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

    rc = pccc_plc_request_start(tag->plc, &(tag->request), (void *)tag, build_write_request_callback, handle_write_response_callback);
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
    (void)attrib_name;
    (void)new_value;

    pdebug(DEBUG_WARN, "Unsupported attribute \"%s\"!", attrib_name);

    raw_tag->status  = PLCTAG_ERR_UNSUPPORTED;

    return PLCTAG_ERR_UNSUPPORTED;
}

#define TRY_SET_BYTE(out, off, val) rc = slice_set_byte(out, off, (uint8_t)(val)); if(rc != PLCTAG_STATUS_OK) break; else (off)++



slice_t build_read_request_callback(slice_t output_buffer, pccc_plc_p plc, void *tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;
    int req_off = 0;
    int max_trans_size = 0;
    int num_elems = 0;
    int trans_size = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* encode the request. */

    do {
        /* PCCC command type byte */
        TRY_SET_BYTE(output_buffer, req_off, PCCC_TYPED_CMD);

        /* status, always zero */
        TRY_SET_BYTE(output_buffer, req_off, 0);

        /* TSN - 16-bit value */
        tag->tsn = pccc_plc_get_tsn(plc);
        TRY_SET_BYTE(output_buffer, req_off, (tag->tsn & 0xFF));
        TRY_SET_BYTE(output_buffer, req_off, ((tag->tsn >> 8) & 0xFF));

        /* PLC5 read function. */
        TRY_SET_BYTE(output_buffer, req_off, PLC5_RANGE_READ_FUNC);

        /* offset of the transfer in words */
        TRY_SET_BYTE(output_buffer, req_off, ((tag->trans_offset/2) & 0xFF));
        TRY_SET_BYTE(output_buffer, req_off, (((tag->trans_offset/2) >> 8) & 0xFF));

        /* total transfer size in words. */
        TRY_SET_BYTE(output_buffer, req_off, ((unsigned int)(tag->base_tag.size/2) & 0xFF));
        TRY_SET_BYTE(output_buffer, req_off, (((unsigned int)(tag->base_tag.size/2) >> 8) & 0xFF));

        /* set the logical PLC-5 address. */
        rc = encode_plc5_logical_address(output_buffer, &req_off, tag->data_file_num, tag->data_file_elem, tag->data_file_sub_elem);
        if(rc != PLCTAG_STATUS_OK) break;

        /* max transfer size in bytes. */
        max_trans_size = PLC5_WORD_RANGE_READ_MAX_PAYLOAD;

        pdebug(DEBUG_DETAIL, "Maximum transfer size %d.", max_trans_size);

        /* max or remaining size of the transfer in bytes. */
        if((tag->base_tag.size - (int32_t)(uint32_t)tag->trans_offset) < max_trans_size) {
            max_trans_size = (int)(tag->base_tag.size - (int32_t)(uint32_t)tag->trans_offset);
        }

        pdebug(DEBUG_DETAIL, "Available transfer size %d.", max_trans_size);

        /* The transfer must be a multiple of the element size. */
        num_elems = max_trans_size / tag->elem_size;

        pdebug(DEBUG_DETAIL, "Number of elements possible to transfer %d.", num_elems);

        trans_size = num_elems * tag->elem_size;

        pdebug(DEBUG_DETAIL, "Actual bytes to transfer %d.", trans_size);

        TRY_SET_BYTE(output_buffer, req_off, trans_size);
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build read request, got error %s!", plc_tag_decode_error(rc));
        tag->base_tag.status = (int8_t)rc;
        return slice_make_err(rc);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return slice_from_slice(output_buffer, 0, req_off);
}


int handle_read_response_callback(slice_t input_buffer, pccc_plc_p plc, void *tag_arg)
{
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;
    int rc = PLCTAG_STATUS_OK;
    int resp_data_size = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* check the response */
    if(slice_len(input_buffer) < 4) {
        pdebug(DEBUG_WARN, "Unexpectedly short PCCC response!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    if(slice_get_byte(input_buffer, 0) != (PCCC_TYPED_CMD | PCCC_CMD_OK)) {
        pdebug(DEBUG_WARN, "Unexpected PCCC packet response type %d!", (int)(unsigned int)slice_get_byte(input_buffer, 0));
        return PLCTAG_ERR_BAD_REPLY;
    }

    if(slice_get_byte(input_buffer, 1) != 0) {
        pdebug(DEBUG_WARN, "Received error response %s (%d)!", pccc_plc_decode_error(slice_from_slice(input_buffer, 1, 5)), slice_get_byte(input_buffer, 1));
        return PLCTAG_ERR_BAD_REPLY;
    }

    /*
     * copy the data.
     *
     * Note that we start at byte 2.  Bytes 0 and 1 are the CMD and
     * STS bytes, respectively.
     */
    resp_data_size = slice_len(input_buffer) - 2;

    for(int i=0; i < resp_data_size; i++) {
        tag->base_tag.data[tag->trans_offset + i] = slice_get_byte(input_buffer, i + 2);
    }

    tag->trans_offset += (uint16_t)(unsigned int)resp_data_size;

    /* do we have more work to do? */
    if(tag->trans_offset < tag->base_tag.size) {
        pdebug(DEBUG_DETAIL, "Starting new read request for remaining data.");
        rc = pccc_plc_request_start(plc, &(tag->request), tag, build_read_request_callback, handle_read_response_callback);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error queuing up next request!");
            return rc;
        }
    } else {
        /* done! */
        tag->trans_offset = 0;
        rc = PLCTAG_STATUS_OK;
        tag->base_tag.status = (int8_t)rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


slice_t build_write_request_callback(slice_t output_buffer, pccc_plc_p plc, void *tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;
    int req_off = 0;
    int encoded_file_start = 0;
    int max_trans_size = 0;
    int num_elems = 0;
    int trans_size = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* encode the request. */

    do {
        /* PCCC command type byte */
        TRY_SET_BYTE(output_buffer, req_off, PCCC_TYPED_CMD);

        /* status, always zero */
        TRY_SET_BYTE(output_buffer, req_off, 0);

        /* TSN - 16-bit value */
        tag->tsn = pccc_plc_get_tsn(plc);
        TRY_SET_BYTE(output_buffer, req_off, (tag->tsn & 0xFF));
        TRY_SET_BYTE(output_buffer, req_off, ((tag->tsn >> 8) & 0xFF));

        /* PLC5 read function. */
        TRY_SET_BYTE(output_buffer, req_off, PLC5_RANGE_WRITE_FUNC);

        /* offset of the transfer in words */
        TRY_SET_BYTE(output_buffer, req_off, ((tag->trans_offset/2) & 0xFF));
        TRY_SET_BYTE(output_buffer, req_off, (((tag->trans_offset/2) >> 8) & 0xFF));

        /* total transfer size in words. */
        TRY_SET_BYTE(output_buffer, req_off, ((unsigned int)(tag->base_tag.size/2) & 0xFF));
        TRY_SET_BYTE(output_buffer, req_off, (((unsigned int)(tag->base_tag.size/2) >> 8) & 0xFF));

        /* set the logical PLC-5 address. */
        encoded_file_start = req_off;
        rc = encode_plc5_logical_address(output_buffer, &req_off, tag->data_file_num, tag->data_file_elem, tag->data_file_sub_elem);
        if(rc != PLCTAG_STATUS_OK) break;

        /* max transfer size. */
        max_trans_size = PLC5_WORD_RANGE_WRITE_MAX_PAYLOAD - (req_off - encoded_file_start);

        pdebug(DEBUG_DETAIL, "Maximum transfer size %d.", max_trans_size);

        /* size of the transfer in bytes. */
        if((tag->base_tag.size - (int32_t)(uint32_t)tag->trans_offset) < max_trans_size) {
            max_trans_size = (int)(tag->base_tag.size - (int32_t)(uint32_t)tag->trans_offset);
        }

        pdebug(DEBUG_DETAIL, "Available transfer size %d.", max_trans_size);

        /* The transfer must be a multiple of the element size. */
        num_elems = max_trans_size / tag->elem_size;

        pdebug(DEBUG_DETAIL, "Number of elements possible to transfer %d.", num_elems);

        trans_size = num_elems * tag->elem_size;

        pdebug(DEBUG_DETAIL, "Actual bytes to transfer %d.", trans_size);

        /* copy the data. */
        for(int i=0; i < trans_size && rc == PLCTAG_STATUS_OK; i++) {
            rc = slice_set_byte(output_buffer, req_off, tag->base_tag.data[tag->trans_offset + i]);
            req_off++;
        }

        if(rc != PLCTAG_STATUS_OK) break;

        /* update the amount transfered. */
        tag->trans_offset += (uint16_t)(unsigned int)trans_size;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build read request, got error %s!", plc_tag_decode_error(rc));
        tag->base_tag.status = (int8_t)rc;
        return slice_make_err(rc);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return slice_from_slice(output_buffer, 0, req_off);
}


int handle_write_response_callback(slice_t input_buffer, pccc_plc_p plc, void *tag_arg)
{
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* check the response */
    if(slice_len(input_buffer) < 4) {
        pdebug(DEBUG_WARN, "Unexpectedly short PCCC response!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    if(slice_get_byte(input_buffer, 0) != (PCCC_TYPED_CMD | PCCC_CMD_OK)) {
        pdebug(DEBUG_WARN, "Unexpected PCCC packet response type %d!", (int)(unsigned int)slice_get_byte(input_buffer, 0));
        return PLCTAG_ERR_BAD_REPLY;
    }

    if(slice_get_byte(input_buffer, 1) != 0) {
        pdebug(DEBUG_WARN, "Received error response %s (%d)!", pccc_plc_decode_error(slice_from_slice(input_buffer, 1, 5)), slice_get_byte(input_buffer, 1));
        return PLCTAG_ERR_BAD_REPLY;
    }

    /* do we have more work to do? */
    if(tag->trans_offset < tag->base_tag.size) {
        pdebug(DEBUG_DETAIL, "Starting new write request for remaining data.");
        rc = pccc_plc_request_start(plc, &(tag->request), tag, build_write_request_callback,handle_write_response_callback);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error queuing up next request!");
            tag->base_tag.status = (int8_t)rc;
            return rc;
        }
    } else {
        /* done! */
        tag->trans_offset = 0;
        rc = PLCTAG_STATUS_OK;
        tag->base_tag.status = (int8_t)rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int encode_plc5_logical_address(slice_t output, int *offset, int data_file_num, int data_file_elem, int data_file_sub_elem)
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
            TRY_SET_BYTE(output, *offset, 0x0E);
        } else {
            TRY_SET_BYTE(output, *offset, 0x06);
        }

        /* add in the data file number. */
        if(data_file_num <= 254) {
            TRY_SET_BYTE(output, *offset, (unsigned int)(data_file_num));
        } else {
            TRY_SET_BYTE(output, *offset, 0xFF);
            TRY_SET_BYTE(output, *offset, (unsigned int)(data_file_num & 0xFF));
            TRY_SET_BYTE(output, *offset, (unsigned int)((data_file_num >> 8) & 0xFF));
        }

        /* add in the element number */
        if(data_file_elem <= 254) {
            TRY_SET_BYTE(output, *offset, (unsigned int)(data_file_elem));
        } else {
            TRY_SET_BYTE(output, *offset, 0xFF);
            TRY_SET_BYTE(output, *offset, (unsigned int)(data_file_elem & 0xFF));
            TRY_SET_BYTE(output, *offset, (unsigned int)((data_file_elem >> 8) & 0xFF));
        }

        /* check to see if we need to put in a subelement. */
        if(data_file_sub_elem >= 0) {
            if(data_file_sub_elem <= 254) {
                TRY_SET_BYTE(output, *offset, (unsigned int)(data_file_sub_elem));
            } else {
                TRY_SET_BYTE(output, *offset, 0xFF);
                TRY_SET_BYTE(output, *offset, (unsigned int)(data_file_sub_elem & 0xFF));
                TRY_SET_BYTE(output, *offset, (unsigned int)((data_file_sub_elem >> 8) & 0xFF));
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


