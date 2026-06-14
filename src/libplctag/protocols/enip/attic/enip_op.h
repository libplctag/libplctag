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
 * enip_op.h — per-tag operation and metadata sub-structs (plan §3.2, §3.3)
 *
 * Included only by engine + strategy code, NOT by lib.c or the app path.
 * Both structs are embedded (not pointed to) inside enip_tag_t so the entire
 * tag is one rc_alloc block with no runtime allocation in the steady state.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct enip_tag_t enip_tag_t;   /* forward; full definition in tag.h */

/* ============================================================================
 * enip_operation_t — engine-owned transient I/O state (plan §3.2)
 * ============================================================================
 *
 * The engine thread is the exclusive writer while an operation is active.
 * No heap pointers it owns.  The request bytes are rebuilt each cycle from
 * chunk_offset into the connection arena; write data is read from tag->data.
 */

typedef enum {
    ENIP_OP_IDLE     = 0,   /* nothing requested; tag not in the active queue     */
    ENIP_OP_REQUEST  = 1,   /* read/write requested; waiting to be sent           */
    ENIP_OP_INFLIGHT = 2,   /* request sent; awaiting (more) response             */
    ENIP_OP_DONE     = 3,   /* terminal; result written to the tag                */
} enip_op_state_t;

typedef enum {
    ENIP_OP_KIND_NONE  = 0,
    ENIP_OP_KIND_READ  = 1,
    ENIP_OP_KIND_WRITE = 2,
} enip_op_kind_t;

typedef struct enip_operation_t {
    /* intrusive active-queue node (plan §6.1).  NULL/NULL when not queued. */
    enip_tag_t *q_next;
    enip_tag_t *q_prev;

    /* scheduling: queue is sorted ascending by op_time */
    int64_t  op_time;

    /* EIP sender_context stamped into the request; matched on response */
    uint64_t transaction_id;

    /* fragmentation cursor: bytes of tag->data already transferred this op */
    uint32_t chunk_offset;

    /* base CIP path — points into the tag tail (immutable after tag create) */
    const uint8_t *encoded_path;
    uint16_t       encoded_path_len;

    int32_t  op_state;   /* enip_op_state_t */
    int32_t  kind;       /* enip_op_kind_t  */
} enip_operation_t;

/* ============================================================================
 * enip_tag_meta_t — type info and the validity gate (plan §3.3)
 * ============================================================================
 *
 * Written by the metadata code (engine thread) on connect/reconnect.
 * Read by the app-side getters and the engine when building requests.
 *
 * Validity rule: metadata is usable iff
 *   meta.state == ENIP_META_READY  &&  meta.generation == conn->metadata_generation
 *
 * conn->metadata_generation is bumped once per successful (re)connect.  Bumping
 * that single integer marks every tag stale without walking them.
 */

typedef enum {
    ENIP_META_NONE      = 0,   /* never resolved on this connection        */
    ENIP_META_RESOLVING = 1,   /* a phase-2 fetch is in progress           */
    ENIP_META_READY     = 2,   /* type info valid for this generation      */
} enip_meta_state_t;

typedef struct enip_tag_meta_t {
    uint32_t instance_id;    /* Symbol instance ID from the name (phase-1) */
    uint32_t array_dims[3];  /* element counts per dimension; 0 = unused  */
    int32_t  elem_count;     /* product of active dims (>= 1)             */
    int32_t  elem_size;      /* bytes per element                         */
    int32_t  generation;     /* conn->metadata_generation when fetched    */
    uint16_t data_type;      /* CIP type code (e.g. 0x00C4 = DINT)       */
    uint8_t  num_dims;       /* 0=scalar, 1..3                            */
    uint8_t  state;          /* enip_meta_state_t                         */
    bool     needs_metadata; /* false for PCCC/DF1 which have fixed types */
} enip_tag_meta_t;
