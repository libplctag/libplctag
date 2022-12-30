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
    int connect_state;

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



int tickle_device(common_device_p device)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    switch(device->) {
        case MICRO800_STATE_SEND_EIP_SESSION_PDU:
            pdebug(DEBUG_DETAIL, "In state MICRO800_STATE_SEND_EIP_SESSION_PDU.");

            rc = eip_build_

        default:
            pdebug(DEBUG_WARN, "Unknown connection state %d!", plc->connect_state);
            return PLCTAG_ERR_UNSUPPORTED;
            break;
    }

    pdebug(DEBUG_DETAIL, "Done with status %s.", plc_tag_decode_error(rc));

    return rc;
}



int open_socket(micro800_plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    char **server_port = NULL;
    char *server = NULL;
    int port = AB_EIP_DEFAULT_PORT;

    pdebug(DEBUG_DETAIL, "Starting for Micro800 PLC %s.", plc->server_name);

    server_port = str_split(plc->server_name, ":");
    if(!server_port) {
        pdebug(DEBUG_WARN, "Unable to split server and port string!");
        return PLCTAG_ERR_BAD_CONFIG;
    }

    if(server_port[0] == NULL) {
        pdebug(DEBUG_WARN, "Server string is malformed or empty!");
        mem_free(server_port);
        return PLCTAG_ERR_BAD_CONFIG;
    } else {
        server = server_port[0];
    }

    if(server_port[1] != NULL) {
        rc = str_to_int(server_port[1], &port);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to extract port number from server string \"%s\"!", plc->server);
            mem_free(server_port);
            server_port = NULL;
            return PLCTAG_ERR_BAD_CONFIG;
        }
    } else {
        port = AB_EIP_DEFAULT_PORT;
    }

    pdebug(DEBUG_DETAIL, "Using server \"%s\" and port %d.", server, port);

    rc = socket_create(&(plc->sock));
    if(rc != PLCTAG_STATUS_OK) {
        /* done with the split string. */
        mem_free(server_port);
        server_port = NULL;

        pdebug(DEBUG_WARN, "Unable to create socket object, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* connect to the socket */
    pdebug(DEBUG_DETAIL, "Connecting to %s on port %d...", server, port);
    rc = socket_connect_tcp_start(plc->sock, server, port);
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        /* done with the split string. */
        mem_free(server_port);

        pdebug(DEBUG_WARN, "Unable to connect to the server \"%s\", got error %s!", plc->server_name, plc_tag_decode_error(rc));
        socket_destroy(&(plc->sock));
        return rc;
    }

    /* done with the split string. */
    if(server_port) {
        mem_free(server_port);
        server_port = NULL;
    }

    pdebug(DEBUG_DETAIL, "Done for Micro800 PLC %s.", plc->server_name);

    return rc;
}



int tickle_all_tags(micro800_plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    struct micro800_tag_list_t idle_list = { NULL, NULL };
    struct micro800_tag_list_t active_list = { NULL, NULL };
    micro800_tag_p tag = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    /*
     * The mutex prevents the list from changing, the PLC
     * from being freed, and the tags from being freed.
     */
    critical_block(plc->mutex) {
        while((tag = pop_tag(&(plc->tag_list)))) {
            int tag_pushed = 0;

            debug_set_tag_id(tag->tag_id);

            /* make sure nothing else can modify the tag while we are */
            critical_block(tag->api_mutex) {
            // if(mutex_try_lock(tag->api_mutex) == PLCTAG_STATUS_OK) {
                rc = tickle_tag(plc, tag);
                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_SPEW, "Pushing tag onto idle list.");
                    push_tag(&idle_list, tag);
                    tag_pushed = 1;
                } else if(rc == PLCTAG_STATUS_PENDING) {
                    pdebug(DEBUG_SPEW, "Pushing tag onto active list.");
                    push_tag(&active_list, tag);
                    tag_pushed = 1;
                } else {
                    pdebug(DEBUG_WARN, "Error %s tickling tag! Pushing tag onto idle list.", plc_tag_decode_error(rc));
                    push_tag(&idle_list, tag);
                    tag_pushed = 1;
                }
            }

            if(tag_pushed) {
                /* call the callbacks outside the API mutex. */
                plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);
            } else {
                pdebug(DEBUG_WARN, "Tag mutex not taken!  Doing emergency push of tag onto idle list.");
                push_tag(&idle_list, tag);
            }

            rc = PLCTAG_STATUS_OK;

            debug_set_tag_id(0);
        }

        /* merge the lists and replace the old list. */
        pdebug(DEBUG_SPEW, "Merging active and idle lists.");
        plc->tag_list = merge_lists(&active_list, &idle_list);
    }

    pdebug(DEBUG_DETAIL, "Done: %s", plc_tag_decode_error(rc));

    return rc;
}



