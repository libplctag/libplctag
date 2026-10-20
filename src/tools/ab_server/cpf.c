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
#include "err.h"
#include "fault.h"
#include "utils.h"
#include "log.h"
#include <libplctag/api/libplctag.h>
#include <utils/byte_slice.h>
#include <stdbool.h>
#include <stdint.h>

#define CPF_ITEM_NAI ((uint16_t)0x0000) /* NULL Address Item */
#define CPF_ITEM_CAI ((uint16_t)0x00A1) /* connected address item */
#define CPF_ITEM_CDI ((uint16_t)0x00B1) /* connected data item */
#define CPF_ITEM_UDI ((uint16_t)0x00B2) /* Unconnected data item */

#define CPF_ITEM_COUNT ((uint16_t)2)     /* address item and data item */
#define CPF_CONN_ID_SIZE ((uint16_t)4)   /* connected address item payload */
#define CPF_CONN_SEQ_SIZE ((uint16_t)2)  /* sequence number at the start of a connected data item */
#define CPF_UCONN_HEADER_SIZE (16)
#define CPF_CONN_HEADER_SIZE (22)


static byte_slice_t read_prefix(byte_slice_t in, uint32_t *interface_handle, uint16_t *router_timeout);
static byte_slice_t read_item_header(byte_slice_t in, uint16_t *type, uint16_t *length);
static byte_slice_t write_prefix(byte_slice_t out, uint32_t interface_handle, uint16_t router_timeout, uint16_t item_count);
static byte_slice_t write_item_header(byte_slice_t out, uint16_t type, uint16_t length);


byte_slice_t handle_cpf_unconnected(byte_slice_t in, byte_slice_t out, plc_s *plc, uint32_t *eip_status) {
    uint32_t interface_handle = 0;
    uint16_t router_timeout = 0;
    uint16_t addr_type = 0;
    uint16_t addr_len = 0;
    uint16_t data_type = 0;
    uint16_t data_len = 0;
    uint16_t item_count = CPF_ITEM_COUNT;
    uint16_t declared_len = 0;
    byte_slice_t hdr = {0};
    byte_slice_t body = {0};
    byte_slice_t rest = {0};
    int32_t payload_len = 0;

    log_info("handle_cpf_unconnected(): got packet:");
    log_info_byte_slice(in);

    in = read_prefix(in, &interface_handle, &router_timeout);
    in = read_item_header(in, &addr_type, &addr_len);
    in = read_item_header(in, &data_type, &data_len);

    if(byte_slice_has_error(in) || addr_type != CPF_ITEM_NAI || addr_len != 0 || data_type != CPF_ITEM_UDI
       || data_len != byte_slice_len(in) || data_len == 0) {
        log_info("Bad unconnected CPF packet: address item %x/%u, data item %x/%u, %d payload bytes.", addr_type, addr_len,
                 data_type, data_len, byte_slice_len(in));
        *eip_status = EIP_STATUS_INCORRECT_DATA;
        return out;
    }

    body = byte_slice_take(out, CPF_UCONN_HEADER_SIZE, &hdr);
    rest = cip_dispatch_request(in, body, plc, true);
    payload_len = byte_slice_len(byte_slice_used(body, rest));

    /* See handle_cpf_connected() below for what the faults do. */
    if(fault_fires(plc, FAULT_CPF_COUNT)) { item_count = 3; }

    if(fault_fires(plc, FAULT_CPF_TYPE)) { addr_type = (uint16_t)0x00FF; }

    /*
     * SHORT_CIP is deliberately absent here.  The unconnected path carries the ForwardOpen
     * reply, and the client checks that one for length before it ever gets to the CIP
     * layer, so applying it here would just duplicate SHORT_CPF.  It belongs on the
     * connected path, where it reaches the read response.
     */
    if(fault_fires(plc, FAULT_SHORT_CPF)) { payload_len = 0; }

    declared_len = (uint16_t)payload_len;

    if(fault_fires(plc, FAULT_ITEM_LEN)) { declared_len = (uint16_t)(payload_len + 4); }

    hdr = write_prefix(hdr, interface_handle, router_timeout, item_count);
    hdr = write_item_header(hdr, addr_type, 0);
    hdr = write_item_header(hdr, CPF_ITEM_UDI, declared_len);
    rest = byte_slice_skip(body, payload_len);

    if(byte_slice_has_error(hdr) || byte_slice_has_error(rest)) {
        *eip_status = EIP_STATUS_INSUFFICIENT_MEMORY;
        return out;
    }

    return rest;
}


