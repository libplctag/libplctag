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

/*
 * cpf.h — Common Packet Format (CPF) layer dispatcher.
 *
 * Parses the CPF item array that follows the EIP header, routes the embedded
 * CIP payload to the appropriate handler, and rewraps the CIP response in a
 * CPF envelope.
 */

#include <stdint.h>

#include "arena.h"
#include "bytes.h"
#include "plc.h"

/* ============================================================================
 * CPF item type codes
 * ============================================================================ */

#define CPF_ITEM_NULL_ADDR  ((uint16_t)0x0000) /* null address item */
#define CPF_ITEM_CONN_ADDR  ((uint16_t)0x00A1) /* connected address item */
#define CPF_ITEM_CONN_DATA  ((uint16_t)0x00B1) /* connected data item */
#define CPF_ITEM_UCONN_DATA ((uint16_t)0x00B2) /* unconnected data item */

/* ============================================================================
 * CPF framing sizes (bytes)
 * ============================================================================ */

/* Common CPF prefix: iface_handle(4) + timeout(2) + item_count(2) */
#define CPF_HEADER_SIZE               ((size_t)8)

/* Null address item: type(2) + len(2) */
#define CPF_UNCONNECTED_ADDR_ITEM_SIZE ((size_t)4)
/* Unconnected data item header: type(2) + len(2) */
#define CPF_UNCONNECTED_DATA_ITEM_SIZE ((size_t)4)

/* Connected address item: type(2) + len(2) + conn_id(4) */
#define CPF_CONNECTED_ADDR_ITEM_SIZE  ((size_t)8)
/* Connected data item header: type(2) + len(2) */
#define CPF_CONNECTED_DATA_ITEM_SIZE  ((size_t)4)
/* Sequence number field inside connected data item */
#define CPF_CONN_SEQ_NUM_SIZE         ((size_t)2)

/* ============================================================================
 * Public API
 * ============================================================================ */

/*
 * Handle an unconnected EIP payload (command 0x006F).
 * Parses the CPF envelope, dispatches the embedded CIP request via
 * cip_dispatch_unconnected(), and returns a CPF-wrapped response.
 */
extern Bytes cpf_handle_unconnected(Arena *a, Bytes payload, eip_session_t *sess, plc_config_t *cfg);

/*
 * Handle a connected EIP payload (command 0x0070).
 * Validates connection ID and sequence number, dispatches the embedded CIP
 * request via cip_dispatch_connected(), and returns a CPF-wrapped response.
 */
extern Bytes cpf_handle_connected(Arena *a, Bytes payload, eip_session_t *sess, plc_config_t *cfg);
