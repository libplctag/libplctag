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
#include <util/protocol.h>
#include <util/slice.h>
#include <util/socket.h>
#include <util/string.h>


#define EIP_DEFAULT_PORT (44818)
#define EIP_VERSION ((uint16_t)1)

/* set the initial buffer size fairly low. */
#define INITIAL_BUFFER_SIZE (200)
#define SESSION_REQUEST_RESPONSE_SIZE (28)

#define REGISTER_SESSION_CMD ((uint16_t)(0x65))
#define SEND_UNCONNECTED_DATA ((uint16_t)0x6F)
#define SEND_CONNECTED_DATA ((uint16_t)0x70)

#define TRY_SET_BYTE(out, off, val) rc = slice_set_byte(out, off, (uint8_t)(val)); if(rc != PLCTAG_STATUS_OK) break; else (off)++

struct eip_s {
    struct protocol_s protocol;

    sock_p socket;

    /* session data */
    uint32_t session_handle;
    uint64_t session_context;

    /* packet buffer */
    uint8_t *data;
    int data_capacity;
    int data_size;
    int data_offset;
};

#define EIP_STACK "EIP"

static int eip_constructor(const char *protocol_key, attr attribs, protocol_p *protocol);
static void eip_rc_destroy(void *plc_arg);

/* calls to handle requests to this protocol layer. */
static int new_eip_request_callback(protocol_p protocol, protocol_request_p request);
static int cleanup_eip_request_callback(protocol_p protocol, protocol_request_p request);
static int build_cip_request_result_callback(protocol_p protocol, protocol_request_p request, int status, slice_t used_slice, slice_t *new_output_slice);
static int process_cip_response_result_callback(protocol_p protocol, protocol_request_p request, int status, slice_t used_slice, slice_t *new_input_slice);

static void write_session_request_packet(sock_p socket, void *eip_arg);
static void read_session_request_response(sock_p socket, void *eip_arg);



protocol_p eip_get(attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    const char *host = NULL;
    const char *path = NULL;
    const char *protocol_key = NULL;
    eip_p result = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    host = attr_get_str(attribs, "gateway", "");
    path = attr_get_str(attribs, "path", "");

    /* if the host is empty, error. */
    if(!host || str_length(host) == 0) {
        pdebug(DEBUG_WARN, "Gateway must not be empty or null!");
        return NULL;
    }

    /* create the protocol key, a copy will be made, so destroy this after use. */
    protocol_key = str_concat(EIP_STACK, "/", host, "/", path);
    if(!protocol_key) {
        pdebug(DEBUG_WARN, "Unable to allocate protocol key string!");
        return PLCTAG_ERR_NO_MEM;
    }

    rc = protocol_get(protocol_key, attribs, (protocol_p *)&result, eip_constructor);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to get EIP protocol stack, error %s!", plc_tag_decode_error(rc));
        mem_free(protocol_key);
        rc_dec(result);
        return NULL;
    }

    /* we are done with the protocol key */
    mem_free(protocol_key);

    pdebug(DEBUG_INFO, "Done.");

    return (protocol_p)result;
}



