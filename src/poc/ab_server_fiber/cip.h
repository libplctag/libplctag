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
 * cip.h — CIP (Common Industrial Protocol) dispatcher.
 *
 * Two entry points:
 *   cip_dispatch_unconnected — for EIP UnconnectedSend (0x006F) payloads.
 *                              Handles Forward Open/Close, PCCC execute, and
 *                              embedded unconnected-send wrapping.
 *   cip_dispatch_connected   — for EIP ConnectedSend (0x0070) payloads.
 *                              Handles Multi-Service, Read, Write.
 */

#include "arena.h"
#include "bytes.h"
#include "plc.h"

/* ============================================================================
 * Public API
 * ============================================================================ */

/*
 * Dispatch a CIP request arriving via an unconnected EIP send.
 * Handles: Forward Open (0x54/0x5B), Forward Close (0x4E),
 *          PCCC Execute (0x4B), Unconnected Send (0x52).
 */
extern Bytes cip_dispatch_unconnected(Arena *a, Bytes payload, eip_session_t *sess, plc_config_t *cfg);

/*
 * Dispatch a CIP request arriving via a connected EIP send.
 * Handles: Multi-Service (0x0A), Read (0x4C/0x52), Write (0x4D/0x53).
 */
extern Bytes cip_dispatch_connected(Arena *a, Bytes payload, eip_session_t *sess, plc_config_t *cfg, size_t max_resp);