int tickle_tag(micro800_plc_p plc, micro800_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    int op = (int)(tag->op);
    int raise_event = 0;
    int event;
    int event_status;

    pdebug(DEBUG_SPEW, "Starting.");

    switch(op) {
        case TAG_OP_IDLE:
            pdebug(DEBUG_SPEW, "Tag is idle.");
            rc = PLCTAG_STATUS_OK;
            break;

        case TAG_OP_READ_REQUEST:
            /* if the PLC is ready and there is no request queued yet, build a request. */
            if(find_request_slot(plc, tag) == PLCTAG_STATUS_OK) {
                rc = create_read_request(plc, tag);
                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_DETAIL, "Read request created.");

                    tag->op = TAG_OP_READ_RESPONSE;
                    plc->flags.request_ready = 1;

                    rc = PLCTAG_STATUS_PENDING;
                } else {
                    pdebug(DEBUG_WARN, "Error %s creating read request!", plc_tag_decode_error(rc));

                    /* remove the tag from the request slot. */
                    micro800_clear_request_slot(plc, tag);

                    tag->op = TAG_OP_IDLE;
                    tag->read_complete = 1;
                    tag->read_in_flight = 0;
                    tag->status = (int8_t)rc;

                    raise_event = 1;
                    event = PLCTAG_EVENT_READ_COMPLETED;
                    event_status = rc;

                    //plc_tag_tickler_wake();
                    plc_tag_generic_wake_tag((plc_tag_p)tag);
                    rc = PLCTAG_STATUS_OK;
                }
            } else {
                pdebug(DEBUG_SPEW, "Request already in flight or PLC not ready, waiting for next chance.");
                rc = PLCTAG_STATUS_PENDING;
            }
            break;

        case TAG_OP_READ_RESPONSE:
            /* cross check the state. */
            if(plc->state == PLC_CONNECT_START
            || plc->state == PLC_CONNECT_WAIT
            || plc->state == PLC_ERR_WAIT) {
                pdebug(DEBUG_WARN, "PLC changed state, restarting request.");
                tag->op = TAG_OP_READ_REQUEST;
                break;
            }

            if(plc->flags.response_ready) {
                rc = check_read_response(plc, tag);
                switch(rc) {
                    case PLCTAG_ERR_PARTIAL:
                        /* partial response, keep going */
                        pdebug(DEBUG_DETAIL, "Found our response, but we are not done.");

                        /* remove the tag from the request slot. */
                        micro800_clear_request_slot(plc, tag);

                        plc->flags.response_ready = 0;
                        tag->op = TAG_OP_READ_REQUEST;

                        rc = PLCTAG_STATUS_OK;
                        break;

                    case PLCTAG_ERR_NO_MATCH:
                        pdebug(DEBUG_SPEW, "Not our response.");
                        rc = PLCTAG_STATUS_PENDING;
                        break;

                    case PLCTAG_STATUS_OK:
                        /* fall through */
                    default:
                        /* set the status before we might change it. */
                        tag->status = (int8_t)rc;

                        if(rc == PLCTAG_STATUS_OK) {
                            pdebug(DEBUG_DETAIL, "Found our response.");
                            plc->flags.response_ready = 0;
                        } else {
                            pdebug(DEBUG_WARN, "Error %s checking read response!", plc_tag_decode_error(rc));
                            rc = PLCTAG_STATUS_OK;
                        }

                        /* remove the tag from the request slot. */
                        micro800_clear_request_slot(plc, tag);

                        plc->flags.response_ready = 0;
                        tag->op = TAG_OP_IDLE;
                        tag->read_in_flight = 0;
                        tag->read_complete = 1;
                        tag->status = (int8_t)rc;

                        raise_event = 1;
                        event = PLCTAG_EVENT_READ_COMPLETED;
                        event_status = rc;

                        /* tell the world we are done. */
                        //plc_tag_tickler_wake();
                        plc_tag_generic_wake_tag((plc_tag_p)tag);

                        break;
                }
            } else {
                pdebug(DEBUG_SPEW, "No response yet, Continue waiting.");
                rc = PLCTAG_STATUS_PENDING;
            }
            break;

        case TAG_OP_WRITE_REQUEST:
            /* if the PLC is ready and there is no request queued yet, build a request. */
            if(find_request_slot(plc, tag) == PLCTAG_STATUS_OK) {
                rc = create_write_request(plc, tag);
                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_DETAIL, "Write request created.");

                    tag->op = TAG_OP_WRITE_RESPONSE;
                    plc->flags.request_ready = 1;

                    rc = PLCTAG_STATUS_PENDING;
                } else {
                    pdebug(DEBUG_WARN, "Error %s creating write request!", plc_tag_decode_error(rc));

                    /* remove the tag from the request slot. */
                    micro800_clear_request_slot(plc, tag);

                    tag->op = TAG_OP_IDLE;
                    tag->write_complete = 1;
                    tag->write_in_flight = 0;
                    tag->status = (int8_t)rc;

                    raise_event = 1;
                    event = PLCTAG_EVENT_WRITE_COMPLETED;
                    event_status = rc;

                    //plc_tag_tickler_wake();
                    plc_tag_generic_wake_tag((plc_tag_p)tag);

                    rc = PLCTAG_STATUS_OK;
                }
            } else {
                pdebug(DEBUG_SPEW, "Request already in flight or PLC not ready, waiting for next chance.");
                rc = PLCTAG_STATUS_PENDING;
            }
            break;

        case TAG_OP_WRITE_RESPONSE:
            /* cross check the state. */
            if(plc->state == PLC_CONNECT_START
            || plc->state == PLC_CONNECT_WAIT
            || plc->state == PLC_ERR_WAIT) {
                pdebug(DEBUG_WARN, "PLC changed state, restarting request.");
                tag->op = TAG_OP_WRITE_REQUEST;
                break;
            }

            if(plc->flags.response_ready) {
                rc = check_write_response(plc, tag);
                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_DETAIL, "Found our response.");

                    /* remove the tag from the request slot. */
                    micro800_clear_request_slot(plc, tag);

                    plc->flags.response_ready = 0;
                    tag->op = TAG_OP_IDLE;
                    tag->write_complete = 1;
                    tag->write_in_flight = 0;
                    tag->status = (int8_t)rc;

                    raise_event = 1;
                    event = PLCTAG_EVENT_WRITE_COMPLETED;
                    event_status = rc;

                    /* tell the world we are done. */
                    //plc_tag_tickler_wake();
                    plc_tag_generic_wake_tag((plc_tag_p)tag);

                    rc = PLCTAG_STATUS_OK;
                } else if(rc == PLCTAG_ERR_PARTIAL) {
                    pdebug(DEBUG_DETAIL, "Found our response, but we are not done.");

                    plc->flags.response_ready = 0;
                    tag->op = TAG_OP_WRITE_REQUEST;

                    rc = PLCTAG_STATUS_OK;
                } else if(rc == PLCTAG_ERR_NO_MATCH) {
                    pdebug(DEBUG_SPEW, "Not our response.");
                    rc = PLCTAG_STATUS_PENDING;
                } else {
                    pdebug(DEBUG_WARN, "Error %s checking write response!", plc_tag_decode_error(rc));

                    /* remove the tag from the request slot. */
                    micro800_clear_request_slot(plc, tag);

                    plc->flags.response_ready = 0;
                    tag->op = TAG_OP_IDLE;
                    tag->write_complete = 1;
                    tag->write_in_flight = 0;
                    tag->status = (int8_t)rc;

                    raise_event = 1;
                    event = PLCTAG_EVENT_WRITE_COMPLETED;
                    event_status = rc;

                    /* tell the world we are done. */
                    //plc_tag_tickler_wake();
                    plc_tag_generic_wake_tag((plc_tag_p)tag);

                    rc = PLCTAG_STATUS_OK;
                }
            } else {
                pdebug(DEBUG_SPEW, "No response yet, Continue waiting.");
                rc = PLCTAG_STATUS_PENDING;
            }
            break;

        default:
            pdebug(DEBUG_WARN, "Unknown tag operation %d!", op);

            tag->op = TAG_OP_IDLE;
            tag->status = (int8_t)PLCTAG_ERR_NOT_IMPLEMENTED;

            /* tell the world we are done. */
            //plc_tag_tickler_wake();
            plc_tag_generic_wake_tag((plc_tag_p)tag);

            rc = PLCTAG_STATUS_OK;
            break;
    }

    if(raise_event) {
        tag_raise_event((plc_tag_p)tag, event, (int8_t)event_status);
        plc_tag_generic_handle_event_callbacks((plc_tag_p)tag);
    }
    
    /*
     * Call the generic tag tickler function to handle auto read/write and set
     * up events.
     */
    plc_tag_generic_tickler((plc_tag_p)tag);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}




