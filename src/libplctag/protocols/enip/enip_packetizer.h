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
 * ENIP Packet Budget Calculation
 *
 * STATUS: KEEP AS-IS.  Wire in during Phase 5 (Multi-service 0x0A batching).
 *   enip_packetizer_cip_budget is also useful earlier (Phase 1) to compute the
 *   budget to pass as cip_budget to encode_chunk.
 *   Phase 5: call enip_packetizer_plan before building a 0x0A wrapper to verify
 *   the aggregate of all sub-requests fits in conn->max_packet_buffer_size.
 *
 * All byte counts are exact.  Any overrun causes PLC errors.
 *
 * Unconnected overhead (SendRRData 0x006F):
 *   EIP header(24) + CPF header(8) + NAI(4) + UDI header(4) = 40 bytes
 *
 * Connected overhead (SendUnitData 0x0070):
 *   EIP header(24) + CPF header(8) + CAI(8) + CDI header(4) + seq_num(2) = 46 bytes
 *
 * Multiple-Service-Request wrapper (CIP service 0x0A):
 *   request:  service(1)+path(3)+reserved(1)+count(2)+offsets(2*N) = 7+2*N bytes
 *   response: service(1)+reserved(1)+status(1)+ext(1)+count(2)+offsets(2*N) = 6+2*N bytes
 *
 * Practical slot maximum: with a 4000-byte negotiated buffer and the smallest
 * possible CIP request (6-8 bytes) the request side allows ~494 slots; with the
 * smallest possible response (4 bytes) the response side allows ~664 slots.
 * ENIP_PKT_MAX_SLOTS = 512 covers all realistic cases.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Fixed overhead consumed by EIP+CPF framing */
#define ENIP_PKT_UNCONNECTED_OVERHEAD ((size_t)40)
#define ENIP_PKT_CONNECTED_OVERHEAD   ((size_t)46)

/* Overhead of the Multiple-Service-Request (0x0A) wrapper */
#define ENIP_PKT_MULTI_REQ_FIXED     ((size_t)7) /* header bytes before offset table */
#define ENIP_PKT_MULTI_RESP_FIXED    ((size_t)6)
#define ENIP_PKT_MULTI_SLOT_OVERHEAD ((size_t)2) /* one uint16_t offset entry per slot */

/* Maximum slots per packed frame.  At 6 bytes/request in a 4000-byte buffer the
 * request-side limit is (4000-40-7)/(6+2) = ~494; 512 gives headroom. */
#define ENIP_PKT_MAX_SLOTS ((uint32_t)512)

/*
 * Packet plan: caller populates slot_count, slot_req_sizes[], slot_resp_sizes[],
 * use_connected, and max_buffer, then calls enip_packetizer_plan() to verify fit.
 *
 * Sizes are uint16_t because no individual CIP request or response can exceed
 * 65535 bytes (the EIP length field is also uint16_t).
 */
typedef struct {
    uint32_t slot_count;
    uint16_t slot_req_sizes[ENIP_PKT_MAX_SLOTS];
    uint16_t slot_resp_sizes[ENIP_PKT_MAX_SLOTS];
    bool     use_connected;
    size_t   max_buffer;
} enip_pkt_plan_t;

/* Phase 1+: call to get the CIP payload budget to pass as cip_budget to encode_chunk.
 * Net CIP bytes available after EIP+CPF overhead.
 * Returns 0 if max_buffer is smaller than the framing overhead. */
extern size_t enip_packetizer_cip_budget(size_t max_buffer, bool use_connected);

/* Phase 5: call to decide whether to use a single request or 0x0A batching.
 * True if a single CIP request/response pair fits within max_buffer after
 * framing overhead, without any multi-service packing. */
extern bool enip_packetizer_fits_single(size_t max_buffer, bool use_connected,
                                        size_t req_size, size_t resp_size);

/* Phase 5: call before building the 0x0A wrapper to verify aggregate fits.
 * Verify that all slots in plan pack into plan->max_buffer using 0x0A packing.
 * Returns PLCTAG_STATUS_OK if the aggregate fits.
 * Returns PLCTAG_ERR_TOO_LARGE if the frame overruns on either the request or
 * the response side.
 * Returns PLCTAG_ERR_OUT_OF_BOUNDS if slot_count is 0 or > ENIP_PKT_MAX_SLOTS. */
extern int32_t enip_packetizer_plan(const enip_pkt_plan_t *plan);
