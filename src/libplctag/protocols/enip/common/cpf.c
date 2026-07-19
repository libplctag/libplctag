/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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

/*
 * cpf.c — Common Packet Format (CPF) wrap/unwrap codec. Direction-agnostic:
 * no device_t, no eip_session_t, no I/O. Used by both the client
 * (client/enip_session.c) and the server (server/cpf_dispatch.c).
 * Adapted from src/poc/ab_server_fiber/cpf.c.
 */

#include <stdbool.h>
#include <stdint.h>

#include "utils/arena.h"
#include "utils/bytes.h"
#include "utils/debug.h"
#include "cpf.h"


/* ============================================================================
 * Public functions
 * ============================================================================ */

extern Bytes cpf_wrap_unconnected(Arena *a, Bytes cip) {
    if(!a || bytes_is_null(cip)) { return bytes_null(); }

    /* header: iface_handle(4)=0, timeout(2)=0, item_count(2)=2 */
    /* Null Address Item: type(2)=0x0000, length(2)=0 */
    /* Unconnected Data Item header: type(2)=0x00B2, length(2)=cip.len */
    return bytes_pack(a, BYTES_LE, (uint32_t)0, (uint16_t)0, (uint16_t)2, CPF_ITEM_NULL_ADDR, (uint16_t)0,
                       CPF_ITEM_UCONN_DATA, (uint16_t)cip.len, cip);
}


extern Bytes cpf_wrap_connected(Arena *a, uint32_t conn_id, uint16_t seq, Bytes cip) {
    if(!a || bytes_is_null(cip)) { return bytes_null(); }

    /* header: iface_handle(4)=0, timeout(2)=0, item_count(2)=2 */
    /* Connected Address Item: type(2)=0x00A1, length(2)=4, conn_id(4) */
    /* Connected Data Item header: type(2)=0x00B1, length(2)=(seq(2)+cip.len) */
    uint16_t cdi_len = (uint16_t)(2 + cip.len);

    return bytes_pack(a, BYTES_LE, (uint32_t)0, (uint16_t)0, (uint16_t)2, CPF_ITEM_CONN_ADDR, (uint16_t)4, conn_id,
                       CPF_ITEM_CONN_DATA, cdi_len, seq, cip);
}


extern bool cpf_unwrap(Bytes in, bool connected, uint32_t *conn_id_out, uint16_t *seq_out, Bytes *cip_out) {
    if(bytes_is_null(in) || !seq_out || !cip_out) { return false; }

    uint32_t iface_handle = 0;
    uint16_t timeout = 0;
    uint16_t item_count = 0;
    uint16_t item0_type = 0;
    uint16_t item0_len = 0;
    uint16_t item1_type = 0;
    uint16_t item1_len = 0;

    if(connected) {
        uint32_t conn_id = 0;
        uint16_t seq = 0;

        Bytes rest = bytes_unpack(in, BYTES_LE, &iface_handle, &timeout, &item_count, &item0_type, &item0_len, &conn_id,
                                  &item1_type, &item1_len, &seq);
        if(bytes_is_null(rest)) {
            pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0, "cpf_unwrap: connected header unpack failed.");
            return false;
        }
        if(item_count != 2 || item0_type != CPF_ITEM_CONN_ADDR || item1_type != CPF_ITEM_CONN_DATA) {
            pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0,
                   "cpf_unwrap: unexpected connected items count=%u item0=0x%04x item1=0x%04x.", (unsigned)item_count,
                   (unsigned)item0_type, (unsigned)item1_type);
            return false;
        }
        if(item1_len < 2 || rest.len != (size_t)(item1_len - 2)) {
            pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0,
                   "cpf_unwrap: connected payload length mismatch, expected %zu, got %zu.",
                   item1_len >= 2 ? (size_t)(item1_len - 2) : (size_t)0, rest.len);
            return false;
        }

        if(conn_id_out) { *conn_id_out = conn_id; }
        *seq_out = seq;
        *cip_out = rest;
        return true;
    }

    Bytes rest = bytes_unpack(in, BYTES_LE, &iface_handle, &timeout, &item_count, &item0_type, &item0_len, &item1_type,
                              &item1_len);
    if(bytes_is_null(rest)) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0, "cpf_unwrap: unconnected header unpack failed.");
        return false;
    }
    if(item_count != 2 || item0_type != CPF_ITEM_NULL_ADDR || item1_type != CPF_ITEM_UCONN_DATA) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0,
               "cpf_unwrap: unexpected unconnected items count=%u item0=0x%04x item1=0x%04x.", (unsigned)item_count,
               (unsigned)item0_type, (unsigned)item1_type);
        return false;
    }
    if(rest.len != (size_t)item1_len) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0,
               "cpf_unwrap: unconnected payload length mismatch, expected %zu, got %zu.", (size_t)item1_len, rest.len);
        return false;
    }

    if(conn_id_out) { *conn_id_out = 0; }
    *seq_out = 0;
    *cip_out = rest;
    return true;
}
