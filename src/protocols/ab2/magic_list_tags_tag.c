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
#include <ab2/magic_list_tags_tag.h>
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

    /* request prefix, program name */
    char *prefix;

    /* count of bytes sent or received. */
    int request_offset;

    /* the last tag ID we saw. */
    uint32_t last_tag_id;
} magic_list_tags_tag_t;
typedef magic_list_tags_tag_t *magic_list_tags_tag_p;


static void magic_list_tags_tag_destroy(void *tag_arg);
static int encode_request_prefix(const char *name, uint8_t *buffer, int *encoded_size);


/* vtable functions */
static int magic_list_tags_tag_abort(plc_tag_p tag);
static int magic_list_tags_tag_read(plc_tag_p tag);
static int magic_list_tags_tag_status(plc_tag_p tag);
static int magic_list_tags_tag_write(plc_tag_p tag);

static int magic_list_tags_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value);
static int magic_list_tags_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value);


/* vtable for raw CIP tags */
static struct tag_vtable_t magic_list_tags_tag_vtable = {
    magic_list_tags_tag_abort,
    magic_list_tags_tag_read,
    magic_list_tags_tag_status,
    /* magic_list_tags_tag_tickler */ NULL,
    magic_list_tags_tag_write,

    /* attribute accessors */
    magic_list_tags_get_int_attrib,
    magic_list_tags_set_int_attrib
};


static tag_byte_order_t magic_list_tags_tag_byte_order = {
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




static int magic_list_tags_build_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);
static int magic_list_tags_handle_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id);


