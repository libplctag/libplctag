#pragma once

/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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
 * plc.h — core data structures for ab_server_fiber.
 *
 * Three structs:
 *   tag_def_t     — one per configured tag; read-only after init except ->data.
 *   plc_config_t  — global server config; read-only after init, shared by all fibers.
 *   eip_session_t — per-fiber (per-connection) EIP/CIP session state.
 */

#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * CIP data type codes
 * ============================================================================ */

typedef uint16_t tag_type_t;

#define TAG_CIP_TYPE_BOOL   ((tag_type_t)0x00C1)
#define TAG_CIP_TYPE_SINT   ((tag_type_t)0x00C2)
#define TAG_CIP_TYPE_INT    ((tag_type_t)0x00C3)
#define TAG_CIP_TYPE_DINT   ((tag_type_t)0x00C4)
#define TAG_CIP_TYPE_LINT   ((tag_type_t)0x00C5)
#define TAG_CIP_TYPE_REAL   ((tag_type_t)0x00CA)
#define TAG_CIP_TYPE_LREAL  ((tag_type_t)0x00CB)
#define TAG_CIP_TYPE_STRING ((tag_type_t)0x00D0) /* 88-byte: 4-byte count, 82 data, 2 padding */

/* ============================================================================
 * PCCC data type codes (stored in tag_type field for PCCC tags)
 * ============================================================================ */

#define TAG_PCCC_TYPE_BIT    ((tag_type_t)0x0085)
#define TAG_PCCC_TYPE_INT    ((tag_type_t)0x0089)
#define TAG_PCCC_TYPE_DINT   ((tag_type_t)0x0091)
#define TAG_PCCC_TYPE_REAL   ((tag_type_t)0x008a)
#define TAG_PCCC_TYPE_STRING ((tag_type_t)0x008d)

/* ============================================================================
 * PLC type enum
 * ============================================================================ */

typedef enum {
    PLC_CONTROL_LOGIX,
    PLC_MICRO800,
    PLC_OMRON,
    PLC_PLC5,
    PLC_SLC,
    PLC_MICROLOGIX
} plc_type_t;

/* ============================================================================
 * tag_def_t — allocated once at startup; ->data written by handlers.
 *             Single-threaded fiber model: no mutex needed.
 * ============================================================================ */

typedef struct tag_def_s {
    struct tag_def_s *next_tag;
    char             *name;          /* heap-allocated C string; NULL for PCCC tags */
    tag_type_t        tag_type;
    size_t            elem_size;     /* bytes per element */
    size_t            elem_count;    /* total elements (product of dimensions) */
    size_t            data_file_num; /* PCCC data file number; 0 for CIP tags */
    size_t            num_dimensions;
    size_t            dimensions[3];
    uint8_t          *data;          /* heap-allocated data buffer */

    /* Per-tag request statistics. */
    int64_t request_count;
    int64_t total_latency_us;
    int64_t min_latency_us;
    int64_t max_latency_us;
} tag_def_t;

/* ============================================================================
 * plc_config_t — global, read-only after init; shared by all client fibers.
 * ============================================================================ */

typedef struct {
    plc_type_t  plc_type;
    const char *port_str;                  /* e.g. "44818"; points into argv */
    uint8_t     path[20];                  /* CIP connection path bytes */
    uint8_t     path_len;

    uint32_t    client_to_server_max_packet;
    uint32_t    server_to_client_max_packet;

    int32_t     reject_fo_count;           /* initial ForwardOpen rejection count */
    int32_t     response_delay_ms;         /* artificial per-response delay (0 = none) */

    tag_def_t  *tags;                      /* linked list of served tags */
} plc_config_t;

/* ============================================================================
 * eip_session_t — per-fiber EIP/CIP connection state.
 *                 Allocated on the client fiber's local scope (stack or arena).
 * ============================================================================ */

typedef struct {
    /* EIP session layer */
    uint32_t session_handle;
    uint64_t sender_context;

    /* CIP connected state (set by Forward Open, cleared by Forward Close) */
    uint32_t server_connection_id;
    uint16_t server_connection_seq;
    uint32_t client_connection_id;
    uint16_t client_connection_seq;
    uint16_t client_connection_serial_number;
    uint16_t client_vendor_id;
    uint32_t client_serial_number;
    uint32_t client_to_server_rpi;
    uint32_t server_to_client_rpi;

    /* Packet size limits negotiated during Forward Open. */
    uint32_t client_to_server_max_packet;
    uint32_t server_to_client_max_packet;

    /* PCCC sequence ID echoed in every PCCC response. */
    uint16_t pccc_seq_id;

    /* Remaining ForwardOpen rejections for this connection. */
    int32_t reject_fo_count;
} eip_session_t;