int eip_constructor(const char *protocol_key, attr attribs, protocol_p *protocol)
{
    int rc = PLCTAG_STATUS_OK;
    eip_p result = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    result = (eip_p)rc_alloc(sizeof(*result), eip_rc_destroy);
    if(result) {
        const char *host_args = attr_get_str(attribs, "gateway", NULL);
        char **host_segments = NULL;
        const char *host = NULL;
        int port = EIP_DEFAULT_PORT;

        rc = protocol_init((protocol_p)result, protocol_key, new_eip_request_callback, cleanup_eip_request_callback);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to initialize new protocol, error %s!", plc_tag_decode_error(rc));
            rc_dec(result);
            return rc;
        }

        if(!host_args || str_length(host_args) == 0) {
            pdebug(DEBUG_WARN, "Host/gateway not provided!");
            rc_dec(result);
            return PLCTAG_ERR_BAD_GATEWAY;
        }

        host_segments = str_split(host_args, ":");

        if(!host_segments) {
            pdebug(DEBUG_WARN, "Unable to split gateway string!");
            rc_dec(result);
            return PLCTAG_ERR_BAD_GATEWAY;
        }

        host = host_segments[0];

        if(!host || str_length(host) == 0) {
            pdebug(DEBUG_WARN, "Host/gateway not provided!");
            mem_free(host_segments);
            rc_dec(result);
            return PLCTAG_ERR_BAD_GATEWAY;
        }

        if(host_segments[1] && str_length(host_segments[1])) {
            rc = str_to_int(host_segments[1], &port);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to parse port number, error %s!", plc_tag_decode_error(rc));
                mem_free(host_segments);
                rc_dec(result);
                return PLCTAG_ERR_BAD_GATEWAY;
            }

            if(port <= 0 || port > 65535) {
                pdebug(DEBUG_WARN, "Port value (%d) must be between 0 and 65535!", port);
                mem_free(host_segments);
                rc_dec(result);
                return PLCTAG_ERR_BAD_GATEWAY;
            }
        }

        rc = socket_create(&(result->socket));
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, trying to create new socket!", plc_tag_decode_error(rc));
            mem_free(host_segments);
            rc_dec(result);
            return PLCTAG_ERR_BAD_CONNECTION;
        }

        /* TODO - push this to a separate thread to avoid blocking. */
        rc = socket_tcp_connect(result->socket, host, port);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, trying to create new socket!", plc_tag_decode_error(rc));
            mem_free(host_segments);
            rc_dec(result);
            return PLCTAG_ERR_BAD_CONNECTION;
        }

        mem_free(host_segments);


        /* set up the initial data buffer. */
        result->data = mem_alloc(INITIAL_BUFFER_SIZE);
        if(!result->data) {
            pdebug(DEBUG_WARN, "Unable to allocate initial buffer!");
            rc_dec(result);
            return PLCTAG_ERR_NO_MEM;
        }

        result->data_capacity = INITIAL_BUFFER_SIZE;

        result->session_context = rand();

        /* kick off the session set up process. */
        rc = socket_callback_when_write_ready(result->socket, write_session_request_packet, result);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, trying to set socket callback!", plc_tag_decode_error(rc));
            rc_dec(result);
            return PLCTAG_ERR_BAD_CONNECTION;
        }

        *protocol = (protocol_p)result;
    } else {
        pdebug(DEBUG_WARN, "Unable to allocate EIP stack!");
        *protocol = NULL;
        return PLCTAG_ERR_NO_MEM;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



void eip_rc_destroy(void *eip_arg)
{
    eip_p eip = (eip_p)eip_arg;

    if(!eip) {
        pdebug(DEBUG_WARN, "Destructor function called with null pointer!");
        return;
    }

    pdebug(DEBUG_INFO, "Starting for protocol layer %s.", eip->protocol.protocol_key);

    /* destroy EIP specific features first, then destroy the generic protocol. */

    /* TODO - if the connect thread is still running, terminate it and join with it. */

    if(eip->socket) {
        socket_close(eip->socket);
        socket_destroy(&(eip->socket));
        eip->socket = NULL;
    }

    if(eip->data) {
        mem_free(eip->data);
        eip->data = NULL;
    }

    /* destroy the generic protocol */
    protocol_cleanup((protocol_p)&(eip->protocol));

    pdebug(DEBUG_INFO, "Done.");
}




int new_eip_request_callback(protocol_p protocol, protocol_request_p request)
{
    int rc = PLCTAG_STATUS_OK;
    eip_p eip = (eip_p)protocol;

    (void)request;

    if(!eip) {
        pdebug(DEBUG_WARN, "Called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_INFO, "Starting for protocol layer %s.", eip->protocol.protocol_key);

    /* set up a callback when the socket can write. */
    if(eip->socket) {
        rc = socket_callback_when_write_ready(eip->socket, write_eip_packet, eip);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, setting up callback on socket!", plc_tag_decode_error(rc));
            return rc;
        }
    } else {
        pdebug(DEBUG_WARN, "Socket is not created!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_INFO, "Done for protocol layer %s.", eip->protocol.protocol_key);

    return rc;
}


int cleanup_eip_request_callback(protocol_p protocol, protocol_request_p request);
int build_cip_request_result_callback(protocol_p protocol, protocol_request_p request, int status, slice_t used_slice, slice_t *new_output_slice);
int process_cip_response_result_callback(protocol_p protocol, protocol_request_p request, int status, slice_t used_slice, slice_t *new_input_slice);




void write_session_request_packet(sock_p socket, void *eip_arg)
{
    int rc = PLCTAG_STATUS_OK;
    eip_p eip = (eip_p)eip_arg;

    pdebug(DEBUG_INFO, "Starting for EIP layer %s.", eip->protocol.protocol_key);

    /* do we have a session registration request built? */
    if(eip->data_size == 0) {
        do {
            slice_t output = slice_make(eip->data, eip->data_capacity);
            int offset = 0;

            /* register session command. */
            TRY_SET_BYTE(output, offset, (REGISTER_SESSION_CMD & 0xFF));
            TRY_SET_BYTE(output, offset, ((REGISTER_SESSION_CMD >> 8) & 0xFF));

            /* command length. */
            TRY_SET_BYTE(output, offset, 4);
            TRY_SET_BYTE(output, offset, 0);

            /* session handle, zero here. */
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);

            /* session status, zero here. */
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);

            /* session context, zero here. */
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);

            /* options, unused, zero here. */
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);

            /* requested EIP version. */
            TRY_SET_BYTE(output, offset, (EIP_VERSION & 0xFF));
            TRY_SET_BYTE(output, offset, ((EIP_VERSION >> 8) & 0xFF));

            /* requested EIP options. */
            TRY_SET_BYTE(output, offset, 0);
            TRY_SET_BYTE(output, offset, 0);

            eip->data_offset = 0;
            eip->data_size = offset;
        } while(0);

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Set up of session registration request had error %s!", plc_tag_decode_error(rc));

            /* TODO - remove the callback? */

            /* this is probably fatal. */
            return rc;
        }
    }

    /* do we have anything to write? */
    if(eip->data_offset < eip->data_size) {
        int num_bytes_written = socket_tcp_write(socket, eip->data + eip->data_offset, (eip->data_size - eip->data_offset));

        /* was there an error? */
        if(num_bytes_written < 0) {
            pdebug(DEBUG_WARN, "Error, %s, writing to socket!", plc_tag_decode_error(rc));
            return rc;
        } else {
            eip->data_offset += num_bytes_written;

            /* are we done? */
            if(eip->data_offset < eip->data_size) {
                rc = socket_enable_write_ready_callback(socket, write_session_request_packet, eip);
            } else {
                /* done! */
                eip->data_size = 0;
                eip->data_offset = 0;
                rc = socket_enable_read_ready_callback(socket, read_session_request_response, eip);
            }
        }


    }


    pdebug(DEBUG_INFO, "Done for EIP layer %s.", eip->protocol.protocol_key);

    return rc;
}





