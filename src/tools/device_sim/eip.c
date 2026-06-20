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
 * eip.c — EIP (Ethernet/IP) layer dispatcher.
 * Adapted from src/poc/ab_server_fiber/eip.c.
 *
 * EIP header layout (all little-endian):
 *   offset 0  uint16  command
 *   offset 2  uint16  payload length
 *   offset 4  uint32  session handle
 *   offset 8  uint32  status
 *   offset 12 uint64  sender context
 *   offset 20 uint32  options
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/arena.h"
#include "utils/bytes.h"
#include "utils/atomic_utils.h"
#include "utils/debug.h"
#include "cpf.h"
#include "device.h"
#include "discovery.h"
#include "eip.h"

#define DEBUG_MOD DEBUG_MODULE_UTILS

/* ============================================================================
 * Constants
 * ============================================================================ */

#define EIP_REG_SESSION_VERSION ((uint16_t)1)

/* ============================================================================
 * Counters — shared across threads, must be atomic.
 * ============================================================================ */

static atomic_int32_t s_next_session_handle = 1;

/* ============================================================================
 * Internal types
 * ============================================================================ */

typedef struct {
    uint16_t cmd;
    uint16_t payload_len;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;
} eip_hdr_t;

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

static bool  eip_parse_hdr(Bytes hdr_buf, eip_hdr_t *hdr);
static Bytes eip_encode_hdr(Arena *a, eip_hdr_t *hdr);
static Bytes handle_register_session(Arena *a, Bytes payload, eip_session_t *sess);
static Bytes handle_unregister_session(Arena *a, eip_session_t *sess);
static Bytes make_eip_response(Arena *a, eip_hdr_t *req_hdr, eip_session_t *sess, Bytes body);
static Bytes make_eip_error(Arena *a, eip_hdr_t *req_hdr);

/* ============================================================================
 * Public functions
 * ============================================================================ */

extern Bytes eip_dispatch(Arena *a, Bytes hdr, Bytes payload, eip_session_t *sess, device_t *dev) {
    eip_hdr_t req_hdr = {0};
    Bytes response_body = {0};

    if(!eip_parse_hdr(hdr, &req_hdr)) { return (Bytes){0}; }

    sess->sender_context = req_hdr.sender_context;

    if(sess->max_eip_packet_size > 0 && (size_t)req_hdr.payload_len > sess->max_eip_packet_size) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "EIP payload_len=%u exceeds negotiated max %zu — closing connection.",
               (unsigned)req_hdr.payload_len, sess->max_eip_packet_size);
        return (Bytes){0};
    }

    if(payload.len != (size_t)req_hdr.payload_len) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "EIP payload size mismatch: declared %u received %zu — closing connection.",
               (unsigned)req_hdr.payload_len, payload.len);
        return (Bytes){0};
    }

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0,
           "eip_dispatch: cmd=0x%04x payload len=%zu.", (unsigned)req_hdr.cmd, payload.len);

    switch(req_hdr.cmd) {
        case EIP_CMD_LIST_IDENTITY:
            response_body = discovery_list_identity_cpf(a, dev);
            break;

        case EIP_CMD_REGISTER_SESSION:
            response_body = handle_register_session(a, payload, sess);
            break;

        case EIP_CMD_UNREGISTER_SESSION:
            handle_unregister_session(a, sess);
            return (Bytes){0};

        case EIP_CMD_UNCONNECTED_SEND:
            response_body = cpf_handle_unconnected(a, payload, sess, dev);
            break;

        case EIP_CMD_CONNECTED_SEND:
            response_body = cpf_handle_connected(a, payload, sess, dev);
            break;

        default:
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                   "Unknown EIP command 0x%04x.", (unsigned)req_hdr.cmd);
            return make_eip_error(a, &req_hdr);
    }

    if(bytes_is_null(response_body)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "EIP handler returned null body for cmd=0x%04x.", (unsigned)req_hdr.cmd);
        return make_eip_error(a, &req_hdr);
    }

    return make_eip_response(a, &req_hdr, sess, response_body);
}


