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

#include <stdlib.h>
#include <ab2/eip_layer.h>
#include <lib/libplctag.h>
#include <util/atomic_int.h>
#include <util/attr.h>
#include <util/mem.h>
#include <util/mutex.h>
#include <util/plc.h>
#include <util/socket.h>
#include <util/string.h>


#define EIP_DEFAULT_PORT (44818)
#define EIP_VERSION ((uint16_t)1)

#define EIP_HEADER_SIZE (24)
#define MAX_EIP_PAYLOAD_SIZE (0x10000) /* 64k */
#define SESSION_REQUEST_SIZE (28)
#define SESSION_REQUEST_RESPONSE_SIZE (28)

#define REGISTER_SESSION_CMD ((uint16_t)(0x65))
#define UNREGISTER_SESSION_CMD   ((uint16_t)0x0066)

#define SEND_UNCONNECTED_DATA_CMD ((uint16_t)0x6F)
#define SEND_CONNECTED_DATA_CMD ((uint16_t)0x70)


struct eip_layer_state_s {
    /* session data */
    plc_p plc;

    bool is_connected;

    uint32_t session_handle;
    uint64_t session_context;

    /* save this for checking. */
    int payload_start;
};


static int eip_layer_initialize(void *context);
static int eip_layer_connect(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end);
// static int eip_layer_disconnect(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end);
static int eip_layer_reserve_space(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id *req_id);
static int eip_layer_fix_up_request(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id *req_id);
static int eip_layer_process_response(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id *req_id);
static int eip_layer_destroy_layer(void *context);




