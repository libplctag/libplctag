#pragma once
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

/*
 * ENIP Transaction Seam (plan §8)
 *
 * One place for CIP request→response round trips: CPF+EIP framing, socket I/O,
 * unwrap, and return the CIP response slice. All send/recv blocks in the system
 * route through here; framing changes touch one file.
 *
 * STATUS: Phase 2. Core transaction logic implemented; used by bootstrap sequence
 * (GetIdentity, ForwardOpen/Close, metadata, symbol count). Identity round-trip
 * tested.
 */

#include <libplctag/protocols/enip/client/enip_conn.h>
#include <utils/arena.h>
#include <utils/bytes.h>
#include <stdint.h>

typedef enum {
    ENIP_MSG_UNCONNECTED,   /* SendRRData 0x006F + CPF unconnected (NAI + UDI) */
    ENIP_MSG_CONNECTED,     /* SendUnitData 0x0070 + CPF connected (CAI + CDI)  */
} enip_msg_mode_t;

/* Execute one CIP transaction: wrap CIP request in CPF+EIP, send, receive,
 * unwrap, return CIP response slice.
 *
 * Inputs:
 *   link — transport (socket, I/O state)
 *   session — EIP+CIP session state (session_handle, sender_context, CIP conn IDs for connected)
 *   tx_arena — scratch arena for building the frame (reset before use; can be reused)
 *   rx_arena — scratch arena for receiving the response (reset before use)
 *   mode — UNCONNECTED (0x006F SendRRData) or CONNECTED (0x0070 SendUnitData)
 *   cip_request — the pre-built CIP request bytes
 *
 * Outputs:
 *   out_context — the sender_context value used for this request (for correlation)
 *   out_cip_response — CIP response slice (service reply + status + data), into rx_arena
 *
 * Returns:
 *   PLCTAG_STATUS_OK + populated out_* on success
 *   PLCTAG_ERR_* on frame-build, socket, or parse errors.
 *
 * Side effects:
 *   - Resets tx_arena to build the frame
 *   - Resets rx_arena to receive the response
 *   - Increments session->sender_context (stamped into the request before send)
 *   - For CONNECTED: increments session->cip_seq_num
 */
extern int32_t enip_txn(enip_link_t *link, enip_session_t *session,
                        Arena *tx_arena, Arena *rx_arena,
                        enip_msg_mode_t mode, Bytes cip_request,
                        uint64_t *out_context, Bytes *out_cip_response);
