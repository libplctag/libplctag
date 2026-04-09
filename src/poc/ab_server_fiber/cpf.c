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
 * cpf.c — Common Packet Format (CPF) layer.
 *
 * Unconnected CPF header (16 bytes total before the CIP data):
 *   uint32  interface handle  (always 0)
 *   uint16  timeout           (always 0)
 *   uint16  item count        (always 2)
 *   --- item 0: null address ---
 *   uint16  type  (0x0000)
 *   uint16  length (0)
 *   --- item 1: unconnected data ---
 *   uint16  type  (0x00B2)
 *   uint16  length
 *   <CIP data follows>
 *
 * Connected CPF header (22 bytes before the CIP data):
 *   uint32  interface handle  (always 0)
 *   uint16  timeout           (always 0)
 *   uint16  item count        (always 2)
 *   --- item 0: connected address ---
 *   uint16  type   (0x00A1)
 *   uint16  length (4)
 *   uint32  connection ID
 *   --- item 1: connected data ---
 *   uint16  type   (0x00B1)
 *   uint16  length
 *   uint16  sequence number
 *   <CIP data follows>
 */

#include <stdint.h>

#include "arena.h"
#include "bytes.h"
#include "cip.h"
#include "cpf.h"
#include "log.h"
#include "plc.h"

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

static Bytes wrap_unconnected(Arena *a, Bytes cip_response);
static Bytes wrap_connected(Arena *a, Bytes cip_response, uint32_t conn_id, uint16_t seq);

/* ============================================================================
 * Public functions
 * ============================================================================ */

extern Bytes cpf_handle_unconnected(Arena *a, Bytes payload, eip_session_t *sess, plc_config_t *cfg) {
    uint32_t iface_handle = 0;
    uint16_t timeout = 0;
    uint16_t item_count = 0;
    uint16_t item0_type = 0;
    uint16_t item0_len = 0;
    uint16_t item1_type = 0;
    uint16_t item1_len = 0;
    Bytes cip_data = {0};
    Bytes cip_response = {0};

    pdlog(LOG_MODULE_CPF, LOG_LEVEL_DETAIL, "cpf_handle_unconnected: payload len=%zu", payload.len);

    Bytes rest = bytes_unpack(payload, "<IHHH", &iface_handle, &timeout, &item_count, &item0_type);
    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF unconnected: header unpack failed");
        return (Bytes){0};
    }

    rest = bytes_unpack(rest, "<HHH", &item0_len, &item1_type, &item1_len);
    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF unconnected: item headers unpack failed");
        return (Bytes){0};
    }

    if(item_count != 2 || item0_type != CPF_ITEM_NULL_ADDR || item1_type != CPF_ITEM_UCONN_DATA) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF unconnected: unexpected items count=%u item0=0x%04x item1=0x%04x", item_count,
              item0_type, item1_type);
        return (Bytes){0};
    }

    cip_data = bytes_slice(rest, 0, item1_len);
    if(bytes_is_null(cip_data)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF unconnected: CIP data slice failed len=%u", item1_len);
        return (Bytes){0};
    }

    cip_response = cip_dispatch_unconnected(a, cip_data, sess, cfg);
    if(bytes_is_null(cip_response)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF unconnected: CIP dispatch returned null");
        return (Bytes){0};
    }

    return wrap_unconnected(a, cip_response);
}


