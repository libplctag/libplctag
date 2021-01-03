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
#include <ab2/eip.h>
#include <lib/libplctag.h>
#include <util/atomic_int.h>
#include <util/attr.h>
#include <util/mem.h>
#include <util/mutex.h>
#include <util/plc.h>
#include <util/slice.h>
#include <util/socket.h>
#include <util/string.h>


#define EIP_DEFAULT_PORT (44818)
#define EIP_VERSION ((uint16_t)1)

#define EIP_HEADER_SIZE (24)
#define SESSION_REQUEST_SIZE (28)
#define SESSION_REQUEST_RESPONSE_SIZE (28)

#define REGISTER_SESSION_CMD ((uint16_t)(0x65))
#define SEND_UNCONNECTED_DATA ((uint16_t)0x6F)
#define SEND_CONNECTED_DATA ((uint16_t)0x70)




#define TRY_GET_BYTE(buffer, capacity, offset, val) if(offset < capacity) { (val) = buffer[offset]; } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++)

#define TRY_GET_U16_LE(buffer, capacity, offset, val) \
        if(offset < capacity) { (val) = (uint16_t)buffer[offset]; } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint16_t)buffer[offset] << 8); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++)

#define TRY_GET_U32_LE(buffer, capacity, offset, val) \
        if(offset < capacity) { (val) = (uint32_t)buffer[offset]; } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint32_t)buffer[offset] << 8); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint32_t)buffer[offset] << 16); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint32_t)buffer[offset] << 24); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++)

#define TRY_GET_U64_LE(buffer, capacity, offset, val) \
        if(offset < capacity) { (val) = (uint64_t)buffer[offset]; } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint64_t)buffer[offset] << 8); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint64_t)buffer[offset] << 16); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint64_t)buffer[offset] << 24); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint64_t)buffer[offset] << 32); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint64_t)buffer[offset] << 40); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint64_t)buffer[offset] << 48); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { (val) |= ((uint64_t)buffer[offset] << 56); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++) \

#define TRY_SET_BYTE(buffer, capacity, offset, val) if(offset < capacity) { buffer[offset] = (val); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++)

#define TRY_SET_U16_LE(buffer, capacity, offset, val) \
        if(offset < capacity) { buffer[offset] = (uint8_t)((val) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 8) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++)

#define TRY_SET_U32_LE(buffer, capacity, offset, val) \
        if(offset < capacity) { buffer[offset] = (uint8_t)((val) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 8) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 16) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 24) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++)

#define TRY_SET_U64_LE(buffer, capacity, offset, val) \
        if(offset < capacity) { buffer[offset] = (uint8_t)((val) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 8) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 16) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 24) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 32) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 40) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 48) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++); \
        if(offset < capacity) { buffer[offset] = (uint8_t)(((val) << 56) & 0xFF); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++)


struct eip_layer_state_s {
    /* session data */
    plc_p plc;
    bool is_connected;
    uint32_t session_handle;
    uint64_t session_context;
};



static int eip_layer_initialize(void *context);
static int eip_layer_connect(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end);
static int eip_layer_disconnect(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end);
static int eip_layer_prepare_for_request(void *context);
static int eip_layer_build_request(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_num);
static int eip_layer_prepare_for_response(void *context);
static int eip_layer_process_response(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_num);
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

    state->is_connected = FALSE;
    state->plc = plc;

    rc = plc_set_layer(plc,
                       layer_index,
                       state,
                       eip_layer_initialize,
                       eip_layer_connect,
                       /*eip_layer_disconnect*/ NULL,
                       eip_layer_prepare_for_request,
                       eip_layer_build_request,
                       eip_layer_prepare_for_response,
                       eip_layer_process_response,
                       eip_layer_destroy_layer);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error setting layer!");
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

    state->is_connected = FALSE;
    state->session_handle = 0;
    state->session_context = (uint64_t)rand();

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



int eip_layer_connect(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end)
{
    int rc = PLCTAG_STATUS_OK;
    struct eip_layer_state_s *state = (struct eip_layer_state_s *)context;
    int offset = 0;

    pdebug(DEBUG_INFO, "Building EIP connect packet.");

    /* check space */
    if(buffer_capacity < SESSION_REQUEST_SIZE) {
        pdebug(DEBUG_WARN, "Insufficient space to build session request!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    *data_start = 0;
    offset = 0;

    do {
        /* command */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, REGISTER_SESSION_CMD);

        /* packet/payload length. */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, 4);

        /* session handle, zero here. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, 0);

        /* session status, zero here. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, 0);

        /* session context, zero here. */
        TRY_SET_U64_LE(buffer, buffer_capacity, offset, 0);

        /* options, unused, zero here. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, 0);

        /* payload */

        /* requested EIP version. */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, EIP_VERSION);

        /* requested EIP options. */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, 0);

        *data_start = offset;
        *data_end = offset;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, building session registration request!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



