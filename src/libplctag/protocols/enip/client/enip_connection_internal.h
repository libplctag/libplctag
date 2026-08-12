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
 * enip_connection_internal.h — the full struct enip_connection_t definition
 * (3.d), shared ONLY between enip_session.c and its dialect plugins
 * (dialects/rockwell/logix_client.c, dialects/omron/omron_client.c,
 * dialects/pccc/pccc_client.c). Every other module (enip_tag.c, the public
 * API, anything outside this connection-engine-plus-dialects seam) still
 * sees enip_connection_t as the opaque type declared in enip_session.h --
 * do not include this header from anywhere else.
 */

#include <stdbool.h>
#include <stdint.h>
#include <libplctag/protocols/enip/client/enip_dialect.h>
#include <libplctag/protocols/enip/client/enip_session.h>
#include <libplctag/protocols/enip/client/enip_tag.h>
#include <platform.h>
#include <utils/arena.h>
#include <utils/atomic_utils.h>

/* Connection-status event ring (§ @connection tag). Single producer (IO
 * thread), multiple consumers (each @connection tag keeps its own read idx). */
#define ENIP_CONN_STATUS_RING_SIZE ((int32_t)16)
#define ENIP_CONN_STATUS_RING_MASK (ENIP_CONN_STATUS_RING_SIZE - 1)

/* §4 + §14.3 connection structure. */
struct enip_connection_t {
    enip_connection_t *next; /* registry singly-linked list, under s_registry_mutex */

    char *gateway;
    char *path;
    char *model; /* optional; overrides identity-based classification (§ model=) */
    int tcp_port;

    /*
     * Encoded length of `path` as a CIP route, measured once at create while
     * the arena is still empty. On the unconnected path the route rides every
     * request, so it is part of cip_overhead.
     */
    size_t route_len;

    /*
     * Bytes of framing between max_cip_packet_size and the CIP payload one
     * request may use: the Connected Data Item plus sequence number when
     * connected, the Unconnected Data Item plus any Unconnected_Send envelope
     * and route when not. Set with is_connected_path, so every window and
     * batch budget stays a single subtraction.
     */
    size_t cip_overhead;

    /*
     * Transport for tag requests. Connected means ForwardOpen at bring-up and
     * a Connected Data Item over SendUnitData per request; unconnected means
     * no ForwardOpen and an Unconnected_Send over SendRRData per request.
     *
     * Bring-up (RegisterSession, Identity) is unconnected either way, so this
     * is provisional until on_identity_reply decides it from
     * enip_plc_prefers_connected(c->plc_type) or from connected_pref.
     */
    bool is_connected_path;

    /*
     * use_connected_msg= as supplied: connected_pref_set false means the
     * attribute was absent and the identified PLC family chooses. Both are in
     * the registry key (conn_key_matches) because the choice is not known
     * until Identity returns, so connections must be keyed on the request.
     */
    bool connected_pref_set;
    bool connected_pref;

    /* Large Forward Open (0x5B) try/fallback (ENIP-SESSION-DESIGN.md §16.4):
     * try_large_fo is what the *next* step_open() attempt should use, reset
     * true at connection creation; on_open_reply clears it (remembered for
     * this connection's later reconnects) the first time a Large attempt is
     * rejected with CIP 0x08 (Service Not Supported). used_large_fo and
     * requested_cip_size record what the in-flight attempt actually asked
     * for, so on_open_reply/parse_forward_open_reply don't have to re-derive
     * it from the dialect. */
    bool try_large_fo;
    bool used_large_fo;
    size_t requested_cip_size;

    sock_p sock;
    thread_p thread;
    bool connect_started;

    uint32_t session_handle;
    uint32_t cip_conn_id;
    uint16_t conn_seq;

    uint32_t our_conn_id;
    uint16_t conn_serial;

    mutex_p sched_mutex;
    enip_tag_p sched_head, sched_tail;
    enip_tag_p in_flight;
    enip_tag_p sched_cursor; /* service_special_tags walk position; sched_mutex */

    enip_tag_p batch_head;
    uint16_t batch_count;
    uint16_t batch_complete_idx;

    Arena arena;

    size_t max_cip_packet_size;
    size_t rx_cap;
    uint16_t max_batch;

    int64_t reconnect_at_ms;

    /* idle disconnect (§ test_idle_disconnect); plain scalars, best-effort
     * cross-thread access -- the IO thread writes conn_status/last_activity_ms,
     * the API thread reads status and reads/writes inactivity_timeout_ms. */
    int64_t inactivity_timeout_ms;
    int64_t last_activity_ms;
    uint8_t conn_status; /* PLCTAG_CONN_STATUS_* */

    /* conn-status event ring; IO thread writes via set_conn_status, @connection
     * tags drain via enip_session_next_conn_status. */
    uint8_t conn_status_ring[ENIP_CONN_STATUS_RING_SIZE];
    atomic_int32_t conn_status_ring_write_idx;

    /* CIP Identity object, queried once during bring-up (§ @identity). Raw
     * Get_Attributes_All payload cached for @identity tags; parsed fields kept
     * for device detection. identity_data is mem_alloc'd, freed in destructor. */
    bool identity_valid;
    uint8_t *identity_data;
    uint16_t identity_len;
    uint16_t ident_vendor_id, ident_device_type, ident_product_code;
    uint8_t ident_rev_major, ident_rev_minor;
    uint16_t ident_status;
    uint32_t ident_serial;
    enip_plc_type_t plc_type; /* auto-detected PLC family; drives feature selection */

    /* Manufacturer dialect (§16a.4): build/apply function pointers + the two
     * sizing numbers. Defaults to &enip_logix_dialect at creation; reselected
     * from the Identity reply at the end of bring-up. Never NULL. */
    const enip_dialect_t *dialect;

    uint8_t state;
    uint8_t resume_state;

    uint8_t *tx_buf;
    size_t tx_len, tx_off;

    uint8_t *rx_buf;
    size_t rx_len;

    atomic_bool terminate;
};

/* Shared by build_tag_request/apply_tag_reply (enip_session.c) and the
 * Logix dialect's build/apply (dialects/rockwell/logix_client.c). Defined in
 * enip_session.c; item 3.f will split apply_tag_reply further. */
extern uint32_t write_window_count(enip_tag_p t);
extern int32_t apply_tag_reply(enip_connection_t *c, enip_tag_p t, uint8_t status, Bytes data);
