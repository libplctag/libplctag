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

#include "cpf.h"
#include "cip.h"
#include "eip.h"
#include "fault.h"
#include "utils.h"
#include "log.h"
#include <stdint.h>

#define CPF_ITEM_NAI ((uint16_t)0x0000) /* NULL Address Item */
#define CPF_ITEM_CAI ((uint16_t)0x00A1) /* connected address item */
#define CPF_ITEM_CDI ((uint16_t)0x00B1) /* connected data item */
#define CPF_ITEM_UDI ((uint16_t)0x00B2) /* Unconnected data item */


typedef struct {
    uint32_t interface_handle;
    uint16_t router_timeout;
    uint16_t item_count; /* should be 2 for now. */
    uint16_t item_addr_type;
    uint16_t item_addr_length;
    uint16_t item_data_type;
    uint16_t item_data_length;
} cpf_uc_header_s;

#define CPF_UCONN_HEADER_SIZE (16)

typedef struct {
    uint32_t interface_handle;
    uint16_t router_timeout;
    uint16_t item_count; /* should be 2 for now. */
    uint16_t item_addr_type;
    uint16_t item_addr_length;
    uint32_t conn_id;
    uint16_t item_data_type;
    uint16_t item_data_length;
    uint16_t conn_seq;
} cpf_co_header_s;

#define CPF_CONN_HEADER_SIZE (22)


