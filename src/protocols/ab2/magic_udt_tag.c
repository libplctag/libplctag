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
#include <ab2/magic_udt_tag.h>
#include <ab2/cip_plc.h>
#include <util/attr.h>
#include <util/bool.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/plc.h>
#include <util/string.h>


#define UDT_HEADER_SIZE (10)

typedef struct {
    struct plc_tag_t base_tag;

    /* plc and request info */
    plc_p plc;
    struct plc_request_s request;

    uint16_t udt_id;
    uint16_t field_count;
    uint16_t struct_handle;
    uint32_t field_def_size;
    uint32_t instance_size;


    /* count of bytes sent or received. */
    int request_offset;
} magic_udt_tag_t;
typedef magic_udt_tag_t *magic_udt_tag_p;


static void magic_udt_tag_destroy(void *tag_arg);

/* vtable functions */
static int magic_udt_tag_abort(plc_tag_p tag);
static int magic_udt_tag_read(plc_tag_p tag);
static int magic_udt_tag_status(plc_tag_p tag);
static int magic_udt_tag_write(plc_tag_p tag);

static int magic_udt_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value);
static int magic_udt_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value);


/* vtable for raw CIP tags */
static struct tag_vtable_t magic_udt_tag_vtable = {
    magic_udt_tag_abort,
    magic_udt_tag_read,
    magic_udt_tag_status,
    /* magic_udt_tag_tickler */ NULL,
    magic_udt_tag_write,

    /* attribute accessors */
    magic_udt_get_int_attrib,
    magic_udt_set_int_attrib
};


static tag_byte_order_t magic_udt_tag_byte_order = {
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




static int magic_udt_info_build_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);
static int magic_udt_info_handle_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);
static int magic_udt_field_info_build_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);
static int magic_udt_field_info_handle_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);


