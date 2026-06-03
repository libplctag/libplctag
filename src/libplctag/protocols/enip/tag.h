#pragma once
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
 * ENIP tag type definitions.
 *
 * STATUS: MOSTLY CORRECT.  Several fields must be added in Phases 4 and 6.
 *
 * Phase 4 (Fragmentation): rename byte_offset -> chunk_offset (plan §4).
 *   The field is already uint32_t and byte-unit for both AB and OMRON; only the
 *   name changes.
 *
 * Phase 6 (Engine): add three fields to enip_tag_t:
 *   struct enip_connection_t *conn   back-pointer to owning connection
 *   int64_t op_time                  time_ms() when read/write was queued, for sorting
 *   bool    in_active_tags           true while the tag is in conn->active_tags
 *
 * Phase 6: remove the op_state values ENIP_TAG_OP_RESPONSE and ENIP_TAG_OP_COMPLETE
 *   if the engine handles those transitions internally without leaving the tag in
 *   those states; or keep them if the vtable status() function needs to report them.
 *
 * Phase 4: rename the comment on byte_offset to read:
 *   "chunk_offset: byte cursor into tag data for fragmented I/O (AB and OMRON)"
 */

#include <libplctag/lib/tag.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ENIP_TAG_OP_IDLE              = 0,
    ENIP_TAG_OP_METADATA_PHASE1   = 1,
    ENIP_TAG_OP_METADATA_PHASE2   = 2,
    ENIP_TAG_OP_REQUEST           = 3,
    ENIP_TAG_OP_RESPONSE          = 4,
    ENIP_TAG_OP_COMPLETE          = 5,
    ENIP_TAG_OP_ERROR             = 6,
} enip_tag_op_state_t;

typedef struct enip_tag_t {
    TAG_BASE_STRUCT;

    int32_t op_state;
    int32_t metadata_state;

    uint32_t sequence_id;
    uint32_t transaction_id;

    /* Element geometry (filled from Phase-2 metadata) */
    int32_t elem_count;    /* number of elements in the tag */
    int32_t elem_size;     /* size of each element in bytes */
    uint16_t data_type;    /* CIP data type code (e.g. 0x00C4 = DINT) */

    /* Phase 4: rename to chunk_offset; update all sites that reference byte_offset.
     * Used by both AB (0x52/0x53 byte offset field) and OMRON (data segment byte_offset field). */
    uint32_t byte_offset;  /* chunk cursor in bytes; 0 = start of tag */

    bool metadata_phase1_ready;
    bool metadata_phase2_ready;
    bool metadata_required;

    bool rearm_on_reconnect;
    bool was_in_response_state;

    /* Phase 6: add three fields here:
     *   - struct enip_connection_t *conn  (back-pointer to owning connection)
     *   - int64_t op_time                 (time_ms() when read/write was queued, for queue sorting)
     *   - bool in_active_tags             (true while this tag is in conn->active_tags)
     */

    /* Tag identification */
    char *tag_name;            /* root name (e.g. "myTag" from "myTag[0].field") */
    uint32_t tag_instance_id;  /* instance ID from Phase-1 root symbol inventory */

    /* Pre-encoded tag path (allocated contiguously with the tag struct) */
    uint8_t *encoded_tag_path;
    size_t   encoded_tag_path_len;
} enip_tag_t;

typedef struct enip_connection_tag_t {
    TAG_BASE_STRUCT;

    int32_t callback_latency_last_ms;
    int32_t callback_latency_max_ms;
    int32_t queue_depth;
} enip_connection_tag_t;
