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

#include <stdlib.h>
#include <ab2/ab.h>
#include <ab2/eip_layer.h>
#include <lib/libplctag.h>
#include <util/atomic_int.h>
#include <util/attr.h>
#include <util/mem.h>
#include <util/mutex.h>
#include <util/plc.h>
#include <util/slice.h>
#include <util/socket.h>
#include <util/string.h>


#define CIP_PLC5_CONN_PARAM ((uint16_t)0x4302)
#define CIP_SLC_CONN_PARAM ((uint16_t)0x4302)
#define CIP_LGX_CONN_PARAM ((uint16_t)0x43F8)

//#define CIP_LGX_CONN_PARAM ((uint16_t)0x4200)
//0100 0011 1111 1000
//0100 001 1 1111 1000
#define CIP_LGX_CONN_PARAM_EX ((uint32_t)0x42000000)
#define LOGIX_LARGE_PAYLOAD_SIZE (4002)

#define CIP_CMD_EXECUTED_FLAG (0x80)
#define CIP_FORWARD_CLOSE_REQUEST (0x4E)
#define CIP_FORWARD_OPEN_REQUEST (0x54)

#define CIP_SERVICE_STATUS_OK   (0x00)

#define CPF_UNCONNECTED_HEADER_SIZE (16)
#define CPF_CONNECTED_HEADER_SIZE (20)

/* CPF definitions */

/* Unconnected data item type */
#define CPF_UNCONNECTED_ADDRESS_ITEM ((uint16_t)0x0000)
#define CPF_UNCONNECTED_DATA_ITEM ((uint16_t)0x00B2)
#define CPF_CONNECTED_ADDRESS_ITEM ((uint16_t)0x00A1)
#define CPF_CONNECTED_DATA_ITEM ((uint16_t)0x00B1)

/* forward open constants */
#define FORWARD_OPEN_SECONDS_PER_TICK (10)
#define FORWARD_OPEN_TIMEOUT_TICKS  (5)
#define CIP_VENDOR_ID ((uint16_t)0xF33D)
#define CIP_VENDOR_SERIAL_NUMBER ((uint32_t)0x21504345)   /* "!pce" */
#define CIP_TIMEOUT_MULTIPLIER (1)
#define CIP_RPI_uS (1000000)
#define CIP_CONNECTION_TYPE (0xA3)


#define MAX_CIP_PATH_SIZE (256)


struct cip_layer_state_s {
    plc_p plc;

    bool forward_open_ex_enabled;

    bool is_connected;

    uint32_t conn_params;

    uint16_t sequence_id;
    uint16_t max_cip_payload;
    uint32_t connection_id;
    uint32_t plc_connection_id;

    int cip_header_start_offset;

    bool is_dhp;
    uint8_t dhp_port;
    uint8_t dhp_dest;

    int encoded_path_size;
    uint8_t encoded_path[];
};


static int cip_layer_initialize(void *context);
static int cip_layer_connect(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end);
static int cip_layer_disconnect(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end);
static int cip_layer_prepare(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_id);
static int cip_layer_fix_up_request(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_id);
static int cip_layer_process_response(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_id);
static int cip_layer_destroy_layer(void *context);

static int encode_cip_path(struct cip_layer_state_s *state, uint8_t *data, int data_capacity, int *data_offset, const char *path);
static int encode_bridge_segment(struct cip_layer_state_s *state, uint8_t *data, int data_capacity, int *data_offset, const char **path_segments, int *segment_index);
static int encode_dhp_addr_segment(struct cip_layer_state_s *state, uint8_t *data, int data_capacity, int *data_offset, const char **path_segments, int *segment_index);
static int encode_numeric_segment(struct cip_layer_state_s *state, uint8_t *data, int data_capacity, int *data_offset, const char **path_segments, int *segment_index);




