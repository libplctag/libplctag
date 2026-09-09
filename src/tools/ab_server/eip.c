/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
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

#include "eip.h"
#include "cpf.h"
#include "err.h"
#include "fault.h"
#include "slice.h"
#include "utils.h"
#include "log.h"
#include <stdlib.h>

#define EIP_REGISTER_SESSION ((uint16_t)0x0065)
#define EIP_REGISTER_SESSION_SIZE (4) /* 4 bytes, 2 16-bit words */

#define EIP_UNREGISTER_SESSION ((uint16_t)0x0066)
#define EIP_UNCONNECTED_SEND ((uint16_t)0x006F)
#define EIP_CONNECTED_SEND ((uint16_t)0x0070)

/* supported EIP version */
#define EIP_VERSION ((uint16_t)1)


typedef struct {
    uint16_t command;
    uint16_t length;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;
} eip_header_s;


static slice_s register_session(slice_s input, slice_s output, plc_s *plc, eip_header_s *header);
static slice_s unregister_session(slice_s input, slice_s output, plc_s *plc, eip_header_s *header);


slice_s eip_dispatch_request(slice_s input, slice_s raw_output, plc_s *plc) {
    log_info("eip_dispatch_request(): raw_output size = %zu, server_to_client_max_packet = %zu", slice_len(raw_output),
             plc->server_to_client_max_packet);

    slice_s output = slice_from_slice(raw_output, 0, plc->server_to_client_max_packet);

    log_info("eip_dispatch_request(): output size = %zu", slice_len(output));

    slice_s response = slice_from_slice(output, EIP_HEADER_SIZE, slice_len(output) - EIP_HEADER_SIZE);

    eip_header_s header;

    log_info("eip_dispatch_request(): got packet:");
    log_info_slice(input);

    /* unpack header. */
    header.command = slice_get_uint16_le(input, 0);
    header.length = slice_get_uint16_le(input, 2);
    header.session_handle = slice_get_uint32_le(input, 4);
    header.status = slice_get_uint32_le(input, 8);
    header.sender_context = slice_get_uint64_le(input, 12);
    header.options = slice_get_uint32_le(input, 20);

    /* sanity checks */
    if(slice_len(input) != (size_t)(header.length + EIP_HEADER_SIZE)) {
        log_info("Illegal EIP packet.   Length should be %d but is %d!", header.length + EIP_HEADER_SIZE, slice_len(input));
        return slice_make_err(ERR_TCP_BAD_REQUEST);
    }

    /* dispatch the request */
    switch(header.command) {
        case EIP_REGISTER_SESSION:
            response =
                register_session(slice_from_slice(input, EIP_HEADER_SIZE, EIP_REGISTER_SESSION_SIZE), response, plc, &header);
            break;

        case EIP_UNREGISTER_SESSION:
            response =
                unregister_session(slice_from_slice(input, EIP_HEADER_SIZE, EIP_REGISTER_SESSION_SIZE), response, plc, &header);
            break;

        case EIP_UNCONNECTED_SEND:
            response =
                handle_cpf_unconnected(slice_from_slice(input, EIP_HEADER_SIZE, slice_len(input) - EIP_HEADER_SIZE),
                                       slice_from_slice(output, EIP_HEADER_SIZE, slice_len(output) - EIP_HEADER_SIZE), plc);
            break;

        case EIP_CONNECTED_SEND:
            response = handle_cpf_connected(slice_from_slice(input, EIP_HEADER_SIZE, slice_len(input) - EIP_HEADER_SIZE),
                                            slice_from_slice(output, EIP_HEADER_SIZE, slice_len(output) - EIP_HEADER_SIZE), plc);
            break;

        default: response = slice_make_err(ERR_TCP_UNSUPPORTED); break;
    }

    if(!slice_has_err(response)) {
        uint16_t response_command = header.command;
        uint32_t response_session = plc->session_handle;
        uint64_t response_context = header.sender_context;

        /*
         * Fault injection.  The length field still describes the bytes actually sent, because a
         * response whose length does not match its contents makes the client wait for data that
         * never comes and time out, which tests nothing.  Only the fields the client checks
         * against what it sent are corrupted here.
         */
        if(fault_fires(plc, FAULT_EIP_CMD)) { response_command = (uint16_t)(header.command + 1); }

        /*
         * Never corrupt the handle in the RegisterSession reply: that reply is where the client
         * learns the handle in the first place, so corrupting it just teaches the client the
         * wrong value and every later packet then agrees with it.  The check being tested only
         * applies once the client has a handle to compare against.
         */
        if(header.command != EIP_REGISTER_SESSION && fault_fires(plc, FAULT_SESSION)) {
            response_session = plc->session_handle ^ (uint32_t)0xA5A5A5A5;
        }

        /* the client only checks the echoed context on unconnected sends. */
        if(fault_fires(plc, FAULT_CONTEXT)) { response_context = header.sender_context ^ (uint64_t)0xA5A5A5A5A5A5A5A5ULL; }

        /* build response */
        slice_set_uint16_le(output, 0, response_command);
        slice_set_uint16_le(output, 2, (uint16_t)slice_len(response));
        slice_set_uint32_le(output, 4, response_session);
        slice_set_uint32_le(output, 8, (uint32_t)0); /* status == 0 -> no error */
        slice_set_uin64_le(output, 12, response_context);
        slice_set_uint32_le(output, 20, header.options);

        /* The payload is already in place. */
        return slice_from_slice(output, 0, EIP_HEADER_SIZE + slice_len(response));
    } else if(slice_get_err(response) == ERR_TCP_DONE) {
        /* just pass this through, normally not an error. */
        log_info("Done with connection.");

        return response;
    } else {
        /* error condition. */
        slice_set_uint16_le(output, 0, header.command);
        slice_set_uint16_le(output, 2, (uint16_t)0); /* no payload. */
        slice_set_uint32_le(output, 4, plc->session_handle);
        slice_set_uint32_le(output, 8, (uint32_t)(int32_t)slice_get_err(response)); /* status */
        slice_set_uin64_le(output, 12, header.sender_context);
        slice_set_uint32_le(output, 20, header.options);

        return slice_from_slice(output, 0, EIP_HEADER_SIZE);
    }
}


