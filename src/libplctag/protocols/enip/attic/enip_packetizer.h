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
 * STATUS: PHASE 1. Constants updated per §5.9; enip_packetizer_cip_budget updated.
 * Phase 5: Wire enip_packetizer_plan and fits_single into enip_connection_build_requests
 *   for multi-service 0x0A batching.
 *
 * All byte counts are exact.  Any overrun causes PLC errors.
 *
 * CRUCIAL: The budget values passed to enip_packetizer_cip_budget are the raw
 * ForwardOpen-negotiated sizes (cip_size_o_to_t, cip_size_t_to_o), NOT the EIP+CPF
 * frame sizes. The overhead subtracted is only the CIP-layer overhead (CDI for
 * connected, UDI for unconnected), not the full EIP+CPF stack.
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

/* CIP-layer overhead subtracted from FO-negotiated sizes (plan §5.9).
 * Connected: CDI header(4) + seq_num(2) = 6 bytes
 * Unconnected: UDI header(4) = 4 bytes */
#define ENIP_PKT_CONNECTED_CIP_OVERHEAD   ((size_t)6)
#define ENIP_PKT_UNCONNECTED_CIP_OVERHEAD ((size_t)4)

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

/* Phase 1+: compute CIP-payload budget from a ForwardOpen-negotiated size (plan §5.9).
 * Pass the raw cip_size (e.g., conn->session.cip_size_o_to_t) and use_connected.
 * Returns cip_size minus the CIP-layer overhead (6 connected, 4 unconnected).
 * Returns 0 if cip_size is smaller than the overhead. */
extern size_t enip_packetizer_cip_budget(size_t cip_size, bool use_connected);

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


/* ============================================================================
 * Fragmentation Utilities
 * ============================================================================
 *
 * Standardized element-count / index calculations shared across all manufacturers.
 * "flat index" is the zero-based linear element number across all dimensions,
 * stored in row-major order: flat = d0_idx*(d1*d2) + d1_idx*d2 + d2_idx.
 *
 * dims[0] = outermost dimension size, dims[2] = innermost.
 * num_dims = 0 (scalar), 1, 2, or 3.
 */

/* How many elements of elem_size bytes fit within data_budget bytes.
 * Result is capped at remaining_elems and UINT16_MAX (CIP element_count is uint16_t).
 * Returns 0 if elem_size == 0 or data_budget < elem_size. */
extern uint32_t enip_chunk_elem_count(size_t data_budget, size_t elem_size,
                                      uint32_t remaining_elems);

/* Convert a flat element index to per-dimension indices.
 * out_idx[0]=outermost, out_idx[2]=innermost.
 * Active dimensions are inferred: dim[i] == 0 means not present, treated as 1. */
extern void enip_chunk_flat_to_index(uint32_t flat_idx, const uint32_t dims[3],
                                     int num_dims, uint32_t out_idx[3]);

/* Convert per-dimension indices back to a flat element index. */
extern uint32_t enip_chunk_index_to_flat(const uint32_t idx[3], const uint32_t dims[3],
                                         int num_dims);

/* ============================================================================
 * Dual-budget chunk splitting (plan §15.4)
 * ============================================================================
 *
 * Split a tag's data into chunks respecting both the request and response budgets,
 * element-aligned. Used by the engine to decide how much data fits in one cycle.
 *
 * Returns a structure with:
 *   - data_bytes: how many bytes of payload fit (0 if not even one element fits)
 *   - req_body: the CIP request body size (req_fixed + data_bytes for a write)
 *   - resp_body: the CIP response body size (resp_fixed + data_bytes for a read)
 * If neither req_body nor resp_body changes, the chunk does not fit.
 *
 * Inputs:
 *   - req_avail, resp_avail: available bytes in request and response budgets
 *     (after subtracting any fixed overhead from enip_packetizer_cip_budget)
 *   - remaining: how many elements are left to transfer
 *   - elem_size: bytes per element
 *   - req_fixed: CIP request body overhead (e.g. service+path)
 *   - resp_fixed: CIP response body overhead (e.g. service+status)
 *   - is_write: true if writing (use request budget for data), false if reading
 */
typedef struct {
    uint32_t data_bytes;  /* bytes of actual tag data (0 = does not fit) */
    uint32_t req_body;    /* complete CIP request body size (req_fixed + payload) */
    uint32_t resp_body;   /* complete CIP response body size (resp_fixed + payload) */
} enip_chunk_split_result_t;

extern enip_chunk_split_result_t enip_chunk_split(
    size_t req_avail,
    size_t resp_avail,
    uint32_t remaining,
    size_t elem_size,
    uint32_t req_fixed,
    uint32_t resp_fixed,
    bool is_write
);