plc_tag_p magic_list_tags_tag_create(ab2_plc_type_t plc_type, attr attribs)
{
    magic_list_tags_tag_p tag = NULL;
    const char *name = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    tag = (magic_list_tags_tag_p)base_tag_create(sizeof(*tag), (void (*)(void*))magic_list_tags_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_WARN, "Unable to allocate new magic tag listing tag!");
        return NULL;
    }

    tag->base_tag.is_bit = 0;
    tag->base_tag.bit = 0;
    tag->base_tag.data = NULL;
    tag->base_tag.size = 0;

    tag->last_tag_id = 0;

    /* check the name. */
    name = attr_get_str(attribs, "name", NULL);
    if(name == NULL || str_length(name) == 0) {
        pdebug(DEBUG_WARN, "Tag name must not be missing or zero length!");
        return rc_dec(tag);
    }

    /* is there a prefix? */
    if(str_cmp_i_n("program:", name, str_length("program:")) == 0) {
        int prefix_size = 0;

        /* there is a prefix, copy it. */

        while(name[prefix_size] != '.' && name[prefix_size] != 0) {
            prefix_size++;
        }

        if(name[prefix_size] == 0) {
            pdebug(DEBUG_WARN, "Malformed program prefix, \"%s\", found while trying to create tag listing tag!");
            return rc_dec(tag);
        }

        tag->prefix = mem_alloc(prefix_size+1); /* +1 for zero termination */
        if(tag->prefix == NULL) {
            pdebug(DEBUG_WARN, "Unable to allocate prefix!");
            return rc_dec(tag);
        }

        /* copy the prefix */
        for(int index=0; index < prefix_size; index++) {
            tag->prefix[index] = name[index];
        }
    } else {
        tag->prefix = NULL;
    }

    /* get the PLC */

    /* Only supported on Logix-class PLCs. */
    switch(plc_type) {
        case AB2_PLC_LGX:
            /* if it is not set, set it to true/1. */
            attr_set_int(attribs, "cip_payload", attr_get_int(attribs, "cip_payload", CIP_STD_EX_PAYLOAD));
            tag->base_tag.vtable = &magic_list_tags_tag_vtable;
            tag->base_tag.byte_order = &magic_list_tags_tag_byte_order;
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


void magic_list_tags_tag_destroy(void *tag_arg)
{
    magic_list_tags_tag_p tag = (magic_list_tags_tag_p)tag_arg;

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

    /* get rid of any tag prefix. */
    if(tag->prefix) {
        mem_free(tag->prefix);
        tag->prefix = NULL;
    }

    /* delete the base tag parts. */
    base_tag_destroy((plc_tag_p)tag);

    pdebug(DEBUG_INFO, "Done.");
}



int magic_list_tags_tag_abort(plc_tag_p tag_arg)
{
    magic_list_tags_tag_p tag = (magic_list_tags_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    plc_stop_request(tag->plc, &(tag->request));

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int magic_list_tags_tag_read(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    magic_list_tags_tag_p tag = (magic_list_tags_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_start_request(tag->plc, &(tag->request), tag, magic_list_tags_build_request_callback, magic_list_tags_handle_response_callback);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to start read request!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int magic_list_tags_tag_status(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    magic_list_tags_tag_p tag = (magic_list_tags_tag_p)tag_arg;

    pdebug(DEBUG_SPEW, "Starting.");

    rc = GET_STATUS(tag->base_tag.status);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}



int magic_list_tags_tag_write(plc_tag_p tag_arg)
{
    magic_list_tags_tag_p tag = (magic_list_tags_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_WARN, "Tag listing tag does not support write operations.");

    return PLCTAG_ERR_UNSUPPORTED;
}



int magic_list_tags_build_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    magic_list_tags_tag_p tag = (magic_list_tags_tag_p)context;
    int req_off = *payload_start;
    // int max_trans_size = 0;
    // int trans_size = 0;
    uint8_t encoded_prefix[256 + 3] = {0}; /* FIXME MAGIC */
    int encoded_prefix_size = 0;
    uint8_t raw_payload[] = { 0x20,
                              0x6b,
                              0x25,
                              0x00,
                              0x00, /* tag entry ID byte 0 */
                              0x00, /* tag entry ID byte 1 */
                              0x04, /* get 4 attributes */
                              0x00,
                              0x02, /* symbol type */
                              0x00,
                              0x07, /* element length */
                              0x00,
                              0x08, /* array dimensions */
                              0x00,
                              0x01, /* symbol name string */
                              0x00 };
    int tag_id_index = 0;

    (void)buffer_capacity;
    (void)req_id;

    pdebug(DEBUG_DETAIL, "Starting.");


    do {
        if(tag->prefix && str_length(tag->prefix) > 0) {
            rc = encode_request_prefix(tag->prefix, &encoded_prefix[0], &encoded_prefix_size);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to encode program prefix \"%s\", error %s!", tag->prefix, plc_tag_decode_error(rc));
                break;
            }
        }

        /* set the CIP command */
        TRY_SET_BYTE(buffer, *payload_end, req_off, CIP_CMD_LIST_TAGS);

        /* set the path length. */
        TRY_SET_BYTE(buffer, *payload_end, req_off, (uint8_t)(unsigned int)((6 + encoded_prefix_size)/2)); /* MAGIC - 6 = raw payload/request path */

        /* copy the encoded prefix, if any */
        for(int index=0; index < encoded_prefix_size  && rc == PLCTAG_STATUS_OK; index++) {
            TRY_SET_BYTE(buffer, *payload_end, req_off, encoded_prefix[index]);
        }
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s while copying the encoded prefix into the request!", plc_tag_decode_error(rc));
            break;
        }

        /* copy the tag list request payload. */
        for(int index=0; index < (int)(unsigned int)(sizeof(raw_payload)) && rc == PLCTAG_STATUS_OK; index++) {
            /* capture the location of the tag ID */
            if(index == 4) {
                tag_id_index = req_off;
            }
            TRY_SET_BYTE(buffer, *payload_end, req_off, raw_payload[index]);
        }
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s while filling in the tag listing CIP request!", plc_tag_decode_error(rc));
            break;
        }

        /* start from the last recorded ID. */
        TRY_SET_U16_LE(buffer, *payload_end, tag_id_index, tag->last_tag_id);

        /* done! */

        *payload_end = req_off;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build raw CIP request, got error %s!", plc_tag_decode_error(rc));
        SET_STATUS(tag->base_tag.status, rc);
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Tag listing request packet:");
    pdebug_dump_bytes(DEBUG_DETAIL, buffer + *payload_start, *payload_end - *payload_start);

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}




int magic_list_tags_handle_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    magic_list_tags_tag_p tag = (magic_list_tags_tag_p)context;
    int resp_data_size = 0;
    int offset = *payload_start;
    uint8_t service_response;
    uint8_t reserved;
    uint8_t service_status;
    uint8_t status_extra_words;
    int response_start = 0;

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

        if(service_response != (CIP_CMD_OK | CIP_CMD_LIST_TAGS)) {
            pdebug(DEBUG_WARN, "Unexpected CIP service response type %" PRIu8 "!", service_response);

            /* this is an error we should report about the tag, but not kill the PLC over it. */
            SET_STATUS(tag->base_tag.status, PLCTAG_ERR_BAD_REPLY);
            tag->request_offset = 0;
            rc = PLCTAG_STATUS_OK;
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
            SET_STATUS(tag->base_tag.status, rc);
            tag->request_offset = 0;
            rc = PLCTAG_STATUS_OK;

            break;
        }

        /* how much data do we have? */
        response_start = offset;
        resp_data_size = *payload_end - offset;

        /* make sure that the tag can hold the new payload. */
        rc = base_tag_resize_data((plc_tag_p)tag, tag->request_offset + resp_data_size);
        if(rc != PLCTAG_STATUS_OK) {
            /* this is fatal! */
            pdebug(DEBUG_WARN, "Unable to resize tag data buffer, error %s!", plc_tag_decode_error(rc));
            break;
        }

        /* cycle through the data to get the last tag ID */
        while(offset < *payload_end) {
            uint32_t instance_id = 0;
            uint16_t string_len = 0;
            int cursor_index = offset;

            /* each entry looks like this:
                uint32_t instance_id    monotonically increasing but not contiguous
                uint16_t symbol_type    type of the symbol.
                uint16_t element_length length of one array element in bytes.
                uint32_t array_dims[3]  array dimensions.
                uint16_t string_len     string length count.
                uint8_t string_data[]   string bytes (string_len of them)
            */

            TRY_GET_U32_LE(buffer, *payload_end, cursor_index, instance_id);

            /* skip the fields we do not care about. */
            cursor_index += 2 + 2 + 12;

            TRY_GET_U16_LE(buffer, *payload_end, cursor_index, string_len);

            pdebug(DEBUG_DETAIL, "tag id %u.", (unsigned int)instance_id);
            tag->last_tag_id = instance_id + 1;

            /* skip past the name. */
            offset += 4 + 2 + 2 + 12 + 2 + (int)(unsigned int)string_len;
        }

        /* copy the tag listing data into the tag. */
        for(int index=0; index < resp_data_size; index++) {
            TRY_GET_BYTE(buffer, *payload_end, response_start, tag->base_tag.data[tag->request_offset + index]);
        }
        if(rc != PLCTAG_STATUS_OK) {
            /* this is fatal! */
            pdebug(DEBUG_WARN, "Unable to copy data into the tag data buffer, error %s!", plc_tag_decode_error(rc));
            break;
        }

        if(service_status == CIP_STATUS_FRAG) {
            tag->request_offset += resp_data_size;

            pdebug(DEBUG_DETAIL, "Starting new tag listing request for remaining data.");
            rc = plc_start_request(tag->plc, &(tag->request), tag, magic_list_tags_build_request_callback, magic_list_tags_handle_response_callback);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error queuing up next request!");
                break;
            }
        } else {
            /* done! */
            pdebug(DEBUG_DETAIL, "Read of tag listing data complete.");

            tag->request_offset = 0;
            rc = PLCTAG_STATUS_OK;

            /* clear any in-flight flags. */
            critical_block(tag->base_tag.api_mutex) {
                tag->base_tag.read_complete = 1;
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
        pdebug(DEBUG_WARN, "Error, %s, handling CIP response!", plc_tag_decode_error(rc));
        SET_STATUS(tag->base_tag.status, rc);
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}




int magic_list_tags_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value)
{
    int res = default_value;
    magic_list_tags_tag_p tag = (magic_list_tags_tag_p)raw_tag;

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


int magic_list_tags_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value)
{
    int rc = PLCTAG_STATUS_OK;
    magic_list_tags_tag_p tag = (magic_list_tags_tag_p)raw_tag;

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



int encode_request_prefix(const char *name, uint8_t *buffer, int *encoded_size)
{
    int symbol_length_index = 0;
    int name_len = str_length(name);

    /* start with the symbolic type identifier */
    *encoded_size = 0;
    buffer[*encoded_size] = (uint8_t)(unsigned int)0x91; (*encoded_size)++;

    /* dummy value for the encoded length */
    symbol_length_index = *encoded_size;
    buffer[*encoded_size] = (uint8_t)(unsigned int)0; (*encoded_size)++;

    /* copy the string. */
    for(int index=0; index < name_len; index++) {
        buffer[*encoded_size] = (uint8_t)name[index];
        (*encoded_size)++;
    }

    /* backfill the encoded size */
    buffer[symbol_length_index] = (uint8_t)(unsigned int)name_len;

    /* make sure we have an even number of bytes. */
    if((*encoded_size) & 0x01) {
        buffer[*encoded_size] = 0;
        (*encoded_size)++;
    }

    return PLCTAG_STATUS_OK;
}