int cip_layer_setup(plc_p plc, int layer_index, attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    struct cip_layer_state_s *state = NULL;
    const char *path = NULL;
    int encoded_path_size = 0;

    pdebug(DEBUG_INFO, "Starting.");

    path = attr_get_str(attribs, "path", "");

    /* call once for the size and validation. */
    rc = encode_cip_path(state, NULL, MAX_CIP_PATH_SIZE, &encoded_path_size, path);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Path, \"%s\", check failed with error %s!", path, plc_tag_decode_error(rc));
        return rc;
    }

    /* now we have the size so we can allocate the state. */

    state = mem_alloc((int)(unsigned int)sizeof(*state) + encoded_path_size);
    if(!state) {
        pdebug(DEBUG_WARN, "Unable to allocate CIP layer state!");
        return PLCTAG_ERR_NO_MEM;
    }

    state->is_connected = FALSE;
    state->plc = plc;

    /* now encode it for real */
    state->encoded_path_size = 0;
    rc = encode_cip_path(state, &(state->encoded_path[0]), encoded_path_size, &(state->encoded_path_size), path);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Path, \"%s\", encoding failed with error %s!", path, plc_tag_decode_error(rc));
        return rc;
    }

    /* finally set up the layer. */
    rc = plc_set_layer(plc,
                       layer_index,
                       state,
                       cip_layer_initialize,
                       cip_layer_connect,
                       cip_layer_disconnect,
                       cip_layer_prepare,
                       cip_layer_fix_up_request,
                       cip_layer_process_response,
                       cip_layer_destroy_layer);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error setting up layer!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


/*
 * reset our state back to something sane.
 */
int cip_layer_initialize(void *context)
{
    int rc = PLCTAG_STATUS_OK;
    struct cip_layer_state_s *state = (struct cip_layer_state_s *)context;

    pdebug(DEBUG_INFO, "Starting.");

    state->is_connected = FALSE;

    state->connection_id = (uint32_t)rand() & (uint32_t)0xFFFFFFFF;
    state->sequence_id = (uint16_t)rand() & 0xFFFF;

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



int cip_layer_connect(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end)
{
    int rc = PLCTAG_STATUS_OK;
    struct cip_layer_state_s *state = (struct cip_layer_state_s *)context;
    int offset = *data_start;
    int max_payload_size = buffer_capacity - *data_start;

    pdebug(DEBUG_INFO, "Building CIP connect packet.");

    if(state->is_connected == TRUE) {
        pdebug(DEBUG_WARN, "Connect called while CIP layer is already connected!");
        return PLCTAG_ERR_BUSY;
    }

    /* check space */
    if(max_payload_size < 92) { /* MAGIC */
        pdebug(DEBUG_WARN, "Insufficient space to build CIP connection request!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    offset = 0;

    do {
        /* build a Forward Open or Forward Open Extended request. */

        /* header part. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, 0);
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, 5); /* TODO MAGIC */

        /* now the unconnected CPF (Common Packet Format) */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, 2); /* two items. */

        /* first item, the address. */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, 0); /* Null Address Item type */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, 0); /* null address length = 0 */

        /* second item, the payload description */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, CPF_UNCONNECTED_DATA_ITEM);
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, (*data_end < offset ? 0 : *data_end - offset));

        /* now the connection manager request. */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, CIP_FORWARD_OPEN_REQUEST);
        TRY_SET_BYTE(buffer, buffer_capacity, offset, 2); /* size in words of the path. */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, 0x20); /* class, 8-bits */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, 0x06); /* Connection Manager class */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, 0x24); /* instance, 8-bits */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, 0x01); /* Connection Manager, instance 1 */


        /* the actual Forward Open parameters */

        /* overall timeout parameters. */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, FORWARD_OPEN_SECONDS_PER_TICK);
        TRY_SET_BYTE(buffer, buffer_capacity, offset, FORWARD_OPEN_TIMEOUT_TICKS);

        /* connection ID params. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, 0); /* will be returned with the PLC's connection ID */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, state->connection_id); /* our connection ID */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, state->sequence_id++);   /* our connection sequence ID */

        /* identify us */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, CIP_VENDOR_ID);
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, CIP_VENDOR_SERIAL_NUMBER);

        /* timeout multiplier */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, CIP_TIMEOUT_MULTIPLIER);

        /* reserved space */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, 0);
        TRY_SET_BYTE(buffer, buffer_capacity, offset, 0);
        TRY_SET_BYTE(buffer, buffer_capacity, offset, 0);

        /* Our connection params */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, CIP_RPI_uS);
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, state->conn_params);

        /* the PLC's connection params that we are requesting. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, CIP_RPI_uS);
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, state->conn_params);

        /* What kind of connection are we asking for?  Class 3, connected, application trigger. */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, CIP_CONNECTION_TYPE);

        /* copy the encoded path */
        for(int index = 0; index < state->encoded_path_size; index++) {
            TRY_SET_BYTE(buffer, buffer_capacity, offset, state->encoded_path[index]);
        }
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s while copying encoded path!", plc_tag_decode_error(rc));
            break;
        }

        /* the next payload will start after this one. */
        *data_start = offset;
        pdebug(DEBUG_DETAIL, "Set data_start=%d", *data_start);

        /* if the data end is not set, then make it the maximum we can have. */
        if(*data_end < *data_start) {
            *data_end = buffer_capacity;
            pdebug(DEBUG_DETAIL, "Set data_end=%d", *data_end);
        }
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, building CIP forward open request!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



