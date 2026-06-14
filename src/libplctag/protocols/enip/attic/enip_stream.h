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
 * ENIP Stream Framing
 *
 * Implements EIP frame reading over a socket using stream framing: read 24-byte
 * EIP header, extract length field, read exactly that many additional bytes.
 * Used by RegisterSession, GetIdentity, and all subsequent message exchanges.
 *
 * STATUS: Phase 1 implementation. Correct for all phases.
 */

#include <utils/arena.h>
#include <utils/bytes.h>
#include <utils/enip_wait.h>
#include <platform.h>

/* Read complete EIP frame from socket using stream framing.
 * Reads 24-byte header, parses length field, reads body.
 * Returns complete frame (header + body) or bytes_null() on error.
 * timeout_ms: maximum time to wait for complete frame.
 * io: persistent I/O state for restartable reads (for Phase 6 async). */
extern int32_t enip_recv_frame(sock_p socket, Arena *arena, int timeout_ms,
                               socket_wait_state_t *io, Bytes *out_frame);
