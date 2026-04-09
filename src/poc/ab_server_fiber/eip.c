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
 * Forward declarations
 * ============================================================================ */

static Bytes handle_register_session(Arena *a, Bytes payload, eip_session_t *sess);
static Bytes handle_unregister_session(Arena *a, eip_session_t *sess);
static Bytes make_eip_response(Arena *a, uint16_t cmd, uint32_t session, uint64_t context, Bytes body);
static Bytes make_eip_error(Arena *a, uint16_t cmd, uint64_t context);

/* ============================================================================
 * Public functions
 * ============================================================================ */

extern Bytes eip_dispatch(Arena *a, uint16_t cmd, Bytes payload, eip_session_t *sess, plc_config_t *cfg) {
    Bytes response_body = {0};

    pdlog(LOG_MODULE_EIP, LOG_LEVEL_DETAIL, "eip_dispatch: cmd=0x%04x payload len=%zu", cmd, payload.len);

    switch(cmd) {
        case EIP_CMD_REGISTER_SESSION: response_body = handle_register_session(a, payload, sess); break;

        case EIP_CMD_UNREGISTER_SESSION:
            handle_unregister_session(a, sess);
            /* UnregisterSession has no response per spec — signal caller to close. */
            return (Bytes){0};

        case EIP_CMD_UNCONNECTED_SEND: response_body = cpf_handle_unconnected(a, payload, sess, cfg); break;

        case EIP_CMD_CONNECTED_SEND: response_body = cpf_handle_connected(a, payload, sess, cfg); break;

        default:
            pdlog(LOG_MODULE_EIP, LOG_LEVEL_WARN, "Unknown EIP command 0x%04x", cmd);
            return make_eip_error(a, cmd, sess->sender_context);
    }

    if(bytes_is_null(response_body)) {
        pdlog(LOG_MODULE_EIP, LOG_LEVEL_WARN, "EIP handler returned null body for cmd=0x%04x", cmd);
        return make_eip_error(a, cmd, sess->sender_context);
    }

    return make_eip_response(a, cmd, sess->session_handle, sess->sender_context, response_body);
}

/* ============================================================================
 * Static functions
 * ============================================================================ */

static Bytes handle_register_session(Arena *a, Bytes payload, eip_session_t *sess) {
    uint16_t proto_version = 0;
    uint16_t proto_options = 0;

    (void)payload;

    sess->session_handle = s_next_session_handle++;
    if(s_next_session_handle == 0) { s_next_session_handle = 1; } /* skip 0 */

    pdlog(LOG_MODULE_EIP, LOG_LEVEL_INFO, "RegisterSession: assigned handle 0x%08x", sess->session_handle);

    proto_version = EIP_REG_SESSION_VERSION;
    proto_options = 0;

    return bytes_pack(a, "<HH", proto_version, proto_options);
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
static Bytes make_eip_response(Arena *a, uint16_t cmd, uint32_t session, uint64_t context, Bytes body) {
    Bytes hdr = bytes_pack(a, "<HHIIQI", cmd, (uint16_t)body.len, session, (uint32_t)0, /* status = success */
                           context, (uint32_t)0);                                       /* options */
    if(bytes_is_null(hdr)) { return (Bytes){0}; }
    return bytes_concat(a, hdr, body);
}

/*
 * Build a minimal EIP error response with status = 0x0065 (unsupported).
 * Returns an EIP header with empty payload and non-zero status.
 */
static Bytes make_eip_error(Arena *a, uint16_t cmd, uint64_t context) {
    return bytes_pack(a, "<HHIIQI", cmd, (uint16_t)0, (uint32_t)0, (uint32_t)0x0065, context, (uint32_t)0);
}
