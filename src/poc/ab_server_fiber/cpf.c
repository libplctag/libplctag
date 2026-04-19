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
 * Struct types
 * ============================================================================ */

typedef struct {
    uint32_t iface_handle;
    uint16_t timeout;
    uint16_t item_count;
    uint16_t item0_type;
    uint16_t item0_len;
    uint16_t item1_type;
    uint16_t item1_len;
} cpf_unconnected_hdr_t;

typedef struct {
    uint32_t iface_handle;
    uint16_t timeout;
    uint16_t item_count;
    uint16_t item0_type;
    uint16_t item0_len;
    uint32_t conn_id;
    uint16_t item1_type;
    uint16_t item1_len;
    uint16_t seq_num;
} cpf_connected_hdr_t;

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

static Bytes cpf_parse_unconnected(Bytes payload, cpf_unconnected_hdr_t *hdr);
static Bytes cpf_parse_connected(Bytes payload, cpf_connected_hdr_t *hdr);
static Bytes cpf_encode_unconnected(Arena *a, cpf_unconnected_hdr_t *hdr);
static Bytes cpf_encode_connected(Arena *a, cpf_connected_hdr_t *hdr);
static Bytes wrap_unconnected(Arena *a, Bytes cip_response);
static Bytes wrap_connected(Arena *a, Bytes cip_response, uint32_t conn_id, uint16_t seq);

/* ============================================================================
 * Public functions
 * ============================================================================ */

extern Bytes cpf_handle_unconnected(Arena *a, Bytes payload, eip_session_t *sess, plc_config_t *cfg) {
    cpf_unconnected_hdr_t hdr = {0};

    pdlog(LOG_MODULE_CPF, LOG_LEVEL_DETAIL, "cpf_handle_unconnected: payload len=%zu", payload.len);

    Bytes cip_data = cpf_parse_unconnected(payload, &hdr);
    if(bytes_is_null(cip_data)) {
        return (Bytes){0};
    }

    if(hdr.item_count != 2 || hdr.item0_type != CPF_ITEM_NULL_ADDR || hdr.item1_type != CPF_ITEM_UCONN_DATA) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF unconnected: unexpected items count=%u item0=0x%04x item1=0x%04x",
              hdr.item_count, hdr.item0_type, hdr.item1_type);
        return (Bytes){0};
    }

    Bytes cip_response = cip_dispatch_unconnected(a, cip_data, sess, cfg);
    if(bytes_is_null(cip_response)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF unconnected: CIP dispatch returned null");
        return (Bytes){0};
    }

    return wrap_unconnected(a, cip_response);
}


extern Bytes cpf_handle_connected(Arena *a, Bytes payload, eip_session_t *sess, plc_config_t *cfg) {
    cpf_connected_hdr_t hdr = {0};

    pdlog(LOG_MODULE_CPF, LOG_LEVEL_DETAIL, "cpf_handle_connected: payload len=%zu", payload.len);

    Bytes cip_data = cpf_parse_connected(payload, &hdr);
    if(bytes_is_null(cip_data)) {
        return (Bytes){0};
    }

    if(hdr.item_count != 2 || hdr.item0_type != CPF_ITEM_CONN_ADDR || hdr.item1_type != CPF_ITEM_CONN_DATA) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: unexpected items count=%u item0=0x%04x item1=0x%04x",
              hdr.item_count, hdr.item0_type, hdr.item1_type);
        return (Bytes){0};
    }

    /*
     * The connected address item (0x00A1) carries the O->T connection ID:
     * the value the server assigned and returned in the ForwardOpen response
     * (sess->server_connection_id), NOT the T->O NCI the client proposed.
     */
    if(hdr.conn_id != sess->server_connection_id) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: connection ID mismatch: got 0x%08x expected 0x%08x",
              hdr.conn_id, sess->server_connection_id);
        return (Bytes){0};
    }

    sess->client_connection_seq = hdr.seq_num;
    sess->server_connection_seq++;

    Bytes cip_response = cip_dispatch_connected(a, cip_data, sess, cfg, sess->max_cip_packet_size);
    if(bytes_is_null(cip_response)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: CIP dispatch returned null");
        return (Bytes){0};
    }

    return wrap_connected(a, cip_response, sess->server_connection_id, sess->server_connection_seq);
}

/* ============================================================================
 * Static functions
 * ============================================================================ */