int cip_layer_disconnect(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end)
{
    int rc = PLCTAG_STATUS_OK;
    struct cip_layer_state_s *state = (struct cip_layer_state_s *)context;
    int offset = *data_start;
    int max_payload_size = buffer_capacity - *data_start;

    pdebug(DEBUG_INFO, "Building CIP connect packet.");

    if(state->is_connected == TRUE) {
        pdebug(DEBUG_WARN, "Connect called while CIP layer is already connected!");
        return PLCTAG_ERR_BUSY;
    }

    /* check space */
    if(max_payload_size < 92) { /* MAGIC */
        pdebug(DEBUG_WARN, "Insufficient space to build CIP connection request!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    offset = 0;

    do {
        /* build a Forward Close request. */

        /* header part. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, 0);
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, 5); /* TODO MAGIC */

        /* now the unconnected CPF (Common Packet Format) */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, 2); /* two items. */

        /* first item, the address. */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, 0); /* Null Address Item type */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, 0); /* null address length = 0 */

        /* second item, the payload type and length */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, CPF_UNCONNECTED_DATA_ITEM);
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, (*data_end < offset ? 0 : *data_end - offset));

        /* now the connection manager request. */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, CIP_FORWARD_CLOSE_REQUEST);
        TRY_SET_BYTE(buffer, buffer_capacity, offset, 2); /* size in words of the path. */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, 0x20); /* class, 8-bits */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, 0x06); /* Connection Manager class */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, 0x24); /* instance, 8-bits */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, 0x01); /* Connection Manager, instance 1 */

        /* overall timeout parameters. */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, FORWARD_OPEN_SECONDS_PER_TICK);
        TRY_SET_BYTE(buffer, buffer_capacity, offset, FORWARD_OPEN_TIMEOUT_TICKS);

        /* connection ID params. */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, state->sequence_id++);   /* our connection sequence ID */

        /* identify us */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, CIP_VENDOR_ID);
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, CIP_VENDOR_SERIAL_NUMBER);

        /* copy the encoded path */
        for(int index = 0; index < state->encoded_path_size; index++) {
            TRY_SET_BYTE(buffer, buffer_capacity, offset, state->encoded_path[index]);
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error %s while copying encoded path!", plc_tag_decode_error(rc));
            break;
        }

        /* There is no next payload. */
        *data_start = offset;
        *data_end = offset;

        pdebug(DEBUG_DETAIL, "Set data_start=%d", *data_start);
        pdebug(DEBUG_DETAIL, "Set data_end=%d", *data_end);
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, building CIP forward open request!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



