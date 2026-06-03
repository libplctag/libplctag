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
 * Stub Implementations (TODO: Fill in with real PCCC logic)
 * ============================================================================ */

/* Phase 0: stub to return PLCTAG_ERR_UNSUPPORTED (one line).
 * Phase 9: DELETE — replaced by encode_chunk. */
static int enip_mfg_pccc_estimate_request_size(struct enip_tag_t *tag, struct enip_connection_t *conn, Arena *arena,
                                               size_t req_budget, size_t resp_budget, enip_req_desc_t *result) {
    /* Estimate sizes for PCCC CIP requests using service 0x4B (Execute PCCC)
     * PCCC requests wrap DF1 protocol in CIP envelope.
     */

    if(!result) { return PLCTAG_ERR_NULL_PTR; }
    memset(result, 0, sizeof(*result));

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    /* Request: Service 0x4B + routing + PCCC command = ~40-60 bytes typical */
    result->request_size = 50;

    /* Response: PCCC status + data (tag_size bytes) */
    uint32_t elem_count = tag->elem_count > 0 ? (uint32_t)tag->elem_count : 1;
    uint32_t elem_size = tag->elem_size > 0 ? (uint32_t)tag->elem_size : 4;
    size_t data_size = elem_count * elem_size;

    if(data_size > 2048) { data_size = 2048; }
    result->estimated_response_size = 8 + data_size;

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, 0, "ENIP/PCCC: estimate %zu req / %zu resp", result->request_size,
           result->estimated_response_size);

    return PLCTAG_STATUS_OK;
}

/* Phase 0: stub to return PLCTAG_ERR_UNSUPPORTED (one line) — currently passes
 * wrong argument count to enip_cip_read/write_tag_request and will not compile.
 * Phase 9: DELETE — replaced by encode_chunk (see file-level comment).
 * Salvageable: the file-type → element_size mapping logic (move to a helper). */
static int enip_mfg_pccc_encode_request(struct enip_tag_t *tag, struct enip_connection_t *conn, Arena *arena,
                                        enip_req_desc_t *result) {
    /* Encode CIP request for PCCC devices using shared CIP layer
     *
     * PCCC uses standard CIP services 0x4C/0x4D (ReadTag/WriteTag) when available,
     * or service 0x4B (Execute PCCC) for DF1 encapsulation when necessary.
     * For now, use shared encoding layer (TODO: add DF1 wrapper).
     */

    if(!result || !tag || !conn || !arena) { return PLCTAG_ERR_NULL_PTR; }

    /* Phase-2 metadata gate: PCCC/DF1 tags determine element size from file type prefix
     * File type prefixes:
     *   N, I, L, S, R → 16-bit (elem_size = 2)
     *   F → 32-bit IEEE float (elem_size = 4)
     *   B, T, C → 16-bit word/bits (elem_size = 2)
     *   ST → string (variable size, elem_size = per-element length)
     * No PLC metadata fetch needed - element size is implicit in file type
     */
    if(!tag->metadata_phase2_ready) {
        int elem_size = 2; /* Default to 16-bit */

        /* Extract file type from tag name (e.g., 'N' from "N7", 'F' from "F8", etc.)
         * Tag name format: [FileType][FileNumber][Index...]
         * e.g., "N7[0]", "F8[2]", "B3[5]"
         */
        if(tag->tag_name && tag->tag_name[0]) {
            char file_type = tag->tag_name[0];

            /* Map file type to element size */
            if(file_type == 'F') {
                elem_size = 4; /* 32-bit IEEE float */
            } else if(file_type == 'N' || file_type == 'I' || file_type == 'L' || file_type == 'S' || file_type == 'R'
                      || file_type == 'B' || file_type == 'T' || file_type == 'C') {
                elem_size = 2; /* 16-bit */
            } else if(tag->tag_name[0] == 'S' && tag->tag_name[1] == 'T') {
                elem_size = 84; /* STRING type: 84 bytes standard */
            }
        }

        tag->elem_size = elem_size;
        tag->elem_count = tag->size > 0 ? (int32_t)(tag->size / elem_size) : 1;
        tag->metadata_phase2_ready = 1;

        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0,
               "ENIP/PCCC: No metadata fetch needed (DF1 native), file_type=%c, elem_size=%u, elem_count=%u",
               tag->tag_name ? tag->tag_name[0] : '?', tag->elem_size, tag->elem_count);
    }

    /* Use shared CIP layer with pre-encoded tag path (encoded at tag creation time) */
    Bytes cip_request;
    if(result->is_write) {
        size_t write_len = (tag->elem_count > 0) ? (tag->elem_count * tag->elem_size) : tag->size;
        cip_request = enip_cip_write_tag_request(arena, tag->encoded_tag_path, tag->encoded_tag_path_len, result->sequence_id,
                                                 tag->data, write_len);
    } else {
        uint32_t elem_count = (tag->elem_count > 0) ? (uint32_t)tag->elem_count : 1;
        if(elem_count > 2048) { elem_count = 2048; }
        cip_request = enip_cip_read_tag_request(arena, tag->encoded_tag_path, tag->encoded_tag_path_len, result->sequence_id,
                                                (uint16_t)elem_count);
    }

    if(bytes_is_null(cip_request)) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "ENIP/PCCC: CIP request encoding failed");
        return PLCTAG_ERR_NO_MEM;
    }

    result->request_size = cip_request.len;
    result->estimated_response_size =
        20 + ((tag->elem_count > 0 ? tag->elem_count : 1) * (tag->elem_size > 0 ? tag->elem_size : 4));
    if(result->estimated_response_size > 2048) { result->estimated_response_size = 2048; }

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, 0, "ENIP/PCCC: encoded request (%zu bytes)", cip_request.len);

    return PLCTAG_STATUS_OK;
}

