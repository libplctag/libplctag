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
 * CPF (Common Packet Format) Layer
 *
 * STATUS: KEEP AS-IS for all phases.  No changes needed.
 *   enip_cpf_build_unconnected is used from Phase 1.
 *   enip_cpf_build_connected is used from Phase 3 (after ForwardOpen succeeds).
 *
 * Unconnected framing (SendRRData 0x006F):
 *   CPF header(8) + NAI(4) + UDI header(4) + CIP payload
 *   Overhead = 16 bytes
 *
 * Connected framing (SendUnitData 0x0070):
 *   CPF header(8) + CAI(8) + CDI header(4) + seq_num(2) + CIP payload
 *   Overhead = 22 bytes
 */

#include <stdint.h>
#include <utils/arena.h>
#include <utils/bytes.h>

/* CPF item type codes */
#define ENIP_CPF_ITEM_NAI       ((uint16_t)0x0000) /* Null Address Item      */
#define ENIP_CPF_ITEM_CONN_ADDR ((uint16_t)0x00A1) /* Connected Address Item */
#define ENIP_CPF_ITEM_CONN_DATA ((uint16_t)0x00B1) /* Connected Data Item    */
#define ENIP_CPF_ITEM_UDI       ((uint16_t)0x00B2) /* Unconnected Data Item  */

/* CPF framing sizes in bytes */
#define ENIP_CPF_HEADER_SIZE          ((size_t)8)  /* iface_handle(4)+timeout(2)+item_count(2) */
#define ENIP_CPF_NAI_ITEM_SIZE        ((size_t)4)  /* type(2)+length(2) */
#define ENIP_CPF_UDI_ITEM_HEADER_SIZE ((size_t)4)  /* type(2)+length(2) */
#define ENIP_CPF_CONN_ADDR_ITEM_SIZE  ((size_t)8)  /* type(2)+length(2)+conn_id(4) */
#define ENIP_CPF_CONN_DATA_HDR_SIZE   ((size_t)4)  /* type(2)+length(2) */
#define ENIP_CPF_CONN_SEQ_NUM_SIZE    ((size_t)2)  /* sequence number */

/* Total overhead added by CPF to the CIP payload */
#define ENIP_CPF_UNCONNECTED_OVERHEAD ((size_t)16) /* header+NAI+UDI header */
#define ENIP_CPF_CONNECTED_OVERHEAD   ((size_t)22) /* header+CAI+CDI header+seq */

typedef struct {
    uint32_t interface_handle;
    uint16_t router_timeout;
    uint16_t item_count;
} enip_cpf_header_t;

/* Phase 1: used for all unconnected messaging (RegisterSession, GetIdentity, tag reads/writes
 * before ForwardOpen).  Build an unconnected CPF frame (for SendRRData).
 * Returns: CPF header + NAI + UDI header + cip_payload, or bytes_null(). */
extern Bytes enip_cpf_build_unconnected(Arena *arena, Bytes cip_payload);

/* Phase 3: used after ForwardOpen succeeds.  Build a connected CPF frame (for SendUnitData).
 * connection_id is conn->cip_targ_conn_id (O->T id from ForwardOpen response).
 * seq_num is conn->cip_conn_seq_num, incremented before each call.
 * Returns: CPF header + CAI(conn_id) + CDI header + seq_num + cip_payload, or bytes_null(). */
extern Bytes enip_cpf_build_connected(Arena *arena, uint32_t connection_id, uint16_t seq_num,
                                      Bytes cip_payload);

/* Phase 1: used internally for response parsing.  Parse the 8-byte CPF header.
 * Returns PLCTAG_STATUS_OK on success. */
extern int32_t enip_cpf_parse_header(Bytes cpf_frame, enip_cpf_header_t *out_header);

/* Phase 1: extract the unconnected CIP payload from a CPF response frame.
 * Returns bytes_null() if not found or frame is malformed. */
extern Bytes enip_cpf_extract_udi_payload(Bytes cpf_frame);

/* Phase 3: extract the connected CIP payload from a CPF response frame
 * (begins after the 2-byte sequence number).
 * Returns bytes_null() if not found or frame is malformed. */
extern Bytes enip_cpf_extract_cdi_payload(Bytes cpf_frame);
