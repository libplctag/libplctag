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
 * EIP encapsulation header encode/decode (design doc §14.3).
 *
 * 24-byte little-endian header:
 *   offset  0  uint16  command
 *   offset  2  uint16  length (payload bytes following the header)
 *   offset  4  uint32  session_handle
 *   offset  8  uint32  status (0 in requests)
 *   offset 12  uint64  sender_context
 *   offset 20  uint32  options (0 in requests)
 */

#include <stdbool.h>
#include <stdint.h>
#include <utils/arena.h>
#include <utils/bytes.h>

#define ENIP_EIP_HEADER_SIZE ((size_t)24)

/* EIP command codes */
#define ENIP_CMD_REGISTER_SESSION   ((uint16_t)0x0065)
#define ENIP_CMD_UNREGISTER_SESSION ((uint16_t)0x0066)
#define ENIP_CMD_UNCONNECTED_SEND   ((uint16_t)0x006F) /* SendRRData   */
#define ENIP_CMD_CONNECTED_SEND     ((uint16_t)0x0070) /* SendUnitData */

typedef struct {
    uint16_t command;
    uint16_t length;
    uint32_t session_handle;
    uint32_t status;
    uint64_t sender_context;
    uint32_t options;
} enip_eip_hdr_t;

/* Encode header + payload into a single arena-allocated frame.  h->length is
 * overwritten with payload.len.  Returns bytes_null() on arena exhaustion. */
extern Bytes enip_eip_encode(Arena *a, enip_eip_hdr_t *h, Bytes payload);

/* Split a received frame into its header and payload (zero-copy slice of
 * `in`).  Returns false if in.len < 24 or in.len < 24 + h->length. */
extern bool enip_eip_decode(Bytes in, enip_eip_hdr_t *h, Bytes *payload);

/* Build a RegisterSession request (command 0x0065, session_handle=0,
 * protocol_version=1, options=0). */
extern Bytes enip_eip_register_session(Arena *a);

/* Build an UnregisterSession request (command 0x0066, no payload). The target
 * sends no reply to this command -- fire-and-forget, same as the server's
 * handle_unregister_session (common/eip.c). */
extern Bytes enip_eip_unregister_session(Arena *a, uint32_t session_handle);

/* Wrap a CPF frame in a SendRRData request (command 0x006F, unconnected). */
extern Bytes enip_eip_send_rr_data(Arena *a, uint32_t session_handle, Bytes cpf);

/* Wrap a CPF frame in a SendUnitData request (command 0x0070, connected). */
extern Bytes enip_eip_send_unit_data(Arena *a, uint32_t session_handle, Bytes cpf);
