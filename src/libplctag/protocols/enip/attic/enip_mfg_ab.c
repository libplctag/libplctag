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
 *     using tag->op.chunk_offset as byte cursor. Returns bytes_null() when done.
 *   - accept_chunk: Processes CIP response, strips type code, advances chunk_offset.
 *     Returns PLCTAG_ERR_PARTIAL if more data, PLCTAG_STATUS_OK if complete.
 *   - fetch_phase1_metadata: Calls enip_metadata_fetch_root_symbols (Phase 7 refactored).
 *
 * Phase 7 (Metadata): fetch_phase1_metadata will be refactored to call
 *   enip_metadata_fetch_root_symbols instead of hand-rolled loop.
 */

#include <libplctag/protocols/enip/client/enip_mfg_ops.h>
#include <libplctag/protocols/enip/client/enip.h>
#include <libplctag/protocols/enip/client/enip_conn.h>
#include <libplctag/protocols/enip/client/enip_cip.h>
#include <libplctag/protocols/enip/client/enip_packetizer.h>
#include <libplctag/protocols/enip/tag.h>
#include <inttypes.h>
#include <platform.h>
#include <utils/debug.h>
#include <utils/enip_wait.h>
#include <utils/bytes.h>
#include <utils/arena.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>


/* ============================================================================
 * Phase 4 Fragmentation Strategy (encode_chunk / accept_chunk)
 * ============================================================================ */

/* Build a tag path that includes the explicit chunk start index.
 * Used when chunk_start_flat > 0 or elem_count > UINT16_MAX.
 * Returns the number of encoded bytes written to buf, or 0 on error. */
static size_t enip_ab_encode_chunk_path(const char *tag_name, uint32_t chunk_start_flat,
                                         const uint32_t array_dims[3], int num_dims,
                                         uint8_t *buf, size_t buf_size) {
    char path_str[288]; /* tag name (256) + "[z,y,x]" (up to ~32) */
    uint32_t idx[3] = {0, 0, 0};
    enip_chunk_flat_to_index(chunk_start_flat, array_dims, num_dims, idx);

    if(num_dims <= 1) {
        snprintf_platform(path_str, sizeof(path_str), "%s[%u]", tag_name, (unsigned)idx[0]);
    } else if(num_dims == 2) {
        snprintf_platform(path_str, sizeof(path_str), "%s[%u,%u]",
                          tag_name, (unsigned)idx[0], (unsigned)idx[1]);
    } else {
        snprintf_platform(path_str, sizeof(path_str), "%s[%u,%u,%u]",
                          tag_name, (unsigned)idx[0], (unsigned)idx[1], (unsigned)idx[2]);
    }
    return enip_cip_encode_tag_path(path_str, buf, buf_size);
}


/* Encode next chunk: Build CIP ReadTag or WriteTag request for the current cursor.
 *
 * Uses element-count chunking with index-based path encoding:
 *   - chunk_elems = enip_chunk_elem_count(data_budget, elem_size, remaining_elems)
 *   - Starting element index encoded directly in the path as "MyTag[x]" or "MyTag[z,y,x]"
 *
 * This correctly handles arrays larger than 65535 elements (no uint16_t overflow)
 * and is equivalent to the byte-offset approach for smaller arrays.
 *
 * Returns CIP request bytes, or bytes_null() when all data has been transferred. */
