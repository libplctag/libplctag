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
#include "cip.h"
#include "cpf.h"
#include "err.h"
#include "fault.h"
#include "utils.h"
#include "log.h"
#include <libplctag/api/libplctag.h>
#include <utils/byte_slice.h>
#include <utils/random_utils.h>
#include <stdbool.h>
#include <stdlib.h>

#define EIP_REGISTER_SESSION ((uint16_t)0x0065)
#define EIP_UNREGISTER_SESSION ((uint16_t)0x0066)

/* ListIdentity: no session, and over UDP as well as TCP. */
#define EIP_LIST_IDENTITY ((uint16_t)0x0063)
#define EIP_CPF_ITEM_IDENTITY ((uint16_t)0x000C)
#define EIP_IDENTITY_STATE_OPERATIONAL ((uint8_t)0x03)
#define EIP_DEFAULT_PORT (44818)
#define EIP_AF_INET ((uint16_t)2)

/* supported EIP version */
#define EIP_VERSION ((uint16_t)1)


/* the request header; status becomes the reply status once the request checks pass. */
typedef struct {
    uint16_t command;
    uint16_t length;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;
} eip_header_s;


/*
 * Command handlers write the reply body into out and return what is left of it.  On failure
 * they set header->status to the ODVA code and return out untouched.
 */
static byte_slice_t register_session(byte_slice_t in, byte_slice_t out, plc_s *plc, eip_header_s *header);
static byte_slice_t list_identity(byte_slice_t out, plc_s *plc, eip_header_s *header);
static uint32_t check_session(plc_s *plc, eip_header_s *header);


byte_slice_t eip_dispatch_request(byte_slice_t packet, byte_slice_t out, plc_s *plc) {
    eip_header_s header = {0};
    byte_slice_t in = packet;
    byte_slice_t hdr = {0};
    byte_slice_t body = {0};
    byte_slice_t rest = {0};
    byte_slice_t reply_body = {0};
    uint16_t response_command = 0;
    uint32_t response_session = 0;
    uint64_t response_context = 0;

    log_info("eip_dispatch_request(): got packet:");
    log_info_byte_slice(packet);

    in = byte_slice_read_u16_le(in, &header.command);
    in = byte_slice_read_u16_le(in, &header.length);
    in = byte_slice_read_u32_le(in, &header.session_handle);
    in = byte_slice_read_u32_le(in, &header.status);
    in = byte_slice_read_u64_le(in, &header.sender_context);
    in = byte_slice_read_u32_le(in, &header.options);

    if(byte_slice_has_error(in) || byte_slice_len(in) != header.length) {
        log_info("Illegal EIP packet.  Payload length should be %u but is %d!", header.length, byte_slice_len(in));
        header.status = EIP_STATUS_INVALID_LENGTH;
    } else if(header.status != EIP_STATUS_SUCCESS || header.options != 0) {
        /* status is only set in a reply and options is reserved: a request sends zero in both. */
        log_info("Request failed sanity check: status is %u and options is %u, both should be zero.", header.status,
                 header.options);
        header.status = EIP_STATUS_INCORRECT_DATA;
    }

    if((size_t)byte_slice_len(out) > plc->server_to_client_max_packet) {
        byte_slice_cut(out, (int32_t)plc->server_to_client_max_packet, &out, NULL);
    }

    body = byte_slice_take(out, EIP_HEADER_SIZE, &hdr);
    rest = body;

    if(header.status == EIP_STATUS_SUCCESS) {
        switch(header.command) {
            case EIP_REGISTER_SESSION: rest = register_session(in, body, plc, &header); break;

            case EIP_UNREGISTER_SESSION:
                if(header.session_handle == plc->session_handle) {
                    log_info("Done with connection.");
                    return byte_slice_fail(out, PLCTAG_ERR_CLOSE);
                }

                header.status = EIP_STATUS_INVALID_SESSION;
                break;

            case EIP_LIST_IDENTITY: rest = list_identity(body, plc, &header); break;

            case EIP_UNCONNECTED_SEND:
                if((header.status = check_session(plc, &header)) == EIP_STATUS_SUCCESS) {
                    rest = handle_cpf_unconnected(in, body, plc, &header.status);
                }
                break;

            case EIP_CONNECTED_SEND:
                if((header.status = check_session(plc, &header)) == EIP_STATUS_SUCCESS) {
                    rest = handle_cpf_connected(in, body, plc, &header.status);
                }
                break;

            default: header.status = EIP_STATUS_INVALID_COMMAND; break;
        }
    }

    /* after the command: RegisterSession has just set the handle. */
    response_command = header.command;
    response_session = plc->session_handle;
    response_context = header.sender_context;

    if(header.status != EIP_STATUS_SUCCESS) {
        rest = body;
    } else {
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
    }

    reply_body = byte_slice_used(body, rest);

    hdr = byte_slice_write_u16_le(hdr, response_command);
    hdr = byte_slice_write_u16_le(hdr, (uint16_t)byte_slice_len(reply_body));
    hdr = byte_slice_write_u32_le(hdr, response_session);
    hdr = byte_slice_write_u32_le(hdr, header.status);
    hdr = byte_slice_write_u64_le(hdr, response_context);
    hdr = byte_slice_write_u32_le(hdr, header.options);

    if(byte_slice_has_error(hdr)) { return hdr; }

    return byte_slice_used(out, rest);
}


