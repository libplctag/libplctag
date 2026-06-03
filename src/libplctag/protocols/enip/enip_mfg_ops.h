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
 * ENIP Manufacturer Operations Strategy Interface
 *
 * STATUS: NEEDS REPLACEMENT of the callback set in Phase 4.
 *
 * Phase 4: Replace the five current callbacks with two (plan §4):
 *
 *   Bytes (*encode_chunk)(struct enip_tag_t *tag, Arena *arena, size_t cip_budget);
 *   int32_t (*accept_chunk)(struct enip_tag_t *tag, Bytes cip_response);
 *
 *   Keep fetch_phase1_metadata and name.  Remove estimate_request_size,
 *   encode_request, decode_response, and needs_more.
 *   Remove enip_req_desc_t and enip_chunk_result_t (no longer needed).
 *
 * Phase 4 also: remove the #ifndef guard and replace with #pragma once (plan §0).
 *
 * The file header uses #ifndef ENIP_MFG_OPS_H — change to #pragma once on Phase 4 touch.
 * DEBUG_MODULE_LIB calls in selector must become DEBUG_MODULE_ENIP.
 *
 * Design contract: zero PLC-type branching in shared code; all manufacturer-specific
 * logic behind this callback interface; deterministic packet budget enforcement;
 * manufacturer isolation per struct (AB, OMRON, PCCC).
 */

#ifndef ENIP_MFG_OPS_H
#define ENIP_MFG_OPS_H

#include <libplctag/lib/tag.h>
#include <utils/bytes.h>
#include <utils/arena.h>
#include <stdint.h>
#include <stddef.h>

/* Forward declarations */
struct enip_tag_t;
struct enip_connection_t;


/* ============================================================================
 * Request Descriptor (used for packetizer budget planning)
 * ============================================================================
 *
 * Output of mfg_ops.estimate_request_size() and mfg_ops.encode_request().
 * Describes one atomic request (which may be part of a multi-request packet).
 */

/* Phase 4: DELETE enip_req_desc_t — replaced by the encode_chunk/accept_chunk interface. */
typedef struct {
    /* Request bytes that will be encoded in the CIP payload */
    uint32_t request_size;

    /* Estimated response payload size (used for budget enforcement) */
    uint32_t estimated_response_size;

    /* Sequence/correlation identifiers for response matching */
    uint32_t sequence_id;
    uint32_t transaction_id;

    /* For multi-request (0x0A) packets, the index in the packed frame */
    uint16_t multi_request_index;

    /* Byte offset within multi-request aggregate where this request starts */
    uint32_t offset_in_aggregate;

    /* Status for packetizer (OK, PARTIAL, OVERSIZED, etc.) */
    int status;

    /* Read/write classification for budget enforcement */
    uint8_t is_write;

    /* If PARTIAL, how many elements were encoded (for chunked operations) */
    uint32_t elements_encoded;
} enip_req_desc_t;


/* ============================================================================
 * Response/Chunk Result (output of mfg_ops.decode_response())
 * ============================================================================
 *
 * Describes the result of decoding one atomic response (from a multi-request
 * or single-request packet). May indicate partial completion if chunking.
 */

/* Phase 4: DELETE enip_chunk_result_t — replaced by the encode_chunk/accept_chunk interface. */
typedef struct {
    /* CIP status code (0=success, 0x04=path error, 0x10=partial data, etc.) */
    uint8_t cip_status;

    /* Extended status code (manufacturer-specific) */
    uint8_t cip_extended_status;

    /* Data bytes written to tag->data (may be less than requested if chunked) */
    uint32_t data_bytes;

    /* For reads: number of elements successfully retrieved */
    uint32_t elements_decoded;

    /* libplctag status code (OK, PARTIAL_DATA, TIMEOUT, REMOTE_ERR, etc.) */
    int status;

    /* If PARTIAL, caller should enqueue another request for remaining data */
    uint8_t needs_retry;

    /* Estimated bytes for next chunk (if chunking) */
    uint32_t next_chunk_estimate;
} enip_chunk_result_t;


/* ============================================================================
 * Manufacturer Operations Callback Interface
 * ============================================================================
 *
 * Strategy pattern: Each manufacturer (AB, OMRON, PCCC) implements these
 * callbacks. Connection loop and packetizer call these, never branch on
 * manufacturer type directly.
 */

/* Phase 4: REPLACE the five callback fields below with:
 *
 *   Bytes (*encode_chunk)(struct enip_tag_t *tag, Arena *arena, size_t cip_budget);
 *
 *     Build the next chunk CIP request given the current tag cursor and budget.
 *     Returns the CIP bytes or bytes_null() when there is nothing left to send.
 *     Cannot fail: arena is sized from cip_budget, validation happens at tag setup.
 *     bytes_null() means end-of-data only, never error (plan §4).
 *
 *   int32_t (*accept_chunk)(struct enip_tag_t *tag, Bytes cip_response);
 *
 *     Consume one CIP response chunk.  Appends data to tag->data and advances
 *     the cursor.  Returns PLCTAG_STATUS_OK when done, PLCTAG_ERR_PARTIAL when
 *     another chunk must be requested, or PLCTAG_ERR_* on failure (plan §4).
 *
 * Keep fetch_phase1_metadata and name unchanged. */
