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
 * ENIP Packet Budget Implementation
 *
 * STATUS: CORRECT, but currently unused.  Wire it in during Phase 5.
 *
 * Phase 5 (Multi-service 0x0A): this file is the gating logic for deciding
 *   whether to use 0x0A batching or a single request.  Wire it in from
 *   enip_connection_build_requests before packing sub-requests.
 *   Call enip_packetizer_plan to fill the slot_count / slot_req_sizes /
 *   slot_resp_sizes arrays, then build the 0x0A wrapper in enip_multi.c.
 *
 * No functional changes needed.  One constant audit (plan §6): the overhead
 * values (ENIP_PKT_OVERHEAD_UNCONNECTED=40, ENIP_PKT_OVERHEAD_CONNECTED=46)
 * should be verified against the actual EIP+CPF header sizes produced by the
 * layer functions (EIP=24, CPF header=8, NAI=4, UDI header=4 = 40 total; that
 * matches — no change needed).
 */

#include <libplctag/protocols/enip/enip.h>
#include <libplctag/protocols/enip/enip_packetizer.h>
#include <utils/debug.h>

/* Phase 1: compute CIP-payload budget from a ForwardOpen-negotiated size (plan §5.9).
 * Subtracts only the CIP-layer overhead, not the full EIP+CPF stack. */
size_t enip_packetizer_cip_budget(size_t cip_size, bool use_connected) {
    size_t overhead = use_connected ? ENIP_PKT_CONNECTED_CIP_OVERHEAD
                                    : ENIP_PKT_UNCONNECTED_CIP_OVERHEAD;

    if(cip_size <= overhead) { return 0; }

    return cip_size - overhead;
}

/* Phase 5: correct as-is — use this to gate whether a single tag fits without 0x0A. */
bool enip_packetizer_fits_single(size_t max_buffer, bool use_connected,
                                 size_t req_size, size_t resp_size) {
    size_t budget = enip_packetizer_cip_budget(max_buffer, use_connected);

    /* Both the request and the response must independently fit within the
     * same buffer limit -- the PLC uses the same negotiated size for both. */
    return (budget > 0) && (req_size <= budget) && (resp_size <= budget);
}

uint32_t enip_chunk_elem_count(size_t data_budget, size_t elem_size, uint32_t remaining_elems) {
    if(elem_size == 0 || data_budget < elem_size) { return 0; }
    uint32_t fits = (uint32_t)(data_budget / elem_size);
    if(fits > 65535u) { fits = 65535u; }
    if(fits > remaining_elems) { fits = remaining_elems; }
    return fits;
}


void enip_chunk_flat_to_index(uint32_t flat_idx, const uint32_t dims[3],
                               int num_dims, uint32_t out_idx[3]) {
    out_idx[0] = out_idx[1] = out_idx[2] = 0;

    if(num_dims <= 0) { return; }

    if(num_dims == 1) {
        out_idx[0] = flat_idx;
        return;
    }

    if(num_dims == 2) {
        uint32_t d1 = (dims[1] > 0) ? dims[1] : 1u;
        out_idx[1] = flat_idx % d1;
        out_idx[0] = flat_idx / d1;
        return;
    }

    /* 3D */
    uint32_t d2 = (dims[2] > 0) ? dims[2] : 1u;
    uint32_t d1 = (dims[1] > 0) ? dims[1] : 1u;
    out_idx[2] = flat_idx % d2;
    out_idx[1] = (flat_idx / d2) % d1;
    out_idx[0] = flat_idx / (d2 * d1);
}


uint32_t enip_chunk_index_to_flat(const uint32_t idx[3], const uint32_t dims[3],
                                   int num_dims) {
    if(num_dims <= 1) { return idx[0]; }

    uint32_t d2 = (dims[2] > 0) ? dims[2] : 1u;
    uint32_t d1 = (dims[1] > 0) ? dims[1] : 1u;

    if(num_dims == 2) { return idx[0] * d1 + idx[1]; }

    return idx[0] * (d1 * d2) + idx[1] * d2 + idx[2];
}