/* NOT USED */
int eip_layer_disconnect(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Disconnect EIP layer.");

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




int eip_layer_prepare_for_request(void *context)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Preparing layer for building a request.");

    /* Anything to do here? */

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




int eip_layer_build_request(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_num)
{
    int rc = PLCTAG_STATUS_OK;
    struct eip_layer_state_s *state = (struct eip_layer_state_s *)context;
    int offset = 0;
    int payload_size = 0;
    uint16_t command = SEND_UNCONNECTED_DATA;

    pdebug(DEBUG_INFO, "Building a request.");

    /* check to see if we are connected or not. */
    if(!state->is_connected) {
        /* nope.*/
        pdebug(DEBUG_WARN, "EIP session is not connected!");
        return PLCTAG_ERR_BAD_CONNECTION;
    }

    if(buffer_capacity < EIP_HEADER_SIZE) {
        pdebug(DEBUG_WARN, "Insufficient space for new request!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* this might be the first call. */

    /* what kind of request is it? Cheat and peak at the payload. */
    if(*data_end >= EIP_HEADER_SIZE + 10) {
        uint16_t address_item_type = 0;

        address_item_type = (uint16_t)(buffer[EIP_HEADER_SIZE + 8])
                          + (uint16_t)(((uint16_t)buffer[EIP_HEADER_SIZE + 9]) << 8);

        if(address_item_type == 0) {
            /* unconnected message. */
            command = SEND_UNCONNECTED_DATA;
        } else {
            command = SEND_CONNECTED_DATA;
        }
    } else {
        command = SEND_UNCONNECTED_DATA;
    }

    /* fix up data end if this is the first time through. */
    if(*data_end  < EIP_HEADER_SIZE) {
        *data_end = EIP_HEADER_SIZE;
    }

    /* calculate the payload size. */
    payload_size = *data_end - EIP_HEADER_SIZE;

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

        if(command == SEND_CONNECTED_DATA) {
            /* session context, zero here. */
            TRY_SET_U64_LE(buffer, buffer_capacity, offset, 0);
        } else {
            state->session_context++;

            /* session context. */
            TRY_SET_U64_LE(buffer, buffer_capacity, offset, state->session_context);
        }

        /* options, unused, zero here. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, 0);

        /* fix up data start for next layer */
        *data_start = EIP_HEADER_SIZE;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build EIP header packet, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




int eip_layer_prepare_for_response(void *context)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Preparing for response.");

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




int eip_layer_process_response(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_num)
{
    int rc = PLCTAG_STATUS_OK;
    struct eip_layer_state_s *state = (struct eip_layer_state_s *)context;
    uint16_t command = 0;
    uint16_t payload_size = 0;
    uint32_t status = 0;
    uint32_t session_handle = 0;
    int offset = 0;

    pdebug(DEBUG_INFO, "Processing EIP response.");

    if(buffer_capacity < EIP_HEADER_SIZE){
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
        if(buffer_capacity < (int)(unsigned int)(payload_size + EIP_HEADER_SIZE)) {
            pdebug(DEBUG_DETAIL, "Need more data than the header, we need %d bytes and have %d bytes.", buffer_capacity, (int)(unsigned int)(payload_size + EIP_HEADER_SIZE));
            rc = PLCTAG_ERR_PARTIAL;
            break;
        }

        if(status != 0) {
            pdebug(DEBUG_WARN, "Got bad EIP status %d!", (int)(unsigned int)status);
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* what we do depends on the command */
        if(command == REGISTER_SESSION_CMD) {
            /* copy the information into the state. */
            state->session_handle = session_handle;

            state->is_connected = TRUE;

            /* signal that we have consumed the whole payload. */
            *data_end = (int)(unsigned int)(payload_size + EIP_HEADER_SIZE);
            *data_start = *data_end;
        } else {
            /* other layers will need to process this. */
            *data_start = EIP_HEADER_SIZE;
            *data_end = (int)(unsigned int)(payload_size + EIP_HEADER_SIZE);
        }
    } while(0);

    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_PARTIAL) {
        pdebug(DEBUG_WARN, "Unable to build EIP header packet, error %s!", plc_tag_decode_error(rc));
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