int cip_layer_prepare(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_id)
{
    int rc = PLCTAG_STATUS_OK;
    struct cip_layer_state_s *state = (struct cip_layer_state_s *)context;
    int needed_capacity = (state->is_connected == TRUE ? CPF_CONNECTED_HEADER_SIZE : CPF_UNCONNECTED_HEADER_SIZE);
    int remaining_capacity = buffer_capacity - *data_start;

    (void)context;
    (void)buffer;
    (void)req_id;

    pdebug(DEBUG_INFO, "Preparing layer for building a request.");

    /* allocate space for the CIP header. */
    if(remaining_capacity < needed_capacity) {
        pdebug(DEBUG_WARN, "Buffer size, (%d) is too small for CIP CPF header (size %d)!", remaining_capacity, needed_capacity);
        return PLCTAG_ERR_TOO_SMALL;
    }

    state->cip_header_start_offset = *data_start;

    if(*data_start < needed_capacity) {
        *data_start = needed_capacity;
    }

    if(*data_end < needed_capacity) {
        *data_end = buffer_capacity;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




int cip_layer_fix_up_request(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_id)
{
    int rc = PLCTAG_STATUS_OK;
    struct cip_layer_state_s *state = (struct cip_layer_state_s *)context;
    int offset = *data_start;
    int payload_size = 0;

    pdebug(DEBUG_INFO, "Building a request.");

    /* set this to something.   Not used at this layer. */
    *req_id = 1;

    /* check to see if we are connected or not. */
    if(!state->is_connected) {
        /* nope.*/
        pdebug(DEBUG_WARN, "CIP session is not connected!");
        return PLCTAG_ERR_BAD_CONNECTION;
    }

    /* build CPF header. */
    do {
        payload_size = *data_end - (*data_start + CPF_CONNECTED_HEADER_SIZE);
        if(payload_size <= 2) {  /* MAGIC - leave space for the sequence ID */
            pdebug(DEBUG_WARN, "Insufficient space for payload!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        /* header part. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, 0);
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, 5); /* TODO MAGIC */

        /* now the connected CPF (Common Packet Format) */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, 2); /* two items. */

        /* first item, the connected address. */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, CPF_CONNECTED_ADDRESS_ITEM); /* Null Address Item type */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, 4); /* address length = 4 */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, state->plc_connection_id);

        /* second item, the data item and size */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, CPF_CONNECTED_DATA_ITEM); /* Null Address Item type */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, payload_size); /* data length, note includes the sequence ID below! */

        /* set the connection sequence id */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, state->sequence_id++);

        /* mark where the payload packet can start. */
        *data_start = offset;
    } while(0);


    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build CIP header packet, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




