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
 *
 * Parses the 24-byte EIP header, dispatches to the appropriate handler, and
 * wraps the response payload back into an EIP response header.
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

#include "arena.h"
#include "bytes.h"
#include "cpf.h"
#include "eip.h"
#include "log.h"
#include "plc.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

/*
 * Session handle counter — incremented for each RegisterSession.
 * Single-threaded: no atomics needed.
 */
static uint32_t s_next_session_handle = 1;

/* RegisterSession response payload: protocol version (2 bytes) + options (2 bytes) */
#define EIP_REG_SESSION_VERSION ((uint16_t)1)

/* ============================================================================
 * Struct types
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

static bool eip_parse_hdr(Bytes hdr_buf, eip_hdr_t *hdr);
static Bytes eip_encode_hdr(Arena *a, eip_hdr_t *hdr);
static Bytes handle_register_session(Arena *a, Bytes payload, eip_session_t *sess);
static Bytes handle_unregister_session(Arena *a, eip_session_t *sess);
static Bytes make_eip_response(Arena *a, eip_hdr_t *req_hdr, eip_session_t *sess, Bytes body);
static Bytes make_eip_error(Arena *a, eip_hdr_t *req_hdr);

/* ============================================================================
 * Public functions
 * ============================================================================ */

extern Bytes eip_dispatch(Arena *a, Bytes hdr, Bytes payload, eip_session_t *sess, plc_config_t *cfg) {
    eip_hdr_t req_hdr = {0};
    Bytes response_body = {0};

    if(!eip_parse_hdr(hdr, &req_hdr)) {
        return (Bytes){0};
    }

    sess->sender_context = req_hdr.sender_context;

    if(sess->max_eip_packet_size > 0 && (size_t)req_hdr.payload_len > sess->max_eip_packet_size) {
        pdlog(LOG_MODULE_EIP, LOG_LEVEL_WARN,
              "EIP payload_len=%u exceeds negotiated max %zu — closing connection",
              req_hdr.payload_len, sess->max_eip_packet_size);
        return (Bytes){0};
    }

    if(payload.len != (size_t)req_hdr.payload_len) {
        pdlog(LOG_MODULE_EIP, LOG_LEVEL_WARN,
              "EIP payload size mismatch: declared %u received %zu — closing connection",
              req_hdr.payload_len, payload.len);
        return (Bytes){0};
    }

    pdlog(LOG_MODULE_EIP, LOG_LEVEL_DETAIL, "eip_dispatch: cmd=0x%04x payload len=%zu", req_hdr.cmd, payload.len);

    switch(req_hdr.cmd) {
        case EIP_CMD_REGISTER_SESSION:
            response_body = handle_register_session(a, payload, sess);
            break;

        case EIP_CMD_UNREGISTER_SESSION:
            handle_unregister_session(a, sess);
            /* UnregisterSession has no response per spec — signal caller to close. */
            return (Bytes){0};

        case EIP_CMD_UNCONNECTED_SEND:
            response_body = cpf_handle_unconnected(a, payload, sess, cfg);
            break;

        case EIP_CMD_CONNECTED_SEND:
            response_body = cpf_handle_connected(a, payload, sess, cfg);
            break;

        default:
            pdlog(LOG_MODULE_EIP, LOG_LEVEL_WARN, "Unknown EIP command 0x%04x", req_hdr.cmd);
            return make_eip_error(a, &req_hdr);
    }

    if(bytes_is_null(response_body)) {
        pdlog(LOG_MODULE_EIP, LOG_LEVEL_WARN, "EIP handler returned null body for cmd=0x%04x", req_hdr.cmd);
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

    size_t eip_payload = (raw_packet_size > EIP_HEADER_SIZE) ? (size_t)raw_packet_size - EIP_HEADER_SIZE : (size_t)0;
    sess->max_eip_packet_size = eip_payload;

    size_t cpf_conn_framing = CPF_HEADER_SIZE + CPF_CONNECTED_ADDR_ITEM_SIZE + CPF_CONNECTED_DATA_ITEM_SIZE + CPF_CONN_SEQ_NUM_SIZE;
    sess->max_cpf_packet_size = (eip_payload > cpf_conn_framing) ? eip_payload - cpf_conn_framing : (size_t)0;

    sess->max_cip_packet_size = sess->max_cpf_packet_size;
}


/* ============================================================================
 * Static functions
 * ============================================================================ */

static bool eip_parse_hdr(Bytes hdr_buf, eip_hdr_t *hdr) {
    Bytes rest = bytes_unpack(hdr_buf, BYTES_LE,
        &hdr->cmd,
        &hdr->payload_len,
        &hdr->session_handle,
        &hdr->status,
        &hdr->sender_context,
        &hdr->options
    );

    if(bytes_is_null(rest)) {
        pdlog(LOG_MODULE_EIP, LOG_LEVEL_WARN, "eip_parse_hdr: header unpack failed");
        return false;
    }

    return true;
}


static Bytes eip_encode_hdr(Arena *a, eip_hdr_t *hdr) {
    return bytes_pack(a, BYTES_LE,
        hdr->cmd,
        hdr->payload_len,
        hdr->session_handle,
        hdr->status,
        hdr->sender_context,
        hdr->options
    );
}


static Bytes handle_register_session(Arena *a, Bytes payload, eip_session_t *sess) {
    (void)payload;

    sess->session_handle = s_next_session_handle++;
    if(s_next_session_handle == 0) { s_next_session_handle = 1; } /* skip 0 */

    pdlog(LOG_MODULE_EIP, LOG_LEVEL_INFO, "RegisterSession: assigned handle 0x%08x", sess->session_handle);

    return bytes_pack(a, BYTES_LE, EIP_REG_SESSION_VERSION, (uint16_t)0);
}


static Bytes handle_unregister_session(Arena *a, eip_session_t *sess) {
    (void)a;
    pdlog(LOG_MODULE_EIP, LOG_LEVEL_INFO, "UnregisterSession: handle 0x%08x", sess->session_handle);
    sess->session_handle = 0;
    return (Bytes){0};
}


/*
 * Build a complete EIP response: 24-byte header with status=0, then body.
 */
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
        pdlog(LOG_MODULE_EIP, LOG_LEVEL_WARN, "make_eip_response: arena alloc failed");
        return (Bytes){0};
    }
    return bytes_concat(a, hdr_bytes, body);
}


/*
 * Build a minimal EIP error response with status = 0x0065 (unsupported).
 * Returns an EIP header with empty payload and non-zero status.
 */
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
