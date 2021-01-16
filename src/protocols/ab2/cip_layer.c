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

#define CIP_FORWARD_OPEN_REQUEST (0x54)
#define FORWARD_OPEN_REQUEST_SIZE (88)
#define FORWARD_OPEN_REQUEST_EX_SIZE (92)

/* CPF definitions */

/* Unconnected data item type */
#define CPF_UNCONNECTED_DATA_ITEM ((uint16_t)0x00B2)


struct cip_layer_state_s {
    plc_p plc;

    bool forward_open_ex_enabled;

    bool is_connected;

    uint32_t conn_params;

    uint32_t connection_id;
    uint16_t sequence_id;

    const char *path;
};


static int cip_layer_initialize(void *context);
static int cip_layer_connect(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end);
static int cip_layer_disconnect(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end);
static int cip_layer_prepare(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_id);
static int cip_layer_fix_up_request(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_id);
static int cip_layer_process_response(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_id);
static int cip_layer_destroy_layer(void *context);


static int cip_encode_path(uint8_t *data, int data_capacity, int *data_offset, const char *path);
static int encode_dhp_addr_segment(uint8_t *data, int data_capacity, int *data_offset, const char *segment, uint8_t *port, uint8_t *src_node, uint8_t *dest_node);
static int encode_ip_addr_segment(uint8_t *data, int data_capacity, int *data_offset, const char *segment, uint8_t *conn_path, size_t *conn_path_index);
static int match_numeric_segment(uint8_t *data, int data_capacity, int *data_offset, const char *segment, uint8_t *conn_path, size_t *conn_path_index);





