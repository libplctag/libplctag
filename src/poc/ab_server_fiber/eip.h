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
 * eip.h — EIP (Ethernet/IP) layer dispatcher.
 *
 * Parses the 24-byte EIP header, routes to RegisterSession / UnregisterSession /
 * UnconnectedSend / ConnectedSend, and returns a fully-formed EIP response.
 */

#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "bytes.h"
#include "plc.h"

/* ============================================================================
 * EIP header constants
 * ============================================================================ */

#define EIP_HEADER_SIZE ((size_t)24)

#define EIP_CMD_REGISTER_SESSION   ((uint16_t)0x0065)
#define EIP_CMD_UNREGISTER_SESSION ((uint16_t)0x0066)
#define EIP_CMD_UNCONNECTED_SEND   ((uint16_t)0x006F)
#define EIP_CMD_CONNECTED_SEND     ((uint16_t)0x0070)

/* ============================================================================
 * Public API
 * ============================================================================ */

/*
 * Dispatch an EIP command and return the complete EIP response
 * (header + payload) allocated from arena a.
 * cmd and payload are decoded from the EIP header by the caller.
 * sess->sender_context must be set by the caller before this call.
 * Returns {NULL, 0} on UnregisterSession or fatal error — caller should close.
 */
extern Bytes eip_dispatch(Arena *a, uint16_t cmd, Bytes payload, eip_session_t *sess, plc_config_t *cfg);
