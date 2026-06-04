
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
 * PCCC Strategy Implementation (PLC5 / SLC 5/05)
 *
 * STATUS: BROKEN STUB — complete rewrite required in Phase 9.
 *
 * Phase 0: This file currently compiles but is entirely wrong:
 *   - encode_request calls enip_cip_write_tag_request and enip_cip_read_tag_request
 *     with stale extra arguments (sequence_id) that don't match current signatures.
 *     FIX (Phase 0): stub every function body to return PLCTAG_ERR_UNSUPPORTED
 *     so the tree compiles.  Leave all signatures and the ops struct.
 *   - DEBUG_MODULE_LIB must become DEBUG_MODULE_ENIP in all pdebug calls.
 *
 * Phase 9 (PCCC, plan §3 M): REWRITE all five functions.
 *   - PCCC uses CIP service 0x4B (Execute PCCC), NOT 0x4C/0x4D ReadTag/WriteTag.
 *     The current code calling enip_cip_read/write_tag_request is wrong.
 *   - encode_chunk must build:
 *       service(1)=0x4B + path_size(1) + path(to Connection Manager 0x20 0x06 0x24 0x01)
 *       + requestor_id_size(1) + vendor_id(2) + serial_number(4)
 *       + pccc_command(1)=0x0F + pccc_status(1)=0x00 + tns(2)
 *       + pccc_function(1) [0x26 for PLC5 read, 0xa2/0xaa/0xab for SLC variants]
 *       + byte_count(2) + file_number(1) + file_type(1) + element(1) + sub_element(1)
 *     Reference: simulator src/poc/ab_server_fiber/pccc.c for exact field layout.
 *   - accept_chunk: parse PCCC response; extract data bytes; copy to tag->data.
 *   - fetch_phase1_metadata: PCCC devices do not support symbol enumeration.
 *     Return PLCTAG_STATUS_OK immediately (no-op is correct, just like OMRON).
 *   - The element_size-from-file-type logic in encode_request is the only salvageable
 *     piece; move it to accept_chunk or a helper.
 *   - Remove estimate_request_size, encode_request, decode_response, needs_more.
 *   - Update ops struct to encode_chunk/accept_chunk fields.
 *
 * Supports multiple PCCC variants via EtherNet/IP gateway:
 * - PLC5 (native DF1 PCCC over EtherNet/IP)
 * - SLC 5/05+ (native PCCC over EtherNet/IP)
 * - Logix variant running on PLC5/SLC platform via "Logix Bridge"
 * - DH+ serial connections (bridged via EN2T gateway)
 *
 * Encodes as CIP service 0x4B (Execute PCCC) with:
 * - Vendor ID and Serial Number routing
 * - PCCC command/status codes
 * - Protected type (read/write) for PLC memory areas
 * - Byte-oriented offsets
 *
 * Reference: docs/enip-pccc-*.md (5 files with variant details)
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
 * Phase 9: Stub Implementations (encode_chunk / accept_chunk)
 * ============================================================================ */

/* Phase 9: this no-op is correct for PCCC devices — they do not support symbol
 * enumeration.  Keep this body; fix debug module to DEBUG_MODULE_ENIP and
 * remove the misleading TODO. */
static int enip_mfg_pccc_fetch_phase1_metadata(struct enip_connection_t *conn, Arena *arena) {
    /*
     * Phase-1 metadata for PCCC devices: manufacturer-specific queries via 0x4B Execute PCCC
     *
     * PCCC metadata fetch is more complex than AB/Logix:
     * - Must encode PCCC command/status structure
     * - Requires "protected type" routing and memory area mapping
     * - Different payload structure than CIP GetInstanceAttributeList
     *
     * PCCC Command 0x06 (ProtectedTypedLogicalRead) can enumerate:
     * - File structure (if available)
     * - Memory area layout
     * - Tag definitions (varies by PLC variant)
     *
     * For now, skip metadata phase and allow Phase C (request building) to proceed.
     */

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP/PCCC: Phase-1 metadata fetch (stub - skipping)");

    if(!conn || !arena) { return PLCTAG_ERR_NULL_PTR; }

    /* TODO: Implement PCCC-specific metadata query (0x4B Execute PCCC wrapper) */
    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP/PCCC: Phase-1 metadata not yet implemented");

    return PLCTAG_STATUS_OK;
}


/* ============================================================================
 * PCCC Strategy Structure
 * ============================================================================ */

/* Phase 4: stubs for fragmentation callbacks. Phase 9 will implement PCCC logic. */
static Bytes enip_mfg_pccc_encode_chunk(struct enip_tag_t *tag, Arena *arena, size_t cip_budget) {
    (void)tag;
    (void)arena;
    (void)cip_budget;
    return bytes_null();
}

static int32_t enip_mfg_pccc_accept_chunk(struct enip_tag_t *tag, Bytes cip_response) {
    (void)tag;
    (void)cip_response;
    return PLCTAG_ERR_UNSUPPORTED;
}

/* ============================================================================
 * PCCC Strategy Structure (Phase 4)
 * ============================================================================ */

enip_mfg_ops_t enip_mfg_pccc = {.encode_chunk = enip_mfg_pccc_encode_chunk,
                                .accept_chunk = enip_mfg_pccc_accept_chunk,
                                .fetch_phase1_metadata = enip_mfg_pccc_fetch_phase1_metadata,
                                .name = "PCCC"};