int cip_layer_setup(plc_p plc, int layer_index, attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    struct cip_layer_state_s *state = NULL;
    ab2_plc_type_t plc_type;

    pdebug(DEBUG_INFO, "Starting.");

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
        pdebug(DEBUG_WARN, "Error setting layer!");
        return rc;
    }

    state = mem_alloc(sizeof(*state));
    if(!state) {
        pdebug(DEBUG_WARN, "Unable to allocate CIP layer state!");
        return PLCTAG_ERR_NO_MEM;
    }

    state->is_connected = FALSE;
    state->plc = plc;

    state->path = str_dup(attr_get_str(attribs, "path", ""));
    if(state->path) {
        pdebug(DEBUG_WARN, "Unable to copy path string!");
        mem_free(state);
        return PLCTAG_ERR_NO_MEM;
    }

    plc_type = ab2_get_plc_type(attribs);


    /* STOPPED
        * Untangle cip path encoding.
           * split path into segments.
           * encode each segment type.
        * make DHP check public so that it can be used in the tag code. */

    /* TODO set up path */

    switch(plc_type) {
        case AB2_PLC_PLC5:
            /* fall through */
        case AB2_PLC_SLC:
            /* fall through */
        case AB2_PLC_MLGX:
            /* fall through */
        case AB2_PLC_LGX_PCCC:
            pdebug(DEBUG_DETAIL, "Setting up for PCCC-based PLC connection.");
            state->forward_open_ex_enabled = FALSE;
            state->conn_params = CIP_PLC5_CONN_PARAM;
            break;

        case AB2_PLC_LGX:
            pdebug(DEBUG_DETAIL, "Setting up for Logix PLC connection.");
            state->forward_open_ex_enabled = TRUE;
            state->conn_params = CIP_LGX_CONN_PARAM_EX | LOGIX_LARGE_PAYLOAD_SIZE;
            break;

        case AB2_PLC_MLGX800:
            pdebug(DEBUG_DETAIL, "Setting up for Micro800 PLC connection.");
            state->forward_open_ex_enabled = FALSE;
            state->conn_params = CIP_LGX_CONN_PARAM;
            break;

        case AB2_PLC_OMRON_NJNX:
            pdebug(DEBUG_DETAIL, "Setting up for Omron NJ/NX PLC connection.");
            state->forward_open_ex_enabled = FALSE;
            state->conn_params = CIP_LGX_CONN_PARAM;
            break;

        default:
            pdebug(DEBUG_WARN, "Unsupported PLC type!");
            return PLCTAG_ERR_UNSUPPORTED;
            break;
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

    pdebug(DEBUG_INFO, "Initializing CIP layer.");

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
    if(max_payload_size < FORWARD_OPEN_REQUEST_SIZE) {
        pdebug(DEBUG_WARN, "Insufficient space to build CIP connection request!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    offset = 0;

    do {
        /* build a Forward Open or Forward Open Extended request. */

    // uint32_le interface_handle;      /* ALWAYS 0 */
    // uint16_le router_timeout;        /* in seconds */

    // /* Common Packet Format - CPF Unconnected */
    // uint16_le cpf_item_count;        /* ALWAYS 2 */
    // uint16_le cpf_nai_item_type;     /* ALWAYS 0 */
    // uint16_le cpf_nai_item_length;   /* ALWAYS 0 */
    // uint16_le cpf_udi_item_type;     /* ALWAYS 0x00B2 - Unconnected Data Item */
    // uint16_le cpf_udi_item_length;   /* REQ: fill in with length of remaining data. */

    // /* CM Service Request - Connection Manager */
    // uint8_t cm_service_code;        /* ALWAYS 0x54 Forward Open Request */
    // uint8_t cm_req_path_size;       /* ALWAYS 2, size in words of path, next field */
    // uint8_t cm_req_path[4];         /* ALWAYS 0x20,0x06,0x24,0x01 for CM, instance 1*/

    // /* Forward Open Params */
    // uint8_t secs_per_tick;          /* seconds per tick */
    // uint8_t timeout_ticks;          /* timeout = srd_secs_per_tick * src_timeout_ticks */
    // uint32_le orig_to_targ_conn_id;  /* 0, returned by target in reply. */
    // uint32_le targ_to_orig_conn_id;  /* what is _our_ ID for this connection, use ab_connection ptr as id ? */
    // uint16_le conn_serial_number;    /* our connection ID/serial number */
    // uint16_le orig_vendor_id;        /* our unique vendor ID */
    // uint32_le orig_serial_number;    /* our unique serial number */
    // uint8_t conn_timeout_multiplier;/* timeout = mult * RPI */
    // uint8_t reserved[3];            /* reserved, set to 0 */

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

#define FORWARD_OPEN_SECONDS_PER_TICK (10)
#define FORWARD_OPEN_TIMEOUT_TICKS  (5)
#define CIP_VENDOR_ID ((uint16_t)0xF33D)
#define CIP_VENDOR_SERIAL_NUMBER ((uint32_t)0x21504345)   /* "!pce" */
#define CIP_TIMEOUT_MULTIPLIER (1)
#define CIP_RPI_uS (1000000)
#define CIP_CONNECTION_TYPE (0xA3)

        /* the actual Forward Open parameters */

        /* overall timeout parameters. */
        TRY_SET_BYTE(buffer, buffer_capacity, offset, FORWARD_OPEN_SECONDS_PER_TICK);
        TRY_SET_BYTE(buffer, buffer_capacity, offset, FORWARD_OPEN_TIMEOUT_TICKS);

        /* connection ID params. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, 0); /* will be returned with the PLC's connection ID */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, state->connection_id); /* our connection ID */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, state->sequence_id);   /* our connection sequence ID */

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

        rc = cip_path_encode(buffer, buffer_capacity, &offset, state->path);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to encode CIP path!");
            break;
        }



    uint32_le orig_to_targ_rpi;      /* us to target RPI - Request Packet Interval in microseconds */
    uint16_le orig_to_targ_conn_params; /* some sort of identifier of what kind of PLC we are??? */
    uint32_le targ_to_orig_rpi;      /* target to us RPI, in microseconds */
    uint16_le targ_to_orig_conn_params; /* some sort of identifier of what kind of PLC the target is ??? */
    uint8_t transport_class;        /* ALWAYS 0xA3, server transport, class 3, application trigger */
    uint8_t path_size;              /* size of connection path in 16-bit words
                                     * connection path from MSG instruction.
                                     *
                                     * EG LGX with 1756-ENBT and CPU in slot 0 would be:
                                     * 0x01 - backplane port of 1756-ENBT
                                     * 0x00 - slot 0 for CPU
                                     * 0x20 - class
                                     * 0x02 - MR Message Router
                                     * 0x24 - instance
                                     * 0x01 - instance #1.
                                     */

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

        /* requested CIP version. */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, CIP_VERSION);

        /* requested CIP options. */
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
int cip_layer_disconnect(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end)
{
    int rc = PLCTAG_STATUS_OK;
    struct cip_layer_state_s *state = (struct cip_layer_state_s *)context;
    int offset = 0;

    pdebug(DEBUG_INFO, "Building CIP disconnect packet.");

    if(state->is_connected == FALSE) {
        pdebug(DEBUG_WARN, "Disconnect called while CIP layer is not connected!");
        return PLCTAG_ERR_BUSY;
    }

    /* check space */
    if(buffer_capacity < SESSION_REQUEST_SIZE) {
        pdebug(DEBUG_WARN, "Insufficient space to build session request!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    offset = 0;

    do {
        /* command */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, UNREGISTER_SESSION_CMD);

        /* packet/payload length. */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, 4);

        /* session handle, session handle from session setup. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, state->session_handle);

        /* session status, zero here. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, 0);

        /* session context, zero here. */
        TRY_SET_U64_LE(buffer, buffer_capacity, offset, 0);

        /* options, unused, zero here. */
        TRY_SET_U32_LE(buffer, buffer_capacity, offset, 0);

        /* payload */

        /* requested CIP version. */
        TRY_SET_U16_LE(buffer, buffer_capacity, offset, CIP_VERSION);

        /* requested CIP options. */
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




int cip_layer_prepare(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_id)
{
    int rc = PLCTAG_STATUS_OK;

    (void)context;
    (void)buffer;
    (void)req_id;

    pdebug(DEBUG_INFO, "Preparing layer for building a request.");

    /* allocate space for the CIP header. */
    if(buffer_capacity < CIP_HEADER_SIZE) {
        pdebug(DEBUG_WARN, "Buffer size, (%d) is too small for CIP header (size %d)!", buffer_capacity, CIP_HEADER_SIZE);
        return PLCTAG_ERR_TOO_SMALL;
    }

    if(*data_start < CIP_HEADER_SIZE) {
        *data_start = CIP_HEADER_SIZE;
    }

    if(*data_end < CIP_HEADER_SIZE) {
        *data_end = buffer_capacity;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




int cip_layer_fix_up_request(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_id)
{
    int rc = PLCTAG_STATUS_OK;
    struct cip_layer_state_s *state = (struct cip_layer_state_s *)context;
    int offset = 0;
    int payload_size = 0;
    uint16_t command = SEND_UNCONNECTED_DATA_CMD;

    pdebug(DEBUG_INFO, "Building a request.");

    /* set this to something.   Not used at this layer. */
    *req_id = 1;

    /* check to see if we are connected or not. */
    if(!state->is_connected) {
        /* nope.*/
        pdebug(DEBUG_WARN, "CIP session is not connected!");
        return PLCTAG_ERR_BAD_CONNECTION;
    }

    if(buffer_capacity < CIP_HEADER_SIZE) {
        pdebug(DEBUG_WARN, "Insufficient space for new request!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    if(*data_end <= CIP_HEADER_SIZE) {
        pdebug(DEBUG_WARN, "data end is %d, payload would be zero length!", *data_end);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* this might be the first call. */

    /* what kind of request is it? Cheat and peak at the payload. */
    if(*data_end >= CIP_HEADER_SIZE + 10) {
        uint16_t address_item_type = 0;

        address_item_type = (uint16_t)(buffer[CIP_HEADER_SIZE + 8])
                          + (uint16_t)(((uint16_t)buffer[CIP_HEADER_SIZE + 9]) << 8);

        if(address_item_type == 0) {
            /* unconnected message. */
            command = SEND_UNCONNECTED_DATA_CMD;
        } else {
            command = SEND_CONNECTED_DATA_CMD;
        }
    } else {
        command = SEND_UNCONNECTED_DATA_CMD;
    }

    /* the payload starts after the header. */
    *data_start = CIP_HEADER_SIZE;

    /* calculate the payload size. */
    payload_size = *data_end - *data_start;

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

        /* fix up data start for next layer */
        *data_start = CIP_HEADER_SIZE;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build CIP header packet, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




// int cip_layer_prepare_for_response(void *context)
// {
//     int rc = PLCTAG_STATUS_OK;

//     (void)context;

//     pdebug(DEBUG_INFO, "Preparing for response.");

//     pdebug(DEBUG_INFO, "Done.");

//     return rc;
// }




int cip_layer_process_response(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id *req_id)
{
    int rc = PLCTAG_STATUS_OK;
    struct cip_layer_state_s *state = (struct cip_layer_state_s *)context;
    uint16_t command = 0;
    uint16_t payload_size = 0;
    uint32_t status = 0;
    uint32_t session_handle = 0;
    int offset = 0;

    pdebug(DEBUG_INFO, "Processing CIP response.");

    /* there is only one CIP response in a packet. */
    *req_id = 1;

    if(buffer_capacity < CIP_HEADER_SIZE){
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
        if(*data_end < (int)(unsigned int)(payload_size + CIP_HEADER_SIZE)) {
            pdebug(DEBUG_DETAIL, "Need more data than the header, we need %d bytes and have %d bytes.", (int)(unsigned int)(payload_size + CIP_HEADER_SIZE), *data_end);
            rc = PLCTAG_ERR_PARTIAL;
            break;
        }

        if(status != 0) {
            pdebug(DEBUG_WARN, "Got bad CIP status %d!", (int)(unsigned int)status);
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* what we do depends on the command */
        if(command == REGISTER_SESSION_CMD) {
            /* copy the information into the state. */
            state->session_handle = session_handle;

            state->is_connected = TRUE;

            /* signal that we have consumed the whole payload. */
            *data_end = 0;
            *data_start = 0;
        } else {
            /* other layers will need to process this. */
            *data_start = CIP_HEADER_SIZE;
            *data_end = payload_size + CIP_HEADER_SIZE;
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




int cip_encode_path(uint8_t *data, int data_capacity, int *data_offset, const char *path)
{
    size_t path_len = 0;
    size_t conn_path_index = 0;
    size_t path_index = 0;
    int is_dhp = 0;
    uint8_t dhp_port;
    uint8_t dhp_src_node;
    uint8_t dhp_dest_node;
    uint8_t tmp_conn_path[MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN];

    pdebug(DEBUG_DETAIL, "Starting");

    // if(!path || str_length(path) == (size_t)0) {
    //     pdebug(DEBUG_DETAIL, "Path is NULL or empty.");
    //     return PLCTAG_ERR_NULL_PTR;
    // }

    path_len = (size_t)(ssize_t)str_length(path);

    while(path_index < path_len && path[path_index] && conn_path_index < MAX_CONN_PATH) {
        if(path[path_index] == ',') {
            /* skip separators. */
            pdebug(DEBUG_DETAIL, "Skipping separator character '%c'.", (char)path[path_index]);

            path_index++;
        } else if(match_numeric_segment(path, &path_index, tmp_conn_path, &conn_path_index) == PLCTAG_STATUS_OK) {
            pdebug(DEBUG_DETAIL, "Found numeric segment.");
        } else if(encode_ip_addr_segment(path, &path_index, tmp_conn_path, &conn_path_index) == PLCTAG_STATUS_OK) {
            pdebug(DEBUG_DETAIL, "Found IP address segment.");
        } else if(encode_dhp_addr_segment(path, &path_index, &dhp_port, &dhp_src_node, &dhp_dest_node) == PLCTAG_STATUS_OK) {
            pdebug(DEBUG_DETAIL, "Found DH+ address segment.");

            /* check if it is last. */
            if(path_index < path_len) {
                pdebug(DEBUG_WARN, "DH+ address must be the last segment in a path! %d %d", (int)(ssize_t)path_index, (int)(ssize_t)path_len);
                return PLCTAG_ERR_BAD_PARAM;
            }

            is_dhp = 1;
        } else {
            /* unknown, cannot parse this! */
            pdebug(DEBUG_WARN, "Unable to parse remaining path string from position %d, \"%s\".", (int)(ssize_t)path_index, (char*)&path[path_index]);
            return PLCTAG_ERR_BAD_PARAM;
        }
    }

    if(conn_path_index >= MAX_CONN_PATH) {
        pdebug(DEBUG_WARN, "Encoded connection path is too long (%d >= %d).", (int)(ssize_t)conn_path_index, MAX_CONN_PATH);
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(is_dhp && (plc_type == AB_PLC_PLC5 || plc_type == AB_PLC_SLC || plc_type == AB_PLC_MLGX)) {
        /* DH+ bridging always needs a connection. */
        *needs_connection = 1;

        /* add the special PCCC/DH+ routing on the end. */
        tmp_conn_path[conn_path_index + 0] = 0x20;
        tmp_conn_path[conn_path_index + 1] = 0xA6;
        tmp_conn_path[conn_path_index + 2] = 0x24;
        tmp_conn_path[conn_path_index + 3] = dhp_port;
        tmp_conn_path[conn_path_index + 4] = 0x2C;
        tmp_conn_path[conn_path_index + 5] = 0x01;
        conn_path_index += 6;

        *dhp_dest = (uint16_t)dhp_dest_node;
    } else if(!is_dhp) {
        if(*needs_connection) {
            pdebug(DEBUG_DETAIL, "PLC needs connection, adding path to the router object.");

            /*
             * we do a generic path to the router
             * object in the PLC.  But only if the PLC is
             * one that needs a connection.  For instance a
             * Micro850 needs to work in connected mode.
             */
            tmp_conn_path[conn_path_index + 0] = 0x20;
            tmp_conn_path[conn_path_index + 1] = 0x02;
            tmp_conn_path[conn_path_index + 2] = 0x24;
            tmp_conn_path[conn_path_index + 3] = 0x01;
            conn_path_index += 4;
        }

        *dhp_dest = 0;
    } else {
        /*
         *we had the special DH+ format and it was
         * either not last or not a PLC5/SLC.  That
         * is an error.
         */

        *dhp_dest = 0;

        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * zero pad the path to a multiple of 16-bit
     * words.
     */
    pdebug(DEBUG_DETAIL,"IOI size before %d", conn_path_index);
    if(conn_path_index & 0x01) {
        tmp_conn_path[conn_path_index] = 0;
        conn_path_index++;
    }

    if(conn_path_index > 0) {
        /* allocate space for the connection path */
        *conn_path = mem_alloc((int)(unsigned int)conn_path_index);
        if(! *conn_path) {
            pdebug(DEBUG_WARN, "Unable to allocate connection path!");
            return PLCTAG_ERR_NO_MEM;
        }

        mem_copy(*conn_path, &tmp_conn_path[0], (int)(unsigned int)conn_path_index);
    } else {
        *conn_path = NULL;
    }

    *conn_path_size = (uint8_t)conn_path_index;

    pdebug(DEBUG_DETAIL, "Done");

    return PLCTAG_STATUS_OK;
}



int match_numeric_segment(const char *path, size_t *path_index, uint8_t *conn_path, size_t *conn_path_index)
{
    int val = 0;
    size_t p_index = *path_index;
    size_t c_index = *conn_path_index;

    pdebug(DEBUG_DETAIL, "Starting at position %d in string %s.", (int)(ssize_t)*path_index, path);

    while(isdigit(path[p_index])) {
        val = (val * 10) + (path[p_index] - '0');
        p_index++;
    }

    /* did we match anything? */
    if(p_index == *path_index) {
        pdebug(DEBUG_DETAIL,"Did not find numeric path segment at position %d.", (int)(ssize_t)p_index);
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* was the numeric segment valid? */
    if(val < 0 || val > 0x0F) {
        pdebug(DEBUG_WARN, "Numeric segment in path at position %d is out of bounds!", (int)(ssize_t)(*path_index));
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /* store the encoded segment data. */
    conn_path[c_index] = (uint8_t)(unsigned int)(val);
    c_index++;
    *conn_path_index = c_index;

    /* bump past our last read character. */
    *path_index = p_index;

    pdebug(DEBUG_DETAIL, "Done.   Found numeric segment %d.", val);

    return PLCTAG_STATUS_OK;
}

/*
 * match symbolic IP address segments.
 *  18,10.206.10.14 - port 2/A -> 10.206.10.14
 *  19,10.206.10.14 - port 3/B -> 10.206.10.14
 */

int encode_ip_addr_segment(const char *path, size_t *path_index, uint8_t *conn_path, size_t *conn_path_index)
{
    uint8_t *addr_seg_len = NULL;
    int val = 0;
    size_t p_index = *path_index;
    size_t c_index = *conn_path_index;

    pdebug(DEBUG_DETAIL, "Starting at position %d in string %s.", (int)(ssize_t)*path_index, path);

    /* first part, the extended address marker*/
    val = 0;
    while(isdigit(path[p_index])) {
        val = (val * 10) + (path[p_index] - '0');
        p_index++;
    }

    if(val != 18 && val != 19) {
        pdebug(DEBUG_DETAIL, "Path segment at %d does not match IP address segment.", (int)(ssize_t)*path_index);
        return PLCTAG_ERR_NOT_FOUND;
    }

    if(val == 18) {
        pdebug(DEBUG_DETAIL, "Extended address on port A.");
    } else {
        pdebug(DEBUG_DETAIL, "Extended address on port B.");
    }

    /* is the next character a comma? */
    if(path[p_index] != ',') {
        pdebug(DEBUG_DETAIL, "Not an IP address segment starting at position %d of path.  Remaining: \"%s\".",(int)(ssize_t)p_index, &path[p_index]);
        return PLCTAG_ERR_NOT_FOUND;
    }

    p_index++;

    /* start building up the connection path. */
    conn_path[c_index] = (uint8_t)(unsigned int)val;
    c_index++;

    /* point into the encoded path for the symbolic segment length. */
    addr_seg_len = &conn_path[c_index];
    *addr_seg_len = 0;
    c_index++;

    /* get the first IP address digit. */
    val = 0;
    while(isdigit(path[p_index]) && (int)(unsigned int)(*addr_seg_len) < (MAX_IP_ADDR_SEG_LEN - 1)) {
        val = (val * 10) + (path[p_index] - '0');
        conn_path[c_index] = (uint8_t)path[p_index];
        c_index++;
        p_index++;
        (*addr_seg_len)++;
    }

    if(val < 0 || val > 255) {
        pdebug(DEBUG_WARN, "First IP address part is out of bounds (0 <= %d < 256) for an IPv4 octet.", val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    pdebug(DEBUG_DETAIL, "First IP segment: %d.", val);

    /* is the next character a dot? */
    if(path[p_index] != '.') {
        pdebug(DEBUG_DETAIL, "Unexpected character '%c' found at position %d in first IP address part.", path[p_index], p_index);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* copy the dot. */
    conn_path[c_index] = (uint8_t)path[p_index];
    c_index++;
    p_index++;
    (*addr_seg_len)++;

    /* get the second part. */
    val = 0;
    while(isdigit(path[p_index]) && (int)(unsigned int)(*addr_seg_len) < (MAX_IP_ADDR_SEG_LEN - 1)) {
        val = (val * 10) + (path[p_index] - '0');
        conn_path[c_index] = (uint8_t)path[p_index];
        c_index++;
        p_index++;
        (*addr_seg_len)++;
    }

    if(val < 0 || val > 255) {
        pdebug(DEBUG_WARN, "Second IP address part is out of bounds (0 <= %d < 256) for an IPv4 octet.", val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    pdebug(DEBUG_DETAIL, "Second IP segment: %d.", val);

    /* is the next character a dot? */
    if(path[p_index] != '.') {
        pdebug(DEBUG_DETAIL, "Unexpected character '%c' found at position %d in second IP address part.", path[p_index], p_index);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* copy the dot. */
    conn_path[c_index] = (uint8_t)path[p_index];
    c_index++;
    p_index++;
    (*addr_seg_len)++;

    /* get the third part. */
    val = 0;
    while(isdigit(path[p_index]) && (int)(unsigned int)(*addr_seg_len) < (MAX_IP_ADDR_SEG_LEN - 1)) {
        val = (val * 10) + (path[p_index] - '0');
        conn_path[c_index] = (uint8_t)path[p_index];
        c_index++;
        p_index++;
        (*addr_seg_len)++;
    }

    if(val < 0 || val > 255) {
        pdebug(DEBUG_WARN, "Third IP address part is out of bounds (0 <= %d < 256) for an IPv4 octet.", val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    pdebug(DEBUG_DETAIL, "Third IP segment: %d.", val);

    /* is the next character a dot? */
    if(path[p_index] != '.') {
        pdebug(DEBUG_DETAIL, "Unexpected character '%c' found at position %d in third IP address part.", path[p_index], p_index);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* copy the dot. */
    conn_path[c_index] = (uint8_t)path[p_index];
    c_index++;
    p_index++;
    (*addr_seg_len)++;

    /* get the fourth part. */
    val = 0;
    while(isdigit(path[p_index]) && (int)(unsigned int)(*addr_seg_len) < (MAX_IP_ADDR_SEG_LEN - 1)) {
        val = (val * 10) + (path[p_index] - '0');
        conn_path[c_index] = (uint8_t)path[p_index];
        c_index++;
        p_index++;
        (*addr_seg_len)++;
    }

    if(val < 0 || val > 255) {
        pdebug(DEBUG_WARN, "Fourth IP address part is out of bounds (0 <= %d < 256) for an IPv4 octet.", val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    pdebug(DEBUG_DETAIL, "Fourth IP segment: %d.", val);

    /* We need to zero pad if the length is not a multiple of two. */
    if((*addr_seg_len) && (uint8_t)0x01) {
        conn_path[c_index] = (uint8_t)0;
        c_index++;
    }

    /* set the return values. */
    *path_index = p_index;
    *conn_path_index = c_index;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * match DH+ address segments.
 *  A:1:2 - port 2/A -> DH+ node 2
 *  B:1:2 - port 3/B -> DH+ node 2
 *
 * A and B can be lowercase or numeric.
 */

int encode_dhp_addr_segment(const char *path, size_t *path_index, uint8_t *port, uint8_t *src_node, uint8_t *dest_node)
{
    int val = 0;
    size_t p_index = *path_index;

    pdebug(DEBUG_DETAIL, "Starting at position %d in string %s.", (int)(ssize_t)*path_index, path);

    /* Get the port part. */
    switch(path[p_index]) {
        case 'A':
            /* fall through */
        case 'a':
            /* fall through */
        case '2':
            *port = 1;
            break;

        case 'B':
            /* fall through */
        case 'b':
            /* fall through */
        case '3':
            *port = 2;
            break;

        default:
            pdebug(DEBUG_DETAIL, "Character '%c' at position %d does not match start of DH+ segment.", path[p_index], (int)(ssize_t)p_index);
            return PLCTAG_ERR_NOT_FOUND;
            break;
    }

    p_index++;

    /* is the next character a colon? */
    if(path[p_index] != ':') {
        pdebug(DEBUG_DETAIL, "Character '%c' at position %d does not match first colon expected in DH+ segment.", path[p_index], (int)(ssize_t)p_index);
        return PLCTAG_ERR_BAD_PARAM;
    }

    p_index++;

    /* get the source node */
    val = 0;
    while(isdigit(path[p_index])) {
        val = (val * 10) + (path[p_index] - '0');
        p_index++;
    }

    /* is the source node a valid number? */
    if(val < 0 || val > 255) {
        pdebug(DEBUG_WARN, "Source node DH+ address part is out of bounds (0 <= %d < 256).", val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    *src_node = (uint8_t)(unsigned int)val;

    /* is the next character a colon? */
    if(path[p_index] != ':') {
        pdebug(DEBUG_DETAIL, "Character '%c' at position %d does not match the second colon expected in DH+ segment.", path[p_index], (int)(ssize_t)p_index);
        return PLCTAG_ERR_BAD_PARAM;
    }

    p_index++;

    /* get the destination node */
    val = 0;
    while(isdigit(path[p_index])) {
        val = (val * 10) + (path[p_index] - '0');
        p_index++;
    }

    /* is the destination node a valid number? */
    if(val < 0 || val > 255) {
        pdebug(DEBUG_WARN, "Destination node DH+ address part is out of bounds (0 <= %d < 256).", val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    *dest_node = (uint8_t)(unsigned int)val;
    *path_index = p_index;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}
