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
 * Omron NJ/NX Strategy Implementation
 *
 * STATUS: BROKEN STUB — complete rewrite required in Phase 8.
 *
 * Phase 0: This file currently compiles but is entirely wrong:
 *   - encode_request calls enip_cip_write_tag_request and enip_cip_read_tag_request
 *     with stale extra arguments (sequence_id) that don't match the current
 *     signatures — this will cause compile errors when the build is clean.
 *     FIX (Phase 0): stub every function body to return PLCTAG_ERR_UNSUPPORTED
 *     so the tree compiles.  Leave all function signatures and the ops struct.
 *   - DEBUG_MODULE_LIB must become DEBUG_MODULE_ENIP in all pdebug calls.
 *
 * Phase 8 (OMRON, plan §3 I, §4): REWRITE all five functions.
 *   - Remove estimate_request_size and encode_request/decode_response/needs_more.
 *   - Add encode_chunk: emit service 0x4C/0x4D with path + element_count(2) +
 *     the 8-byte 0x80 data segment:
 *       {0x80, 0x03, total_elem_count(2 LE), byte_offset(4 LE)}
 *     where total_elem_count is constant across chunks and byte_offset is
 *     tag->byte_offset.  Returns bytes_null() when byte_offset >= tag total.
 *   - Add accept_chunk: parse response with enip_cip_parse_response; strip 2-byte
 *     type code on first chunk; copy data; advance tag->byte_offset; return
 *     PLCTAG_ERR_PARTIAL until byte_offset reaches total, then PLCTAG_STATUS_OK.
 *     OMRON connected buffer is ~1996 bytes.
 *   - fetch_phase1_metadata: OMRON does not support GetInstanceAttributeList on
 *     class 0x6B — return PLCTAG_STATUS_OK immediately (no-op is correct).
 *   - Update ops struct to encode_chunk/accept_chunk fields.
 *   - Confirm all wire format details against src/external_docs/aphytcomm.
 */

#include <libplctag/protocols/enip/enip_mfg_ops.h>
#include <libplctag/protocols/enip/enip.h>
#include <libplctag/protocols/enip/enip_conn.h>
#include <libplctag/protocols/enip/enip_cip.h>
#include <libplctag/protocols/enip/tag.h>
#include <utils/debug.h>
#include <utils/enip_wait.h>
#include <utils/bytes.h>
#include <utils/arena.h>
#include <stdbool.h>
#include <string.h>


/* ============================================================================
 * Stub Implementations (TODO: Fill in with real Omron 0x80 logic)
 * ============================================================================ */


/* Phase 8: this no-op is correct for OMRON — NJ/NX does not support the
 * GetInstanceAttributeList on class 0x6B.  Keep this body; just remove the
 * misleading TODO and fix the debug module to DEBUG_MODULE_ENIP. */
static int32_t enip_mfg_omron_fetch_phase1_metadata(struct enip_connection_t *conn, Arena *arena) {
    /*
     * Phase-1 metadata for Omron devices: manufacturer-specific queries
     *
     * TODO: Determine the correct CIP service and path for Omron metadata fetch.
     * Omron typically uses:
     * - Service 0x80 (simple data segment) for data access
     * - Word-addressed memory model
     * - Different attribute lists than AB
     *
     * For now, skip the metadata phase and allow Phase C (request building) to proceed.
     */

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP/OMRON: Phase-1 metadata fetch (stub - skipping)");

    if(!conn || !arena) { return PLCTAG_ERR_NULL_PTR; }

    /* TODO: Implement Omron-specific metadata query */
    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP/OMRON: Phase-1 metadata not yet implemented");

    return PLCTAG_STATUS_OK;
}


/* ============================================================================
 * OMRON Strategy Structure
 * ============================================================================ */

/* Phase 4: stubs for fragmentation callbacks. Phase 8 will implement 0x80 data segment logic. */
static Bytes enip_mfg_omron_encode_chunk(struct enip_tag_t *tag, Arena *arena, size_t cip_budget) {
    (void)tag; (void)arena; (void)cip_budget;
    return bytes_null();
}

static int32_t enip_mfg_omron_accept_chunk(struct enip_tag_t *tag, Bytes cip_response) {
    (void)tag; (void)cip_response;
    return PLCTAG_ERR_UNSUPPORTED;
}

/* ============================================================================
 * OMRON Strategy Structure (Phase 4)
 * ============================================================================ */

enip_mfg_ops_t enip_mfg_omron = {
    .encode_chunk = enip_mfg_omron_encode_chunk,
    .accept_chunk = enip_mfg_omron_accept_chunk,
    .fetch_phase1_metadata = enip_mfg_omron_fetch_phase1_metadata,
    .name = "OMRON"
};