/* Phase 0: stub to return PLCTAG_ERR_UNSUPPORTED (one line).
 * Phase 9: DELETE — replaced by accept_chunk (see file-level comment). */
static int enip_mfg_pccc_decode_response(struct enip_tag_t *tag, struct enip_connection_t *conn, Bytes response_payload,
                                         uint32_t correlation_id, enip_chunk_result_t *result) {
    /* Decode CIP response from PCCC PLC using shared response parser
     *
     * PCCC uses standard CIP response format when using shared encoding
     */

    if(!result || !tag || !response_payload.data) { return PLCTAG_ERR_NULL_PTR; }
    memset(result, 0, sizeof(*result));

    /* Use shared CIP response parser */
    uint16_t cip_status = 0;
    uint8_t ext_status_size = 0;
    Bytes data = {NULL, 0};

    Bytes parsed = enip_cip_parse_response(response_payload, &cip_status, &ext_status_size, &data);

    if(bytes_is_null(parsed)) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "ENIP/PCCC: Response parsing failed");
        result->cip_status = 0xFFFF;
        return PLCTAG_ERR_REMOTE_ERR;
    }

    result->cip_status = cip_status;

    if(cip_status != 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "ENIP/PCCC: CIP status error: 0x%04x", cip_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Copy response data to tag buffer (if present) */
    if(!bytes_is_null(data) && data.len > 0 && tag->data) {
        size_t copy_len = (data.len < tag->size) ? data.len : tag->size;
        memcpy(tag->data, data.data, copy_len);
        result->elements_decoded = (copy_len + tag->elem_size - 1) / tag->elem_size;
    } else {
        result->elements_decoded = tag->elem_count > 0 ? (uint32_t)tag->elem_count : 1;
    }

    result->needs_retry = 0;

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, 0, "ENIP/PCCC: response decoded status=0x%04x, %d elements", cip_status,
           result->elements_decoded);

    return PLCTAG_STATUS_OK;
}

/* Phase 0: leave as-is (returns 0, compiles fine).
 * Phase 9: DELETE — replaced by accept_chunk return value. */
static int enip_mfg_pccc_needs_more(struct enip_tag_t *tag, enip_chunk_result_t *result) {
    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, 0, "ENIP/PCCC: needs more (stub)");
    return 0;
}

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

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "ENIP/PCCC: Phase-1 metadata fetch (stub - skipping)");

    if(!conn || !arena) { return PLCTAG_ERR_NULL_PTR; }

    /* TODO: Implement PCCC-specific metadata query (0x4B Execute PCCC wrapper) */
    pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "ENIP/PCCC: Phase-1 metadata not yet implemented");

    return PLCTAG_STATUS_OK;
}


/* ============================================================================
 * PCCC Strategy Structure
 * ============================================================================ */

enip_mfg_ops_t enip_mfg_pccc = {.estimate_request_size = enip_mfg_pccc_estimate_request_size,
                                .encode_request = enip_mfg_pccc_encode_request,
                                .decode_response = enip_mfg_pccc_decode_response,
                                .needs_more = enip_mfg_pccc_needs_more,
                                .fetch_phase1_metadata = enip_mfg_pccc_fetch_phase1_metadata,
                                .name = "PCCC"};
