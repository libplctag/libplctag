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

/* Phase 0: stub to return PLCTAG_ERR_UNSUPPORTED (one line).
 * Phase 8: DELETE — replaced by encode_chunk. */
static int enip_mfg_omron_estimate_request_size(struct enip_tag_t *tag, struct enip_connection_t *conn, Arena *arena,
                                                size_t req_budget, size_t resp_budget, enip_req_desc_t *result) {
    /* Estimate sizes for Omron CIP requests using service 0x80 (simple data segment)
     * Omron typically uses fixed-size segments for direct memory access.
     */

    if(!result) { return PLCTAG_ERR_NULL_PTR; }
    memset(result, 0, sizeof(*result));

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    /* Request: Service (1) + segment type (1) + offset (2-4) + size = ~20-30 bytes */
    result->request_size = 30;

    /* Response: Status (2) + data (tag_size bytes) */
    uint32_t elem_count = tag->elem_count > 0 ? (uint32_t)tag->elem_count : 1;
    uint32_t elem_size = tag->elem_size > 0 ? (uint32_t)tag->elem_size : 4;
    size_t data_size = elem_count * elem_size;

    if(data_size > 2048) { data_size = 2048; }
    result->estimated_response_size = 8 + data_size;

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, 0, "ENIP/OMRON: estimate %zu req / %zu resp", result->request_size,
           result->estimated_response_size);

    return PLCTAG_STATUS_OK;
}

/* Phase 0: stub to return PLCTAG_ERR_UNSUPPORTED (one line) — this currently
 * passes wrong argument count to enip_cip_read/write_tag_request and will not compile.
 * Phase 8: DELETE — replaced by encode_chunk (see file-level comment). */
static int enip_mfg_omron_encode_request(struct enip_tag_t *tag, struct enip_connection_t *conn, Arena *arena,
                                         enip_req_desc_t *result) {
    /* Encode CIP request for Omron using shared ReadTag/WriteTag services
     *
     * Omron uses standard CIP services 0x4C (ReadTag) and 0x4D (WriteTag),
     * identical to Allen-Bradley. Uses shared encoding layer.
     */

    if(!result || !tag || !conn || !arena) { return PLCTAG_ERR_NULL_PTR; }

    /* Phase-2 metadata gate: Fetch metadata on first request if not already fetched */
    if(!tag->metadata_phase2_ready) {
        const char *tag_name = "tag"; /* TODO: get actual tag name from tag structure */

        uint16_t symbol_type = 0;
        uint16_t element_size = 0;
        uint32_t array_dims[3] = {0, 0, 0};

        int rc = enip_metadata_fetch_tag_info(conn, tag_name, &symbol_type, &element_size, array_dims);

        if(rc == PLCTAG_STATUS_OK) {
            tag->elem_size = element_size;
            tag->elem_count = array_dims[0];
            tag->metadata_phase2_ready = 1;

            pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "ENIP/OMRON: Metadata fetched for instance %u (size=%u, count=%u)",
                   tag->tag_instance_id, element_size, array_dims[0]);
        } else if(rc == PLCTAG_ERR_NOT_FOUND) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, 0, "ENIP/OMRON: Metadata not available yet for instance %u",
                   tag->tag_instance_id);
            return PLCTAG_STATUS_PENDING;
        } else {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "ENIP/OMRON: Metadata fetch failed for instance %u: %d", tag->tag_instance_id,
                   rc);
            tag->elem_size = tag->elem_size > 0 ? tag->elem_size : 4;
            tag->metadata_phase2_ready = 1;
        }
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
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "ENIP/OMRON: CIP request encoding failed");
        return PLCTAG_ERR_NO_MEM;
    }

    result->request_size = cip_request.len;
    result->estimated_response_size =
        20 + ((tag->elem_count > 0 ? tag->elem_count : 1) * (tag->elem_size > 0 ? tag->elem_size : 4));
    if(result->estimated_response_size > 2048) { result->estimated_response_size = 2048; }

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, 0, "ENIP/OMRON: encoded request (%zu bytes)", cip_request.len);

    return PLCTAG_STATUS_OK;
}

/* Phase 0: stub to return PLCTAG_ERR_UNSUPPORTED (one line).
 * Phase 8: DELETE — replaced by accept_chunk (see file-level comment). */
static int enip_mfg_omron_decode_response(struct enip_tag_t *tag, struct enip_connection_t *conn, Bytes response_payload,
                                          uint32_t correlation_id, enip_chunk_result_t *result) {
    /* Decode CIP response from Omron PLC using shared response parser
     *
     * Omron uses standard CIP response format, identical to Allen-Bradley
     */

    if(!result || !tag || !response_payload.data) { return PLCTAG_ERR_NULL_PTR; }
    memset(result, 0, sizeof(*result));

    /* Use shared CIP response parser */
    uint16_t cip_status = 0;
    uint8_t ext_status_size = 0;
    Bytes data = {NULL, 0};

    Bytes parsed = enip_cip_parse_response(response_payload, &cip_status, &ext_status_size, &data);

    if(bytes_is_null(parsed)) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "ENIP/OMRON: Response parsing failed");
        result->cip_status = 0xFFFF;
        return PLCTAG_ERR_REMOTE_ERR;
    }

    result->cip_status = cip_status;

    if(cip_status != 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "ENIP/OMRON: CIP status error: 0x%04x", cip_status);
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

    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, 0, "ENIP/OMRON: response decoded status=0x%04x, %d elements", cip_status,
           result->elements_decoded);

    return PLCTAG_STATUS_OK;
}

/* Phase 0: leave as-is (returns 0, compiles fine).
 * Phase 8: DELETE — replaced by accept_chunk return value. */
static int enip_mfg_omron_needs_more(struct enip_tag_t *tag, enip_chunk_result_t *result) {
    pdebug(DEBUG_MODULE_LIB, DEBUG_SPEW, 0, "ENIP/OMRON: needs more (stub)");
    return 0;
}

/* Phase 8: this no-op is correct for OMRON — NJ/NX does not support the
 * GetInstanceAttributeList on class 0x6B.  Keep this body; just remove the
 * misleading TODO and fix the debug module to DEBUG_MODULE_ENIP. */
static int enip_mfg_omron_fetch_phase1_metadata(struct enip_connection_t *conn, Arena *arena) {
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

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, 0, "ENIP/OMRON: Phase-1 metadata fetch (stub - skipping)");

    if(!conn || !arena) { return PLCTAG_ERR_NULL_PTR; }

    /* TODO: Implement Omron-specific metadata query */
    pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "ENIP/OMRON: Phase-1 metadata not yet implemented");

    return PLCTAG_STATUS_OK;
}


/* ============================================================================
 * OMRON Strategy Structure
 * ============================================================================ */

enip_mfg_ops_t enip_mfg_omron = {.estimate_request_size = enip_mfg_omron_estimate_request_size,
                                 .encode_request = enip_mfg_omron_encode_request,
                                 .decode_response = enip_mfg_omron_decode_response,
                                 .needs_more = enip_mfg_omron_needs_more,
                                 .fetch_phase1_metadata = enip_mfg_omron_fetch_phase1_metadata,
                                 .name = "OMRON"};
