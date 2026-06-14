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
#include <utils/attr.h>
#include <utils/enip_wait.h>
#include <utils/hashtable.h>
#include <utils/vector.h>

typedef struct enip_connection_t enip_connection_t;
typedef struct enip_tag_t enip_tag_t;

/* Root symbol entry for the phase-1 name → instance_id table.
 * Stored in a single contiguous block of symbol_count rows (plan §4.4).
 * No per-symbol malloc; the whole table is freed as a unit on disconnect. */
typedef struct {
    char     name[128];
    uint32_t instance_id;
} enip_root_symbol_entry_t;

/* ============================================================================
 * enip_link_t — transport sub-struct (plan §4.1)
 * ============================================================================ */
typedef struct enip_link_t {
    sock_p  socket;              /* created ONCE in create; close() keeps the wake pipe */
    socket_wait_state_t io;      /* restartable I/O state shared by send/recv wrappers  */
    char    host[128];
    uint16_t port;               /* default 44818                                        */

    /* route to the CPU: parsed from the "path" attribute, e.g. "1,0"                   */
    uint8_t  route_path[64];     /* raw CIP segment bytes                                */
    uint8_t  route_path_words;   /* size in 16-bit words; 0 = no routing                 */
    int8_t   cpu_slot;           /* convenience: backplane slot, or -1 if none           */
} enip_link_t;

/* ============================================================================
 * enip_session_t — negotiated EIP + CIP session sub-struct (plan §4.2)
 * ============================================================================ */
typedef struct enip_session_t {
    /* EIP encapsulation session */
    uint32_t session_handle;     /* from RegisterSession                                  */
    uint64_t sender_context;     /* monotonically increasing; stamped into each request   */
    bool     established;

    /* CIP connected messaging (from ForwardOpen) */
    uint32_t cip_targ_conn_id;   /* O->T id returned by the PLC; goes in the CAI         */
    uint32_t cip_orig_conn_id;   /* T->O id we assigned                                   */
    uint16_t cip_conn_serial;    /* connection serial used in ForwardOpen/Close           */
    uint16_t cip_seq_num;        /* incremented before each connected send                */
    bool     cip_connection_open;

    /* Negotiated CIP packet sizes from ForwardOpen, per direction (plan §5.9).
     * The usable CIP-payload budget = these values minus the CDI/UDI overhead. */
    uint32_t cip_size_o_to_t;   /* originator->target: bounds our REQUEST frames         */
    uint32_t cip_size_t_to_o;   /* target->originator: bounds the RESPONSE frames        */
    uint32_t unconnected_cap;   /* unconnected message cap (~504 typical)                */

    /* capabilities */
    bool     supports_multi_service;       /* generic CIP 0x0A packing (plan §9.2)       */
    bool     used_extended_forward_open;   /* true if 0x5B succeeded                      */
} enip_session_t;

/* ============================================================================
 * enip_connection_t — one connection, shared by all tags to the same gateway
 * (plan §4.3)
 * ============================================================================ */
struct enip_connection_t {
    struct enip_connection_t *next;  /* for global registry linked list (phase 3)         */

    enip_link_t    link;
    enip_session_t session;

    enip_mfg_ops_t *mfg_ops;       /* selected after GetIdentity (plan §9)              */

    /* phase-1 root symbol cache: name → instance_id (plan §4.4)                        */
    hashtable_p root_symbol_cache;  /* hash(name) -> enip_root_symbol_entry_t*           */
    mutex_p     root_symbol_mutex;
    int32_t     symbol_count;       /* from class 0x6B attr 3; used to presize the table */
    uint32_t    symbol_max_instance;/* from class 0x6B attr 2; iteration stop bound      */

    /* active-tag list (Phase 5 replaces this with the intrusive queue below).           */
    vector_p active_tags;
    mutex_p  active_tags_mutex;

    /* Phase 5: intrusive active-tag queue (plan §6).  HEAD sorted ascending by op_time. */
    enip_tag_t *queue_head;
    enip_tag_t *queue_tail;
    mutex_p     queue_mutex;

    /* per-direction scratch arenas; reset each cycle, never freed until destroy         */
    Arena tx_arena;
    Arena rx_arena;

    /* reported status + reconnect bookkeeping */
    int32_t state;                  /* PLCTAG_CONN_STATUS_*; reporting only              */
    int32_t metadata_generation;    /* bumped per successful (re)connect (plan §3.3)     */
    int64_t last_message_time_ms;
    int32_t connect_attempt_count;

    /* statistics */
    uint64_t messages_sent;
    uint64_t messages_received;

    bool     shutdown_requested;
    thread_p thread;
};

/* Create a new connection: initializes vectors/mutexes, parses gateway and path
 * from attribs ("gateway"="host[:port]", "path"="1,0,...").  Returns NULL on error. */
extern enip_connection_t *enip_connection_create(attr attribs);

/* phase-1: fetch all root symbol names and instance IDs from the PLC.
 * Populates conn->root_symbol_cache.  Called once during connection setup. */
extern int32_t enip_metadata_fetch_root_symbols(enip_connection_t *conn);

/* phase-2: fetch per-tag type, size, and dimension metadata on first use.
 * array_dims_out must point to a uint32_t[3] array. */
extern int32_t enip_metadata_fetch_tag_info(enip_connection_t *conn,
                                            uint32_t tag_instance_id,
                                            uint16_t *symbol_type_out,
                                            uint16_t *element_size_out,
                                            uint32_t *array_dims_out);

/* Look up a root symbol by name.  Returns the entry or NULL if not found. */
extern enip_root_symbol_entry_t *enip_metadata_find_root_symbol(enip_connection_t *conn,
                                                                 const char *name);

/* Clear and free the phase-1 root symbol cache. */
extern void enip_root_symbol_cache_clear(enip_connection_t *conn);

/* Extract sender_context from an EIP response header (bytes 12-19, little-endian). */
extern uint64_t enip_connection_extract_sender_context(const uint8_t *eip_header);
