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
 * pccc.h — PCCC (Programmable Controller Communications Command) dispatcher.
 *
 * Called from CIP when service 0x4B (PCCC Execute) is received.
 * Supports PLC/5 read/write/RMW and SLC/Micrologix read/write/RMW.
 * The full CIP payload (starting at the CIP service byte) is passed in;
 * this function skips the 13-byte CIP+PCCC wrapper to reach the PCCC command.
 */

#include "arena.h"
#include "bytes.h"
#include "plc.h"

/* ============================================================================
 * Public API
 * ============================================================================ */

/*
 * Dispatch a PCCC request.
 * payload is the full CIP payload received after stripping the EIP/CPF headers.
 * Returns a fully-formed response including the 11-byte PCCC response prefix.
 */
extern Bytes pccc_dispatch(Arena *a, Bytes payload, eip_session_t *sess, plc_config_t *cfg);
