/***************************************************************************
 *   Copyright (C) 2022 by Kyle Hayes                                      *
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
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <platform.h>
#include <lib/libplctag.h>
#include <common_protocol/common_protocol.h>
#include <common_protocol/slice.h>
#include <ab/cip.h>
#include <ab/defs.h>
#include <ab/micro800/micro800.h>
#include <util/atomic_int.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/rc.h>

/* data definitions */
#define MAX_MICRO800_REQUESTS (100)
#define MICRO800_CONNECT_TIMEOUT_MS (100)
#define MICRO800_INACTIVITY_TIMEOUT_MS (5000)
#define MICRO800_INITIAL_DATA_SIZE (600)

typedef enum {
    MICRO800_STATE_SEND_EIP_SESSION_PDU = 0,
    MICRO800_STATE_OPEN_SOCKET_STARTED,
    MICRO800_STATE_CONNECT_COMPLETE
} connect_state_t;


typedef struct micro800_tag_t *micro800_tag_p;

struct micro800_plc_t {
    COMMON_DEVICE_FIELDS;

    /* connection state for connecting state machine */
    int state;

    /* connection info, if used. */
    uint8_t *connection_path;
    uint8_t connection_path_size;

};

typedef struct micro800_plc_t *micro800_plc_p;


struct micro800_tag_t {
    /* common tag parts. */
    COMMON_TAG_FIELDS;

    /* data for the tag. */
    int elem_count;
    int elem_size;
};


/* default string types used for Modbus PLCs. */
tag_byte_order_t micro800_tag_byte_order = {
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

    .str_count_word_bytes = 1,
    .str_max_capacity = 255,
    .str_total_length = 0,
    .str_pad_bytes = 0
};


/* helper functions */
static int create_tag_object(attr attribs, micro800_tag_p *tag);
static common_device_p create_device_unsafe(void *arg);
static void tag_destructor(void *tag_arg);
static void device_destructor(void *plc_arg);

static int tickle_device(common_device_p device);
static int connect_device(common_device_p device);
static int disconnect_device(common_device_p device);
static int process_pdu(common_device_p device);

struct common_protocol_vtable_t micro800_device_vtable = {
    .tickle = tickle_device,
    .connect = connect_device,
    .disconnect = disconnect_device,
    .process_pdu = process_pdu
};


/* data accessors */
static int micro800_get_int_attrib(plc_tag_p tag, const char *attrib_name, int default_value);
static int micro800_set_int_attrib(plc_tag_p tag, const char *attrib_name, int new_value);

struct tag_vtable_t micro800_tag_vtable = {
    /* reuse the common versions */
    .abort = common_tag_abort,
    .read = common_tag_read_start,
    .status = common_tag_status,
    .wake_plc = common_tag_wake_device,
    .write = common_tag_write_start,

    .get_int_attrib = micro800_get_int_attrib,
    .set_int_attrib = micro800_set_int_attrib
};

/* PDU functions */
static int check_read_response(common_tag_p tag);
static int create_read_request(common_tag_p tag);
static int check_write_response(common_tag_p tag);
static int create_write_request(common_tag_p tag);

struct common_tag_pdu_vtable_t micro800_tag_pdu_vtable = {
    .check_read_response = check_read_response,
    .create_read_request = create_read_request,
    .check_write_response = check_write_response,
    .create_write_request = create_write_request
} ;



/****** main entry point *******/

plc_tag_p micro800_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata), void *userdata)
{
    int rc = PLCTAG_STATUS_OK;
    micro800_tag_p tag = NULL;
    micro800_plc_p device = NULL;
    int connection_group_id = attr_get_int(attribs, "connection_group_id", 0);
    const char *server_name = attr_get_str(attribs, "gateway", NULL);

    pdebug(DEBUG_INFO, "Starting.");

    if(!server_name || str_length(server_name) == 0) {
        pdebug(DEBUG_WARN, "Gateway attribute must not be missing or zero length!");
        return NULL;
    }

    /* create the tag object. */
    rc = create_tag_object(attribs, &tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create new tag!  Error %s!", plc_tag_decode_error(rc));
        return NULL;
    }

    /* set up the common parts. */
    rc = common_tag_init((common_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to initialize generic tag parts!");
        rc_dec(tag);
        return (plc_tag_p)NULL;
    }

    /* find the PLC object. */
    rc = common_protocol_get_device(server_name, AB_EIP_DEFAULT_PORT, connection_group_id, &device, create_device_unsafe, (void *)attribs);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create new tag!  Error %s!", plc_tag_decode_error(rc));
        tag->status = (int8_t)rc;
    }

    /* kick off a read. */
    common_tag_read_start((plc_tag_p)tag);

    pdebug(DEBUG_INFO, "Done.");

    return (plc_tag_p)tag;
}




/***** helper functions *****/