void read_session_request_response(sock_p socket, void *eip_arg)
{
    int rc = PLCTAG_STATUS_OK;
    eip_p eip = (eip_p)eip_arg;
    int bytes_read = 0;

    pdebug(DEBUG_INFO, "Starting for EIP layer %s.", eip->protocol.protocol_key);

    /* try to read the data. */
    bytes_read = socket_tcp_read(socket, eip->data + eip->data_size, eip->data_capacity - eip->data_size);
    if(bytes_read < 0) {
        /* error ! */
        pdebug(DEBUG_WARN, "Error, %s, reading socket!", plc_tag_decode_error(bytes_read));
        return;
    }

    eip->data_size += bytes_read;

    /* are we done? */
    if(eip->data_size < SESSION_REQUEST_RESPONSE_SIZE) {
        pdebug(DEBUG_DETAIL, "Did not get complete session request response packet yet.  Continuing to wait.");
        socket_callback_when_read_ready(socket, read_session_request_response, eip);
    } else {
        uint32_t status;

        pdebug(DEBUG_DETAIL, "Got full session request response packet:");
        pdebug_dump_bytes(DEBUG_DETAIL, eip->data, eip->data_size);

        status = (uint32_t)eip->data[8]
               | (uint32_t)((uint32_t)(eip->data[9]) << 8)
               | (uint32_t)((uint32_t)(eip->data[10]) << 16)
               | (uint32_t)((uint32_t)(eip->data[11]) << 24);

        if(status != 0) {
            pdebug(DEBUG_WARN, "Got error code %u back from session request!", (unsigned int)status);
            return;
        }

        /* peel out the data we care about. */
        eip->session_handle = 0;

        eip->session_handle = (uint32_t)eip->data[4]
                            | (uint32_t)((uint32_t)(eip->data[5]) << 8)
                            | (uint32_t)((uint32_t)(eip->data[6]) << 16)
                            | (uint32_t)((uint32_t)(eip->data[7]) << 24);
    }

    pdebug(DEBUG_INFO, "Done for EIP layer %s.", eip->protocol.protocol_key);
}




