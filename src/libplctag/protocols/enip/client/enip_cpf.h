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
 * Common Packet Format (CPF), connected and unconnected (design doc §14.4).
 *
 * Unconnected framing (for SendRRData, command 0x006F):
 *   CPF header(8) + Null Address Item(4) + Unconnected Data Item header(4) + cip
 *   Overhead = 16 bytes
 *
 * Connected framing (for SendUnitData, command 0x0070):
 *   CPF header(8) + Connected Address Item(8) + Connected Data Item header(4)
 *     + sequence number(2) + cip
 *   Overhead = 22 bytes
 */

#include <stdbool.h>
#include <stdint.h>
#include <utils/arena.h>
#include <utils/bytes.h>

/* CPF item type codes */
#define CPF_NULL_ADDR  ((uint16_t)0x0000)
#define CPF_CONN_ADDR  ((uint16_t)0x00A1)
#define CPF_CONN_DATA  ((uint16_t)0x00B1)
#define CPF_UCONN_DATA ((uint16_t)0x00B2)

/* CPF framing overhead, in bytes, added to the CIP payload */
#define ENIP_CPF_UNCONNECTED_OVERHEAD ((size_t)16)
#define ENIP_CPF_CONNECTED_OVERHEAD   ((size_t)22)

/*
 * Wrap a CIP payload for unconnected messaging (SendRRData):
 *   header(iface=0, timeout=0, count=2) + Null Address Item + Unconnected Data Item(cip)
 * Returns bytes_null() on arena exhaustion or a NULL cip.
 */
extern Bytes enip_cpf_wrap_unconnected(Arena *a, Bytes cip);

/*
 * Wrap a CIP payload for connected messaging (SendUnitData):
 *   header(count=2) + Connected Address Item(conn_id) + Connected Data Item(seq + cip)
 * Returns bytes_null() on arena exhaustion or a NULL cip.
 */
extern Bytes enip_cpf_wrap_connected(Arena *a, uint32_t conn_id, uint16_t seq, Bytes cip);

/*
 * Parse a CPF item array and return the embedded CIP payload as a zero-copy
 * slice of `in`.  If `connected`, looks for the Connected Data Item, strips
 * its leading 2-byte sequence number into *seq_out, and returns the rest as
 * *cip_out; otherwise looks for the Unconnected Data Item and sets *seq_out
 * to 0.  Returns false if the frame is malformed or the expected item is
 * missing.
 */
extern bool enip_cpf_unwrap(Bytes in, bool connected, uint16_t *seq_out, Bytes *cip_out);