static Bytes cpf_parse_unconnected(Bytes payload, cpf_unconnected_hdr_t *hdr) {
    Bytes rest = bytes_unpack(payload, BYTES_LE,
        &hdr->iface_handle,
        &hdr->timeout,
        &hdr->item_count,
        &hdr->item0_type,
        &hdr->item0_len,
        &hdr->item1_type,
        &hdr->item1_len
    );

    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF unconnected: header unpack failed");
        return (Bytes){0};
    }

    if(rest.len != (size_t)hdr->item1_len) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF unconnected: payload length mismatch, expected %zu, got %zu",
              (size_t)hdr->item1_len, rest.len);
        return (Bytes){0};
    }

    return rest;
}


static Bytes cpf_parse_connected(Bytes payload, cpf_connected_hdr_t *hdr) {
    Bytes rest = bytes_unpack(payload, BYTES_LE,
        &hdr->iface_handle,
        &hdr->timeout,
        &hdr->item_count,
        &hdr->item0_type,
        &hdr->item0_len,
        &hdr->conn_id,
        &hdr->item1_type,
        &hdr->item1_len,
        &hdr->seq_num
    );

    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: header unpack failed");
        return (Bytes){0};
    }

    /* item1_len includes the 2-byte seq_num already consumed above. */
    if(hdr->item1_len < 2 || rest.len != (size_t)(hdr->item1_len - 2)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "CPF connected: payload length mismatch, expected %zu, got %zu",
              hdr->item1_len >= 2 ? (size_t)(hdr->item1_len - 2) : (size_t)0, rest.len);
        return (Bytes){0};
    }

    return rest;
}


static Bytes cpf_encode_unconnected(Arena *a, cpf_unconnected_hdr_t *hdr) {
    return bytes_pack(a, BYTES_LE,
        hdr->iface_handle,
        hdr->timeout,
        hdr->item_count,
        hdr->item0_type,
        hdr->item0_len,
        hdr->item1_type,
        hdr->item1_len
    );
}


static Bytes cpf_encode_connected(Arena *a, cpf_connected_hdr_t *hdr) {
    return bytes_pack(a, BYTES_LE,
        hdr->iface_handle,
        hdr->timeout,
        hdr->item_count,
        hdr->item0_type,
        hdr->item0_len,
        hdr->conn_id,
        hdr->item1_type,
        hdr->item1_len,
        hdr->seq_num
    );
}


/*
 * Build an unconnected CPF response envelope:
 *   iface_handle(4) timeout(2) item_count(2)
 *   null-addr: type(2) len(2)
 *   uconn-data: type(2) len(2) <cip_response>
 */
static Bytes wrap_unconnected(Arena *a, Bytes cip_response) {
    cpf_unconnected_hdr_t hdr = {0};
    hdr.item_count = 2;
    hdr.item0_type = CPF_ITEM_NULL_ADDR;
    hdr.item1_type = CPF_ITEM_UCONN_DATA;
    hdr.item1_len  = (uint16_t)cip_response.len;

    Bytes hdr_bytes = cpf_encode_unconnected(a, &hdr);
    if(bytes_is_null(hdr_bytes)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "wrap_unconnected: arena alloc failed");
        return (Bytes){0};
    }
    return bytes_concat(a, hdr_bytes, cip_response);
}


/*
 * Build a connected CPF response envelope:
 *   iface_handle(4) timeout(2) item_count(2)
 *   conn-addr: type(2) len(2) conn_id(4)
 *   conn-data: type(2) len(2) seq(2) <cip_response>
 */
static Bytes wrap_connected(Arena *a, Bytes cip_response, uint32_t conn_id, uint16_t seq) {
    cpf_connected_hdr_t hdr = {0};
    hdr.item_count = 2;
    hdr.item0_type = CPF_ITEM_CONN_ADDR;
    hdr.item0_len  = 4;
    hdr.conn_id    = conn_id;
    hdr.item1_type = CPF_ITEM_CONN_DATA;
    hdr.item1_len  = (uint16_t)(2 + cip_response.len);
    hdr.seq_num    = seq;

    Bytes hdr_bytes = cpf_encode_connected(a, &hdr);
    if(bytes_is_null(hdr_bytes)) {
        pdlog(LOG_MODULE_CPF, LOG_LEVEL_WARN, "wrap_connected: arena alloc failed");
        return (Bytes){0};
    }
    return bytes_concat(a, hdr_bytes, cip_response);
}