void write_eip_packet(sock_p socket, void *eip_arg)
{
    int rc = PLCTAG_STATUS_OK;
    eip_p eip = (eip_p)eip_arg;
    int bytes_written = 0;

    pdebug(DEBUG_INFO, "Starting for EIP layer %s.", eip->protocol.protocol_key);

    /* do we need to build a packet first? */
    if(eip->data_size == 0 && protocol_has_requests((protocol_p)eip)) {
        rc = build_eip_packet(eip);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while attempting to build EIP packet!", plc_tag_decode_error(rc));
            return;
        }
    }

    bytes_written = socket_tcp_write(eip->socket, eip->data + eip->data_offset, eip->data_size - eip->data_offset);
    if(bytes_written < 0) {
        pdebug(DEBUG_WARN, "Error, %s, while writing data!", plc_tag_decode_error(bytes_written));
        return;
    }

    eip->data_offset += bytes_written;

    /* do we need to continue? */
    if(eip->data_offset < eip->data_size) {
        rc = socket_callback_when_write_ready(eip->socket, write_eip_packet, eip);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while enabling write callback on socket!", plc_tag_decode_error(rc));
            return;
        }
    } else {
        /* done, so wait for a read. */

        eip->data_size = 0;
        eip->data_offset = 0;

        rc = socket_callback_when_read_ready(eip->socket, read_eip_packet, eip);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, while enabling write callback on socket!", plc_tag_decode_error(rc));
            return;
        }
    }

    pdebug(DEBUG_INFO, "Done for EIP layer %s.", eip->protocol.protocol_key);
}


