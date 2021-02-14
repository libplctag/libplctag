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
#include <ab2/cip.h>
#include <ab2/cip_layer.h>
#include <ab2/raw_cip_tag.h>
#include <ab2/cip_plc.h>
#include <util/attr.h>
#include <util/bool.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/plc.h>
#include <util/string.h>


typedef struct {
    struct plc_tag_t base_tag;

    /* plc and request info */
    plc_p plc;
    struct plc_request_s request;

    /* count of bytes sent or received. */
    uint32_t payload_size;
} raw_cip_tag_t;
typedef raw_cip_tag_t *raw_cip_tag_p;



static void raw_cip_tag_destroy(void *tag_arg);



/* vtable functions */
static int raw_cip_tag_abort(plc_tag_p tag);
static int raw_cip_tag_read(plc_tag_p tag);
static int raw_cip_tag_status(plc_tag_p tag);
static int raw_cip_tag_write(plc_tag_p tag);

static int raw_cip_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value);
static int raw_cip_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value);


/* vtable for raw CIP tags */
static struct tag_vtable_t raw_cip_tag_vtable = {
    raw_cip_tag_abort,
    raw_cip_tag_read,
    raw_cip_tag_status,
    /* raw_cip_tag_tickler */ NULL,
    raw_cip_tag_write,

    /* attribute accessors */
    raw_cip_get_int_attrib,
    raw_cip_set_int_attrib
};


static tag_byte_order_t raw_cip_tag_byte_order = {
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




static int raw_cip_build_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);
static int raw_cip_handle_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);


plc_tag_p raw_cip_tag_create(ab2_plc_type_t plc_type, attr attribs)
{
    raw_cip_tag_p tag = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    tag = (raw_cip_tag_p)base_tag_create(sizeof(*tag), (void (*)(void*))raw_cip_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_WARN, "Unable to allocate new CIP tag!");
        return NULL;
    }

    tag->base_tag.is_bit = 0;
    tag->base_tag.bit = 0;
    tag->base_tag.data = NULL;
    tag->base_tag.size = 0;

    /* get the PLC */

    /* TODO - add any PLC-specific attributes here. */
    switch(plc_type) {
        case AB2_PLC_LGX:
            /* if it is not set, set it to true/1. */
            attr_set_int(attribs, "cip_payload", attr_get_int(attribs, "cip_payload", CIP_STD_EX_PAYLOAD));
            /* fall through */
        case AB2_PLC_MLGX800:
            /* fall through */
        case AB2_PLC_OMRON_NJNX:
            tag->base_tag.vtable = &raw_cip_tag_vtable;
            tag->base_tag.byte_order = &raw_cip_tag_byte_order;
            tag->plc = cip_plc_get(attribs);
            break;

        default:
            pdebug(DEBUG_WARN, "Unsupported PLC type %d!", plc_type);
            tag->plc = NULL;
            break;
    }

    if(!tag->plc) {
        pdebug(DEBUG_WARN, "Unable to get PLC!");
        rc_dec(tag);
        return NULL;
    }

    pdebug(DEBUG_INFO, "Done.");

    return (plc_tag_p)tag;
}


void raw_cip_tag_destroy(void *tag_arg)
{
    raw_cip_tag_p tag = (raw_cip_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer passed to destructor!");
        return;
    }

    /* get rid of any outstanding requests, timers and events. */
    if(tag->plc) {
        plc_stop_request(tag->plc, &(tag->request));

        /* unlink the protocol layers. */
        tag->plc = rc_dec(tag->plc);
    }

    /* delete the base tag parts. */
    base_tag_destroy((plc_tag_p)tag);

    pdebug(DEBUG_INFO, "Done.");
}



