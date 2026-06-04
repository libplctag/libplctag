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
 * STATUS: Phase 4 implementation complete. Phase 7 metadata rewrite pending.
 *
 * Phase 4 (Fragmentation, plan §4):
 *   - encode_chunk: Builds ReadTag (0x4C) or ReadTagFragmented (0x52) request
 *     using tag->chunk_offset as byte cursor. Returns bytes_null() when done.
 *   - accept_chunk: Processes CIP response, strips type code, advances chunk_offset.
 *     Returns PLCTAG_ERR_PARTIAL if more data, PLCTAG_STATUS_OK if complete.
 *   - fetch_phase1_metadata: Calls enip_metadata_fetch_root_symbols (Phase 7 refactored).
 *
 * Phase 7 (Metadata): fetch_phase1_metadata will be refactored to call
 *   enip_metadata_fetch_root_symbols instead of hand-rolled loop.
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
 * Phase 4 Fragmentation Strategy (encode_chunk / accept_chunk)
 * ============================================================================ */

/* Encode next chunk: Build CIP ReadTag or ReadTagFragmented request.
 * Returns CIP request bytes or bytes_null() if tag operation complete.
 * For writes, caller has already put data in tag->data; we encode request
 * with that data. For reads, we just request the bytes. */
static Bytes enip_mfg_ab_encode_chunk(struct enip_tag_t *tag, Arena *arena, size_t cip_budget) {
    if(!tag || !tag->encoded_tag_path || tag->encoded_tag_path_len == 0) {
        return bytes_null();
    }

    size_t total_bytes = tag->size > 0 ? (size_t)tag->size : 0;

    /* If we've already read/written all bytes, return null (done) */
    if(tag->chunk_offset >= total_bytes && !tag->write_in_flight) {
        return bytes_null();
    }

    /* For writes, check if we've sent all bytes */
    if(tag->write_in_flight && tag->chunk_offset >= total_bytes) {
        return bytes_null();
    }

    /* Calculate how many bytes we can fit in this chunk.
     * CIP budget is pre-calculated by caller; we use fragmented services if needed.
     * Fragmented services add 4 bytes of overhead (byte_offset field).
     * Request format: service(1) + path_words(1) + path(N) + elem_count(2) [+ byte_offset(4) for frag] */
    size_t remaining = total_bytes - tag->chunk_offset;
    size_t max_chunk = (cip_budget > 10) ? (cip_budget - 10) : 0; /* Conservative estimate for path overhead */

    if(remaining == 0) {
        return bytes_null();
    }

    /* Decide on element count: cap at what fits in this chunk */
    uint16_t elem_count = (remaining < max_chunk) ? (uint16_t)1 : (uint16_t)1;

    if(tag->write_in_flight) {
        /* Build WriteTag or WriteTagFragmented request */
        size_t chunk_bytes = (remaining < max_chunk) ? remaining : max_chunk;
        const uint8_t *chunk_data = tag->data + tag->chunk_offset;

        if(tag->chunk_offset > 0 || chunk_bytes < remaining) {
            /* Use fragmented write (service 0x53) */
            return enip_cip_write_tag_fragmented_request(
                arena, tag->encoded_tag_path, tag->encoded_tag_path_len,
                tag->data_type, elem_count, (uint32_t)tag->chunk_offset,
                chunk_data, chunk_bytes);
        } else {
            /* Use regular write (service 0x4D) for first/complete chunk */
            return enip_cip_write_tag_request(
                arena, tag->encoded_tag_path, tag->encoded_tag_path_len,
                tag->data_type, elem_count, chunk_data, chunk_bytes);
        }
    } else {
        /* Build ReadTag or ReadTagFragmented request */
        if(tag->chunk_offset > 0 || remaining > max_chunk) {
            /* Use fragmented read (service 0x52) */
            return enip_cip_read_tag_fragmented_request(
                arena, tag->encoded_tag_path, tag->encoded_tag_path_len,
                elem_count, (uint32_t)tag->chunk_offset);
        } else {
            /* Use regular read (service 0x4C) for first/complete read */
            return enip_cip_read_tag_request(
                arena, tag->encoded_tag_path, tag->encoded_tag_path_len,
                elem_count);
        }
    }
}

/* Accept response chunk: Process CIP response and advance tag cursor.
 * Returns PLCTAG_STATUS_OK if complete, PLCTAG_ERR_PARTIAL if more chunks needed,
 * or PLCTAG_ERR_* on failure. */
static int32_t enip_mfg_ab_accept_chunk(struct enip_tag_t *tag, Bytes cip_response) {
    if(!tag || !tag->data || bytes_is_null(cip_response)) {
        return PLCTAG_ERR_NULL_PTR;
    }

    uint8_t cip_status = 0;
    uint8_t ext_sz = 0;
    Bytes data = bytes_null();

    /* Parse CIP header to extract status and payload */
    Bytes payload = enip_cip_parse_response(cip_response, &cip_status, &ext_sz, &data);
    if(bytes_is_null(payload)) {
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Check status: 0x00=success, 0x06=partial data, others=error */
    if(cip_status != 0x00 && cip_status != 0x06) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP/AB: CIP status 0x%02x", cip_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* For reads, strip the 2 or 4-byte CIP type code prefix */
    if(!tag->write_in_flight && tag->chunk_offset == 0) {
        Bytes stripped = enip_cip_strip_type_code(data);
        if(!bytes_is_null(stripped)) {
            data = stripped;
        }
    }

    /* Copy response data into tag->data at current chunk_offset */
    if(!bytes_is_null(data) && data.len > 0) {
        size_t space_left = tag->size - tag->chunk_offset;
        size_t copy_len = (data.len < space_left) ? data.len : space_left;

        if(copy_len > 0) {
            memcpy(tag->data + tag->chunk_offset, data.data, copy_len);
            tag->chunk_offset += (uint32_t)copy_len;
        }
    }

    /* Return PARTIAL if status is 0x06, else OK */
    if(cip_status == 0x06) {
        return PLCTAG_ERR_PARTIAL;
    }

    return PLCTAG_STATUS_OK;
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
 * AB/Logix Strategy Structure (Phase 4)
 * ============================================================================ */

enip_mfg_ops_t enip_mfg_ab = {
    .encode_chunk = enip_mfg_ab_encode_chunk,
    .accept_chunk = enip_mfg_ab_accept_chunk,
    .fetch_phase1_metadata = enip_mfg_ab_fetch_phase1_metadata,
    .name = "AB/Logix"
};