int build_eip_packet(eip_p eip)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for EIP layer %s.", eip->protocol.protocol_key);

    /* set up the buffer. */
    eip->data_size = 0;
    eip->data_offset = 0;

    /* a number of things can go wrong, fake exceptions. */
    do {
        /* build the header */
        slice_t output = slice_make(eip->data, eip->data_capacity);
        slice_t payload = slice_make_err(0);
        int offset = 0;
        int header_length = 0;
        int packet_length_offset = 0;
        int session_context_offset = 0;

        /* register session command. */
        TRY_SET_BYTE(output, offset, 0); /* fill in later */
        TRY_SET_BYTE(output, offset, 0);

        /* packet length. */
        packet_length_offset = offset;
        TRY_SET_BYTE(output, offset, 0); /* fill in later */
        TRY_SET_BYTE(output, offset, 0);

        /* session handle.  */
        TRY_SET_BYTE(output, offset, (eip->session_handle & 0xFF));
        TRY_SET_BYTE(output, offset, ((eip->session_handle >> 8) & 0xFF));
        TRY_SET_BYTE(output, offset, ((eip->session_handle >> 16) & 0xFF));
        TRY_SET_BYTE(output, offset, ((eip->session_handle >> 24) & 0xFF));

        /* session status, zero here. */
        TRY_SET_BYTE(output, offset, 0);
        TRY_SET_BYTE(output, offset, 0);
        TRY_SET_BYTE(output, offset, 0);
        TRY_SET_BYTE(output, offset, 0);

        /* session context. */
        session_context_offset = offset;  /* might fill in later */
        TRY_SET_BYTE(output, offset, 0);
        TRY_SET_BYTE(output, offset, 0);
        TRY_SET_BYTE(output, offset, 0);
        TRY_SET_BYTE(output, offset, 0);
        TRY_SET_BYTE(output, offset, 0);
        TRY_SET_BYTE(output, offset, 0);
        TRY_SET_BYTE(output, offset, 0);
        TRY_SET_BYTE(output, offset, 0);

        /* options, unused, zero here. */
        TRY_SET_BYTE(output, offset, 0);
        TRY_SET_BYTE(output, offset, 0);
        TRY_SET_BYTE(output, offset, 0);
        TRY_SET_BYTE(output, offset, 0);

        header_length = offset;

        rc = protocol_build_request((protocol_p)eip, slice_from_slice(output, offset, slice_len(output)), &payload, NULL);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error, %s, building request!", plc_tag_decode_error(rc));
            break;
        }

        /* fill in the length */
        offset = packet_length_offset;
        TRY_SET_BYTE(output, offset, slice_len(payload) & 0xFF);
        TRY_SET_BYTE(output, offset, (slice_len(payload) >> 8) & 0xFF);

        /* HACKS HAX HAX! Peek in the payload to see what kind of EIP packet we need to send. */
        if(slice_get_byte(payload, 8) == 0) {
            /* unconnected. */
            offset = 0;
            TRY_SET_BYTE(output, offset, SEND_UNCONNECTED_DATA & 0xFF);
            TRY_SET_BYTE(output, offset, (SEND_UNCONNECTED_DATA >> 8) & 0xFF);

            /* if we are sending an unconnected payload, we need to send the session context too. */
            offset = session_context_offset;
            eip->session_context++;
            TRY_SET_BYTE(output, offset, (eip->session_context & 0xFF));
            TRY_SET_BYTE(output, offset, ((eip->session_context >> 8) & 0xFF));
            TRY_SET_BYTE(output, offset, ((eip->session_context >> 16) & 0xFF));
            TRY_SET_BYTE(output, offset, ((eip->session_context >> 24) & 0xFF));
            TRY_SET_BYTE(output, offset, ((eip->session_context >> 32) & 0xFF));
            TRY_SET_BYTE(output, offset, ((eip->session_context >> 40) & 0xFF));
            TRY_SET_BYTE(output, offset, ((eip->session_context >> 48) & 0xFF));
            TRY_SET_BYTE(output, offset, ((eip->session_context >> 56) & 0xFF));
        } else {
            /* connected packet */
            offset = 0;
            TRY_SET_BYTE(output, offset, SEND_CONNECTED_DATA & 0xFF);
            TRY_SET_BYTE(output, offset, (SEND_CONNECTED_DATA >> 8) & 0xFF);
        }

        eip->data_size = header_length + slice_len(payload);

        pdebug(DEBUG_INFO, "Build packet:");
        pdebug_dump_bytes(DEBUG_INFO, eip->data, eip->data_size);

    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error while setting up EIP packet!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done for EIP layer %s.", eip->protocol.protocol_key);

    return rc;
}