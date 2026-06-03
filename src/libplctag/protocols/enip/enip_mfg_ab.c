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
 * Allen-Bradley / Logix Strategy Implementation
 *
 * STATUS: PARTIAL STUB — rewrite required in Phases 4 and 7.
 *
 * Phase 0: This file compiles today.  No Phase 0 work here.
 *
 * Phase 4 (Fragmentation strategy, plan §4): REWRITE all five functions.
 *   - Remove estimate_request_size and encode_request entirely.
 *   - Remove needs_more (replaced by the encode_chunk/accept_chunk contract).
 *   - Add encode_chunk: calls enip_cip_read_tag_fragmented_request (0x52) or
 *     enip_cip_write_tag_fragmented_request (0x53) using tag->byte_offset.
 *     Returns bytes_null() when byte_offset >= total tag size.
 *   - Add accept_chunk: calls enip_cip_parse_response; strips the CIP type code
 *     (2 bytes for atomic, 4 bytes when first byte is 0xA0 struct — plan §3 G);
 *     appends data to tag->data; advances tag->byte_offset; returns
 *     PLCTAG_ERR_PARTIAL when CIP status is 0x06, PLCTAG_STATUS_OK on 0x00,
 *     or PLCTAG_ERR_* on any other status.
 *   - Update enip_mfg_ops_t initializer to use encode_chunk/accept_chunk fields.
 *
 * Phase 7 (Metadata): REPLACE fetch_phase1_metadata stub.
 *   - Delete the hand-rolled EIP/CPF/CIP framing in the current stub.
 *   - Replace with a direct call to enip_metadata_fetch_root_symbols(conn)
 *     which already implements the correct GetInstanceAttributeList loop.
 *
 * Debug module: change DEBUG_MODULE_LIB -> DEBUG_MODULE_ENIP in all pdebug calls.
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
 * Stub Implementations (TODO: Fill in with real AB/Logix logic)
 * ============================================================================ */

/* Phase 4: DELETE this function.  The encode_chunk/accept_chunk interface replaces
 * estimate_request_size + encode_request + needs_more.  The packetizer
 * (enip_packetizer.c) will call encode_chunk with a cip_budget argument instead. */
static int enip_mfg_ab_estimate_request_size(struct enip_tag_t *tag, struct enip_connection_t *conn, Arena *arena,
                                             size_t req_budget, size_t resp_budget, enip_req_desc_t *result) {
    /* Estimate CIP request and response sizes for AB read/write operations
     *
     * Request: Service (1) + Path (2-10) + Tag name/instance (4-20) = ~25-30 bytes typical
     * Response: Status (1) + Extended status (2) + Data (tag_size bytes)
     *
     * For now, use conservative estimates:
     * - Requests are typically 30-50 bytes for read/write
     * - Responses vary by tag size (8-4096 bytes typical)
     */

    if(!result) { return PLCTAG_ERR_NULL_PTR; }
    memset(result, 0, sizeof(*result));

    if(!tag) { return PLCTAG_ERR_NULL_PTR; }

    /* Request size estimate: ~40 bytes typical (service + path + tag instance) */
    result->request_size = 40;

    /* Response size estimate: 4 bytes header + tag data
     * Use tag->size if available, otherwise conservative estimate
     */
    size_t data_size = tag->size > 0 ? (size_t)tag->size : 256;

    /* Cap response at reasonable size (e.g., 2KB per tag in batch)
     * Larger tags will be fragmented in subsequent cycles
     */
    if(data_size > 2048) { data_size = 2048; }
    result->estimated_response_size = 8 + data_size; /* 8 bytes for CIP header + data */

    pdebug(DEBUG_MODULE_ENIP, DEBUG_SPEW, 0, "ENIP/AB: estimate %zu req / %zu resp", result->request_size,
           result->estimated_response_size);

    return PLCTAG_STATUS_OK;
}

