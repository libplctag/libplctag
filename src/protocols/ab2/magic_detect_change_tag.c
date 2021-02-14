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
#include <ab2/magic_detect_change_tag.h>
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
} magic_detect_change_tag_t;
typedef magic_detect_change_tag_t *magic_detect_change_tag_p;


static void magic_detect_change_tag_destroy(void *tag_arg);


/* vtable functions */
static int magic_detect_change_tag_abort(plc_tag_p tag);
static int magic_detect_change_tag_read(plc_tag_p tag);
static int magic_detect_change_tag_status(plc_tag_p tag);
static int magic_detect_change_tag_write(plc_tag_p tag);

static int magic_detect_change_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value);
static int magic_detect_change_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value);


/* vtable for raw CIP tags */
static struct tag_vtable_t magic_detect_change_tag_vtable = {
    magic_detect_change_tag_abort,
    magic_detect_change_tag_read,
    magic_detect_change_tag_status,
    /* magic_detect_change_tag_tickler */ NULL,
    magic_detect_change_tag_write,

    /* attribute accessors */
    magic_detect_change_get_int_attrib,
    magic_detect_change_set_int_attrib
};


static tag_byte_order_t magic_detect_change_tag_byte_order = {
    .is_allocated = 0,

    .int16_order = {0,1},
    .int32_order = {0,1,2,3},
    .int64_order = {0,1,2,3,4,5,6,7},
    .float32_order = {0,1,2,3},
    .float64_order = {0,1,2,3,4,5,6,7},

    .str_is_defined = 1,
    .str_is_counted = 1,
    .str_is_fixed_length = 0,
    .str_is_zero_terminated = 0,
    .str_is_byte_swapped = 0,

    .str_count_word_bytes = 2,
    .str_max_capacity = 0,
    .str_total_length = 0,
    .str_pad_bytes = 0
};



static int magic_detect_change_build_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);
static int magic_detect_change_handle_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);