slice_s handle_cpf_unconnected(slice_s input, slice_s output, plc_s *plc) {
    slice_s result;
    cpf_uc_header_s header;

    log_info("handle_cpf_unconnected(): got packet:");
    log_info_slice(input);

    /* we must have some sort of payload. */
    if(slice_len(input) <= CPF_UCONN_HEADER_SIZE) {
        log_info("Unusable size of unconnected CPF packet!");
        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    /* unpack the request. */
    header.interface_handle = slice_get_uint32_le(input, 0);
    header.router_timeout = slice_get_uint16_le(input, 4);
    header.item_count = slice_get_uint16_le(input, 6);

    /* sanity check the number of items. */
    if(header.item_count != (uint16_t)2) {
        log_info("Unsupported unconnected CPF packet, expected two items but found %u!", header.item_count);
        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    header.item_addr_type = slice_get_uint16_le(input, 8);
    header.item_addr_length = slice_get_uint16_le(input, 10);
    header.item_data_type = slice_get_uint16_le(input, 12);
    header.item_data_length = slice_get_uint16_le(input, 14);

    /* sanity check the data. */
    if(header.item_addr_type != CPF_ITEM_NAI) {
        log_info("Expected null address item but found %x!", header.item_addr_type);
        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    if(header.item_addr_length != 0) {
        log_info("Expected zero address item length but found %d bytes!", header.item_addr_length);
        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    if(header.item_data_type != CPF_ITEM_UDI) {
        log_info("Expected unconnected data item but found %x!", header.item_data_type);
        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    if(header.item_data_length != (slice_len(input) - CPF_UCONN_HEADER_SIZE)) {
        log_info("CPF unconnected payload length, %d, does not match passed length, %d!",
                 (slice_len(input) - CPF_UCONN_HEADER_SIZE - 2), header.item_data_length);
        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    /* dispatch and handle the result. */
    result = cip_dispatch_unconnected_request(
        slice_from_slice(input, (size_t)CPF_UCONN_HEADER_SIZE, (size_t)((uint16_t)slice_len(input) - CPF_UCONN_HEADER_SIZE)),
        slice_from_slice(output, (size_t)CPF_UCONN_HEADER_SIZE, (size_t)((uint16_t)slice_len(output) - CPF_UCONN_HEADER_SIZE)),
        plc);

    if(!slice_has_err(result)) {
        /* build outbound header.  See handle_cpf_connected() below for what the faults do. */
        size_t payload_len = slice_len(result);
        uint16_t item_count = 2;
        uint16_t addr_item_type = CPF_ITEM_NAI;
        uint16_t declared_len = 0;

        if(fault_fires(plc, FAULT_CPF_COUNT)) { item_count = 3; }

        if(fault_fires(plc, FAULT_CPF_TYPE)) { addr_item_type = (uint16_t)0x00FF; }

        /*
         * SHORT_CIP is deliberately absent here.  The unconnected path carries the ForwardOpen
         * reply, and the client checks that one for length before it ever gets to the CIP
         * layer, so applying it here would just duplicate SHORT_CPF.  It belongs on the
         * connected path, where it reaches the read response.
         */
        if(fault_fires(plc, FAULT_SHORT_CPF)) { payload_len = 0; }

        declared_len = (uint16_t)payload_len;

        if(fault_fires(plc, FAULT_ITEM_LEN)) { declared_len = (uint16_t)(payload_len + 4); }

        slice_set_uint32_le(output, 0, header.interface_handle);
        slice_set_uint16_le(output, 4, header.router_timeout);
        slice_set_uint16_le(output, 6, item_count);    /* two items. */
        slice_set_uint16_le(output, 8, addr_item_type); /* connected address type. */
        slice_set_uint16_le(output, 10, 0);            /* No connection ID. */
        slice_set_uint16_le(output, 12, CPF_ITEM_UDI); /* connected data type */
        slice_set_uint16_le(output, 14, declared_len); /* result from CIP processing downstream. */

        /* create a new slice with the CPF header and the response packet in it. */
        result = slice_from_slice(output, (size_t)0, (size_t)(payload_len + (size_t)CPF_UCONN_HEADER_SIZE));
    }

    /* errors are pass through. */

    return result;
}


slice_s handle_cpf_connected(slice_s input, slice_s output, plc_s *plc) {
    slice_s result;
    cpf_co_header_s header;

    /* we must have some sort of payload. */
    if(slice_len(input) <= CPF_UCONN_HEADER_SIZE) {
        log_info("Unusable size of connected CPF packet!");
        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    /* unpack the request. */
    header.interface_handle = slice_get_uint32_le(input, 0);
    header.router_timeout = slice_get_uint16_le(input, 4);
    header.item_count = slice_get_uint16_le(input, 6);

    /* sanity check the number of items. */
    if(header.item_count != (uint16_t)2) {
        log_info("Unsupported connected CPF packet, expected two items but found %u!", header.item_count);
        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    header.item_addr_type = slice_get_uint16_le(input, 8);
    header.item_addr_length = slice_get_uint16_le(input, 10);
    header.conn_id = slice_get_uint32_le(input, 12);
    header.item_data_type = slice_get_uint16_le(input, 16);
    header.item_data_length = slice_get_uint16_le(input, 18);
    header.conn_seq = slice_get_uint16_le(input, 20);

    /* sanity check the data. */
    if(header.item_addr_type != CPF_ITEM_CAI) {
        log_info("Expected connected address item but found %x!", header.item_addr_type);
        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    if(header.item_addr_length != 4) {
        log_info("Expected address item length of 4 but found %d bytes!", header.item_addr_length);
        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    if(header.conn_id != plc->server_connection_id) {
        log_info("Expected connection ID %x but found connection ID %x!", plc->server_connection_id, header.conn_id);
        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    if(header.item_data_type != CPF_ITEM_CDI) {
        log_info("Expected connected data item but found %x!", header.item_data_type);
        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    if(header.item_data_length != (slice_len(input) - (CPF_CONN_HEADER_SIZE - 2))) {
        log_info("CPF payload length, %d, does not match passed length, %d!", (slice_len(input) - (CPF_CONN_HEADER_SIZE - 2)),
                 header.item_data_length);
        return slice_make_err(EIP_ERR_BAD_REQUEST);
    }

    /* do we care about the sequence ID?   Should check. */
    plc->client_connection_seq = header.conn_seq;

    /* dispatch and handle the result. */
    result = cip_dispatch_request(
        slice_from_slice(input, (size_t)CPF_CONN_HEADER_SIZE, (size_t)((uint16_t)slice_len(input) - CPF_CONN_HEADER_SIZE)),
        slice_from_slice(output, (size_t)CPF_CONN_HEADER_SIZE, (size_t)((uint16_t)slice_len(output) - CPF_CONN_HEADER_SIZE)),
        plc);

    if(!slice_has_err(result)) {
        /* build outbound header. */
        size_t offset = 0;
        size_t payload_len = slice_len(result);

        /*
         * Fault injection.  Everything the client checks about this header is derived from the
         * four values below, so they are computed first and then written normally -- the header
         * layout stays in one piece and readable.
         */
        uint16_t item_count = 2;
        uint16_t addr_item_type = CPF_ITEM_CAI;
        uint32_t conn_id = plc->client_connection_id;

        if(fault_fires(plc, FAULT_CPF_COUNT)) { item_count = 3; }

        if(fault_fires(plc, FAULT_CPF_TYPE)) { addr_item_type = (uint16_t)0x00FF; }

        if(fault_fires(plc, FAULT_CONN_ID)) { conn_id = plc->client_connection_id ^ (uint32_t)0xA5A5A5A5; }

        /*
         * Drop the CIP payload but keep the CPF item length honest, so the packet is coherent
         * all the way down to the connected data item and only the CIP layer sees something too
         * short to parse.  Chopping bytes without fixing the length would hang the client
         * instead.
         */
        if(fault_fires(plc, FAULT_SHORT_CPF)) { payload_len = 0; }

        /* as above, but leave two bytes: not enough for a CIP response header. */
        if(fault_fires(plc, FAULT_SHORT_CIP) && payload_len > 2) { payload_len = 2; }

        slice_set_uint32_le(output, offset, header.interface_handle);
        offset += 4;
        slice_set_uint16_le(output, offset, header.router_timeout);
        offset += 2;
        slice_set_uint16_le(output, offset, item_count);
        offset += 2; /* two items. */
        slice_set_uint16_le(output, offset, addr_item_type);
        offset += 2; /* connected address type. */
        slice_set_uint16_le(output, offset, 4);
        offset += 2; /* connection ID is 4 bytes. */
        slice_set_uint32_le(output, offset, conn_id);
        offset += 4;
        slice_set_uint16_le(output, offset, CPF_ITEM_CDI);
        offset += 2; /* connected data type */

        /*
         * The declared data item length normally matches the bytes that follow it.  FAULT_ITEM_LEN
         * makes it disagree while leaving the packet itself intact, which is the one case the
         * client can only catch by comparing the two.
         */
        if(fault_fires(plc, FAULT_ITEM_LEN)) {
            slice_set_uint16_le(output, offset, (uint16_t)(payload_len + 2 + 4));
        } else {
            slice_set_uint16_le(output, offset, (uint16_t)(payload_len + 2));
        }

        offset += 2; /* result from CIP processing downstream.  Plus 2 bytes for sequence number. */
        slice_set_uint16_le(output, offset, header.conn_seq);
        offset += 2;

        /* create a new slice with the CPF header and the response packet in it. */
        result = slice_from_slice(output, (size_t)0, (size_t)(payload_len + offset));
    }

    /* errors are pass through. */

    return result;
}