typedef struct enip_mfg_ops_t {
    /*
     * estimate_request_size(tag, connection, arena, req_budget, resp_budget, result)
     *
     * Estimate how many bytes a request for this tag will occupy, given:
     * - req_budget: bytes available for request encoding
     * - resp_budget: bytes expected for response (used for aggregate check)
     *
     * Populates result->request_size, result->estimated_response_size.
     * Returns PLCTAG_STATUS_OK if estimate fits in budget, else PARTIAL or error.
     *
     * NOTE: Does NOT encode the request; just calculates sizes. Called by packetizer
     *       to plan frame layout before calling encode_request().
     */
    int (*estimate_request_size)(struct enip_tag_t *tag, struct enip_connection_t *conn, Arena *arena, size_t req_budget,
                                 size_t resp_budget, enip_req_desc_t *result);

    /*
     * encode_request(tag, connection, arena, result)
     *
     * Encode a CIP request (service + path + data) into arena->buffer.
     * Must respect arena->length for current offset (increments it).
     *
     * Populates result->request_size (actual bytes written), sequence_id, etc.
     * Returns PLCTAG_STATUS_OK or partial status.
     *
     * Called AFTER estimate_request_size() confirmed size fits in budget.
     */
    int (*encode_request)(struct enip_tag_t *tag, struct enip_connection_t *conn, Arena *arena, enip_req_desc_t *result);

    /*
     * decode_response(tag, connection, response_bytes, correlation_id, result)
     *
     * Parse a CIP response (status + extended status + data) for this tag.
     * Extract data into tag->data, update result->data_bytes.
     *
     * Populates result->cip_status, data_bytes, elements_decoded, needs_retry.
     * Returns PLCTAG_STATUS_OK (complete), PARTIAL (chunked, needs more),
     *         or error code.
     *
     * NOTE: Manufacturer is responsible for parsing multi-element responses
     *       (e.g., AB 0x52 responses may contain multiple DWORD values).
     */
    int (*decode_response)(struct enip_tag_t *tag, struct enip_connection_t *conn, Bytes response_payload,
                           uint32_t correlation_id, enip_chunk_result_t *result);

    /*
     * needs_more(tag, result)
     *
     * Quick check: does tag need more data/requests after this response?
     * Used by connection loop to decide if tag stays in active_tags vector.
     *
     * Returns 1 if chunking continues, 0 if complete.
     */
    int (*needs_more)(struct enip_tag_t *tag, enip_chunk_result_t *result);

    /*
     * fetch_phase1_metadata(connection, arena)
     *
     * Fetch manufacturer-specific phase-1 metadata (tag inventory, capability profile).
     * Called after GetIdentity and ForwardOpen to populate connection->metadata.
     *
     * - AB: GetInstanceAttributeList on Class 0x6B (Symbol inventory)
     * - OMRON: Manufacturer-specific queries
     * - PCCC: Bridge queries through EtherNet/IP gateway
     *
     * Returns PLCTAG_STATUS_OK on success, error code on failure.
     * Caller (enip_connection.c) handles reconnect on failure.
     */
    int (*fetch_phase1_metadata)(struct enip_connection_t *conn, Arena *arena);

    /* Human-readable name for debugging/logging (e.g., "AB/Logix") */
    const char *name;
} enip_mfg_ops_t;


/* ============================================================================
 * Identity Structure (from Get Identity response)
 * ============================================================================
 *
 * Extracted from EIP Get Identity response to determine manufacturer/device type.
 * Used by select_mfg_ops() to choose correct strategy.
 */

typedef struct {
    uint16_t vendor_id;
    uint16_t device_type;
    uint16_t product_code;
    uint16_t revision_major;
    uint16_t revision_minor;
    uint32_t serial_number;
    char product_name[64]; /* Null-terminated */

    /* Device-specific capability bits */
    uint8_t supports_class_3;           /* Class 3 (explicit messaging) */
    uint8_t supports_class_1_2;         /* Class 1/2 (implicit messaging) */
    uint8_t supports_multiple_services; /* Supports service 0x0A packing */
} enip_identity_t;


/* ============================================================================
 * Strategy Selection Function
 * ============================================================================
 *
 * Called after Get Identity response is received.
 * Returns appropriate mfg_ops struct based on vendor_id, device_type, etc.
 */

enip_mfg_ops_t *enip_select_mfg_ops(enip_identity_t *identity);


/* ============================================================================
 * Built-in Manufacturer Strategy Implementations
 * ============================================================================
 *
 * Each manufacturer has a global struct instance exported here.
 * They may be returned directly by select_mfg_ops() or used for fallback.
 */

extern enip_mfg_ops_t enip_mfg_ab;    /* Allen-Bradley / Logix variants */
extern enip_mfg_ops_t enip_mfg_omron; /* Omron */
extern enip_mfg_ops_t enip_mfg_pccc;  /* PCCC (PLC5, SLC5, Logix-via-PCCC, DH+) */


#endif /* ENIP_MFG_OPS_H */