enip_chunk_split_result_t enip_chunk_split(
    size_t req_avail,
    size_t resp_avail,
    uint32_t remaining,
    size_t elem_size,
    uint32_t req_fixed,
    uint32_t resp_fixed,
    bool is_write) {

    enip_chunk_split_result_t result = {0, 0, 0};

    if(elem_size == 0 || remaining == 0) { return result; }
    if(req_avail < req_fixed || resp_avail < resp_fixed) { return result; }

    /* Available data budget in each direction */
    size_t req_data_budget = req_avail - (size_t)req_fixed;
    size_t resp_data_budget = resp_avail - (size_t)resp_fixed;

    /* For writes, the request carries the data; for reads, the response does.
     * Both directions must accommodate their share. */
    size_t usable_bytes = is_write
        ? (req_data_budget < resp_data_budget ? req_data_budget : resp_data_budget)
        : (resp_data_budget < req_data_budget ? resp_data_budget : req_data_budget);

    /* Cap to the number of remaining elements */
    if(usable_bytes > (size_t)remaining * elem_size) {
        usable_bytes = (size_t)remaining * elem_size;
    }

    /* Element alignment: for atomic types (elem_size <= 8), align to elem_size;
     * for aggregates, align to 8-byte boundaries. */
    size_t unit = elem_size < 8 ? elem_size : 8;
    size_t chunk_bytes = (usable_bytes / unit) * unit;

    if(chunk_bytes < unit) { return result; }

    /* Compute the request and response body sizes */
    size_t req_body_size = (size_t)req_fixed + (is_write ? chunk_bytes : 0);
    size_t resp_body_size = (size_t)resp_fixed + (is_write ? 0 : chunk_bytes);

    if(req_body_size > (size_t)UINT32_MAX || resp_body_size > (size_t)UINT32_MAX) {
        return result;
    }

    result.data_bytes = (uint32_t)chunk_bytes;
    result.req_body = (uint32_t)req_body_size;
    result.resp_body = (uint32_t)resp_body_size;

    return result;
}


/* Phase 5: correct as-is — call this to determine how many tags fit in one 0x0A batch.
 * On return, plan->slot_count tells the caller how many sub-requests to pack. */
int32_t enip_packetizer_plan(const enip_pkt_plan_t *plan) {
    if(!plan) { return PLCTAG_ERR_NULL_PTR; }

    if(plan->slot_count == 0 || plan->slot_count > ENIP_PKT_MAX_SLOTS) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP packetizer: invalid slot_count %u", plan->slot_count);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    size_t cip_budget = enip_packetizer_cip_budget(plan->max_buffer, plan->use_connected);
    if(cip_budget == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP packetizer: max_buffer %zu too small for framing overhead",
               plan->max_buffer);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* Multi-service-request overhead: fixed header + one uint16_t offset per slot.
     * This overhead is identical on both the request and response sides. */
    size_t slot_overhead = ENIP_PKT_MULTI_SLOT_OVERHEAD * (size_t)plan->slot_count;

    size_t req_used  = ENIP_PKT_MULTI_REQ_FIXED  + slot_overhead;
    size_t resp_used = ENIP_PKT_MULTI_RESP_FIXED + slot_overhead;

    for(uint32_t i = 0; i < plan->slot_count; i++) {
        req_used  += (size_t)plan->slot_req_sizes[i];
        resp_used += (size_t)plan->slot_resp_sizes[i];
    }

    if(req_used > cip_budget) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP packetizer: request aggregate %zu exceeds CIP budget %zu",
               req_used, cip_budget);
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(resp_used > cip_budget) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP packetizer: response aggregate %zu exceeds CIP budget %zu",
               resp_used, cip_budget);
        return PLCTAG_ERR_TOO_LARGE;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_SPEW, 0,
           "ENIP packetizer: %u slots, req=%zu resp=%zu budget=%zu OK",
           plan->slot_count, req_used, resp_used, cip_budget);

    return PLCTAG_STATUS_OK;
}