int raw_cip_tag_abort(plc_tag_p tag_arg)
{
    raw_cip_tag_p tag = (raw_cip_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    plc_stop_request(tag->plc, &(tag->request));

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int raw_cip_tag_read(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    raw_cip_tag_p tag = (raw_cip_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_start_request(tag->plc, &(tag->request), tag, raw_cip_build_request_callback, raw_cip_handle_response_callback);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to start read request!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int raw_cip_tag_status(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    raw_cip_tag_p tag = (raw_cip_tag_p)tag_arg;

    pdebug(DEBUG_SPEW, "Starting.");

    rc = GET_STATUS(tag->base_tag.status);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}



int raw_cip_tag_write(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    raw_cip_tag_p tag = (raw_cip_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_start_request(tag->plc, &(tag->request), tag, raw_cip_build_request_callback, raw_cip_handle_response_callback);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to start write request!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/* this just copies the tag's buffer into the output. */

int raw_cip_build_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    raw_cip_tag_p tag = (raw_cip_tag_p)context;
    int req_off = *payload_start;
    int max_trans_size = 0;
    int trans_size = 0;

    (void)buffer_capacity;
    (void)req_id;

    pdebug(DEBUG_DETAIL, "Starting.");

    do {
        trans_size = (int)(unsigned int)(tag->payload_size);
        max_trans_size = *payload_end - *payload_start;

        if(max_trans_size < trans_size) {
            pdebug(DEBUG_WARN, "Tag raw CIP command too large to fit!");
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        for(int index=0; index < trans_size; index++) {
            TRY_SET_BYTE(buffer, *payload_end, req_off, tag->base_tag.data[index]);
        }

        *payload_end = req_off;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build raw CIP request, got error %s!", plc_tag_decode_error(rc));
        SET_STATUS(tag->base_tag.status, rc);
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
    raw_cip_tag_p tag = (raw_cip_tag_p)context;
    int resp_data_size = 0;
    int offset = *payload_start;

    (void)buffer_capacity;

    pdebug(DEBUG_DETAIL, "Starting for %" REQ_ID_FMT ".", req_id);

    do {
        resp_data_size = *payload_end - *payload_start;

        /* resize the tag's data buffer. */
        rc = base_tag_resize_data((plc_tag_p)tag, resp_data_size);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to resize tag data buffer, error %s!", plc_tag_decode_error(rc));
            break;
        }

        for(int index=0; index < resp_data_size; index++) {
            TRY_GET_BYTE(buffer, buffer_capacity, offset, tag->base_tag.data[index]);
        }

        /* save the payload size in the tag. */
        tag->payload_size = (uint32_t)(int32_t)(resp_data_size);

        *payload_start = *payload_end;

        pdebug(DEBUG_DETAIL, "Raw CIP operation complete.");

        rc = PLCTAG_STATUS_OK;

        /* clear any in-flight flags. */
        critical_block(tag->base_tag.api_mutex) {
            tag->base_tag.read_complete = 1;
            tag->base_tag.read_in_flight = 0;
            tag->base_tag.write_complete = 1;
            tag->base_tag.write_in_flight = 0;
        }

        /* set the status last as that triggers the waiting thread. */
        SET_STATUS(tag->base_tag.status, rc);
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, handling raw CIP response!", plc_tag_decode_error(rc));
        SET_STATUS(tag->base_tag.status, rc);
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}




int raw_cip_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value)
{
    int res = default_value;
    raw_cip_tag_p tag = (raw_cip_tag_p)raw_tag;

    pdebug(DEBUG_DETAIL, "Starting for attribute \"%s\".", attrib_name);

    /* assume we have a match. */
    SET_STATUS(tag->base_tag.status, PLCTAG_STATUS_OK);

    /* match the attribute. Raw tags only have size.*/
    if(str_cmp_i(attrib_name, "idle_timeout_ms") == 0) {
        res = plc_get_idle_timeout(tag->plc);
    } else if(str_cmp_i(attrib_name, "payload_size") == 0) {
        res = (int)(int32_t)tag->payload_size;
        pdebug(DEBUG_DETAIL, "Returning %d result for payload_size.", res);
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        SET_STATUS(tag->base_tag.status, PLCTAG_ERR_UNSUPPORTED);
        return default_value;
    }

    pdebug(DEBUG_DETAIL, "Done for attribute \"%s\".", attrib_name);

    return res;
}


int raw_cip_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value)
{
    int rc = PLCTAG_STATUS_OK;
    raw_cip_tag_p tag = (raw_cip_tag_p)raw_tag;

    pdebug(DEBUG_DETAIL, "Starting for attribute \"%s\".", attrib_name);

    if(str_cmp_i(attrib_name, "idle_timeout_ms") == 0) {
        rc = plc_set_idle_timeout(tag->plc, new_value);
    } else if(str_cmp_i(attrib_name, "payload_size") == 0) {
        if(new_value > 0) {
            rc = base_tag_resize_data((plc_tag_p)tag, new_value);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to resize tag data buffer, error %s!", plc_tag_decode_error(rc));
                return rc;
            }

            /* store the payload size in the payload_size field */
            tag->payload_size = (uint32_t)(int32_t)new_value;
        } else {
            pdebug(DEBUG_WARN, "The payload size must be greater than zero!");
            rc = PLCTAG_ERR_TOO_SMALL;
            return rc;
        }
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        rc = PLCTAG_ERR_UNSUPPORTED;
        SET_STATUS(tag->base_tag.status, rc);
    }

    pdebug(DEBUG_DETAIL, "Done for attribute \"%s\".", attrib_name);

    return rc;
}