/* Phase 4: DELETE this function and replace with encode_chunk (plan §4):
 *
 *   static Bytes enip_mfg_ab_encode_chunk(enip_tag_t *tag, Arena *arena, size_t cip_budget) {
 *     if(tag->byte_offset >= total_tag_bytes(tag)) { return bytes_null(); }
 *     uint16_t elem_count = elements_fitting_in_budget(tag, cip_budget);
 *     if(tag->write_in_flight) {
 *       return enip_cip_write_tag_fragmented_request(arena, tag->encoded_tag_path,
 *                tag->encoded_tag_path_len, tag->data_type, elem_count,
 *                tag->byte_offset, tag->data + tag->byte_offset, elem_count * tag->elem_size);
 *     } else {
 *       return enip_cip_read_tag_fragmented_request(arena, tag->encoded_tag_path,
 *                tag->encoded_tag_path_len, elem_count, tag->byte_offset);
 *     }
 *   }
 *
 * The first call uses byte_offset == 0 (no fragmentation marker needed); the PLC
 * returns CIP status 0x06 if there is more data, 0x00 if complete. */
static int enip_mfg_ab_encode_request(struct enip_tag_t *tag, struct enip_connection_t *conn, Arena *arena,
                                      enip_req_desc_t *result) {
    /* Encode a CIP read/write request for AB ControlLogix
     *
     * CIP Read Tag Request Format:
     * Service (1 byte): 0x4C (ReadTag)
     * Reserved (1 byte): 0x00
     * RequestHandle (4 bytes): unique request ID
     * Timeout (2 bytes): response timeout
     * ItemCount (2 bytes): items in request
     * PathSegments (variable): CIP path to tag
     *
     * For now, build a minimal placeholder request with service 0x4C and path.
     * TODO: Parse tag name and build proper CIP path with instance/member navigation
     */

    if(!result || !tag || !conn || !arena) { return PLCTAG_ERR_NULL_PTR; }

    /* Phase-2 metadata gate: Fetch metadata on first request if not already fetched
     * Metadata contains tag type, element size, and array dimensions
     * Uses tag_instance_id from Phase-1 metadata fetch (root symbol inventory)
     */
    if(!tag->metadata_phase2_ready) {
        uint16_t symbol_type = 0;
        uint16_t element_size = 0;
        uint32_t array_dims[3] = {0, 0, 0};

        int rc = enip_metadata_fetch_tag_info(conn, tag->tag_instance_id, &symbol_type, &element_size, array_dims);

        if(rc == PLCTAG_STATUS_OK) {
            /* Metadata fetch succeeded, store in tag */
            tag->elem_size = element_size;
            tag->elem_count = array_dims[0]; /* Use first dimension as element count */
            tag->metadata_phase2_ready = 1;

            pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP/AB: Metadata fetched for instance %u (size=%u, count=%u)",
                   tag->tag_instance_id, element_size, array_dims[0]);
        } else if(rc == PLCTAG_ERR_NOT_FOUND) {
            /* Metadata fetch is in progress or tag not found - defer the request */
            pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP/AB: Metadata not available yet for instance %u",
                   tag->tag_instance_id);
            return PLCTAG_STATUS_PENDING;
        } else {
            /* Metadata fetch failed - log and continue with defaults */
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP/AB: Metadata fetch failed for instance %u: %d", tag->tag_instance_id,
                   rc);
            /* Non-fatal: use default sizes and continue */
            tag->elem_size = tag->elem_size > 0 ? tag->elem_size : 4; /* Default to DINT (4 bytes) */
            tag->metadata_phase2_ready = 1;                           /* Mark as attempted to avoid infinite retry */
        }
    }

    /* Build CIP read or write request using the pre-encoded tag path */
    Bytes cip_request;
    if(result->is_write) {
        size_t write_len = (tag->elem_count > 0)
                           ? ((size_t)tag->elem_count * (size_t)tag->elem_size)
                           : (size_t)tag->size;
        uint16_t elem_count = (tag->elem_count > 0) ? (uint16_t)tag->elem_count : (uint16_t)1;

        if(tag->byte_offset > 0) {
            /* AB fragmented write (service 0x53) */
            cip_request = enip_cip_write_tag_fragmented_request(
                arena, tag->encoded_tag_path, tag->encoded_tag_path_len,
                tag->data_type, elem_count, tag->byte_offset,
                tag->data, write_len);
        } else {
            cip_request = enip_cip_write_tag_request(
                arena, tag->encoded_tag_path, tag->encoded_tag_path_len,
                tag->data_type, elem_count,
                tag->data, write_len);
        }
    } else {
        uint16_t elem_count = (tag->elem_count > 0) ? (uint16_t)tag->elem_count : (uint16_t)1;

        if(tag->byte_offset > 0) {
            /* AB fragmented read (service 0x52) */
            cip_request = enip_cip_read_tag_fragmented_request(
                arena, tag->encoded_tag_path, tag->encoded_tag_path_len,
                elem_count, tag->byte_offset);
        } else {
            cip_request = enip_cip_read_tag_request(
                arena, tag->encoded_tag_path, tag->encoded_tag_path_len,
                elem_count);
        }
    }

    if(bytes_is_null(cip_request)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP/AB: CIP request encoding failed");
        return PLCTAG_ERR_NO_MEM;
    }

    /* Update result with encoded request size */
    result->request_size = cip_request.len;
    result->estimated_response_size =
        20 + ((tag->elem_count > 0 ? tag->elem_count : 1) * (tag->elem_size > 0 ? tag->elem_size : 4));
    if(result->estimated_response_size > 2048) { result->estimated_response_size = 2048; }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_SPEW, 0, "ENIP/AB: encoded request (%zu bytes)", cip_request.len);

    return PLCTAG_STATUS_OK;
}