int find_request_slot(micro800_plc_p plc, micro800_tag_p tag)
{
    pdebug(DEBUG_SPEW, "Starting.");

    if(plc->flags.request_ready) {
        pdebug(DEBUG_DETAIL, "There is a request already queued for sending.");
        return PLCTAG_ERR_BUSY;
    }

    if(plc->state != PLC_READY) {
        pdebug(DEBUG_DETAIL, "PLC not ready.");
        return PLCTAG_ERR_BUSY;
    }

    if(tag->tag_id == 0) {
        pdebug(DEBUG_DETAIL, "Tag not ready.");
        return PLCTAG_ERR_BUSY;
    }

    /* search for a slot. */
    for(int slot=0; slot < plc->max_requests_in_flight; slot++) {
        if(plc->tags_with_requests[slot] == 0) {
            pdebug(DEBUG_DETAIL, "Found request slot %d for tag %"PRId32".", slot, tag->tag_id);
            plc->tags_with_requests[slot] = tag->tag_id;
            tag->request_slot = slot;
            return PLCTAG_STATUS_OK;
        }
    }

    pdebug(DEBUG_SPEW, "Done.");

    return PLCTAG_ERR_NO_RESOURCES;
}


void micro800_clear_request_slot(micro800_plc_p plc, micro800_tag_p tag)
{
    pdebug(DEBUG_DETAIL, "Starting for tag %"PRId32".", tag->tag_id);

    /* find the tag in the slots. */
    for(int slot=0; slot < plc->max_requests_in_flight; slot++) {
        if(plc->tags_with_requests[slot] == tag->tag_id) {
            pdebug(DEBUG_DETAIL, "Found tag %"PRId32" in slot %d.", tag->tag_id, slot);

            if(slot != tag->request_slot) {
                pdebug(DEBUG_DETAIL, "Tag was not in expected slot %d!", tag->request_slot);
            }

            plc->tags_with_requests[slot] = 0;
            tag->request_slot = -1;
        }
    }

    pdebug(DEBUG_DETAIL, "Done for tag %"PRId32".", tag->tag_id);
}