slice_s register_session(slice_s input, slice_s output, plc_s *plc, eip_header_s *header) {
    struct {
        uint16_t eip_version;
        uint16_t option_flags;
    } register_request;

    register_request.eip_version = slice_get_uint16_le(input, 0);
    register_request.option_flags = slice_get_uint16_le(input, 2);

    /* sanity checks.  The command and packet length are checked by now. */

    /* session_handle must be zero. */
    if(header->session_handle != (uint32_t)0) {
        log_info("Request failed sanity check: request session handle is %u but should be zero.", header->session_handle);

        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    /* session status must be zero. */
    if(header->status != (uint32_t)0) {
        log_info("Request failed sanity check: request status is %u but should be zero.", header->status);

        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    /* session sender plc must be zero. */
    if(header->sender_context != (uint64_t)0) {
        log_info("Request failed sanity check: request sender context should be zero.");

        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    /* session options must be zero. */
    if(header->options != (uint32_t)0) {
        log_info("Request failed sanity check: request options is %u but should be zero.", header->options);

        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    /* EIP version must be 1. */
    if(register_request.eip_version != EIP_VERSION) {
        log_info("Request failed sanity check: request EIP version is %u but should be %u.", register_request.eip_version,
                 EIP_VERSION);

        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    /* Session request option flags must be zero. */
    if(register_request.option_flags != (uint16_t)0) {
        log_info("Request failed sanity check: request option flags field is %u but should be zero.",
                 register_request.option_flags);

        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    /* all good, generate a session handle. */
    plc->session_handle = header->session_handle = (uint32_t)(random_u64(UINT32_MAX) + 1);

    /* build the response. */
    slice_set_uint16_le(output, 0, register_request.eip_version);
    slice_set_uint16_le(output, 2, register_request.option_flags);

    return slice_from_slice(output, 0, EIP_REGISTER_SESSION_SIZE);
}


slice_s unregister_session(slice_s input, slice_s output, plc_s *plc, eip_header_s *header) {
    (void)input;
    (void)output;
    (void)header;

    if(header->session_handle == plc->session_handle) {
        return slice_make_err(ERR_TCP_DONE);
    } else {
        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }
}