/* Phase 4: DELETE this function and replace with accept_chunk (plan §4, §3 G):
 *
 *   static int32_t enip_mfg_ab_accept_chunk(enip_tag_t *tag, Bytes cip_response) {
 *     uint8_t status = 0, ext_sz = 0;
 *     Bytes data;
 *     enip_cip_parse_response(cip_response, &status, &ext_sz, &data);
 *     if(status != CIP_STATUS_SUCCESS && status != CIP_STATUS_PARTIAL)
 *       return PLCTAG_ERR_REMOTE_ERR;
 *     // Strip type code (plan §3 G): 2 bytes for atomic, 4 if first byte == 0xA0 (struct)
 *     size_t type_bytes = (data.len > 0 && data.data[0] == 0xA0) ? 4 : 2;
 *     if(tag->byte_offset == 0) {
 *       if(data.len < type_bytes) return PLCTAG_ERR_BAD_DATA;
 *       tag->data_type = (uint16_t)(data.data[0] | (data.data[1] << 8));
 *       data = bytes_skip(data, type_bytes);
 *     }
 *     // Append to tag->data at current byte_offset
 *     memcpy(tag->data + tag->byte_offset, data.data, data.len);
 *     tag->byte_offset += (uint32_t)data.len;
 *     return (status == CIP_STATUS_PARTIAL) ? PLCTAG_ERR_PARTIAL : PLCTAG_STATUS_OK;
 *   } */
static int enip_mfg_ab_decode_response(struct enip_tag_t *tag, struct enip_connection_t *conn, Bytes response_payload,
                                       uint32_t correlation_id, enip_chunk_result_t *result) {
    /* Decode CIP read/write response from AB ControlLogix
     *
     * Response format:
     * ReplyService (1 byte): reply service (0xCC for read reply)
     * Reserved (1 byte): 0x00
     * RequestHandle (4 bytes): matches request
     * Status (2 bytes): 0 = success, other = error code
     * ExtendedStatusSize (1 byte): additional status bytes
     * ExtendedStatus (variable): error details if status != 0
     * Data (variable): actual tag data if successful
     *
     * For now, parse basic response and check for errors.
     * TODO: Extract tag data and populate tag->tag_buffer
     */

    if(!result || !tag || !response_payload.data) { return PLCTAG_ERR_NULL_PTR; }
    memset(result, 0, sizeof(*result));

    uint8_t cip_status = 0;
    uint8_t ext_status_size = 0;
    Bytes data = {NULL, 0};

    Bytes parsed = enip_cip_parse_response(response_payload, &cip_status, &ext_status_size, &data);

    if(bytes_is_null(parsed)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP/AB: response parsing failed");
        result->cip_status = 0xFF;
        return PLCTAG_ERR_REMOTE_ERR;
    }

    result->cip_status = cip_status;

    if(cip_status != 0x00) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP/AB: CIP status 0x%02x", cip_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Copy response data to tag buffer (if present) */
    if(!bytes_is_null(data) && data.len > 0 && tag->data) {
        size_t copy_len = (data.len < tag->size) ? data.len : tag->size;
        memcpy(tag->data, data.data, copy_len);
        result->elements_decoded = (copy_len + tag->elem_size - 1) / tag->elem_size; /* Round up */
    } else {
        result->elements_decoded = tag->elem_count > 0 ? (uint32_t)tag->elem_count : 1;
    }

    result->needs_retry = 0;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_SPEW, 0, "ENIP/AB: response decoded status=0x%04x, %d elements", cip_status,
           result->elements_decoded);

    return PLCTAG_STATUS_OK;
}

