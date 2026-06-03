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

#include <libplctag/protocols/enip/enip_mfg_ops.h>
#include <platform.h>
#include <stdbool.h>
#include <utils/arena.h>
#include <utils/hashtable.h>
#include <utils/vector.h>

typedef struct enip_connection_t enip_connection_t;
typedef struct enip_tag_t enip_tag_t;

/* Metadata cache entry for Phase-2 (per-tag type and dimension info).
 * Key in metadata_cache: (int64_t)tag_instance_id. */
typedef struct {
    uint16_t symbol_type;
    uint16_t element_size;
    uint32_t array_dims[3];
} enip_metadata_cache_entry_t;

/* Root symbol entry for Phase-1 (name -> instance_id mapping).
 * Key in root_symbol_cache: hash(name).
 *
 * Phase 1: remove symbol_type, element_size, array_dims.  Phase 1 fetches only
 * attribute 0x01 (name); type/size/dims come from the lazy Phase-2 fetch. */
typedef struct {
    char     name[128];
    uint32_t instance_id;
    uint16_t symbol_type;   /* Phase 1: DELETE — fetched lazily in Phase 2 */
    uint16_t element_size;  /* Phase 1: DELETE — fetched lazily in Phase 2 */
    uint32_t array_dims[3]; /* Phase 1: DELETE — fetched lazily in Phase 2 */
} enip_root_symbol_entry_t;

/*
 * ENIP Connection State
 *
 * Manages one EtherNet/IP session from TCP connect through active I/O to
 * disconnect.  All fields that are not purely read-only after init must be
 * accessed only while holding the relevant mutex.
 */
struct enip_connection_t {
    /* Socket */
    sock_p  socket;
    int64_t last_socket_error_time;

    /* EIP session */
    uint32_t session_handle;
    uint64_t sender_context;
    bool     session_established;

    /* CIP connected-messaging session (established via ForwardOpen) */
    uint32_t cip_orig_conn_id;    /* our connection ID (we assigned) */
    uint32_t cip_targ_conn_id;    /* target's connection ID (from ForwardOpen response) */
    uint16_t cip_conn_seq_num;    /* incremented for each connected send */
    uint16_t cip_conn_serial;     /* serial number used in ForwardOpen */
    bool     cip_connection_open; /* true after a successful ForwardOpen */

    /* Device capability (from GetIdentity response) */
    bool     supports_0x0a;                  /* multi-service (0x0A) */
    bool     supports_extended_forward_open; /* ForwardOpen Extended (0x5B) */
    uint32_t max_packet_buffer_size;         /* negotiated connection size */

    /* Connection path (backplane route, e.g. {0x01, slot, 0x20, 0x02, 0x24, 0x01}) */
    uint8_t conn_path[64];
    uint8_t conn_path_size; /* size in 16-bit words */

    /* Target address */
    char     host[128];
    uint16_t port;
    int8_t   cpu_slot; /* >= 0 for backplane slot, -1 if no slot */

    /* Manufacturer strategy */
    enip_mfg_ops_t *mfg_ops;

    /* Phase-1 root symbol cache: hash(name) -> enip_root_symbol_entry_t */
    hashtable_p root_symbol_cache;
    mutex_p     root_symbol_mutex;

    /* Phase-2 per-tag metadata cache: (int64_t)instance_id -> enip_metadata_cache_entry_t */
    hashtable_p metadata_cache;
    mutex_p     metadata_cache_mutex;

    /* Active tags queued for I/O */
    vector_p active_tags;
    mutex_p  active_tags_mutex;

    /* Pending requests for response correlation */
    vector_p pending_requests;
    mutex_p  pending_requests_mutex;

    /* Per-direction scratch arenas (reset each request cycle) */
    Arena tx_arena;
    Arena rx_arena;

    /* Retry and idle tracking */
    int64_t retry_deadline_ms;
    int32_t connection_attempt_count;
    int64_t last_message_time_ms;
    int64_t last_callback_latency_ms;

    /* Statistics */
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t errors_count;

    bool shutdown_requested;

    /* Phase 6: ADD the following fields:
     *   int32_t state              connection state enum: DISCONNECTED(0)/OPENING(1)/READY(2)
     *   cond_p  wake               condvar — signal to wake the handler thread
     *   socket_wait_state_t io     per-thread restartable I/O state (plan §1.5.2)
     *   size_t  cip_budget         computed from max_packet_buffer_size after ForwardOpen
     */
};

/* Phase-1: fetch all root symbol names and instance IDs from the PLC.
 * Populates conn->root_symbol_cache.  Called once during connection setup. */
extern int32_t enip_metadata_fetch_root_symbols(enip_connection_t *conn);

/* Phase-2: fetch per-tag type, size, and dimension metadata on first use.
 * array_dims_out must point to a uint32_t[3] array. */
extern int32_t enip_metadata_fetch_tag_info(enip_connection_t *conn,
                                            uint32_t tag_instance_id,
                                            uint16_t *symbol_type_out,
                                            uint16_t *element_size_out,
                                            uint32_t *array_dims_out);

/* Look up a root symbol by name.  Returns the entry or NULL if not found. */
extern enip_root_symbol_entry_t *enip_metadata_find_root_symbol(enip_connection_t *conn,
                                                                 const char *name);

/* Clear and free the Phase-2 metadata cache (call on disconnect/error). */
extern void enip_metadata_cache_clear(enip_connection_t *conn);

/* Clear and free the Phase-1 root symbol cache. */
extern void enip_root_symbol_cache_clear(enip_connection_t *conn);

/* Phase G: sender_context correlation helpers */
extern uint64_t    enip_connection_extract_sender_context(const uint8_t *eip_header);
extern enip_tag_t *enip_connection_find_pending_request(enip_connection_t *conn,
                                                         uint64_t sender_context);
extern int32_t     enip_connection_remove_pending_request(enip_connection_t *conn,
                                                           enip_tag_t *tag);