int eip_layer_setup(plc_p plc, int layer_index, attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    struct eip_layer_state_s *state = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    /* set the default port for EIP */
    attr_set_int(attribs, "default_port", EIP_DEFAULT_PORT);

    state = mem_alloc(sizeof(*state));
    if(!state) {
        pdebug(DEBUG_WARN, "Unable to allocate EIP layer state!");
        return PLCTAG_ERR_NO_MEM;
    }

    state->is_connected = false;
    state->plc = plc;

    rc = plc_set_layer(plc,
                       layer_index,
                       state,
                       eip_layer_initialize,
                       eip_layer_connect,
                       /*eip_layer_disconnect*/ NULL,
                       eip_layer_reserve_space,
                       eip_layer_fix_up_request,
                       eip_layer_process_response,
                       eip_layer_destroy_layer);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error setting layer!");
        mem_free(state);
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


/*
 * reset our state back to something sane.
 */
int eip_layer_initialize(void *context)
{
    int rc = PLCTAG_STATUS_OK;
    struct eip_layer_state_s *state = (struct eip_layer_state_s *)context;

    pdebug(DEBUG_INFO, "Initializing EIP layer.");

    state->is_connected = false;

    state->session_handle = 0;
    state->session_context = (uint64_t)rand();

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



int eip_layer_connect(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end)
{
    int rc = PLCTAG_STATUS_OK;
    struct eip_layer_state_s *state = (struct eip_layer_state_s *)context;

    pdebug(DEBUG_INFO, "Building EIP connect packet.");

    if(state->is_connected == true) {
        pdebug(DEBUG_WARN, "Connect called while EIP layer is already connected!");
        return PLCTAG_ERR_BUSY;
    }

    /* check space */
    if(buffer_capacity < SESSION_REQUEST_SIZE) {
        pdebug(DEBUG_WARN, "Insufficient space to build session request!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* this layer has to start at the beginning of the whole buffer. */
    *payload_end = *payload_start = 0;

    do {
        /* command */
        TRY_SET_U16_LE(buffer, buffer_capacity, *payload_end, REGISTER_SESSION_CMD);

        /* packet/payload length. */
        TRY_SET_U16_LE(buffer, buffer_capacity, *payload_end, 4);

        /* session handle, zero here. */
        TRY_SET_U32_LE(buffer, buffer_capacity, *payload_end, 0);

        /* session status, zero here. */
        TRY_SET_U32_LE(buffer, buffer_capacity, *payload_end, 0);

        /* session context, zero here. */
        TRY_SET_U64_LE(buffer, buffer_capacity, *payload_end, 0);

        /* options, unused, zero here. */
        TRY_SET_U32_LE(buffer, buffer_capacity, *payload_end, 0);

        /* payload */

        /* requested EIP version. */
        TRY_SET_U16_LE(buffer, buffer_capacity, *payload_end, EIP_VERSION);

        /* requested EIP options. */
        TRY_SET_U16_LE(buffer, buffer_capacity, *payload_end, 0);

        pdebug(DEBUG_INFO, "Built session registration request packet:");
        pdebug_dump_bytes(DEBUG_INFO, buffer + *payload_start, *payload_end - *payload_start);

        pdebug(DEBUG_DETAIL, "Set payload_start=%d and payload_end=%d.", *payload_start, *payload_end);
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, building session registration request!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



/* called from the bottom up. */

int eip_layer_reserve_space(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id *req_id)
{
    int rc = PLCTAG_STATUS_OK;
    struct eip_layer_state_s *state = (struct eip_layer_state_s *)context;

    (void)buffer;
    (void)req_id;

    pdebug(DEBUG_INFO, "Preparing layer for building a request.");

    /* allocate space for the EIP header. */
    if(buffer_capacity < EIP_HEADER_SIZE) {
        pdebug(DEBUG_WARN, "Buffer size, (%d) is too small for EIP header (size %d)!", buffer_capacity, EIP_HEADER_SIZE);
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* set the payload boundaries for the next layer up. */
    *payload_start = EIP_HEADER_SIZE;

    if(buffer_capacity < (MAX_EIP_PAYLOAD_SIZE + EIP_HEADER_SIZE)) {
        *payload_end = buffer_capacity;
    } else {
        pdebug(DEBUG_DETAIL, "Clamping total packet payload capacity to 64k for EIP.");
        *payload_end = MAX_EIP_PAYLOAD_SIZE  + EIP_HEADER_SIZE;
    }

    state->payload_start = *payload_start;

    pdebug(DEBUG_INFO, "Done with payload_start=%d and payload_end=%d.", *payload_start, *payload_end);

    return rc;
}



/* called top down, the payload end is the end of the whole packet.  */

int eip_layer_fix_up_request(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id *req_id)
{
    int rc = PLCTAG_STATUS_OK;
    struct eip_layer_state_s *state = (struct eip_layer_state_s *)context;
    int offset = 0;
    int payload_size = *payload_end - *payload_start;
    uint16_t command = SEND_UNCONNECTED_DATA_CMD;
    uint16_t address_item_type = 0;

    (void)req_id;

    pdebug(DEBUG_INFO, "Building a request.");

    /* set this to something.   Not used at this layer. */

    /* check to see if we are connected or not. */
    if(!state->is_connected) {
        /* nope.*/
        pdebug(DEBUG_WARN, "EIP session is not connected!");
        return PLCTAG_ERR_BAD_CONNECTION;
    }

    if(payload_size < 10) { /* MAGIC - need some amount of payload! */
        pdebug(DEBUG_WARN, "Insufficient space for new request!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    if(*payload_start != state->payload_start) {
        pdebug(DEBUG_WARN, "Start of next payload, %d, is NOT where we set it, %d! Was eip_layer_reserve_space() called?", *payload_start, state->payload_start);
        return PLCTAG_ERR_NO_MATCH;
    }

    /* what kind of request is it? Cheat and peak at the payload. */
    address_item_type = (uint16_t)(buffer[EIP_HEADER_SIZE + 8])
                        + (uint16_t)(((uint16_t)buffer[EIP_HEADER_SIZE + 9]) << 8);

    if(address_item_type == 0) {
        /* unconnected message. */
        command = SEND_UNCONNECTED_DATA_CMD;

        pdebug(DEBUG_DETAIL, "Building unconnected request.");
    } else {
        command = SEND_CONNECTED_DATA_CMD;

        pdebug(DEBUG_DETAIL, "Building connected request.");
    }

    /* build packet */
    do {
        /* set the command */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, command);

        /* payload length. */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, payload_size);

        /* session handle. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, state->session_handle);

        /* session status, zero here. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, 0);

        if(command == SEND_CONNECTED_DATA_CMD) {
            /* session context, zero here. */
            TRY_SET_U64_LE(buffer, buffer_capacity, offset, 0);
        } else {
            state->session_context++;

            /* session context. */
            TRY_SET_U64_LE(buffer, buffer_capacity, offset, state->session_context);
        }

        /* options, unused, zero here. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, 0);

        if(offset != state->payload_start) {
            pdebug(DEBUG_WARN, "Coding error, actual header size is %d bytes but %d bytes were reserved!", offset, state->payload_start);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        pdebug(DEBUG_INFO, "Built EIP packet for request:");
        pdebug_dump_bytes(DEBUG_INFO, buffer, *payload_end);

        /* fix up payload start. */
        *payload_start = 0;

        pdebug(DEBUG_DETAIL, "Set payload_start=%d and payload_end=%d.", *payload_start, *payload_end);
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build EIP packet, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


/* this is bottom up. */

int eip_layer_process_response(void *context, uint8_t *buffer, int buffer_capacity, int *payload_start, int *payload_end, plc_request_id *req_id)
{
    int rc = PLCTAG_STATUS_OK;
    struct eip_layer_state_s *state = (struct eip_layer_state_s *)context;
    uint16_t command = 0;
    int payload_size = *payload_end - *payload_start;
    uint32_t status = 0;
    uint32_t session_handle = 0;
    int offset = 0;

    pdebug(DEBUG_INFO, "Processing EIP response.");

    /* there is only one EIP response in a packet. */
    *req_id = 1;

    if(payload_size < EIP_HEADER_SIZE){
        pdebug(DEBUG_DETAIL, "Need more data!");
        return PLCTAG_ERR_PARTIAL;
    }

    /* we at least have the header, now get the command, payload size and status. */
    do {
        /* command */
        TRY_GET_U16_LE(buffer, buffer_capacity, offset, command);

        /* get the payload size */
        TRY_GET_U16_LE(buffer, buffer_capacity, offset, payload_size);

        /* get the session handle */
        TRY_GET_U32_LE(buffer, buffer_capacity, offset, session_handle);

        /* get the status */
        TRY_GET_U32_LE(buffer, buffer_capacity, offset, status);

        /* do we have the whole packet? */
        if(*payload_end < (int)(unsigned int)(payload_size + EIP_HEADER_SIZE)) {
            pdebug(DEBUG_DETAIL, "Need more data than the header, we need %d bytes and have %d bytes.", (int)(unsigned int)(payload_size + EIP_HEADER_SIZE), *payload_end);
            rc = PLCTAG_ERR_PARTIAL;
            break;
        }

        if(*payload_end > (int)(unsigned int)(payload_size + EIP_HEADER_SIZE)) {
            pdebug(DEBUG_WARN, "Unexpected packet received with too many bytes, expected %d bytes and have %d bytes.", (int)(unsigned int)(payload_size + EIP_HEADER_SIZE), *payload_end);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(status != 0) {
            /* TODO - fill this in */
            switch(status) {
                case 3: pdebug(DEBUG_WARN, "EIP error: command not understood!"); rc = PLCTAG_ERR_UNSUPPORTED; break;
                default: pdebug(DEBUG_WARN, "Got bad EIP status %d!", (int)(unsigned int)status); rc = PLCTAG_ERR_REMOTE_ERR; break;
            }
            break;
        }

        pdebug(DEBUG_INFO, "Got successful EIP packet:");
        pdebug_dump_bytes(DEBUG_INFO, buffer, *payload_end);

        /* what we do depends on the command */
        if(command == REGISTER_SESSION_CMD) {
            /* copy the information into the state. */
            state->session_handle = session_handle;

            state->is_connected = true;

            /* signal that we have consumed the whole payload. */
            *payload_end = 0;
            *payload_start = 0;
        } else {
            /* other layers will need to process this. */
            *payload_start = EIP_HEADER_SIZE;
        }

        pdebug(DEBUG_DETAIL, "Set payload_start=%d and payload_end=%d.", *payload_start, *payload_end);
    } while(0);

    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_PARTIAL) {
        pdebug(DEBUG_WARN, "Unable to process EIP header packet, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




int eip_layer_destroy_layer(void *context)
{
    int rc = PLCTAG_STATUS_OK;
    struct eip_layer_state_s *state = (struct eip_layer_state_s *)context;

    pdebug(DEBUG_INFO, "Cleaning up EIP layer.");

    if(state) {
        mem_free(state);
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}

