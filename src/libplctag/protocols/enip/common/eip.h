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

#include <stddef.h>
#include <stdint.h>

#include "utils/arena.h"
#include "utils/bytes.h"
#include <libplctag/protocols/enip/server/device.h>

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
 * Public API
 * ============================================================================ */

/*
 * Parse EIP header, dispatch command, return complete EIP response (header + payload).
 * hdr must be exactly EIP_HEADER_SIZE bytes; payload may be empty.
 * Returns {NULL,0} on UnregisterSession or fatal error — caller should close.
 */
extern Bytes eip_dispatch(Arena *a, Bytes hdr, Bytes payload, eip_session_t *sess, device_t *dev);

extern void eip_session_set_unconnected_sizes(eip_session_t *sess, uint32_t raw_packet_size);
extern void eip_session_set_connected_sizes(eip_session_t *sess, uint32_t raw_packet_size);