int create_tag_object(attr attribs, micro800_tag_p *tag)
{
    int rc = PLCTAG_STATUS_OK;
    int data_size = 0;
    int reg_size = 0;
    int elem_count = attr_get_int(attribs, "elem_count", 1);

    pdebug(DEBUG_DETAIL, "Starting.");

    if (elem_count < 0) {
        pdebug(DEBUG_WARN, "Element count should not be a negative value!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    pdebug(DEBUG_INFO, "Starting.");

    *tag = NULL;

    /* allocate the tag */
    *tag = (micro800_tag_p)rc_alloc((int)(unsigned int)sizeof(struct micro800_tag_t)+data_size, tag_destructor);
    if(! *tag) {
        pdebug(DEBUG_WARN, "Unable to allocate Modbus tag!");
        return PLCTAG_ERR_NO_MEM;
    }

    (*tag)->elem_count = elem_count;
    (*tag)->elem_size = 0;
    (*tag)->size = 0;
    (*tag)->data = NULL;

    /* set up the vtable */
    (*tag)->vtable = &micro800_tag_vtable;

    /* set the default byte order */
    (*tag)->byte_order = &micro800_tag_byte_order;

    /* make sure the generic tag tickler thread does not call the generic tickler. */
    (*tag)->skip_tickler = 1;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}




void tag_destructor(void *tag_arg)
{
    micro800_tag_p tag = (micro800_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Destructor called with null pointer!");
        return;
    }

    /* abort everything. */
    common_tag_abort((plc_tag_p)tag);

    /* clean up common elements */
    common_tag_destroy((common_tag_p)tag);

    if(tag->data) {
        mem_free(tag->data);
        tag->data = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");
}



/* must be called with device module mutex held */
common_device_p create_device_unsafe(void *arg)
{
    int rc = PLCTAG_STATUS_OK;
    attr attribs = (attr)arg;
    micro800_plc_p result = NULL;
    const char *path = attr_get_str(attribs, "path", NULL);
    int needs_connection = 0;
    plc_type_t plc_type = AB_PLC_MICRO800;
    int is_dhp = 0;
    uint16_t dhp_dest = 0;

    pdebug(DEBUG_INFO, "Starting.");

    /* see if we can find a matching server. */
    result = (micro800_plc_p)rc_alloc((int)(unsigned int)sizeof(struct micro800_plc_t), device_destructor);
    if(!result) {
        pdebug(DEBUG_WARN, "Unable to allocate Micro800 PLC device object!");
        return NULL;
    } 

    /* set up device vtable.  Thread won't start until after this function is called. */
    result->protocol = &micro800_device_vtable;

    /* set up initial data buffer. */
    result->data = mem_alloc(MICRO800_INITIAL_DATA_SIZE);
    if(!result->data) {
        pdebug(DEBUG_WARN, "Unable to allocate Micro800 initial data buffer!");
        rc_dec(result);
        return NULL;
    }

    result->buffer_size = MICRO800_INITIAL_DATA_SIZE;

    /* encode path */
    if(path && str_length(path) > 0) {
        rc = cip_encode_path(path, &needs_connection, plc_type, &result->connection_path, &result->connection_path_size, &is_dhp, &dhp_dest);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_DETAIL, "Error %s parsing path for Micro800!", plc_tag_decode_error(rc));
            rc_dec(result);
            return NULL;
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return (common_device_p)result;
}


/* never enter this from within the handler thread itself! */
void device_destructor(void *device_arg)
{
    micro800_plc_p device = (micro800_plc_p)device_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!device) {
        pdebug(DEBUG_WARN, "Destructor called with null pointer!");
        return;
    }

    /* call common destructor */
    common_device_destroy((common_device_p)device);

    if(device->connection_path) {
        mem_free(device->connection_path);
        device->connection_path = NULL;
    }

    if(device->data) {
        mem_free(device->data);
        device->data = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");
}


void wake_device(common_device_p device_arg)
{
    micro800_plc_p device = (micro800_plc_p)device_arg;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(device) {
        if(device->sock) {
            socket_wake(device->sock);
        } else {
            pdebug(DEBUG_DETAIL, "PLC socket pointer is NULL.");
        }
    } else {
        pdebug(DEBUG_WARN, "PLC pointer is NULL!");
    }

    pdebug(DEBUG_DETAIL, "Done.");
}



int connect_device(common_device_p device_arg)
{
    int rc = PLCTAG_STATUS_OK;
    micro800_plc_p device = (micro800_plc_p)device_arg;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* set the state to start connecting. */
    device->state = MICRO800_STATE_SEND_EIP_SESSION_PDU;

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



int tickle_device(common_device_p common_device)
{
    int rc = PLCTAG_STATUS_OK;
    micro800_plc_p device = (micro800_plc_p)common_device;
    slice_t header;
    slice_t session_request;

    pdebug(DEBUG_SPEW, "Starting.");

    switch(device->state) {
        case MICRO800_STATE_SEND_EIP_SESSION_PDU:
            pdebug(DEBUG_DETAIL, "In state MICRO800_STATE_SEND_EIP_SESSION_PDU.");

            

        default:
            pdebug(DEBUG_WARN, "Unknown state %d!", device->state);
            return PLCTAG_ERR_UNSUPPORTED;
            break;
    }

    pdebug(DEBUG_DETAIL, "Done with status %s.", plc_tag_decode_error(rc));

    return rc;
}





/****** Data Accessor Functions ******/

int micro800_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value)
{
    int res = default_value;
    micro800_tag_p tag = (micro800_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    tag->status = PLCTAG_STATUS_OK;

    /* match the attribute. */
    if(str_cmp_i(attrib_name, "elem_size") == 0) {
        res = (tag->elem_size + 7)/8; /* return size in bytes! */
    } else if(str_cmp_i(attrib_name, "elem_count") == 0) {
        res = tag->elem_count;
    } else {
        pdebug(DEBUG_WARN,"Attribute \"%s\" is not supported.", attrib_name);
        tag->status = PLCTAG_ERR_UNSUPPORTED;
    }

    return res;
}


int micro800_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value)
{
    (void)attrib_name;
    (void)new_value;

    pdebug(DEBUG_WARN, "Attribute \"%s\" is unsupported!", attrib_name);

    raw_tag->status = PLCTAG_ERR_UNSUPPORTED;

    return PLCTAG_ERR_UNSUPPORTED;
}


