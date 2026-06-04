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
 * STATUS: Phase 4 implementation complete.
 *
 * Callback set (Phase 4):
 *   Bytes (*encode_chunk) — Build next CIP request chunk from tag (handles fragmentation)
 *   int32_t (*accept_chunk) — Process CIP response chunk into tag (returns OK/PARTIAL)
 *   int32_t (*fetch_phase1_metadata) — Fetch manufacturer-specific metadata
 *   const char *name — Human-readable device type for logging
 *
 * Design contract: zero PLC-type branching in shared code; all manufacturer-specific
 * logic behind this callback interface; deterministic packet budget enforcement;
 * manufacturer isolation per struct (AB, OMRON, PCCC).
 */

#pragma once

#include <libplctag/lib/tag.h>
#include <utils/bytes.h>
#include <utils/arena.h>
#include <stdint.h>
#include <stddef.h>

/* Forward declarations */
struct enip_tag_t;
struct enip_connection_t;


/* ============================================================================
 * Manufacturer Operations Callback Interface (Phase 4)
 * ============================================================================
 *
 * Strategy pattern: Each manufacturer (AB, OMRON, PCCC) implements these
 * callbacks. Connection loop calls these, never branches on manufacturer type.
 * All fragmentation, multi-request packing, and device-specific logic is
 * encapsulated within the callbacks.
 */

typedef struct enip_mfg_ops_t {
    /* Encode next chunk: Build CIP request for tag's next operation.
     * Called by connection loop to get the next request to send.
     * Returns CIP request bytes ready to wrap in CPF+EIP, or bytes_null()
     * if no more data to send (operation complete).
     *
     * Cannot fail (arena is pre-sized and validated at tag setup time).
     * Returns bytes_null() only to signal end-of-data, never as error. */
    Bytes (*encode_chunk)(struct enip_tag_t *tag, Arena *arena, size_t cip_budget);

    /* Accept response chunk: Process one CIP response and advance tag cursor.
     * Called by connection loop after receiving a response.
     * Appends data to tag->data, increments tag->chunk_offset, updates flags.
     *
     * Returns: PLCTAG_STATUS_OK if tag operation complete,
     *          PLCTAG_ERR_PARTIAL if more chunks needed,
     *          PLCTAG_ERR_* on failure. */
    int32_t (*accept_chunk)(struct enip_tag_t *tag, Bytes cip_response);

    /* Fetch manufacturer metadata: Retrieve device-specific capability information.
     * Called after GetIdentity and ForwardOpen succeed.
     * Used to populate symbol tables, metadata caches, etc.
     *
     * Returns: PLCTAG_STATUS_OK on success, PLCTAG_ERR_* on failure.
     * Failures trigger reconnect in enip_connection_thread_entry. */
    int32_t (*fetch_phase1_metadata)(struct enip_connection_t *conn, Arena *arena);

    /* Human-readable name for debugging/logging (e.g., "AB/Logix", "OMRON") */
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