/* Phase 4: DELETE this function.  Replaced by accept_chunk returning PLCTAG_ERR_PARTIAL. */
static int enip_mfg_ab_needs_more(struct enip_tag_t *tag, enip_chunk_result_t *result) {
    pdebug(DEBUG_MODULE_ENIP, DEBUG_SPEW, 0, "ENIP/AB: needs more (stub)");
    return 0;
}

/* Phase 7: REPLACE this stub entirely.
 * The hand-rolled EIP/CPF framing here duplicates enip_metadata_fetch_root_symbols
 * and is incomplete (tag_count = 1 hardcoded, response not parsed).
 * Replace the entire body with:
 *
 *   static int32_t enip_mfg_ab_fetch_phase1_metadata(enip_connection_t *conn, Arena *arena) {
 *     (void)arena;
 *     return enip_metadata_fetch_root_symbols(conn);
 *   }
 *
 * Also fix signature: return type must be int32_t, not int (plan §0). */
static int enip_mfg_ab_fetch_phase1_metadata(struct enip_connection_t *conn, Arena *arena) {
    /*
     * Phase-1 metadata for AB devices: GetInstanceAttributeList on Class 0x6B (Symbol)
     * Handles fragmentation by iterating through instance IDs.
     *
     * Fragment iteration pattern:
     *   1. Request GetInstanceAttributeList for instance ID N
     *   2. Receive response with symbol instances (fits in EIP packet)
     *   3. Extract last instance ID from response
     *   4. If more instances likely exist, request with ID = lastID + 1
     *   5. Repeat until response indicates end-of-list
     *
     * CIP path with 16-bit instance ID:
     *   Service 0x55 (GetInstanceAttributeList)
     *   Path size: 0x03 (words)
     *   [0x20=class segment, 0x6B=Class 0x6B (Symbol), 0x25=16-bit instance segment, instanceID]
     */

    uint16_t instance_id = 0x0000;
    uint16_t last_instance_id = 0xFFFF;
    Bytes request;
    Bytes response;
    socket_wait_state_t io_state = {0};
    int rc;
    int fetch_count = 0;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP/AB: Fetching phase-1 metadata (symbols on Class 0x6B, with fragmentation)");

    if(!conn || !arena) { return PLCTAG_ERR_NULL_PTR; }

    /* Fragment loop: fetch symbols in chunks, starting from instance_id 0x0000
     * Stop when: response contains zero tags OR instance_id >= 0xFFFF */
    while(instance_id < 0xFFFF) {
        /* Reset arena for each request */
        arena_reset(arena);

        /* Build CIP GetInstanceAttributeList request with 16-bit instance ID
         * Service 0x55, path size 0x03 words (includes class + instance segments) */
        Bytes cip = bytes_pack(arena, BYTES_LE, (uint8_t)0x55, /* Service: GetInstanceAttributeList */
                               (uint8_t)0x03,                  /* Path size in words */
                               (uint8_t)0x20,                  /* Class segment (8-bit) */
                               (uint8_t)0x6B,                  /* Class 0x6B (Symbol) */
                               (uint8_t)0x25,                  /* Instance segment (16-bit) */
                               instance_id);                   /* Instance ID (uint16_t, little-endian) */

        if(bytes_is_null(cip)) { return PLCTAG_ERR_NO_MEM; }

        /* Build CPF items: NAI and UDI */
        Bytes nai = bytes_pack(arena, BYTES_LE, (uint16_t)0x0000, (uint16_t)0x0000);
        if(bytes_is_null(nai)) { return PLCTAG_ERR_NO_MEM; }

        Bytes udi_hdr = bytes_pack(arena, BYTES_LE, (uint16_t)0x00B2, (uint16_t)(cip.len & 0xFFFF));
        if(bytes_is_null(udi_hdr)) { return PLCTAG_ERR_NO_MEM; }

        Bytes cpf_hdr = bytes_pack(arena, BYTES_LE, (uint32_t)0, (uint16_t)0, (uint16_t)2);
        if(bytes_is_null(cpf_hdr)) { return PLCTAG_ERR_NO_MEM; }

        /* Build EIP header */
        uint16_t payload_len = (uint16_t)((8 + 4 + 4 + cip.len) & 0xFFFF);
        Bytes eip_hdr =
            bytes_pack(arena, BYTES_LE, (uint16_t)0x006F, payload_len, (uint32_t)conn->session_handle, (uint32_t)0, /* Status */
                       (uint64_t)0,  /* Sender context */
                       (uint32_t)0); /* Options */

        if(bytes_is_null(eip_hdr)) { return PLCTAG_ERR_NO_MEM; }

        /* Concatenate full EIP/CPF/CIP request */
        request = bytes_concat(arena, eip_hdr, cpf_hdr, nai, udi_hdr, cip);
        if(bytes_is_null(request)) { return PLCTAG_ERR_NO_MEM; }

        /* Send request */
        rc = socket_write_wait(conn->socket, &request, 5000, &io_state);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP/AB: Phase-1 metadata send failed (instance 0x%04x): %d", instance_id,
                   rc);
            return rc;
        }

        /* Receive response (allocate buffer for symbol list) */
        response = bytes_alloc(arena, 4096);
        if(bytes_is_null(response)) { return PLCTAG_ERR_NO_MEM; }

        rc = socket_read_wait(conn->socket, &response, 5000, &io_state);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP/AB: Phase-1 metadata read failed (instance 0x%04x): %d", instance_id,
                   rc);
            return rc;
        }

        pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
               "ENIP/AB: Phase-1 metadata fragment %d fetched (instance 0x%04x, response size %zu)", fetch_count, instance_id,
               response.len);

        /* TODO: Parse response to:
         *   1. Count symbol instances in response
         *   2. Extract last_instance_id and tag_count from response
         *
         * For now, assume we got at least one tag and exit after first fragment to progress to Phase C
         */

        uint16_t tag_count = 1;         /* TODO: Parse from response */
        last_instance_id = instance_id; /* TODO: Extract from response */

        fetch_count++;

        /* Stop if we got zero tags in this response */
        if(tag_count == 0) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP/AB: Reached end of tag list (zero tags in response)");
            break;
        }

        /* Prepare for next fragment: increment instance ID for next request */
        instance_id = last_instance_id + 1;
    }

    if(instance_id >= 0xFFFF) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP/AB: Reached maximum instance ID (0x%04x)", instance_id);
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP/AB: Phase-1 metadata complete (fetched %d fragments)", fetch_count);

    return PLCTAG_STATUS_OK;
}


/* ============================================================================
 * AB/Logix Strategy Structure
 * ============================================================================ */

enip_mfg_ops_t enip_mfg_ab = {.estimate_request_size = enip_mfg_ab_estimate_request_size,
                              .encode_request = enip_mfg_ab_encode_request,
                              .decode_response = enip_mfg_ab_decode_response,
                              .needs_more = enip_mfg_ab_needs_more,
                              .fetch_phase1_metadata = enip_mfg_ab_fetch_phase1_metadata,
                              .name = "AB/Logix"};