static Bytes enip_mfg_ab_encode_chunk(struct enip_tag_t *tag, Arena *arena,
                                       size_t req_budget, size_t resp_budget) {
    if(!tag || !tag->tag_name || tag->op.encoded_path_len == 0 || !tag->data) {
        return bytes_null();
    }

    size_t total_bytes = (tag->size > 0) ? (size_t)tag->size : 0;
    size_t elem_sz     = (tag->meta.elem_size > 0) ? (size_t)tag->meta.elem_size : 1;

    if(tag->op.chunk_offset >= total_bytes) { return bytes_null(); } /* all done */

    /* Guard: element must fit in a single packet; can't chunk sub-element data. */
    if(elem_sz > req_budget || elem_sz > resp_budget) { return bytes_null(); }

    size_t remaining_bytes = total_bytes - tag->op.chunk_offset;
    uint32_t remaining_elems = (uint32_t)((remaining_bytes + elem_sz - 1) / elem_sz);

    /* Choose the path for this chunk.  When chunk_start > 0 or the total element
     * count exceeds uint16_t, re-encode with the explicit starting index. */
    uint32_t chunk_start_flat = tag->op.chunk_offset / (uint32_t)elem_sz;
    bool need_index_path = (chunk_start_flat > 0) || ((uint32_t)tag->meta.elem_count > 65535u);

    const uint8_t *path     = tag->op.encoded_path;
    size_t         path_len = tag->op.encoded_path_len;
    uint8_t tmp_path_buf[288];

    if(need_index_path) {
        if(!tag->tag_name) { return bytes_null(); }
        size_t tmp_len = enip_ab_encode_chunk_path(tag->tag_name, chunk_start_flat,
                                                    tag->meta.array_dims, tag->meta.num_dims,
                                                    tmp_path_buf, sizeof(tmp_path_buf));
        if(tmp_len == 0) { return bytes_null(); }
        path     = tmp_path_buf;
        path_len = tmp_len;
    }

    if(((tag)->op.kind == ENIP_OP_KIND_WRITE)) {
        /* Write: data goes in the request.
         * Fixed overhead: service(1)+path_words(1)+path(N)+type_info(2)+elem_count(2) = 6+path */
        size_t write_req_fixed = 6 + path_len;
        if(req_budget <= write_req_fixed || resp_budget < 4u) { return bytes_null(); }

        size_t data_budget = req_budget - write_req_fixed;
        uint32_t chunk_elems = enip_chunk_elem_count(data_budget, elem_sz, remaining_elems);
        if(chunk_elems == 0) { return bytes_null(); }

        size_t chunk_bytes   = (size_t)chunk_elems * elem_sz;
        const uint8_t *chunk_data = tag->data + tag->op.chunk_offset;

        return enip_cip_write_tag_request(arena, path, path_len,
                                          tag->meta.data_type, (uint16_t)chunk_elems,
                                          chunk_data, chunk_bytes);
    } else {
        /* Read: data comes back in the response.
         * Fixed overhead: service(1)+path_words(1)+path(N)+elem_count(2) = 4+path request side.
         * Response fixed: reply_hdr(4)+type_info(4) = 8 bytes. */
        size_t read_req_fixed  = 4 + path_len;
        size_t read_resp_fixed = 8;
        if(req_budget < read_req_fixed || resp_budget <= read_resp_fixed) { return bytes_null(); }

        size_t data_budget = resp_budget - read_resp_fixed;
        uint32_t chunk_elems = enip_chunk_elem_count(data_budget, elem_sz, remaining_elems);
        if(chunk_elems == 0) { return bytes_null(); }

        return enip_cip_read_tag_request(arena, path, path_len, (uint16_t)chunk_elems);
    }
}

/* Accept response chunk: Process CIP response and advance tag cursor.
 * Returns PLCTAG_STATUS_OK when all data is transferred, PLCTAG_ERR_PARTIAL when
 * more chunks remain, or PLCTAG_ERR_* on failure. */