extern void eip_session_set_unconnected_sizes(eip_session_t *sess, uint32_t raw_packet_size) {
    sess->raw_packet_size = raw_packet_size;

    size_t eip_payload = (raw_packet_size > EIP_HEADER_SIZE) ? (size_t)raw_packet_size - EIP_HEADER_SIZE : (size_t)0;
    sess->max_eip_packet_size = eip_payload;

    size_t cpf_unc_framing = CPF_HEADER_SIZE + CPF_UNCONNECTED_ADDR_ITEM_SIZE + CPF_UNCONNECTED_DATA_ITEM_SIZE;
    sess->max_cpf_packet_size = (eip_payload > cpf_unc_framing) ? eip_payload - cpf_unc_framing : (size_t)0;

    sess->max_cip_packet_size = sess->max_cpf_packet_size;
}


extern void eip_session_set_connected_sizes(eip_session_t *sess, uint32_t raw_packet_size) {
    sess->raw_packet_size = raw_packet_size;

    size_t cpf_before_data_item = CPF_HEADER_SIZE + CPF_CONNECTED_ADDR_ITEM_SIZE;
    sess->max_eip_packet_size = cpf_before_data_item + (size_t)raw_packet_size;

    size_t di_overhead = CPF_CONNECTED_DATA_ITEM_SIZE + CPF_CONN_SEQ_NUM_SIZE;
    sess->max_cpf_packet_size = (raw_packet_size > di_overhead) ? (size_t)raw_packet_size - di_overhead : (size_t)0;

    sess->max_cip_packet_size = sess->max_cpf_packet_size;
}

/* ============================================================================
 * Static functions
 * ============================================================================ */

static bool eip_parse_hdr(Bytes hdr_buf, eip_hdr_t *hdr) {
    Bytes rest = bytes_unpack(hdr_buf, BYTES_LE,
                              &hdr->cmd, &hdr->payload_len, &hdr->session_handle,
                              &hdr->status, &hdr->sender_context, &hdr->options);
    if(bytes_is_null(rest)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "eip_parse_hdr: header unpack failed.");
        return false;
    }
    return true;
}


static Bytes eip_encode_hdr(Arena *a, eip_hdr_t *hdr) {
    return bytes_pack(a, BYTES_LE,
                      hdr->cmd, hdr->payload_len, hdr->session_handle,
                      hdr->status, hdr->sender_context, hdr->options);
}


static Bytes handle_register_session(Arena *a, Bytes payload, eip_session_t *sess) {
    (void)payload;

    /* Fetch-and-add returns the old value; skip 0. */
    int32_t h = atomic_add_int32(&s_next_session_handle, 1);
    if(h == 0) { h = atomic_add_int32(&s_next_session_handle, 1); }
    sess->session_handle = (uint32_t)h;

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0,
           "RegisterSession: assigned handle 0x%08x.", (unsigned)sess->session_handle);

    return bytes_pack(a, BYTES_LE, EIP_REG_SESSION_VERSION, (uint16_t)0);
}


static Bytes handle_unregister_session(Arena *a, eip_session_t *sess) {
    (void)a;
    pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0,
           "UnregisterSession: handle 0x%08x.", (unsigned)sess->session_handle);
    sess->session_handle = 0;
    return (Bytes){0};
}


static Bytes make_eip_response(Arena *a, eip_hdr_t *req_hdr, eip_session_t *sess, Bytes body) {
    eip_hdr_t resp = {0};
    resp.cmd            = req_hdr->cmd;
    resp.payload_len    = (uint16_t)body.len;
    resp.session_handle = sess->session_handle;
    resp.status         = 0;
    resp.sender_context = req_hdr->sender_context;
    resp.options        = 0;

    Bytes hdr_bytes = eip_encode_hdr(a, &resp);
    if(bytes_is_null(hdr_bytes)) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "make_eip_response: arena alloc failed.");
        return (Bytes){0};
    }
    return bytes_concat(a, hdr_bytes, body);
}


static Bytes make_eip_error(Arena *a, eip_hdr_t *req_hdr) {
    eip_hdr_t resp = {0};
    resp.cmd            = req_hdr->cmd;
    resp.payload_len    = 0;
    resp.session_handle = 0;
    resp.status         = 0x0065;
    resp.sender_context = req_hdr->sender_context;
    resp.options        = 0;
    return eip_encode_hdr(a, &resp);
}
