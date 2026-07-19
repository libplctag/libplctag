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
#include <stdint.h>

#include "utils/arena.h"
#include "utils/bytes.h"

/* ============================================================================
 * CPF item type codes
 * ============================================================================ */

#define CPF_ITEM_NULL_ADDR  ((uint16_t)0x0000)
#define CPF_ITEM_CONN_ADDR  ((uint16_t)0x00A1)
#define CPF_ITEM_CONN_DATA  ((uint16_t)0x00B1)
#define CPF_ITEM_UCONN_DATA ((uint16_t)0x00B2)

/* ============================================================================
 * CPF framing sizes
 * ============================================================================ */

#define CPF_HEADER_SIZE                ((size_t)8)
#define CPF_UNCONNECTED_ADDR_ITEM_SIZE ((size_t)4)
#define CPF_UNCONNECTED_DATA_ITEM_SIZE ((size_t)4)
#define CPF_CONNECTED_ADDR_ITEM_SIZE   ((size_t)8)
#define CPF_CONNECTED_DATA_ITEM_SIZE   ((size_t)4)
#define CPF_CONN_SEQ_NUM_SIZE          ((size_t)2)

/* ============================================================================
 * CPF wrap/unwrap codec — single source of truth for both the client
 * (session.c, building requests / parsing replies) and the server (this
 * file's cpf_handle_* dispatch, parsing requests / building responses).
 * ============================================================================ */

/*
 * Wrap a CIP payload for unconnected messaging (SendRRData):
 *   header(iface=0, timeout=0, count=2) + Null Address Item + Unconnected Data Item(cip)
 * Returns bytes_null() on arena exhaustion or a NULL cip.
 */
extern Bytes cpf_wrap_unconnected(Arena *a, Bytes cip);

/*
 * Wrap a CIP payload for connected messaging (SendUnitData):
 *   header(count=2) + Connected Address Item(conn_id) + Connected Data Item(seq + cip)
 * Returns bytes_null() on arena exhaustion or a NULL cip.
 */
extern Bytes cpf_wrap_connected(Arena *a, uint32_t conn_id, uint16_t seq, Bytes cip);

/*
 * Parse a CPF frame of exactly 2 items (address item + data item, the only
 * shape either direction ever sends) and return the embedded CIP payload as
 * a zero-copy slice of `in`. If `connected`, expects Connected Address/Data
 * Items, strips the data item's leading 2-byte sequence number into
 * *seq_out, and (if non-NULL) the address item's connection id into
 * *conn_id_out; otherwise expects Null Address / Unconnected Data Items and
 * sets *seq_out to 0. Returns false if the frame is malformed or the
 * expected item types/count are wrong.
 */
extern bool cpf_unwrap(Bytes in, bool connected, uint32_t *conn_id_out, uint16_t *seq_out, Bytes *cip_out);

/* Server-side dispatch (cpf_handle_unconnected/connected) lives in
 * server/cpf_dispatch.h -- this file is the direction-agnostic codec only,
 * built whenever ENIP is (client needs it too), with no device_t/
 * eip_session_t dependency. */