int send_request(common_device_p device_arg)
{
    int rc = 1;
    micro800_plc_p plc = (micro800_plc_p)device_arg;
    int data_left = plc->write_data_len - plc->write_data_offset;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* check socket, could be closed due to inactivity. */
    if(!plc->sock) {
        pdebug(DEBUG_DETAIL, "No socket or socket is closed.");
        return PLCTAG_ERR_BAD_CONNECTION;
    }

    /* if we have some data in the buffer, keep the connection open. */
    if(plc->write_data_len > 0) {
        plc->inactivity_timeout_ms = MICRO800_INACTIVITY_TIMEOUT_MS + time_ms();
    }

    /* is there anything to do? */
    if(! plc->request_ready) {
        pdebug(DEBUG_WARN, "No packet to send!");
        return PLCTAG_ERR_NO_DATA;
    }

    /* try to send some data. */
    rc = socket_write(plc->sock, plc->write_data + plc->write_data_offset, data_left, SOCKET_WRITE_TIMEOUT);
    if(rc >= 0) {
        plc->write_data_offset += rc;
        data_left = plc->write_data_len - plc->write_data_offset;
    } else if(rc == PLCTAG_ERR_TIMEOUT) {
        pdebug(DEBUG_DETAIL, "Done.  Timeout writing to socket.");
        rc = PLCTAG_STATUS_OK;
    } else {
        pdebug(DEBUG_WARN, "Error, %s, writing to socket!", plc_tag_decode_error(rc));
        return rc;
    }

    /* clean up if full write was done. */
    if(data_left == 0) {
        pdebug(DEBUG_DETAIL, "Full packet written.");
        pdebug_dump_bytes(DEBUG_DETAIL, plc->write_data, plc->write_data_len);

        // plc->flags.request_ready = 0;
        plc->write_data_len = 0;
        plc->write_data_offset = 0;
        plc->response_tag_id = plc->request_tag_id;
        plc->request_tag_id = 0;

        rc = PLCTAG_STATUS_OK;
    } else {
        pdebug(DEBUG_DETAIL, "Partial packet written.");
        rc = PLCTAG_STATUS_PENDING;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



int receive_response(common_device_p device_arg)
{
    int rc = 0;
    micro800_plc_p plc = (micro800_plc_p)device_arg;
    int data_needed = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* socket could be closed due to inactivity. */
    if(!plc->sock) {
        pdebug(DEBUG_SPEW, "Socket is closed or missing.");
        return PLCTAG_STATUS_OK;
    }

    do {
        /* how much data do we need? */
        if(plc->read_data_len >= MODBUS_MBAP_SIZE) {
            int packet_size = plc->read_data[5] + (plc->read_data[4] << 8);
            data_needed = (MODBUS_MBAP_SIZE + packet_size) - plc->read_data_len;

            pdebug(DEBUG_DETAIL, "Packet header read, data_needed=%d, packet_size=%d, read_data_len=%d", data_needed, packet_size, plc->read_data_len);

            if(data_needed > PLC_READ_DATA_LEN) {
                pdebug(DEBUG_WARN, "Error, packet size, %d, greater than buffer size, %d!", data_needed, PLC_READ_DATA_LEN);
                return PLCTAG_ERR_TOO_LARGE;
            } else if(data_needed < 0) {
                pdebug(DEBUG_WARN, "Read more than a packet!  Expected %d bytes, but got %d bytes!", (MODBUS_MBAP_SIZE + packet_size), plc->read_data_len);
                return PLCTAG_ERR_TOO_LARGE;
            }
        } else {
            data_needed = MODBUS_MBAP_SIZE - plc->read_data_len;
            pdebug(DEBUG_DETAIL, "Still reading packet header, data_needed=%d, read_data_len=%d", data_needed, plc->read_data_len);
        }

        if(data_needed == 0) {
            pdebug(DEBUG_DETAIL, "Got all data needed.");
            break;
        }

        /* read the socket. */
        rc = socket_read(plc->sock, plc->read_data + plc->read_data_len, data_needed, SOCKET_READ_TIMEOUT);
        if(rc >= 0) {
            /* got data! Or got nothing, but no error. */
            plc->read_data_len += rc;

            pdebug_dump_bytes(DEBUG_SPEW, plc->read_data, plc->read_data_len);
        } else if(rc == PLCTAG_ERR_TIMEOUT) {
            pdebug(DEBUG_DETAIL, "Done. Socket read timed out.");
            return PLCTAG_STATUS_PENDING;
        } else {
            pdebug(DEBUG_WARN, "Error, %s, reading socket!", plc_tag_decode_error(rc));
            return rc;
        }

        pdebug(DEBUG_DETAIL, "After reading the socket, total read=%d and data needed=%d.", plc->read_data_len, data_needed);
    } while(rc > 0);


    if(data_needed == 0) {
        /* we got our packet. */
        pdebug(DEBUG_DETAIL, "Received full packet.");
        pdebug_dump_bytes(DEBUG_DETAIL, plc->read_data, plc->read_data_len);
        plc->flags.response_ready = 1;

        rc = PLCTAG_STATUS_OK;
    } else {
        /* data_needed is greater than zero. */
        pdebug(DEBUG_DETAIL, "Received partial packet of %d bytes of %d.", plc->read_data_len, (data_needed + plc->read_data_len));
        rc = PLCTAG_STATUS_PENDING;
    }

    /* if we have some data in the buffer, keep the connection open. */
    if(plc->read_data_len > 0) {
        plc->inactivity_timeout_ms = MICRO800_INACTIVITY_TIMEOUT_MS + time_ms();
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}





int create_read_request(common_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    micro800_tag_p tag = (micro800_tag_p)tag_arg;
    micro800_plc_p plc = tag->device;
    uint16_t seq_id = (++(plc->seq_id) ? plc->seq_id : ++(plc->seq_id)); // disallow zero
    int registers_per_request = (MAX_MODBUS_RESPONSE_PAYLOAD * 8) / tag->elem_size;
    int base_register = tag->reg_base + (tag->request_num * registers_per_request);
    int register_count = tag->elem_count - (tag->request_num * registers_per_request);

    pdebug(DEBUG_DETAIL, "Starting.");

    pdebug(DEBUG_DETAIL, "seq_id=%d", seq_id);
    pdebug(DEBUG_DETAIL, "registers_per_request = %d", registers_per_request);
    pdebug(DEBUG_DETAIL, "base_register = %d", base_register);
    pdebug(DEBUG_DETAIL, "register_count = %d", register_count);

    /* clamp the number of registers we ask for to what will fit. */
    if(register_count > registers_per_request) {
        register_count = registers_per_request;
    }

    pdebug(DEBUG_INFO, "preparing read request for %d registers (of %d total) from base register %d.", register_count, tag->elem_count, base_register);

    /* build the read request.
     *    Byte  Meaning
     *      0    High byte of request sequence ID.
     *      1    Low byte of request sequence ID.
     *      2    High byte of the protocol version identifier (zero).
     *      3    Low byte of the protocol version identifier (zero).
     *      4    High byte of the message length.
     *      5    Low byte of the message length.
     *      6    Device address.
     *      7    Function code.
     *      8    High byte of first register address.
     *      9    Low byte of the first register address.
     *     10    High byte of the register count.
     *     11    Low byte of the register count.
     */

    plc->write_data_len = 0;

    /* build the request sequence ID */
    plc->write_data[plc->write_data_len] = (uint8_t)((seq_id >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((seq_id >> 0) & 0xFF); plc->write_data_len++;

    /* protocol version is always zero */
    plc->write_data[plc->write_data_len] = 0; plc->write_data_len++;
    plc->write_data[plc->write_data_len] = 0; plc->write_data_len++;

    /* request packet length */
    plc->write_data[plc->write_data_len] = 0; plc->write_data_len++;
    plc->write_data[plc->write_data_len] = 6; plc->write_data_len++;

    /* device address */
    plc->write_data[plc->write_data_len] = plc->server_id; plc->write_data_len++;

    /* function code depends on the register type. */
    switch(tag->reg_type) {
        case MB_REG_COIL:
            plc->write_data[7] = MB_CMD_READ_COIL_MULTI; plc->write_data_len++;
            break;

        case MB_REG_DISCRETE_INPUT:
            plc->write_data[7] = MB_CMD_READ_DISCRETE_INPUT_MULTI; plc->write_data_len++;
            break;

        case MB_REG_HOLDING_REGISTER:
            plc->write_data[7] = MB_CMD_READ_HOLDING_REGISTER_MULTI; plc->write_data_len++;
            break;

        case MB_REG_INPUT_REGISTER:
            plc->write_data[7] = MB_CMD_READ_INPUT_REGISTER_MULTI; plc->write_data_len++;
            break;

        default:
            pdebug(DEBUG_WARN, "Unsupported register type %d!", tag->reg_type);
            return PLCTAG_ERR_UNSUPPORTED;
            break;
    }

    /* register base. */
    plc->write_data[plc->write_data_len] = (uint8_t)((base_register >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((base_register >> 0) & 0xFF); plc->write_data_len++;

    /* number of elements to read. */
    plc->write_data[plc->write_data_len] = (uint8_t)((register_count >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((register_count >> 0) & 0xFF); plc->write_data_len++;

    tag->seq_id = seq_id;
    plc->flags.request_ready = 1;
    plc->request_tag_id = tag->tag_id;

    pdebug(DEBUG_DETAIL, "Created read request:");
    pdebug_dump_bytes(DEBUG_DETAIL, plc->write_data, plc->write_data_len);

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}





int check_read_response(common_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    micro800_tag_p tag = (micro800_tag_p)tag_arg;
    micro800_plc_p plc = tag->device;
    int partial_read = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(seq_id == tag->seq_id) {
        uint8_t has_error = plc->read_data[7] & (uint8_t)0x80;

        /* the operation is complete regardless of the outcome. */
        //tag->flags.operation_complete = 1;

        if(has_error) {
            rc = remove_translate_modbus_error(plc->read_data[8]);

            pdebug(DEBUG_WARN, "Got read response %ud, with error %s, of length %d.", (int)(unsigned int)seq_id, plc_tag_decode_error(rc), plc->read_data_len);
        } else {
            int registers_per_request = (MAX_MODBUS_RESPONSE_PAYLOAD * 8) / tag->elem_size;
            int register_offset = (tag->request_num * registers_per_request);
            int byte_offset = (register_offset * tag->elem_size) / 8;
            uint8_t payload_size = plc->read_data[8];
            int copy_size = ((tag->size - byte_offset) < payload_size ? (tag->size - byte_offset) : payload_size);

            /* no error. So copy the data. */
            pdebug(DEBUG_DETAIL, "Got read response %u of length %d with payload of size %d.", (int)(unsigned int)seq_id, plc->read_data_len, payload_size);
            pdebug(DEBUG_DETAIL, "registers_per_request = %d", registers_per_request);
            pdebug(DEBUG_DETAIL, "register_offset = %d", register_offset);
            pdebug(DEBUG_DETAIL, "byte_offset = %d", byte_offset);
            pdebug(DEBUG_DETAIL, "copy_size = %d", copy_size);

            mem_copy(tag->data + byte_offset, &plc->read_data[9], copy_size);

            /* are we done? */
            if(tag->size > (byte_offset + copy_size)) {
                /* Not yet. */
                pdebug(DEBUG_DETAIL, "Not done reading entire tag.");
                partial_read = 1;
            } else {
                /* read is done. */
                pdebug(DEBUG_DETAIL, "Read is complete.");
                partial_read = 0;
            }

            rc = PLCTAG_STATUS_OK;
        }

        /* either way, clean up the PLC buffer. */
        plc->read_data_len = 0;
        plc->flags.response_ready = 0;

        /* clean up tag*/
        if(!partial_read) {
            pdebug(DEBUG_DETAIL, "Read is complete.  Cleaning up tag state.");
            tag->seq_id = 0;
            tag->read_complete = 1;
            tag->read_in_flight = 0;
            tag->status = (int8_t)rc;
            tag->request_num = 0;
        } else {
            pdebug(DEBUG_DETAIL, "Read is partially complete.  We need to do at least one more request.");
            rc = PLCTAG_ERR_PARTIAL;
            tag->request_num++;
            tag->status = (int8_t)PLCTAG_STATUS_PENDING;
        }
    } else {
        pdebug(DEBUG_DETAIL, "Not our response.");

        rc = PLCTAG_ERR_NO_MATCH;
    }

    pdebug(DEBUG_DETAIL, "Done: %s", plc_tag_decode_error(rc));

    return rc;
}



/* build the write request.
 *    Byte  Meaning
 *      0    High byte of request sequence ID.
 *      1    Low byte of request sequence ID.
 *      2    High byte of the protocol version identifier (zero).
 *      3    Low byte of the protocol version identifier (zero).
 *      4    High byte of the message length.
 *      5    Low byte of the message length.
 *      6    Device address.
 *      7    Function code.
 *      8    High byte of first register address.
 *      9    Low byte of the first register address.
 *     10    High byte of the register count.
 *     11    Low byte of the register count.
 *     12    Number of bytes of data to write.
 *     13... Data bytes.
 */

int create_write_request(common_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    micro800_tag_p tag = (micro800_tag_p)tag_arg;
    micro800_plc_p plc = tag->device;
    uint16_t seq_id = (++(plc->seq_id) ? plc->seq_id : ++(plc->seq_id)); // disallow zero
    int registers_per_request = (MAX_MODBUS_REQUEST_PAYLOAD * 8) / tag->elem_size;
    int base_register = tag->reg_base + (tag->request_num * registers_per_request);
    int register_count = tag->elem_count - (tag->request_num * registers_per_request);
    int register_offset = (tag->request_num * registers_per_request);
    int byte_offset = (register_offset * tag->elem_size) / 8;
    int request_payload_size = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    pdebug(DEBUG_SPEW, "seq_id=%d", seq_id);
    pdebug(DEBUG_SPEW, "registers_per_request = %d", registers_per_request);
    pdebug(DEBUG_SPEW, "base_register = %d", base_register);
    pdebug(DEBUG_SPEW, "register_count = %d", register_count);
    pdebug(DEBUG_SPEW, "register_offset = %d", register_offset);
    pdebug(DEBUG_SPEW, "byte_offset = %d", byte_offset);

    /* clamp the number of registers we ask for to what will fit. */
    if(register_count > registers_per_request) {
        register_count = registers_per_request;
    }

    /* how many bytes, rounded up to the nearest byte. */
    request_payload_size = ((register_count * tag->elem_size) + 7) / 8;

    pdebug(DEBUG_DETAIL, "preparing write request for %d registers (of %d total) from base register %d of payload size %d in bytes.", register_count, tag->elem_count, base_register, request_payload_size);

    /* FIXME - remove this when we figure out how to push multiple requests. */
    plc->write_data_len = 0;

    /* build the request sequence ID */
    plc->write_data[plc->write_data_len] = (uint8_t)((seq_id >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((seq_id >> 0) & 0xFF); plc->write_data_len++;

    /* protocol version is always zero */
    plc->write_data[plc->write_data_len] = 0; plc->write_data_len++;
    plc->write_data[plc->write_data_len] = 0; plc->write_data_len++;

    /* request packet length */
    plc->write_data[plc->write_data_len] = (uint8_t)(((request_payload_size + 7) >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)(((request_payload_size + 7) >> 0) & 0xFF); plc->write_data_len++;

    /* device address */
    plc->write_data[plc->write_data_len] = plc->server_id; plc->write_data_len++;

    /* function code depends on the register type. */
    switch(tag->reg_type) {
        case MB_REG_COIL:
            plc->write_data[7] = MB_CMD_WRITE_COIL_MULTI; plc->write_data_len++;
            break;

        case MB_REG_DISCRETE_INPUT:
            pdebug(DEBUG_WARN, "Done. You cannot write a discrete input!");
            return PLCTAG_ERR_UNSUPPORTED;
            break;

        case MB_REG_HOLDING_REGISTER:
            plc->write_data[7] = MB_CMD_WRITE_HOLDING_REGISTER_MULTI; plc->write_data_len++;
            break;

        case MB_REG_INPUT_REGISTER:
            pdebug(DEBUG_WARN, "Done. You cannot write an analog input!");
            return PLCTAG_ERR_UNSUPPORTED;
            break;

        default:
            pdebug(DEBUG_WARN, "Done. Unsupported register type %d!", tag->reg_type);
            return PLCTAG_ERR_UNSUPPORTED;
            break;
    }

    /* register base. */
    plc->write_data[plc->write_data_len] = (uint8_t)((base_register >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((base_register >> 0) & 0xFF); plc->write_data_len++;

    /* number of elements to read. */
    plc->write_data[plc->write_data_len] = (uint8_t)((register_count >> 8) & 0xFF); plc->write_data_len++;
    plc->write_data[plc->write_data_len] = (uint8_t)((register_count >> 0) & 0xFF); plc->write_data_len++;

    /* number of bytes of data to write. */
    plc->write_data[plc->write_data_len] = (uint8_t)(unsigned int)(request_payload_size); plc->write_data_len++;

    /* copy the tag data. */
    mem_copy(&plc->write_data[plc->write_data_len], &tag->data[byte_offset], request_payload_size);
    plc->write_data_len += request_payload_size;

    tag->seq_id = (uint16_t)(unsigned int)seq_id;
    plc->flags.request_ready = 1;
    plc->request_tag_id = tag->tag_id;

    pdebug(DEBUG_DETAIL, "Created write request:");
    pdebug_dump_bytes(DEBUG_DETAIL, plc->write_data, plc->write_data_len);

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}





/* Write response.
 *    Byte  Meaning
 *      0    High byte of request sequence ID.
 *      1    Low byte of request sequence ID.
 *      2    High byte of the protocol version identifier (zero).
 *      3    Low byte of the protocol version identifier (zero).
 *      4    High byte of the message length.
 *      5    Low byte of the message length.
 *      6    Device address.
 *      7    Function code.
 *      8    High byte of first register address/Error code.
 *      9    Low byte of the first register address.
 *     10    High byte of the register count.
 *     11    Low byte of the register count.
 */

int check_write_response(common_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    micro800_tag_p tag = (micro800_tag_p)tag_arg;
    micro800_plc_p plc = tag->device;
    uint16_t seq_id = (uint16_t)((uint16_t)plc->read_data[1] + (uint16_t)(plc->read_data[0] << 8));
    int partial_write = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    if(seq_id == tag->seq_id) {
        uint8_t has_error = plc->read_data[7] & (uint8_t)0x80;

        /* this is our response, so the operation is complete regardless of the status. */
        //tag->flags.operation_complete = 1;

        if(has_error) {
            rc = remove_translate_modbus_error(plc->read_data[8]);

            pdebug(DEBUG_WARN, "Got write response %ud, with error %s, of length %d.", (int)(unsigned int)seq_id, plc_tag_decode_error(rc), plc->read_data_len);
        } else {
            int registers_per_request = (MAX_MODBUS_RESPONSE_PAYLOAD * 8) / tag->elem_size;
            int next_register_offset = ((tag->request_num+1) * registers_per_request);
            int next_byte_offset = (next_register_offset * tag->elem_size) / 8;

            /* no error. So copy the data. */
            pdebug(DEBUG_DETAIL, "registers_per_request = %d", registers_per_request);
            pdebug(DEBUG_DETAIL, "next_register_offset = %d", next_register_offset);
            pdebug(DEBUG_DETAIL, "next_byte_offset = %d", next_byte_offset);

            /* are we done? */
            if(tag->size > next_byte_offset) {
                /* Not yet. */
                pdebug(DEBUG_SPEW, "Not done writing entire tag.");
                partial_write = 1;
            } else {
                /* read is done. */
                pdebug(DEBUG_DETAIL, "Write is complete.");
                partial_write = 0;
            }

            rc = PLCTAG_STATUS_OK;
        }

        /* either way, clean up the PLC buffer. */
        plc->read_data_len = 0;
        plc->flags.response_ready = 0;

        /* clean up tag*/
        if(!partial_write) {
            pdebug(DEBUG_DETAIL, "Write complete. Cleaning up tag state.");
            tag->seq_id = 0;
            tag->request_num = 0;
            tag->write_complete = 1;
            tag->write_in_flight = 0;
            tag->status = (int8_t)rc;
        } else {
            pdebug(DEBUG_DETAIL, "Write partially complete.  We need to do at least one more write request.");
            rc = PLCTAG_ERR_PARTIAL;
            tag->request_num++;
            tag->status = (int8_t)PLCTAG_STATUS_PENDING;
        }
    } else {
        pdebug(DEBUG_SPEW, "Not our response.");

        rc = PLCTAG_STATUS_PENDING;
    }

    pdebug(DEBUG_SPEW, "Done.");

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