extern Bytes cpf_handle_connected(Arena *a, Bytes payload, eip_session_t *sess, plc_config_t *cfg) {
    uint32_t iface_handle = 0;
    uint16_t timeout = 0;
    uint16_t item_count = 0;
    uint16_t item0_type = 0;
    uint16_t item0_len = 0;
    uint32_t conn_id = 0;
    uint16_t item1_type = 0;
    uint16_t item1_len = 0;
    uint16_t seq_num = 0;
    Bytes cip_data = {0};
    Bytes cip_response = {0};

    pdlog(LOG_MODULE_CPF, LOG_LEVEL_DETAIL, "cpf_handle_connected: payload len=%zu", payload.len);

    Bytes rest = bytes_unpack(payload, "<IHHH", &iface_handle, &timeout, &item_count, &item0_type);
    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: header unpack failed");
        return (Bytes){0};
    }

    rest = bytes_unpack(rest, "<HIH", &item0_len, &conn_id, &item1_type);
    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: address item unpack failed");
        return (Bytes){0};
    }

    rest = bytes_unpack(rest, "<HH", &item1_len, &seq_num);
    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: data item header unpack failed");
        return (Bytes){0};
    }

    if(item_count != 2 || item0_type != CPF_ITEM_CONN_ADDR || item1_type != CPF_ITEM_CONN_DATA) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: unexpected items count=%u item0=0x%04x item1=0x%04x", item_count,
              item0_type, item1_type);
        return (Bytes){0};
    }

    /*
     * Validate connection ID.
     * The connected address item (0x00A1) carries the O→T connection ID:
     * the value the server assigned and returned in the ForwardOpen response
     * (sess->server_connection_id), NOT the T→O NCI the client proposed.
     */
    if(conn_id != sess->server_connection_id) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: connection ID mismatch: got 0x%08x expected 0x%08x", conn_id,
              sess->server_connection_id);
        return (Bytes){0};
    }

    /* item1_len includes the 2-byte sequence number already consumed. */
    size_t cip_len = (item1_len >= 2) ? (size_t)(item1_len - 2) : 0;
    cip_data = bytes_slice(rest, 0, cip_len);
    if(bytes_is_null(cip_data) && cip_len > 0) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: CIP data slice failed len=%zu", cip_len);
        return (Bytes){0};
    }

    sess->client_connection_seq = seq_num;
    sess->server_connection_seq++;

    cip_response = cip_dispatch_connected(a, cip_data, sess, cfg);
    if(bytes_is_null(cip_response)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: CIP dispatch returned null");
        return (Bytes){0};
    }

    return wrap_connected(a, cip_response, sess->server_connection_id, sess->server_connection_seq);
}

/* ============================================================================
 * Static functions
 * ============================================================================ */

/*
 * Wrap a CIP response in an unconnected CPF envelope:
 *   iface_handle(4) timeout(2) item_count(2)
 *   null-addr: type(2) len(2)
 *   uconn-data: type(2) len(2) <cip_response>
 */
static Bytes wrap_unconnected(Arena *a, Bytes cip_response) {
    Bytes hdr = bytes_pack(a, "<IHHHHHH", (uint32_t)0, /* interface handle */
                          (uint16_t)0,                /* timeout */
                          (uint16_t)2,                /* item count */
                          CPF_ITEM_NULL_ADDR,         /* item 0 type */
                          (uint16_t)0,                /* item 0 length */
                          CPF_ITEM_UCONN_DATA,        /* item 1 type */
                          (uint16_t)cip_response.len); /* item 1 length */
    if(bytes_is_null(hdr)) { return (Bytes){0}; }
    return bytes_concat(a, hdr, cip_response);
}

/*
 * Wrap a CIP response in a connected CPF envelope:
 *   iface_handle(4) timeout(2) item_count(2)
 *   conn-addr: type(2) len(2) conn_id(4)
 *   conn-data: type(2) len(2) seq(2) <cip_response>
 */
static Bytes wrap_connected(Arena *a, Bytes cip_response, uint32_t conn_id, uint16_t seq) {
    /* Connected data item length = 2 (seq) + cip_response.len */
    uint16_t data_item_len = (uint16_t)(2 + cip_response.len);

    Bytes addr_hdr = bytes_pack(a, "<HHIHH", CPF_ITEM_CONN_ADDR, /* item 0 type */
                                (uint16_t)4,                     /* item 0 length */
                                conn_id, CPF_ITEM_CONN_DATA,     /* item 1 type */
                                data_item_len);
    if(bytes_is_null(addr_hdr)) { return (Bytes){0}; }

    Bytes outer_hdr = bytes_pack(a, "<IHH", (uint32_t)0, /* interface handle */
                                 (uint16_t)0,            /* timeout */
                                 (uint16_t)2);           /* item count */
    if(bytes_is_null(outer_hdr)) { return (Bytes){0}; }

    Bytes seq_bytes = bytes_pack(a, "<H", seq);
    if(bytes_is_null(seq_bytes)) { return (Bytes){0}; }

    return bytes_concat(a, outer_hdr, addr_hdr, seq_bytes, cip_response);
}