plc_tag_p magic_detect_change_tag_create(ab2_plc_type_t plc_type, attr attribs)
{
    magic_detect_change_tag_p tag = NULL;
    const char *name = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    tag = (magic_detect_change_tag_p)base_tag_create(sizeof(*tag), (void (*)(void*))magic_detect_change_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_WARN, "Unable to allocate new magic change detection tag!");
        return NULL;
    }

    tag->base_tag.is_bit = 0;
    tag->base_tag.bit = 0;
    tag->base_tag.data = NULL;
    tag->base_tag.size = 0;

    /* check the name. */
    name = attr_get_str(attribs, "name", NULL);
    if(name == NULL || str_length(name) == 0) {
        pdebug(DEBUG_WARN, "Tag name must not be missing or zero length!");
        return rc_dec(tag);
    }

    /* get the PLC */

    /* Only supported on Logix-class PLCs. */
    switch(plc_type) {
        case AB2_PLC_LGX:
            /* if it is not set, set it to true/1. */
            attr_set_int(attribs, "cip_payload", attr_get_int(attribs, "cip_payload", CIP_STD_EX_PAYLOAD));
            tag->base_tag.vtable = &magic_detect_change_tag_vtable;
            tag->base_tag.byte_order = &magic_detect_change_tag_byte_order;
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


void magic_detect_change_tag_destroy(void *tag_arg)
{
    magic_detect_change_tag_p tag = (magic_detect_change_tag_p)tag_arg;

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



int magic_detect_change_tag_abort(plc_tag_p tag_arg)
{
    magic_detect_change_tag_p tag = (magic_detect_change_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    plc_stop_request(tag->plc, &(tag->request));

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int magic_detect_change_tag_read(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    magic_detect_change_tag_p tag = (magic_detect_change_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_start_request(tag->plc, &(tag->request), tag, magic_detect_change_build_request_callback, magic_detect_change_handle_response_callback);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to start read request!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int magic_detect_change_tag_status(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    magic_detect_change_tag_p tag = (magic_detect_change_tag_p)tag_arg;

    pdebug(DEBUG_SPEW, "Starting.");

    rc = GET_STATUS(tag->base_tag.status);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}



int magic_detect_change_tag_write(plc_tag_p tag_arg)
{
    magic_detect_change_tag_p tag = (magic_detect_change_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_WARN, "Tag listing tag does not support write operations.");

    return PLCTAG_ERR_UNSUPPORTED;
}



int magic_detect_change_build_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    magic_detect_change_tag_p tag = (magic_detect_change_tag_p)context;
    int req_off = *payload_start;
    uint8_t raw_payload[] = {
                              CIP_CMD_GET_ATTRIBS,  /* get multiple attributes. */
                              0x03,  /* path is 3 words long. */
                              0x20,  /* 8-bit class follows */
                              0xAC,  /* class 0xAC */
                              0x25, 0x00,  /* 16-bit instance ID follows */
                              0x01, 0x00, /* instance ID 1. */
                              0x05, 0x00, /* attribute count, 5 */
                              0x01, 0x00, /* attribute #1 */
                              0x02, 0x00, /* attribute #2 */
                              0x03, 0x00, /* attribute #3 */
                              0x04, 0x00, /* attribute #4 */
                              0x0A, 0x00  /* attribute #10 */
                            };

    (void)buffer_capacity;
    (void)req_id;

    pdebug(DEBUG_DETAIL, "Starting.");


    do {
        /* copy the change detection request payload. */
        for(int index=0; index < (int)(unsigned int)(sizeof(raw_payload)) && rc == PLCTAG_STATUS_OK; index++) {
            TRY_SET_BYTE(buffer, *payload_end, req_off, raw_payload[index]);
        }
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s while filling in the change detection CIP request!", plc_tag_decode_error(rc));
            break;
        }

        /* done! */

        *payload_end = req_off;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build raw CIP request, got error %s!", plc_tag_decode_error(rc));
        SET_STATUS(tag->base_tag.status, rc);
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Change detection request packet:");
    pdebug_dump_bytes(DEBUG_DETAIL, buffer + *payload_start, *payload_end - *payload_start);

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}




int magic_detect_change_handle_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    magic_detect_change_tag_p tag = (magic_detect_change_tag_p)context;
    int resp_data_size = 0;
    int offset = *payload_start;
    uint8_t service_response;
    uint8_t reserved;
    uint8_t service_status;
    uint8_t status_extra_words;

    (void)buffer_capacity;

    pdebug(DEBUG_DETAIL, "Starting for %" REQ_ID_FMT ".", req_id);

    do {


        resp_data_size = *payload_end - *payload_start;

        if(resp_data_size < 4) {
            pdebug(DEBUG_WARN, "Unexpectedly small response size!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        TRY_GET_BYTE(buffer, *payload_end, offset, service_response);
        TRY_GET_BYTE(buffer, *payload_end, offset, reserved);
        TRY_GET_BYTE(buffer, *payload_end, offset, service_status);
        TRY_GET_BYTE(buffer, *payload_end, offset, status_extra_words);

        /* ignore reserved byte. */
        (void)reserved;

        if(service_response != (CIP_CMD_OK | CIP_CMD_GET_ATTRIBS)) {
            pdebug(DEBUG_WARN, "Unexpected CIP service response type %" PRIu8 "!", service_response);

            /* this is an error we should report about the tag, but not kill the PLC over it. */
            SET_STATUS(tag->base_tag.status, PLCTAG_ERR_BAD_REPLY);
            rc = PLCTAG_STATUS_OK;
            break;
        }

        /* check the error response */
        if(service_status == CIP_STATUS_FRAG) {
            /* For some reason we did not get all the data. */
            SET_STATUS(tag->base_tag.status, PLCTAG_ERR_PARTIAL);
            rc = PLCTAG_STATUS_OK;
            break;
        } else if(service_status != CIP_STATUS_OK) {
            uint16_t extended_err_status = 0;

            if(status_extra_words > 0) {
                TRY_GET_U16_LE(buffer, *payload_end, offset, extended_err_status);
            }

            pdebug(DEBUG_WARN, "Error response %s (%s) from the PLC!", cip_decode_error_short(service_status, extended_err_status), cip_decode_error_long(service_status, extended_err_status));
            rc = cip_decode_error_code(service_status, extended_err_status);

            /* this is an error we should report about the tag, but not kill the PLC over it. */
            SET_STATUS(tag->base_tag.status, rc);
            rc = PLCTAG_STATUS_OK;

            break;
        }

        /* how much data do we have? */
        resp_data_size = *payload_end - offset;

        /* make sure that the tag can hold the new payload. */
        rc = base_tag_resize_data((plc_tag_p)tag, resp_data_size);
        if(rc != PLCTAG_STATUS_OK) {
            /* this is fatal! */
            pdebug(DEBUG_WARN, "Unable to resize tag data buffer, error %s!", plc_tag_decode_error(rc));
            break;
        }

        /* copy the data into the tag buffer. */
        for(int index=0; index < resp_data_size; index++) {
            TRY_GET_BYTE(buffer, *payload_end, offset, tag->base_tag.data[index]);
        }
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to copy data into the tag buffer, error %s!", plc_tag_decode_error(rc));
            break;
        }

        /* done! */
        pdebug(DEBUG_DETAIL, "Read of tag change detection data complete.");

        rc = PLCTAG_STATUS_OK;

        /* clear any in-flight flags. */
        critical_block(tag->base_tag.api_mutex) {
            tag->base_tag.read_complete = 1;
            tag->base_tag.read_in_flight = 0;
        }

        /* set the status last as that triggers the waiting thread. */
        SET_STATUS(tag->base_tag.status, rc);

        *payload_start = *payload_end;

        pdebug(DEBUG_DETAIL, "Change detection data read complete.");

        rc = PLCTAG_STATUS_OK;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, handling CIP response!", plc_tag_decode_error(rc));
        SET_STATUS(tag->base_tag.status, rc);
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}




int magic_detect_change_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value)
{
    int res = default_value;
    magic_detect_change_tag_p tag = (magic_detect_change_tag_p)raw_tag;

    pdebug(DEBUG_DETAIL, "Starting for attribute \"%s\".", attrib_name);

    /* assume we have a match. */
    SET_STATUS(tag->base_tag.status, PLCTAG_STATUS_OK);

    /* match the attribute. Raw tags only have size.*/
    if(str_cmp_i(attrib_name, "idle_timeout_ms") == 0) {
        res = plc_get_idle_timeout(tag->plc);
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        SET_STATUS(tag->base_tag.status, PLCTAG_ERR_UNSUPPORTED);
        return default_value;
    }

    pdebug(DEBUG_DETAIL, "Done for attribute \"%s\".", attrib_name);

    return res;
}


int magic_detect_change_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value)
{
    int rc = PLCTAG_STATUS_OK;
    magic_detect_change_tag_p tag = (magic_detect_change_tag_p)raw_tag;

    pdebug(DEBUG_DETAIL, "Starting for attribute \"%s\".", attrib_name);

    if(str_cmp_i(attrib_name, "idle_timeout_ms") == 0) {
        rc = plc_set_idle_timeout(tag->plc, new_value);
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        rc = PLCTAG_ERR_UNSUPPORTED;
        SET_STATUS(tag->base_tag.status, rc);
    }

    pdebug(DEBUG_DETAIL, "Done for attribute \"%s\".", attrib_name);

    return rc;
}