static int32_t enip_mfg_ab_accept_chunk(struct enip_tag_t *tag, Bytes cip_response) {
    if(!tag || !tag->data || bytes_is_null(cip_response)) {
        return PLCTAG_ERR_NULL_PTR;
    }

    uint8_t cip_status = 0;
    uint8_t ext_sz = 0;
    Bytes data = bytes_null();

    Bytes payload = enip_cip_parse_response(cip_response, &cip_status, &ext_sz, &data);
    if(bytes_is_null(payload)) {
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* 0x00 = success, 0x06 = partial (PLC-side buffer full); anything else is an error */
    if(cip_status != 0x00 && cip_status != 0x06) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP/AB: CIP status 0x%02" PRIx8, cip_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Strip the 2 or 4-byte CIP type code on the very first read response chunk */
    if(!((tag)->op.kind == ENIP_OP_KIND_WRITE) && tag->op.chunk_offset == 0) {
        Bytes stripped = enip_cip_strip_type_code(data);
        if(!bytes_is_null(stripped)) { data = stripped; }
    }

    /* Copy response data into tag->data at current chunk_offset */
    if(!bytes_is_null(data) && data.len > 0 && tag->op.chunk_offset < (uint32_t)tag->size) {
        size_t space_left = (size_t)tag->size - tag->op.chunk_offset;
        size_t copy_len = (data.len < space_left) ? data.len : space_left;
        if(copy_len > 0) {
            memcpy(tag->data + tag->op.chunk_offset, data.data, copy_len);
            tag->op.chunk_offset += (uint32_t)copy_len;
        }
    }

    /* PARTIAL if: PLC said so (0x06) OR we haven't transferred all bytes yet */
    if(cip_status == 0x06 || tag->op.chunk_offset < (uint32_t)tag->size) {
        return PLCTAG_ERR_PARTIAL;
    }

    tag->op.chunk_offset = 0; /* reset for next operation */
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
     * Phase-1 metadata for AB devices: the full GetInstanceAttributeList walk on the
     * Symbol class (0x6B) lives in enip_metadata_fetch_root_symbols(), which parses each
     * entry (instance_id + name) into the connection's root symbol cache and handles
     * fragmentation.  This strategy hook simply delegates to it, keeping all framing and
     * I/O on the shared transaction seam.
     */
    (void)arena;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP/AB: Fetching phase-1 metadata (root symbol inventory)");

    if(!conn) { return PLCTAG_ERR_NULL_PTR; }

    return enip_metadata_fetch_root_symbols(conn);
}


/* Old hand-rolled fragment loop retained below (disabled) for reference during bring-up. */
#if 0
static int enip_mfg_ab_fetch_phase1_metadata_legacy(struct enip_connection_t *conn, Arena *arena) {
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
            bytes_pack(arena, BYTES_LE, (uint16_t)0x006F, payload_len, (uint32_t)conn->session.session_handle, (uint32_t)0, /* Status */
                       (uint64_t)0,  /* Sender context */
                       (uint32_t)0); /* Options */

        if(bytes_is_null(eip_hdr)) { return PLCTAG_ERR_NO_MEM; }

        /* Concatenate full EIP/CPF/CIP request */
        request = bytes_concat(arena, eip_hdr, cpf_hdr, nai, udi_hdr, cip);
        if(bytes_is_null(request)) { return PLCTAG_ERR_NO_MEM; }

        /* Send request */
        rc = socket_write_wait(conn->link.socket, &request, 5000, &io_state);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP/AB: Phase-1 metadata send failed (instance 0x%04" PRIx16 "): %d", instance_id,
                   rc);
            return rc;
        }

        /* Receive response (allocate buffer for symbol list) */
        response = bytes_alloc(arena, 4096);
        if(bytes_is_null(response)) { return PLCTAG_ERR_NO_MEM; }

        rc = socket_read_wait(conn->link.socket, &response, 5000, &io_state);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP/AB: Phase-1 metadata read failed (instance 0x%04" PRIx16 "): %d", instance_id,
                   rc);
            return rc;
        }

        pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
               "ENIP/AB: Phase-1 metadata fragment %d fetched (instance 0x%04" PRIx16 ", response size %zu)", fetch_count, instance_id,
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
        pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP/AB: Reached maximum instance ID (0x%04" PRIx16 ")", instance_id);
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP/AB: Phase-1 metadata complete (fetched %d fragments)", fetch_count);

    return PLCTAG_STATUS_OK;
}
#endif /* disabled legacy phase-1 loop */


/* Fetch per-tag type/size/dims from the lazy phase-2 metadata cache.
 * Populates tag->meta.data_type, tag->meta.elem_size, tag->meta.elem_count, tag->meta.array_dims, tag->meta.num_dims. */
static int32_t enip_mfg_ab_fetch_tag_metadata(struct enip_tag_t *tag) {
    if(!tag || !tag->conn) { return PLCTAG_ERR_NULL_PTR; }

    uint16_t symbol_type  = 0;
    uint16_t element_size = 0;
    uint32_t dims[3] = {0, 0, 0};

    int32_t rc = enip_metadata_fetch_tag_info(tag->conn, tag->meta.instance_id,
                                              &symbol_type, &element_size, dims);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    tag->meta.data_type    = symbol_type;
    tag->meta.elem_size    = (int32_t)element_size;
    tag->meta.array_dims[0] = dims[0];
    tag->meta.array_dims[1] = dims[1];
    tag->meta.array_dims[2] = dims[2];

    /* Count active dimensions and total elements */
    if(dims[2] > 0) {
        tag->meta.num_dims   = 3;
        tag->meta.elem_count = (int32_t)(dims[0] * dims[1] * dims[2]);
    } else if(dims[1] > 0) {
        tag->meta.num_dims   = 2;
        tag->meta.elem_count = (int32_t)(dims[0] * dims[1]);
    } else if(dims[0] > 0) {
        tag->meta.num_dims   = 1;
        tag->meta.elem_count = (int32_t)dims[0];
    } else {
        tag->meta.num_dims   = 0;
        tag->meta.elem_count = 1;
    }

    return PLCTAG_STATUS_OK;
}


/* ============================================================================
 * AB/Logix Strategy Structure (Phase 4)
 * ============================================================================ */

enip_mfg_ops_t enip_mfg_ab = {
    .encode_chunk          = enip_mfg_ab_encode_chunk,
    .accept_chunk          = enip_mfg_ab_accept_chunk,
    .fetch_phase1_metadata = enip_mfg_ab_fetch_phase1_metadata,
    .fetch_tag_metadata    = enip_mfg_ab_fetch_tag_metadata,
    .name = "AB/Logix"
};