plc_tag_p magic_udt_tag_create(ab2_plc_type_t plc_type, attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    magic_udt_tag_p tag = NULL;
    const char *name = NULL;
    int id = 0;

    pdebug(DEBUG_INFO, "Starting.");

    tag = (magic_udt_tag_p)base_tag_create(sizeof(*tag), (void (*)(void*))magic_udt_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_WARN, "Unable to allocate new magic UDT tag!");
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

    /* parse the UDT ID */
    if(str_length(name) <= str_length("@udt/")) {
        pdebug(DEBUG_WARN, "Tag name is not long enough to contain the UDT ID!");
        return rc_dec(tag);
    }

    rc = str_to_int(name + str_length("@udt/"), &id);
    if(rc != PLCTAG_STATUS_OK || id < 0 || id > 4095) {
        /* ID can only be 0-4095 */
        pdebug(DEBUG_WARN, "UDT IDs must be from 0 to 4095, inclusive.");
        return rc_dec(tag);
    }

    tag->udt_id = (uint16_t)(unsigned int)id;

    /* get the PLC */

    /* Only supported on Logix-class PLCs. */
    switch(plc_type) {
        case AB2_PLC_LGX:
            /* if it is not set, set it to true/1. */
            attr_set_int(attribs, "forward_open_ex_enabled", attr_get_int(attribs, "forward_open_ex_enabled", 1));
            tag->base_tag.vtable = &magic_udt_tag_vtable;
            tag->base_tag.byte_order = &magic_udt_tag_byte_order;
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


void magic_udt_tag_destroy(void *tag_arg)
{
    magic_udt_tag_p tag = (magic_udt_tag_p)tag_arg;

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



int magic_udt_tag_abort(plc_tag_p tag_arg)
{
    magic_udt_tag_p tag = (magic_udt_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    plc_stop_request(tag->plc, &(tag->request));

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int magic_udt_tag_read(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    magic_udt_tag_p tag = (magic_udt_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* kick off the first part of the process. */
    rc = plc_start_request(tag->plc, &(tag->request), tag, magic_udt_info_build_request_callback, magic_udt_info_handle_response_callback);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to start read request!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int magic_udt_tag_status(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    magic_udt_tag_p tag = (magic_udt_tag_p)tag_arg;

    pdebug(DEBUG_SPEW, "Starting.");

    rc = GET_STATUS(tag->base_tag.status);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}



int magic_udt_tag_write(plc_tag_p tag_arg)
{
    magic_udt_tag_p tag = (magic_udt_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_WARN, "UDT info tag does not support write operations.");

    return PLCTAG_ERR_UNSUPPORTED;
}



int magic_udt_info_build_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    magic_udt_tag_p tag = (magic_udt_tag_p)context;
    int req_off = *payload_start;
    uint8_t raw_payload[] = {
                              CIP_CMD_GET_ATTRIBS,  /* Get Attributes List Service */
                              0x03,  /* path is 3 words, 6 bytes. */
                              0x20,  /* class, 8-bit */
                              0x6C,  /* Template class */
                              0x25,  /* 16-bit instance ID next. */
                              0x00,  /* padding */
                              0x00, 0x00,  /* instance ID bytes, fill in with the UDT ID */
                              0x04, 0x00,  /* get 4 attributes. */
                              0x01, 0x00,  /* Attribute 1: Struct handle.  CRC of field definitions. */
                              0x02, 0x00,  /* Attribute 2: Template Member Count: number of fields in the template/UDT. */
                              0x04, 0x00,  /* Attribute 4: Template Object Definition Size, in 32-bit words. */
                              0x05, 0x00   /* Attribute 5: Template Structure Size, in bytes.   Size of the object. */
                            };
    int raw_payload_size = (int)(unsigned int)sizeof(raw_payload);
    int udt_id_index = 0;

    (void)buffer_capacity;
    (void)req_id;

    pdebug(DEBUG_DETAIL, "Starting.");

    do {

        /* copy the UDT info request payload. */
        for(int index=0; index < raw_payload_size && rc == PLCTAG_STATUS_OK; index++) {
            /* capture the location of the tag ID */
            if(index == 6) {
                udt_id_index = req_off;
            }
            TRY_SET_BYTE(buffer, *payload_end, req_off, raw_payload[index]);
        }
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s while filling in the UDT info request!", plc_tag_decode_error(rc));
            break;
        }

        /* start from the last recorded ID. */
        TRY_SET_U16_LE(buffer, *payload_end, udt_id_index, tag->udt_id);

        /* done! */

        *payload_end = req_off;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build UDT info request, got error %s!", plc_tag_decode_error(rc));
        SET_STATUS(tag->base_tag.status, rc);
        return rc;
    }

    pdebug(DEBUG_DETAIL, "UDT info request packet:");
    pdebug_dump_bytes(DEBUG_DETAIL, buffer + *payload_start, *payload_end - *payload_start);

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}




int magic_udt_info_handle_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    magic_udt_tag_p tag = (magic_udt_tag_p)context;
    int resp_data_size = 0;
    int offset = *payload_start;
    uint8_t service_response;
    uint8_t reserved;
    uint8_t service_status;
    uint8_t status_extra_words;
    uint16_t num_attribs = 0;
    // int response_start = 0;
    bool fatal = FALSE;

    (void)buffer_capacity;

    pdebug(DEBUG_DETAIL, "Starting for %" REQ_ID_FMT ".", req_id);

    do {
        resp_data_size = *payload_end - *payload_start;

        if(resp_data_size < 4) {
            pdebug(DEBUG_WARN, "Unexpectedly small response size!");
            fatal = TRUE;
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

            rc = PLCTAG_ERR_BAD_REPLY;
            fatal = FALSE;
            tag->request_offset = 0;

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

            /* this is an error we should report about the tag, but not kill the PLC over it. */
            fatal = FALSE;
            tag->request_offset = 0;

            break;
        }

        /* how much data do we have? */
        // response_start = offset;
        resp_data_size = *payload_end - offset;

        // /* TODO - move this.
        // make sure that the tag can hold the new payload. */
        // rc = base_tag_resize_data((plc_tag_p)tag, tag->request_offset + resp_data_size);
        // if(rc != PLCTAG_STATUS_OK) {
        //     /* this is fatal! */
        //     pdebug(DEBUG_WARN, "Unable to resize tag data buffer, error %s!", plc_tag_decode_error(rc));
        //     break;
        // }

        /* get the number of attribute results, we want 4. */
        TRY_GET_U16_LE(buffer, *payload_end, offset, num_attribs);
        if(num_attribs != 4) {
            pdebug(DEBUG_WARN, "Unexpected number of attributes.  Expected 4 attributes got %u!", num_attribs);

            /* this is an error we should report about the tag, but not kill the PLC over it. */
            rc = PLCTAG_ERR_BAD_DATA;
            fatal = FALSE;
            tag->request_offset = 0;

            break;
        }

        /* read all 4 attributes, not clear that they will come in order. */
        for(int attrib_num=0; attrib_num < (int)(unsigned int)num_attribs; attrib_num++) {
            uint16_t attrib_id = 0;
            uint16_t attrib_status = 0;

            TRY_GET_U16_LE(buffer, *payload_end, offset, attrib_id);
            TRY_GET_U16_LE(buffer, *payload_end, offset, attrib_status);

            if(attrib_status != 0) {
                pdebug(DEBUG_WARN, "Unable to get attribute ID %x, got error %x!", attrib_id, attrib_status);

                rc = PLCTAG_ERR_BAD_DATA;
                fatal = FALSE;
                tag->request_offset = 0;

                break;
            }

            /* process each attribute.   Each one has an ID, a status and a value. */
            switch(attrib_id) {
                case 0x01:
                    /* CRC of the fields, used as a struct handle. */
                    TRY_GET_U16_LE(buffer, *payload_end, offset, tag->struct_handle);
                    pdebug(DEBUG_DETAIL, "UDT struct handle is %x.", (unsigned int)(tag->struct_handle));
                    break;

                case 0x02:
                    /* number of members in the template */
                    TRY_GET_U16_LE(buffer, *payload_end, offset, tag->field_count);
                    pdebug(DEBUG_DETAIL, "UDT field count is %u.", (unsigned int)(tag->field_count));
                    break;

                case 0x04:
                    /* template definitions size in DINTs */
                    TRY_GET_U32_LE(buffer, *payload_end, offset, tag->field_def_size);
                    pdebug(DEBUG_DETAIL, "UDT definition size is %u DINTS.", (unsigned int)(tag->field_def_size));
                    break;

                case 0x05:
                    /* template instance size in bytes */
                    TRY_GET_U32_LE(buffer, *payload_end, offset, tag->instance_size);
                    pdebug(DEBUG_DETAIL, "UDT instance size in bytes is %u.", (unsigned int)(tag->instance_size));
                    break;

                default:
                    pdebug(DEBUG_WARN, "Unexpected attribute %x found!", (unsigned int)attrib_id);

                    rc = PLCTAG_ERR_BAD_DATA;
                    tag->request_offset = 0;

                    break;
            }
        }

        tag->request_offset = 0;

        pdebug(DEBUG_DETAIL, "Starting new request for UDT field information.");
        rc = plc_start_request(tag->plc, &(tag->request), tag, magic_udt_field_info_build_request_callback, magic_udt_field_info_handle_response_callback);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error queuing up next request!");
            break;
        }

        *payload_start = *payload_end;

        pdebug(DEBUG_DETAIL, "Raw CIP operation complete.");

        rc = PLCTAG_STATUS_OK;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, processing response!", plc_tag_decode_error(rc));
        SET_STATUS(tag->base_tag.status, rc);
        if(fatal == FALSE) {
            return PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_WARN, "Error is fatal, restarting PLC!");
            return rc;
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}





int magic_udt_field_info_build_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    magic_udt_tag_p tag = (magic_udt_tag_p)context;
    int req_off = *payload_start;
    uint8_t raw_payload[] = {
                              CIP_CMD_READ_TEMPLATE,  /* Read Template Service */
                              0x03,  /* path is 3 words, 6 bytes. */
                              0x20,  /* class, 8-bit */
                              0x6C,  /* Template class */
                              0x25,  /* 16-bit instance ID next. */
                              0x00,  /* padding */
                              0x00, 0x00,  /* template/UDT instance ID bytes */
                              0x00, 0x00, 0x00, 0x00, /* byte offset. */
                              0x00, 0x00   /* Total bytes to transfer. */
                            };
    int raw_payload_size = (int)(unsigned int)sizeof(raw_payload);
    int udt_id_index = 0;
    int offset_index = 0;
    int transfer_index = 0;

    (void)buffer_capacity;
    (void)req_id;

    pdebug(DEBUG_DETAIL, "Starting.");

    do {
        /* copy the UDT info request payload. */
        for(int index=0; index < raw_payload_size && rc == PLCTAG_STATUS_OK; index++) {
            /* capture the location of the tag ID */
            if(index == 6) {
                udt_id_index = req_off;
                offset_index = req_off + 2;
                transfer_index = offset_index + 4;
            }
            TRY_SET_BYTE(buffer, *payload_end, req_off, raw_payload[index]);
        }
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s while filling in the UDT info request!", plc_tag_decode_error(rc));
            break;
        }

        /* pull the UDT ID. */
        TRY_SET_U16_LE(buffer, *payload_end, udt_id_index, tag->udt_id);

        /* start with the existing offset. */
        TRY_SET_U32_LE(buffer, *payload_end, offset_index, tag->request_offset);

        /* how many bytes to transfer? */
        TRY_SET_U16_LE(buffer, *payload_end, transfer_index, (tag->field_def_size * 4) - 23); /* formula from the docs. */

        /* done! */

        *payload_end = req_off;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build UDT info request, got error %s!", plc_tag_decode_error(rc));
        SET_STATUS(tag->base_tag.status, rc);
        return rc;
    }

    pdebug(DEBUG_DETAIL, "UDT field info request packet:");
    pdebug_dump_bytes(DEBUG_DETAIL, buffer + *payload_start, *payload_end - *payload_start);

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}




int magic_udt_field_info_handle_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    magic_udt_tag_p tag = (magic_udt_tag_p)context;
    int resp_data_size = 0;
    int offset = *payload_start;
    uint8_t service_response;
    uint8_t reserved;
    uint8_t service_status;
    uint8_t status_extra_words;
    // int response_start = 0;
    bool fatal = FALSE;

    (void)buffer_capacity;

    pdebug(DEBUG_DETAIL, "Starting for %" REQ_ID_FMT ".", req_id);

    do {
        resp_data_size = *payload_end - *payload_start;

        if(resp_data_size < 4) {
            pdebug(DEBUG_WARN, "Unexpectedly small response size!");
            fatal = TRUE;
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        TRY_GET_BYTE(buffer, *payload_end, offset, service_response);
        TRY_GET_BYTE(buffer, *payload_end, offset, reserved);
        TRY_GET_BYTE(buffer, *payload_end, offset, service_status);
        TRY_GET_BYTE(buffer, *payload_end, offset, status_extra_words);

        /* ignore reserved byte. */
        (void)reserved;

        if(service_response != (CIP_CMD_OK | CIP_CMD_READ_TEMPLATE)) {
            pdebug(DEBUG_WARN, "Unexpected CIP service response type %" PRIu8 "!", service_response);

            rc = PLCTAG_ERR_BAD_REPLY;
            fatal = FALSE;
            tag->request_offset = 0;

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

            /* this is an error we should report about the tag, but not kill the PLC over it. */
            fatal = FALSE;
            tag->request_offset = 0;

            break;
        }

        /* how much data do we have? */
        // response_start = offset;
        resp_data_size = *payload_end - offset;

        /* we are going to copy the data into the tag buffer almost exactly as is. */

        /* the format in memory will be:
         *
         * A new header:
         *
         * uint16_t - UDT ID
         * uint16_t - number of members (including invisible ones)
         * uint16_t - struct handle/CRC of field defs.
         * uint32_t - instance size in bytes.
         *
         * Then the raw field info.
         *
         * N x field info entries
         *     uint16_t field_data - array element count or bit field number
         *     uint16_t field_type
         *     uint32_t field_offset
         *
         * int8_t string - zero-terminated string, UDT name, but name stops at first semicolon!
         *
         * N x field names
         *     int8_t string - zero-terminated.
         *
         */

        /* resize the tag buffer for this and some additional fields. */

        rc = base_tag_resize_data((plc_tag_p)tag, tag->request_offset + resp_data_size + UDT_HEADER_SIZE);
        if(rc != PLCTAG_STATUS_OK) {
            /* this is fatal! */
            pdebug(DEBUG_WARN, "Unable to resize tag data buffer, error %s!", plc_tag_decode_error(rc));
            break;
        }

        /* copy the data. */

        /* if there is no data copied yet, copy the header */
        if(tag->request_offset == 0) {
            /* copy the UDT ID */
            tag->base_tag.data[0] = (uint8_t)(tag->udt_id & 0xFF);
            tag->base_tag.data[1] = (uint8_t)((tag->udt_id >> 8) & 0xFF);

            /* number of members */
            tag->base_tag.data[2] = (uint8_t)(tag->field_count & 0xFF);
            tag->base_tag.data[3] = (uint8_t)((tag->field_count >> 8) & 0xFF);

            /* struct handle */
            tag->base_tag.data[4] = (uint8_t)(tag->struct_handle & 0xFF);
            tag->base_tag.data[5] = (uint8_t)((tag->struct_handle >> 8) & 0xFF);

            /* instance size in bytes. */
            tag->base_tag.data[6] = (uint8_t)(tag->instance_size & 0xFF);
            tag->base_tag.data[7] = (uint8_t)((tag->instance_size >> 8) & 0xFF);
            tag->base_tag.data[8] = (uint8_t)((tag->instance_size >> 16) & 0xFF);
            tag->base_tag.data[9] = (uint8_t)((tag->instance_size >> 24) & 0xFF);
        }

        /* copy the remaining data. */
        for(int index = 0; index < resp_data_size && rc == PLCTAG_STATUS_OK; index++) {
            TRY_GET_BYTE(buffer, *payload_end, offset, tag->base_tag.data[UDT_HEADER_SIZE + tag->request_offset + index]);
        }
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s while copying data into tag data buffer!", plc_tag_decode_error(rc));
            break;
        }

        pdebug(DEBUG_DETAIL, "Built result tag data:");
        pdebug_dump_bytes(DEBUG_DETAIL, tag->base_tag.data, tag->base_tag.size);

        if(service_status == CIP_STATUS_FRAG) {
            tag->request_offset += resp_data_size;

            pdebug(DEBUG_DETAIL, "Starting new request for more UDT field information.");
            rc = plc_start_request(tag->plc, &(tag->request), tag, magic_udt_field_info_build_request_callback, magic_udt_field_info_handle_response_callback);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error queuing up next request!");
                break;
            }
        } else {
            /* done! */
            pdebug(DEBUG_DETAIL, "Read of UDT field data complete.");

            tag->request_offset = 0;
            rc = PLCTAG_STATUS_OK;

            /* clear any in-flight flags. */
            critical_block(tag->base_tag.api_mutex) {
                tag->base_tag.read_complete = 0;
                tag->base_tag.read_in_flight = 0;
            }

            /* set the status last as that triggers the waiting thread. */
            SET_STATUS(tag->base_tag.status, rc);
        }

        *payload_start = *payload_end;

        pdebug(DEBUG_DETAIL, "Raw CIP operation complete.");

        rc = PLCTAG_STATUS_OK;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, processing response!", plc_tag_decode_error(rc));
        SET_STATUS(tag->base_tag.status, rc);
        if(fatal == FALSE) {
            return PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_WARN, "Error is fatal, restarting PLC!");
            return rc;
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}






int magic_udt_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value)
{
    int res = default_value;
    magic_udt_tag_p tag = (magic_udt_tag_p)raw_tag;

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


int magic_udt_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value)
{
    int rc = PLCTAG_STATUS_OK;
    magic_udt_tag_p tag = (magic_udt_tag_p)raw_tag;

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