byte_slice_t handle_cpf_connected(byte_slice_t in, byte_slice_t out, plc_s *plc, uint32_t *eip_status) {
    uint32_t interface_handle = 0;
    uint16_t router_timeout = 0;
    uint16_t addr_type = 0;
    uint16_t addr_len = 0;
    uint32_t conn_id = 0;
    uint16_t data_type = 0;
    uint16_t data_len = 0;
    uint16_t conn_seq = 0;
    uint16_t item_count = CPF_ITEM_COUNT;
    uint16_t declared_len = 0;
    byte_slice_t hdr = {0};
    byte_slice_t body = {0};
    byte_slice_t rest = {0};
    int32_t payload_len = 0;

    in = read_prefix(in, &interface_handle, &router_timeout);
    in = read_item_header(in, &addr_type, &addr_len);
    in = byte_slice_read_u32_le(in, &conn_id);
    in = read_item_header(in, &data_type, &data_len);
    in = byte_slice_read_u16_le(in, &conn_seq);

    if(byte_slice_has_error(in) || addr_type != CPF_ITEM_CAI || addr_len != CPF_CONN_ID_SIZE || data_type != CPF_ITEM_CDI
       || data_len != byte_slice_len(in) + CPF_CONN_SEQ_SIZE || byte_slice_len(in) == 0) {
        log_info("Bad connected CPF packet: address item %x/%u, data item %x/%u, %d payload bytes.", addr_type, addr_len,
                 data_type, data_len, byte_slice_len(in));
        *eip_status = EIP_STATUS_INCORRECT_DATA;
        return out;
    }

    if(conn_id != plc->server_connection_id) {
        log_info("Expected connection ID %x but found connection ID %x!", plc->server_connection_id, conn_id);
        *eip_status = EIP_STATUS_INCORRECT_DATA;
        return out;
    }

    body = byte_slice_take(out, CPF_CONN_HEADER_SIZE, &hdr);
    rest = cip_dispatch_request(in, body, plc, false);
    payload_len = byte_slice_len(byte_slice_used(body, rest));

    /*
     * Fault injection.  Everything the client checks about this header is derived from the
     * values below, so they are computed first and then written normally -- the header
     * layout stays in one piece and readable.
     */
    conn_id = plc->client_connection_id;

    if(fault_fires(plc, FAULT_CPF_COUNT)) { item_count = 3; }

    if(fault_fires(plc, FAULT_CPF_TYPE)) { addr_type = (uint16_t)0x00FF; }

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

    /*
     * The declared data item length normally matches the bytes that follow it.  FAULT_ITEM_LEN
     * makes it disagree while leaving the packet itself intact, which is the one case the
     * client can only catch by comparing the two.
     */
    declared_len = (uint16_t)(payload_len + CPF_CONN_SEQ_SIZE);

    if(fault_fires(plc, FAULT_ITEM_LEN)) { declared_len = (uint16_t)(declared_len + 4); }

    hdr = write_prefix(hdr, interface_handle, router_timeout, item_count);
    hdr = write_item_header(hdr, addr_type, CPF_CONN_ID_SIZE);
    hdr = byte_slice_write_u32_le(hdr, conn_id);
    hdr = write_item_header(hdr, CPF_ITEM_CDI, declared_len);
    hdr = byte_slice_write_u16_le(hdr, conn_seq);
    rest = byte_slice_skip(body, payload_len);

    if(byte_slice_has_error(hdr) || byte_slice_has_error(rest)) {
        *eip_status = EIP_STATUS_INSUFFICIENT_MEMORY;
        return out;
    }

    return rest;
}


/*
 * The CPF prefix shared by both handlers.  CIP messaging is always interface zero with two
 * items; anything else fails the slice.  Nothing checked the interface handle before, which let
 * a client leave a stale value there and never find out until it met a real PLC.
 */
static byte_slice_t read_prefix(byte_slice_t in, uint32_t *interface_handle, uint16_t *router_timeout) {
    uint16_t item_count = 0;

    in = byte_slice_read_u32_le(in, interface_handle);
    in = byte_slice_read_u16_le(in, router_timeout);
    in = byte_slice_read_u16_le(in, &item_count);

    if(!byte_slice_has_error(in) && (*interface_handle != 0 || item_count != CPF_ITEM_COUNT)) {
        log_info("Expected interface handle 0 and %u items but found %u and %u!", CPF_ITEM_COUNT, *interface_handle, item_count);
        return byte_slice_fail(in, PLCTAG_ERR_BAD_DATA);
    }

    return in;
}


static byte_slice_t read_item_header(byte_slice_t in, uint16_t *type, uint16_t *length) {
    in = byte_slice_read_u16_le(in, type);
    return byte_slice_read_u16_le(in, length);
}


static byte_slice_t write_prefix(byte_slice_t out, uint32_t interface_handle, uint16_t router_timeout, uint16_t item_count) {
    out = byte_slice_write_u32_le(out, interface_handle);
    out = byte_slice_write_u16_le(out, router_timeout);
    return byte_slice_write_u16_le(out, item_count);
}


static byte_slice_t write_item_header(byte_slice_t out, uint16_t type, uint16_t length) {
    out = byte_slice_write_u16_le(out, type);
    return byte_slice_write_u16_le(out, length);
}
