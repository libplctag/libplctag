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
 * Adapted from src/poc/ab_server_fiber/cpf.c.
 */

#include <stdint.h>

#include "utils/arena.h"
#include "utils/bytes.h"
#include "utils/debug.h"
#include "cip.h"
#include "cpf.h"
#include "device.h"

#define DEBUG_MOD DEBUG_MODULE_UTILS

/* ============================================================================
 * Internal types
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

extern Bytes cpf_handle_unconnected(Arena *a, Bytes payload, eip_session_t *sess, device_t *dev) {
    cpf_unconnected_hdr_t hdr = {0};

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0,
           "cpf_handle_unconnected: payload len=%zu.", payload.len);

    Bytes cip_data = cpf_parse_unconnected(payload, &hdr);
    if(bytes_is_null(cip_data)) { return (Bytes){0}; }

    if(hdr.item_count != 2 || hdr.item0_type != CPF_ITEM_NULL_ADDR || hdr.item1_type != CPF_ITEM_UCONN_DATA) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "CPF unconnected: unexpected items count=%u item0=0x%04x item1=0x%04x.",
               (unsigned)hdr.item_count, (unsigned)hdr.item0_type, (unsigned)hdr.item1_type);
        return (Bytes){0};
    }

    Bytes cip_response = cip_dispatch_unconnected(a, cip_data, sess, dev);
    if(bytes_is_null(cip_response)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "CPF unconnected: CIP dispatch returned null.");
        return (Bytes){0};
    }

    return wrap_unconnected(a, cip_response);
}


extern Bytes cpf_handle_connected(Arena *a, Bytes payload, eip_session_t *sess, device_t *dev) {
    cpf_connected_hdr_t hdr = {0};

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0,
           "cpf_handle_connected: payload len=%zu.", payload.len);

    Bytes cip_data = cpf_parse_connected(payload, &hdr);
    if(bytes_is_null(cip_data)) { return (Bytes){0}; }

    if(hdr.item_count != 2 || hdr.item0_type != CPF_ITEM_CONN_ADDR || hdr.item1_type != CPF_ITEM_CONN_DATA) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "CPF connected: unexpected items count=%u item0=0x%04x item1=0x%04x.",
               (unsigned)hdr.item_count, (unsigned)hdr.item0_type, (unsigned)hdr.item1_type);
        return (Bytes){0};
    }

    if(hdr.conn_id != sess->server_connection_id) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "CPF connected: connection ID mismatch: got 0x%08x expected 0x%08x.",
               (unsigned)hdr.conn_id, (unsigned)sess->server_connection_id);
        return (Bytes){0};
    }

    sess->client_connection_seq = hdr.seq_num;
    sess->server_connection_seq++;

    Bytes cip_response = cip_dispatch_connected(a, cip_data, sess, dev, sess->max_cip_packet_size);
    if(bytes_is_null(cip_response)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "CPF connected: CIP dispatch returned null.");
        return (Bytes){0};
    }

    return wrap_connected(a, cip_response, sess->server_connection_id, sess->server_connection_seq);
}

/* ============================================================================
 * Static functions
 * ============================================================================ */

static Bytes cpf_parse_unconnected(Bytes payload, cpf_unconnected_hdr_t *hdr) {
    Bytes rest = bytes_unpack(payload, BYTES_LE,
                              &hdr->iface_handle, &hdr->timeout,
                              &hdr->item_count,
                              &hdr->item0_type, &hdr->item0_len,
                              &hdr->item1_type, &hdr->item1_len);
    if(bytes_is_null(rest)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "CPF unconnected: header unpack failed.");
        return (Bytes){0};
    }
    if(rest.len != (size_t)hdr->item1_len) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "CPF unconnected: payload length mismatch, expected %zu, got %zu.",
               (size_t)hdr->item1_len, rest.len);
        return (Bytes){0};
    }
    return rest;
}


static Bytes cpf_parse_connected(Bytes payload, cpf_connected_hdr_t *hdr) {
    Bytes rest = bytes_unpack(payload, BYTES_LE,
                              &hdr->iface_handle, &hdr->timeout,
                              &hdr->item_count,
                              &hdr->item0_type, &hdr->item0_len,
                              &hdr->conn_id,
                              &hdr->item1_type, &hdr->item1_len,
                              &hdr->seq_num);
    if(bytes_is_null(rest)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "CPF connected: header unpack failed.");
        return (Bytes){0};
    }
    if(hdr->item1_len < 2 || rest.len != (size_t)(hdr->item1_len - 2)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "CPF connected: payload length mismatch, expected %zu, got %zu.",
               hdr->item1_len >= 2 ? (size_t)(hdr->item1_len - 2) : (size_t)0, rest.len);
        return (Bytes){0};
    }
    return rest;
}


static Bytes cpf_encode_unconnected(Arena *a, cpf_unconnected_hdr_t *hdr) {
    return bytes_pack(a, BYTES_LE,
                      hdr->iface_handle, hdr->timeout,
                      hdr->item_count,
                      hdr->item0_type, hdr->item0_len,
                      hdr->item1_type, hdr->item1_len);
}


static Bytes cpf_encode_connected(Arena *a, cpf_connected_hdr_t *hdr) {
    return bytes_pack(a, BYTES_LE,
                      hdr->iface_handle, hdr->timeout,
                      hdr->item_count,
                      hdr->item0_type, hdr->item0_len,
                      hdr->conn_id,
                      hdr->item1_type, hdr->item1_len,
                      hdr->seq_num);
}


static Bytes wrap_unconnected(Arena *a, Bytes cip_response) {
    cpf_unconnected_hdr_t hdr = {0};
    hdr.item_count  = 2;
    hdr.item0_type  = CPF_ITEM_NULL_ADDR;
    hdr.item1_type  = CPF_ITEM_UCONN_DATA;
    hdr.item1_len   = (uint16_t)cip_response.len;

    Bytes hdr_bytes = cpf_encode_unconnected(a, &hdr);
    if(bytes_is_null(hdr_bytes)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "wrap_unconnected: arena alloc failed.");
        return (Bytes){0};
    }
    return bytes_concat(a, hdr_bytes, cip_response);
}


static Bytes wrap_connected(Arena *a, Bytes cip_response, uint32_t conn_id, uint16_t seq) {
    cpf_connected_hdr_t hdr = {0};
    hdr.item_count  = 2;
    hdr.item0_type  = CPF_ITEM_CONN_ADDR;
    hdr.item0_len   = 4;
    hdr.conn_id     = conn_id;
    hdr.item1_type  = CPF_ITEM_CONN_DATA;
    hdr.item1_len   = (uint16_t)(2 + cip_response.len);
    hdr.seq_num     = seq;

    Bytes hdr_bytes = cpf_encode_connected(a, &hdr);
    if(bytes_is_null(hdr_bytes)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "wrap_connected: arena alloc failed.");
        return (Bytes){0};
    }
    return bytes_concat(a, hdr_bytes, cip_response);
}
