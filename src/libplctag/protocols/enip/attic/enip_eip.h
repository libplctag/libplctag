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
 * EIP (EtherNet/IP) Encapsulation Layer
 *
 * STATUS: KEEP AS-IS for all phases.  No changes needed.
 *   All four functions are used from Phase 1 onward.
 *   Phase 6: enip_eip_build_request increments *sender_context_inout; the caller
 *     must store that value in tag->transaction_id for response correlation.
 *
 * Parses and builds the 24-byte EIP header (all little-endian):
 *   offset  0  uint16  command
 *   offset  2  uint16  length (payload bytes after the header)
 *   offset  4  uint32  session_handle
 *   offset  8  uint32  status (0 in requests)
 *   offset 12  uint64  sender_context
 *   offset 20  uint32  options (0 in requests)
 */

#include <stdint.h>
#include <utils/arena.h>
#include <utils/bytes.h>

/* EIP header size in bytes */
#define ENIP_EIP_HEADER_SIZE ((size_t)24)

/* EIP command codes */
#define ENIP_CMD_LIST_IDENTITY      ((uint16_t)0x0063)
#define ENIP_CMD_REGISTER_SESSION   ((uint16_t)0x0065)
#define ENIP_CMD_UNREGISTER_SESSION ((uint16_t)0x0066)
#define ENIP_CMD_UNCONNECTED_SEND   ((uint16_t)0x006F) /* SendRRData  */
#define ENIP_CMD_CONNECTED_SEND     ((uint16_t)0x0070) /* SendUnitData */

typedef struct {
    uint16_t command;
    uint16_t length;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;
} enip_eip_header_t;

/* Phase 1: used to validate every response.  Parse the 24-byte EIP header.
 * Returns PLCTAG_STATUS_OK, or an error code if the buffer is too short. */
extern int32_t enip_eip_parse_header(Bytes response, enip_eip_header_t *out_header);

/* Phase 1: low-level header packer; used internally by enip_eip_build_request. */
extern Bytes enip_eip_pack_header(Arena *arena, const enip_eip_header_t *header);

/* Phase 1: used to send every request.  Build a complete EIP frame: header + cpf_payload.
 * command must be ENIP_CMD_UNCONNECTED_SEND or ENIP_CMD_CONNECTED_SEND.
 * Increments *sender_context_inout — Phase 6: caller must store the post-increment value
 * in tag->transaction_id before calling so the response can be correlated.
 * Returns the full frame or bytes_null() on arena exhaustion. */
extern Bytes enip_eip_build_request(Arena *arena, uint16_t command, uint32_t session_handle,
                                    uint64_t *sender_context_inout, Bytes cpf_payload);

/* Phase 1: extract CPF payload from a response frame (slice after the 24-byte EIP header).
 * Returns bytes_null() if response.len < ENIP_EIP_HEADER_SIZE. */
extern Bytes enip_eip_extract_cpf_payload(Bytes response);