/*
 * The session checks for SendRRData and SendUnitData.  Until this existed, nothing checked the
 * requests that carry all the actual traffic, so a client could send a malformed encapsulation
 * header for the life of a session and never hear about it.
 */
static uint32_t check_session(plc_s *plc, eip_header_s *header) {
    /* the client has to register before it can send anything else. */
    if(plc->session_handle == (uint32_t)0) {
        log_info("Request failed sanity check: no session has been registered yet.");
        return EIP_STATUS_INVALID_SESSION;
    }

    if(header->session_handle != plc->session_handle) {
        log_info("Request failed sanity check: request session handle is %u but this session is %u.",
                 header->session_handle, plc->session_handle);
        return EIP_STATUS_INVALID_SESSION;
    }

    return EIP_STATUS_SUCCESS;
}


static byte_slice_t register_session(byte_slice_t in, byte_slice_t out, plc_s *plc, eip_header_s *header) {
    uint16_t eip_version = 0;
    uint16_t option_flags = 0;

    in = byte_slice_read_u16_le(in, &eip_version);
    in = byte_slice_read_u16_le(in, &option_flags);

    if(byte_slice_has_error(byte_slice_end(in))) {
        log_info("Request failed sanity check: RegisterSession payload must be 4 bytes.");
        header->status = EIP_STATUS_INVALID_LENGTH;
        return out;
    }

    if(header->session_handle != (uint32_t)0) {
        log_info("Request failed sanity check: request session handle is %u but should be zero.", header->session_handle);
        header->status = EIP_STATUS_INCORRECT_DATA;
        return out;
    }

    if(eip_version != EIP_VERSION) {
        log_info("Request failed sanity check: request EIP version is %u but should be %u.", eip_version, EIP_VERSION);
        header->status = EIP_STATUS_UNSUPPORTED_VERSION;
        return out;
    }

    if(option_flags != (uint16_t)0) {
        log_info("Request failed sanity check: request option flags field is %u but should be zero.", option_flags);
        header->status = EIP_STATUS_INCORRECT_DATA;
        return out;
    }

    plc->session_handle = header->session_handle = (uint32_t)(random_u64(UINT32_MAX) + 1);

    out = byte_slice_write_u16_le(out, eip_version);
    return byte_slice_write_u16_le(out, option_flags);
}


/*
 * ListIdentity reply: one CPF item of type 0x0C holding the encapsulation version, a sockaddr
 * in network byte order, the Identity attributes and a state byte.
 *
 * The sockaddr address is the interface facing the sender, which scan_eip_network prints.
 * ponytail: over TCP it is 0.0.0.0, because the TCP server does not record its local address;
 * a TCP client already knows it.
 */
static byte_slice_t list_identity(byte_slice_t out, plc_s *plc, eip_header_s *header) {
    uint16_t port = (uint16_t)(plc->port_str ? atoi(plc->port_str) : EIP_DEFAULT_PORT);
    byte_slice_t item_len = {0};
    byte_slice_t item = {0};
    byte_slice_t rest = {0};

    rest = byte_slice_write_u16_le(out, 1); /* one item */
    rest = byte_slice_write_u16_le(rest, EIP_CPF_ITEM_IDENTITY);
    item = byte_slice_take(rest, 2, &item_len); /* filled in once the item is written */

    rest = byte_slice_write_u16_le(item, EIP_VERSION);
    rest = byte_slice_write_u16_be(rest, EIP_AF_INET);
    rest = byte_slice_write_u16_be(rest, port);
    rest = byte_slice_write_u32_be(rest, plc->local_ip);
    rest = byte_slice_write_u64_be(rest, 0); /* sin_zero */

    rest = cip_encode_identity(rest, plc);
    rest = byte_slice_write_u8(rest, EIP_IDENTITY_STATE_OPERATIONAL);

    byte_slice_write_u16_le(item_len, (uint16_t)byte_slice_len(byte_slice_used(item, rest)));

    if(byte_slice_has_error(rest)) {
        header->status = EIP_STATUS_INSUFFICIENT_MEMORY;
        return out;
    }

    log_info("Answering ListIdentity.");

    return rest;
}
