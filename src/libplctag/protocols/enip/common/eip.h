#pragma once

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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/arena.h"
#include "utils/bytes.h"

/* ============================================================================
 * EIP command codes
 * ============================================================================ */

#define EIP_HEADER_SIZE            ((size_t)24)

#define EIP_CMD_LIST_IDENTITY      ((uint16_t)0x0063)
#define EIP_CMD_REGISTER_SESSION   ((uint16_t)0x0065)
#define EIP_CMD_UNREGISTER_SESSION ((uint16_t)0x0066)
#define EIP_CMD_UNCONNECTED_SEND   ((uint16_t)0x006F)
#define EIP_CMD_CONNECTED_SEND     ((uint16_t)0x0070)

/* ============================================================================
 * EIP encapsulation header codec — single source of truth for both the
 * client (session.c) and the server (this file's eip_dispatch). 24-byte
 * little-endian header: cmd(2) payload_len(2) session_handle(4) status(4)
 * sender_context(8) options(4).
 * ============================================================================ */

typedef struct {
    uint16_t cmd;
    uint16_t payload_len;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;
} eip_hdr_t;

/* Parse a header-only buffer (exactly EIP_HEADER_SIZE bytes, no payload
 * length validation against a companion buffer -- used when the caller
 * already has header and payload as separate slices, e.g. the server). */
extern bool eip_parse_hdr(Bytes hdr_buf, eip_hdr_t *hdr);

/* Encode just the 24-byte header (no payload appended). */
extern Bytes eip_encode_hdr(Arena *a, eip_hdr_t *hdr);

/* Encode header + payload into one arena-allocated frame. hdr->payload_len
 * is overwritten with payload.len. Returns bytes_null() on arena exhaustion. */
extern Bytes eip_encode(Arena *a, eip_hdr_t *hdr, Bytes payload);

/* Split a received frame into header and payload (zero-copy slice of `in`).
 * Returns false if in.len < EIP_HEADER_SIZE or in.len < EIP_HEADER_SIZE +
 * hdr->payload_len. */
extern bool eip_decode(Bytes in, eip_hdr_t *hdr, Bytes *payload);

/* Server-side dispatch (eip_dispatch, eip_session_set_*_sizes) lives in
 * server/eip_dispatch.h -- this file is the direction-agnostic codec only,
 * built whenever ENIP is (client needs it too), with no device_t/
 * eip_session_t dependency. */