int cip_layer_process_response(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_id)
{
    int rc = PLCTAG_STATUS_OK;
    struct cip_layer_state_s *state = (struct cip_layer_state_s *)context;
    int offset = *data_start;
    int payload_size = *data_end - *data_start;
    int min_decode_size = (state->is_connected == TRUE ? CPF_CONNECTED_HEADER_SIZE : CPF_UNCONNECTED_HEADER_SIZE);

    pdebug(DEBUG_INFO, "Processing CIP response.");

    /* there is only one CIP response in a packet. */
    *req_id = 1;

    /* we at least have the header */
    do {
        if(payload_size < min_decode_size) {
            pdebug(DEBUG_DETAIL, "Amount of data is insufficient to decode CPF header.");
            rc = PLCTAG_ERR_PARTIAL;
            break;
        }

        /* we have enough to decode the CPF header, which kind is it? */
        if(state->is_connected == FALSE) {
            uint32_t dummy_u32;
            uint16_t dummy_u16;
            uint16_t cpf_payload_size;
            uint8_t cip_service_code;

            /* get the interface handle and router timeout, discard */
            TRY_GET_U32_LE(buffer, buffer_capacity, offset, dummy_u32);
            TRY_GET_U16_LE(buffer, buffer_capacity, offset, dummy_u16);

            /* get the CPF header */
            TRY_GET_U16_LE(buffer, buffer_capacity, offset, dummy_u16); /* item count */
            TRY_GET_U16_LE(buffer, buffer_capacity, offset, dummy_u16); /* null address item */
            TRY_GET_U16_LE(buffer, buffer_capacity, offset, dummy_u16); /* null address size */
            TRY_GET_U16_LE(buffer, buffer_capacity, offset, dummy_u16); /* unconnected data item */
            TRY_GET_U16_LE(buffer, buffer_capacity, offset, cpf_payload_size); /* payload size */

            if(payload_size < (CPF_UNCONNECTED_HEADER_SIZE + (int)(unsigned int)cpf_payload_size)) {
                pdebug(DEBUG_DETAIL, "Insufficient data for full payload.");
                rc = PLCTAG_ERR_PARTIAL;
                break;
            }

            /* we might have a Forward Open reply */
            if(cpf_payload_size < 4) {
                pdebug(DEBUG_WARN, "Illformed CIP response packet.");
                rc = PLCTAG_ERR_BAD_REPLY;
                break;
            }

            TRY_GET_BYTE(buffer, buffer_capacity, offset, cip_service_code);

            /* don't destructively get this as we might not handle it. */
            cip_service_code = buffer[offset];

            if(cip_service_code == (CIP_FORWARD_OPEN_REQUEST | CIP_CMD_EXECUTED_FLAG) || cip_service_code == (CIP_FORWARD_OPEN_REQUEST | CIP_CMD_EXECUTED_FLAG)) {
                uint8_t dummy_u8;
                uint8_t status;
                uint8_t status_size;
                uint16_t extended_status;

                /* now move past the service code */
                offset++;

                /* it is a response to Forward Open, one of them at least. */
                TRY_GET_BYTE(buffer, buffer_capacity, offset, dummy_u8); /* reserved byte. */
                TRY_GET_BYTE(buffer, buffer_capacity, offset, status); /* status byte. */
                TRY_GET_BYTE(buffer, buffer_capacity, offset, status_size); /* extended status size in 16-bit words. */

                if(status == CIP_SERVICE_STATUS_OK) {
                    /* get the target PLC's connection ID and save it. */
                    TRY_GET_U32_LE(buffer, buffer_capacity, offset, state->plc_connection_id);

                    /* TODO - decode some of the rest of the packet, might be useful. */

                    *data_start = *data_end;

                    state->is_connected = TRUE;

                    rc = PLCTAG_STATUS_OK;
                    break;
                } else {
                    /* Oops, now check to see what to do. */
                    if(status == 0x01 && status_size >= 2) {
                        uint16_t extended_status;

                        /* we might have an error that tells us the actual size to use. */
                        TRY_GET_U16_LE(buffer, buffer_capacity, offset, extended_status);

                        if(extended_status == 0x109) { /* MAGIC */
                            uint16_t supported_size = 0;

                            TRY_GET_U16_LE(buffer, buffer_capacity, offset, supported_size);

                            pdebug(DEBUG_WARN, "Error from Forward Open request, unsupported size, but size %d is supported.", supported_size);

                            state->max_cip_payload = supported_size;

                            /* retry */
                            rc = PLCTAG_STATUS_PENDING;
                            break;
                        } else if(extended_status == 0x100) { /* MAGIC */
                            pdebug(DEBUG_WARN, "Error from forward open request, duplicate connection ID.  Need to try again.");
                            /* retry */
                            rc = PLCTAG_STATUS_PENDING;
                            break;
                        } else {
                            // pdebug(DEBUG_WARN, "CIP error %d (%s)!", decode_cip_error_short(&fo_resp->general_status), decode_cip_error_long(&fo_resp->general_status));
                            pdebug(DEBUG_WARN, "CIP error %x (extended error %x)!", (unsigned int)status, (unsigned int)extended_status);
                            rc = PLCTAG_ERR_REMOTE_ERR;
                            break;
                        }
                    } else {
                        // pdebug(DEBUG_WARN, "CIP error code %s (%s)!", decode_cip_error_short(&fo_resp->general_status), decode_cip_error_long(&fo_resp->general_status));
                        pdebug(DEBUG_WARN, "CIP error %x!", (unsigned int)status,);
                        rc = PLCTAG_ERR_REMOTE_ERR;
                        break;
                    }
                }

            }

            *data_start = *data_start + CPF_CONNECTED_HEADER_SIZE + 2;
            break;
        } else {
            /* the only thing here we expect is a Forward Open response. */
            *data_start = *data_start + CPF_UNCONNECTED_HEADER_SIZE;
        }
    } while(0);

    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_PARTIAL) {
        pdebug(DEBUG_WARN, "Unable to process CIP header packet, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




int cip_layer_destroy_layer(void *context)
{
    int rc = PLCTAG_STATUS_OK;
    struct cip_layer_state_s *state = (struct cip_layer_state_s *)context;

    pdebug(DEBUG_INFO, "Cleaning up CIP layer.");

    if(state) {
        if(state->path) {
            mem_free(state->path);
            state->path = NULL;
        }

        mem_free(state);
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


int encode_cip_path(struct cip_layer_state_s *state, uint8_t *data, int data_capacity, int *data_offset, const char *path)
{
    int rc = PLCTAG_STATUS_OK;
    int segment_index = 0;
    const char **path_segments = NULL;
    int path_len = 0;
    int path_len_offset = 0;

    pdebug(DEBUG_INFO, "Starting with path \"%s\".", path);

    do {
        if(!path) {
            pdebug(DEBUG_WARN, "Path string pointer is null!");
            rc = PLCTAG_ERR_NULL_PTR;
            break;
        }

        /* split the path and then encode the parts. */
        path_segments = str_split(path, ",");
        if(!path_segments) {
            pdebug(DEBUG_WARN, "Unable to split path string!");
            rc = PLCTAG_ERR_NO_MEM;
            break;
        }

        path_len_offset = *data_offset;
        TRY_SET_BYTE(data, data_capacity, *data_offset, 0); /* filler */

        while(path_segments[segment_index] != NULL) {
            if(str_length(path_segments[segment_index]) == 0) {
                pdebug(DEBUG_WARN, "Path segment %d is zero length!", segment_index+1);
                rc = PLCTAG_ERR_BAD_PARAM;
                break;
            }

            /* try the different types. */
            do {
                rc = encode_bridge_segment(data, data_capacity, data_offset, path_segments, &segment_index);
                if(rc != PLCTAG_ERR_NO_MATCH) break;

                rc = encode_dhp_addr_segment(data, data_capacity, data_offset, path_segments, &segment_index);
                if(rc != PLCTAG_ERR_NO_MATCH) break;

                rc = encode_numeric_segment(data, data_capacity, data_offset, path_segments, &segment_index);
                if(rc != PLCTAG_ERR_NO_MATCH) break;
            } while(0);

            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to match segment %d \"%s\", error %s.", segment_index + 1, path_segments[segment_index], plc_tag_decode_error(rc));
                break;
            }
        }

        if(rc != PLCTAG_STATUS_OK) {
            break;
        }

        /* build the routing part. */
        if(state->is_dhp) {
            /* build the DH+ encoding. */
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x20); /* class, 8-bit id */
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0xA6); /* DH+ Router class */
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x24); /* instance, 8-bit id */
            TRY_SET_BYTE(data, data_capacity, *data_offset, state->dhp_port); /* port as 8-bit value */
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x2C); /* ?? 8-bit id */
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x01); /* maybe an 8-bit instance id? */
        } else {
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x20); /* class, 8-bit id */
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x02); /* Message Router class */
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x24); /* instance, 8-bit id */
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x01); /* Message Router instance */
        }

        /*
         * zero pad the path to a multiple of 16-bit
         * words.
         */
        path_len = *data_offset - (path_len_offset + 1);
        pdebug(DEBUG_DETAIL,"Path length before %d", path_len);
        if(path_len & 0x01) {
            TRY_SET_BYTE(data, data_capacity, *data_offset, 0x00); /* pad with zero bytes */
            path_len++;
        }

        /* back fill the path length, in 16-bit words. */
        TRY_SET_BYTE(data, data_capacity, path_len_offset, path_len/2);
    } while(0);

    if(path_segments) {
        mem_free(path_segments);
    }

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Done with path \"\%s\".", path);
    } else {
        pdebug(DEBUG_WARN, "Unable to encode CIP path, error %s!", plc_tag_decode_error(rc));
    }

    return rc;
}


int encode_bridge_segment(struct cip_layer_state_s *state, uint8_t *data, int data_capacity, int *data_offset, const char **path_segments, int *segment_index)
{
    int rc = PLCTAG_STATUS_OK;
    int local_seg_index = *segment_index;
    const char *port_seg = NULL;
    int port = 0;
    const char *addr = NULL;
    int addr_len = 0;
    int next_dot = 0;
    int num_dots = 0;

    (void)state;

    pdebug(DEBUG_DETAIL, "Starting with path segment \"%s\"", path_segments[*segment_index]);

    /* the pattern we want is a port specifier, 18/A, or 19/B, followed by a dotted quad IP address. */

    do {
        if(path_segments[*segment_index] == NULL) {
            pdebug(DEBUG_WARN, "Segment is NULL!");
            rc = PLCTAG_ERR_NULL_PTR;
            break;
        }

        /* is there a next element? */
        if(path_segments[local_seg_index + 1] == NULL) {
            pdebug(DEBUG_DETAIL, "Need two segments to match a bridge, but there is only one left.  Not a bridge segment.");
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        /* match the port. */
        port_seg = path_segments[local_seg_index];
        if(str_cmp_i("a",port_seg) == 0 || str_cmp("18", port_seg) == 0) {
            pdebug(DEBUG_DETAIL, "Matched first bridge segment with port A.");
            port = 18;
        } else if(str_cmp_i("b",port_seg) == 0 || str_cmp("19", port_seg) == 0) {
            pdebug(DEBUG_DETAIL, "Matched first bridge segment with port B.");
            port = 19;
        } else {
            pdebug(DEBUG_DETAIL, "Segment \"%s\" is not a matching port for a bridge segment.", port_seg);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        /* match the IP address.
         *
         * We want a dotted quad.   Each octet must be 0-255.
         */

        addr = path_segments[local_seg_index + 1];

        /* do initial sanity checks */
        addr_len = str_length(addr);
        if(addr_len < 7) {
            pdebug(DEBUG_DETAIL, "Checked for an IP address, but found \"%s\".", addr);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        if(addr_len > 15) {
            pdebug(DEBUG_DETAIL, "Possible address segment, \"%s\", is too long to be a valid IP address.", addr);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        /* is the addr part only digits and dots? */
        for(int index = 0; index < addr_len; index++) {
            if(!isdigit(addr[index]) && addr[index] != '.') {
                pdebug(DEBUG_DETAIL, "The possible address string, \"%s\", contains characters other than digits and dots.  Not an IP address.", addr);
                rc = PLCTAG_ERR_NO_MATCH;
                break;
            }

            if(addr[index] == '.') {
                num_dots++;
            }
        }
        if(rc != PLCTAG_STATUS_OK) {
            break;
        }

        if(num_dots != 3) {
            pdebug(DEBUG_DETAIL, "The possible address string \"%s\" is not a valid dotted quad.", addr);
        }

        /* TODO add checks to make sure each octet is in 0-255 */

        /* build the encoded segment. */
        TRY_SET_BYTE(data, data_capacity, *data_offset, port);

        /* address length */
        TRY_SET_BYTE(data, data_capacity, *data_offset, addr_len);

        /* copy the address string data. */
        for(int index = 0; index < addr_len; index++) {
            TRY_SET_BYTE(data, data_capacity, *data_offset, addr[index]);
        }
    } while(0);

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Matched bridge segment.");
        *segment_index += 2;
    } else {
        pdebug(DEBUG_DETAIL, "Did not match bridge segment.");
    }

    return rc;
}


int encode_dhp_addr_segment(struct cip_layer_state_s *state, uint8_t *data, int data_capacity, int *data_offset, const char **path_segments, int *segment_index)
{
    int rc = PLCTAG_STATUS_OK;
    int local_seg_index = *segment_index;
    const char *port_seg = NULL;
    int port = 0;
    const char *addr = NULL;
    int addr_len = 0;
    int addr_index = 0;
    int val = 0;

    pdebug(DEBUG_DETAIL, "Starting with path segment \"%s\"", path_segments[*segment_index]);

    /* the pattern we want is port:src:dest where src can be ignored and dest is 0-255.
     * Port needs to be 2/A, or 3/B. */

    do {
        /* sanity checks. */
        addr = path_segments[*segment_index];

        if(addr == NULL) {
            pdebug(DEBUG_WARN, "Segment is NULL!");
            rc = PLCTAG_ERR_NULL_PTR;
            break;
        }

        addr_len = str_length(addr);

        if(addr_len < 5) {
            pdebug(DEBUG_DETAIL, "Possible DH+ address segment, \"%s\", is too short to be a valid IP address.", addr);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        if(addr_len > 9) {
            pdebug(DEBUG_DETAIL, "Possible DH+ address segment, \"%s\", is too long to be a valid IP address.", addr);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        /* scan the DH+ address */

        /* Get the port part. */
        addr_index = 0;
        switch(addr[addr_index]) {
            case 'A':
                /* fall through */
            case 'a':
                /* fall through */
            case '2':
                port = 1;
                break;

            case 'B':
                /* fall through */
            case 'b':
                /* fall through */
            case '3':
                port = 2;
                break;

            default:
                pdebug(DEBUG_DETAIL, "Possible DH+ address segment, \"%s\", does not have a valid port identifier.", addr);
                return PLCTAG_ERR_NO_MATCH;
                break;
        }

        addr_index++;

        /* is the next character a colon? */
        if(addr[addr_index] != ':') {
            pdebug(DEBUG_DETAIL, "Possible DH+ address segment, \"%s\", does not have a colon after the port.", addr);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        addr_index++;

        /* get the source node */
        val = 0;
        while(isdigit(addr[addr_index])) {
            val = (val * 10) + (addr[addr_index] - '0');
            addr_index++;
        }

        /* is the source node a valid number? */
        if(val < 0 || val > 255) {
            pdebug(DEBUG_WARN, "Source node DH+ address part is out of bounds (0 <= %d < 256).", val);
            rc = PLCTAG_ERR_BAD_PARAM;
            break;
        }

        /* we ignore the source node. */

        addr_index++;

        /* is the next character a colon? */
        if(addr[addr_index] != ':') {
            pdebug(DEBUG_DETAIL, "Possible DH+ address segment, \"%s\", does not have a colon after the port.", addr);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        addr_index++;

        /* get the destination node */
        val = 0;
        while(isdigit(addr[addr_index])) {
            val = (val * 10) + (addr[addr_index] - '0');
            addr_index++;
        }

        /* is the destination node a valid number? */
        if(val < 0 || val > 255) {
            pdebug(DEBUG_WARN, "Destination node DH+ address part is out of bounds (0 <= %d < 256).", val);
            rc = PLCTAG_ERR_BAD_PARAM;
            break;
        }

        /* store the destination node for later */
        state->is_dhp = TRUE;
        state->dhp_port = (uint8_t)(unsigned int)port;
        state->dhp_dest = (uint8_t)(unsigned int)val;
    } while(0);

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Matched DH+ segment.");
        (*segment_index)++;
    } else {
        pdebug(DEBUG_DETAIL, "Did not match DH+ segment.");
    }

    return rc;
}



int encode_numeric_segment(struct cip_layer_state_s *state, uint8_t *data, int data_capacity, int *data_offset, const char **path_segments, int *segment_index)
{
    int rc = PLCTAG_STATUS_OK;
    const char *segment = path_segments[*segment_index];
    int seg_len = 0;
    int val = 0;
    int seg_index = 0;

    (void)state;

    pdebug(DEBUG_DETAIL, "Starting with segment \"%s\".", segment);

    do {
        if(segment == NULL) {
            pdebug(DEBUG_WARN, "Segment is NULL!");
            rc = PLCTAG_ERR_NULL_PTR;
            break;
        }

        seg_len = str_length(segment);

        if(seg_len > 3) {
            pdebug(DEBUG_DETAIL, "Possible numeric address segment, \"%s\", is too long to be valid.", segment);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        seg_index = 0;
        while(isdigit(segment[seg_index])) {
            val = (val * 10) + (segment[seg_index] - '0');
            seg_index++;
        }

        if(!isdigit(segment[seg_index]) && segment[seg_index] != 0) {
            pdebug(DEBUG_DETAIL, "Possible numeric address segment, \"%s\", contains non-numeric characters.", segment);
            rc = PLCTAG_ERR_NO_MATCH;
            break;
        }

        if(val < 0 || val > 255) {
            pdebug(DEBUG_WARN, "Numeric segment must be between 0 and 255, inclusive!");
            rc = PLCTAG_ERR_BAD_PARAM;
            break;
        }

        TRY_SET_BYTE(data, data_capacity, *data_offset, val);
    } while(0);

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_DETAIL, "Matched numeric segment.");
        *segment_index ++;
    } else {
        pdebug(DEBUG_DETAIL, "Did not match numeric segment.");
    }

    return rc;
}



