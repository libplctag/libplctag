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
 * ENIP Connection Management and Main Loop
 *
 * STATUS: SUBSTANTIAL REWRITE NEEDED across Phases 0, 1, 2, 3, 6.
 *
 * Phase 0 (compile fix):
 *   - Replace conn->session.sender_context_base with conn->session.sender_context everywhere
 *     (four sites: lines ~307, ~949, ~1014, ~1046 — grep to confirm).
 *   - Change all DEBUG_MODULE_ENIP to DEBUG_MODULE_ENIP in pdebug calls.
 *
 * Phase 1 (Transport + EIP session):
 *   - Replace enip_connection_register_session: reads exactly 24 bytes (the EIP
 *     header), parses header.length, then reads header.length more bytes.
 *     Current code reads only 24 bytes, which misses the body.
 *   - Replace enip_connection_thread_entry's Phase B bootstrap to use
 *     conn->host and conn->port (remove the hardcoded "192.168.1.100").
 *
 * Phase 2 (GetIdentity):
 *   - Rewrite enip_connection_get_identity to use the layer helpers:
 *     enip_cpf_build_unconnected + enip_eip_build_request, then parse via
 *     enip_eip_extract_cpf_payload -> enip_cpf_extract_udi_payload ->
 *     enip_cip_parse_response.  The current body has an off-by-one (plan §3 B):
 *     it unpacks only 3 CIP header bytes (reserved, status, ext_sz) but the
 *     CIP reply header is 4 bytes (reply_service + reserved + status + ext_sz).
 *   - Remove the bogus supports_extended_forward_open = (status & 0x0080) check
 *     (plan §3 C).  Set it to 0; determine capability by attempting FO_Ex (0x5B)
 *     and falling back to FO (0x54) on error in Phase 3.
 *
 * Phase 3 (Routing + connected messaging):
 *   - Rewrite enip_connection_forward_open to:
 *     (a) try ForwardOpen Extended (0x5B) first if conn->session.used_extended_forward_open
 *         is set after ForwardOpen detection (O->T and T->O conn params are 4 bytes,
 *         conn-size is low 12 bits, plan §3 D);
 *     (b) fall back to standard FO (0x54) on error (plan §3 D);
 *     (c) parse the FO response correctly: first 4-byte field after CIP header is
 *         O->T conn id (store as cip_targ_conn_id), second is T->O conn id (store as
 *         cip_orig_conn_id) — currently the labels are swapped (plan §3 E);
 *     (d) parse the actual returned O->T buffer size and store as max_packet_buffer_size
 *         (currently hardcoded 504 regardless of response, plan §3 D).
 *   - Add Unconnected_Send routing wrapper (plan §3 K): for tagged reads that require
 *     routing, wrap the inner CIP in service 0x52 to the Connection Manager with
 *     the route path from conn->link.route_path.  This is needed once ForwardOpen provides
 *     a connected path and the unconnected path goes through a bridge.
 *   - In enip_connection_thread_entry, switch Phase C-G to use
 *     enip_cpf_build_connected + ENIP_CMD_CONNECTED_SEND (0x0070) after ForwardOpen
 *     succeeds, incrementing conn->session.cip_seq_num each request.
 *
 * Phase 6 (Connection/session engine + tag queue):
 *   - Rewrite enip_connection_thread_entry into the pseudo-blocking event loop
 *     described in plan §1.5.4:
 *     (a) lazy connect — only open when active_tags is non-empty;
 *     (b) next_due_tag — pick front of active_tags sorted by op_time;
 *     (c) straight-line send/recv using socket_write_wait/socket_read_wait;
 *     (d) fragmentation loop: repeat encode_chunk/accept_chunk while
 *         accept_chunk returns PLCTAG_ERR_PARTIAL;
 *     (e) lazy idle disconnect + reconnect on next request.
 *   - Rewrite enip_connection_decode_response to use enip_connection_find_pending_request
 *     (already exists) for response correlation instead of broadcasting to all tags.
 *   - Add enip_connection_create wiring: host/port/slot from tag attributes.
 *   - Add connection-state enum (DISCONNECTED/OPENING/READY) as conn->state.
 *   - Add condvar conn->wake and per-thread socket_wait_state_t conn->io.
 *   - Remove sleep_ms(10) busy-wait.
 *
 * Phase 0 also: enip_connection_build_requests must remove the unused cip_payloads
 * vector (allocated and immediately destroyed without use, lines ~940-991).
 *
 * Phase 6 wiring: thread entry implements phases A-J from the plan:
 *   A-wake/terminate  B-connect+session+identity+FO  C-build requests
 *   D-send  E-wait  F-receive+frame  G-match+complete  H-callbacks
 *   I-idle-disconnect  J-shutdown
 */

#include <libplctag/protocols/enip/enip.h>
#include <libplctag/protocols/enip/enip_conn.h>
#include <libplctag/protocols/enip/enip_cip.h>
#include <libplctag/protocols/enip/enip_cpf.h>
#include <libplctag/protocols/enip/enip_eip.h>
#include <libplctag/protocols/enip/enip_txn.h>
#include <libplctag/protocols/enip/enip_mfg_ops.h>
#include <libplctag/protocols/enip/enip_packetizer.h>
#include <libplctag/protocols/enip/enip_stream.h>
#include <libplctag/protocols/enip/tag.h>
#include <inttypes.h>
#include <platform.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <utils/arena.h>
#include <utils/attr.h>
#include <utils/bytes.h>
#include <utils/debug.h>
#include <utils/enip_wait.h>
#include <utils/hashtable.h>
#include <utils/rc.h>
#include <utils/vector.h>

/* Forward declaration — defined below; needed by enip_connection_create. */
static void *enip_connection_thread_entry(void *arg);


static void enip_connection_destructor(void *ptr) {
    enip_connection_t *conn = (enip_connection_t *)ptr;

    if(!conn) { return; }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: Destroying connection");

    conn->shutdown_requested = 1;

    /* Wake and join the background thread before freeing any resources it may touch. */
    if(conn->thread) {
        if(conn->link.socket) { socket_wake(conn->link.socket); }
        thread_join(conn->thread);
        thread_destroy(&conn->thread);
        conn->thread = NULL;
    }

    if(conn->root_symbol_cache) { enip_root_symbol_cache_clear(conn); }
    if(conn->root_symbol_mutex) { mutex_destroy(&conn->root_symbol_mutex); }

    if(conn->active_tags) { vector_destroy(conn->active_tags); conn->active_tags = NULL; }
    if(conn->active_tags_mutex) { mutex_destroy(&conn->active_tags_mutex); }

    if(conn->link.socket) { socket_destroy(&conn->link.socket); }
}


/* ============================================================================
 * Phase G: Request/Response Correlation via sender_context (Generic)
 * ============================================================================
 *
 * Extract sender_context from EIP response header and match to pending request.
 * EIP Header layout (bytes 0-27):
 *   0-1:   Command (uint16_t LE)
 *   2-3:   Length (uint16_t LE)
 *   4-7:   Session handle (uint32_t LE)
 *   8-11:  Status (uint32_t LE)
 *   12-19: Sender context (uint64_t LE) ← CORRELATION ID
 *   20-23: Options (uint32_t LE)
 */

/* Phase 6: correct as-is.
 * Used on the receive path to extract the context value, then passed to
 * enip_connection_find_pending_request to route the response. */
uint64_t enip_connection_extract_sender_context(const uint8_t *eip_header) {
    if(!eip_header) { return 0; }

    /* Sender context is at offset 12-19 (8 bytes, little-endian) */
    uint64_t context = 0;
    for(int i = 0; i < 8; i++) { context |= ((uint64_t)eip_header[12 + i]) << (i * 8); }
    return context;
}


/* ============================================================================
 * Phase B: TCP Connection and Session Bootstrap (using Bytes API)
 * ============================================================================ */

/* Phase 1: correct as-is — no changes needed.
 * Called from the Phase B bootstrap.  Phase 1 fix: remove hardcoded host in the
 * caller (enip_connection_thread_entry) and pass conn->host / conn->port instead. */
static int enip_connection_tcp_connect(enip_connection_t *conn, const char *host, int port, int timeout_ms) {
    socket_wait_state_t io_state = {0};
    int rc;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: TCP connect to %s:%d", host, port);

    if(!conn || !host) { return PLCTAG_ERR_NULL_PTR; }

    /* Close the data fd of any previous connection. socket_close() leaves the
     * wake channel intact so socket_wait_event/socket_wake remain usable. */
    if(conn->link.socket) {
        socket_close(conn->link.socket);
    } else {
        /* Fallback: create if somehow missing (should not happen after enip_connection_create). */
        rc = socket_create(&conn->link.socket);
        if(rc != PLCTAG_STATUS_OK || !conn->link.socket) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to create socket: %d", rc);
            return PLCTAG_ERR_OPEN;
        }
    }

    rc = socket_connect_wait(conn->link.socket, host, port, timeout_ms, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: TCP connect failed: %d", rc);
        socket_close(conn->link.socket);
        return rc;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: TCP connected");
    return PLCTAG_STATUS_OK;
}


/* Phase 1: REWRITE the receive path.
 * Current bug: reads a flat 24-byte buffer assuming the RegisterSession response
 * body is already included.  EIP over TCP is a stream: must read exactly 24 bytes
 * first (the EIP header), then parse header.length, then read that many more bytes.
 * Also: remove conn->session.sender_context_base = 1 (Phase 0 fix — use conn->session.sender_context).
 * Also: move metadata_cache init to enip_connection_create, not here. */
static int enip_connection_register_session(enip_connection_t *conn) {
    /* RegisterSession: Command 0x65
     * EIP Header (24 bytes) + protocol_version (2 bytes) + options (2 bytes) = 28 bytes total
     * Payload length = 4 bytes
     */
    Bytes request;
    socket_wait_state_t io_state = {0};
    int32_t rc;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Registering session");

    if(!conn || !conn->link.socket) { return PLCTAG_ERR_NULL_PTR; }

    /* Build RegisterSession request using Bytes API */
    arena_reset(&conn->tx_arena);
    request = bytes_pack(&conn->tx_arena, BYTES_LE, (uint16_t)0x0065, /* Command: RegisterSession */
                         (uint16_t)4,                                 /* Length: 4 bytes (just protocol_version + options) */
                         (uint32_t)0,                                 /* Session (initially 0) */
                         (uint32_t)0,                                 /* Status */
                         (uint64_t)1,                                 /* Sender context */
                         (uint32_t)0,                                 /* Options (reserved) */
                         (uint16_t)0x0001,                            /* protocol_version: 1 */
                         (uint16_t)0x0000);                           /* options */

    if(bytes_is_null(request)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Arena OOM for RegisterSession");
        return PLCTAG_ERR_NO_MEM;
    }

    /* Send request */
    rc = socket_write_wait(conn->link.socket, &request, 5000, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: RegisterSession send failed: %d", rc);
        return rc;
    }

    conn->messages_sent++;

    /* Receive response using stream framing (24-byte header + payload). */
    arena_reset(&conn->rx_arena);
    Bytes response;
    rc = enip_recv_frame(conn->link.socket, &conn->rx_arena, 5000, &io_state, &response);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: RegisterSession read failed: %d", rc);
        return rc;
    }

    conn->messages_received++;

    /* Parse response to extract session handle and status.
     * EIP response structure:
     *   Offset 4-7: session_handle (assigned by target)
     *   Offset 8-11: status code (0 = success)
     */
    if(response.len < 12) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: RegisterSession response too short");
        return PLCTAG_ERR_REMOTE_ERR;
    }

    uint32_t resp_status;
    bytes_unpack(response, BYTES_LE, BYTES_SKIP(4), &conn->session.session_handle, &resp_status);

    if(resp_status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: RegisterSession response status: 0x%08" PRIx32, resp_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    if(conn->session.session_handle == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: RegisterSession returned zero handle");
        return PLCTAG_ERR_REMOTE_ERR;
    }

    conn->session.established = 1;
    conn->session.sender_context = 1;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Session established (handle=0x%08" PRIx32 ")", conn->session.session_handle);

    return PLCTAG_STATUS_OK;
}


/* Phase 1: Send UnregisterSession and close connection.
 * Used to clean up EIP session before disconnecting.
 * Command 0x66 with existing session handle.
 * No response parsing required — just send and close. */
static int32_t enip_connection_unregister_session(enip_connection_t *conn) {
    /* UnregisterSession: Command 0x66
     * EIP Header (24 bytes) + no additional payload = 24 bytes total
     * Payload length = 0 bytes
     */
    Bytes request;
    socket_wait_state_t io_state = {0};
    int32_t rc;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Unregistering session (handle=0x%08" PRIx32 ")", conn->session.session_handle);

    if(!conn || !conn->link.socket) {
        return PLCTAG_ERR_NULL_PTR;
    }

    /* Build UnregisterSession request using Bytes API. */
    arena_reset(&conn->tx_arena);
    request = bytes_pack(&conn->tx_arena, BYTES_LE,
                         (uint16_t)0x0066,              /* Command: UnregisterSession */
                         (uint16_t)0,                   /* Length: no payload */
                         conn->session.session_handle,          /* Session handle to unregister */
                         (uint32_t)0,                   /* Status */
                         (uint64_t)1,                   /* Sender context */
                         (uint32_t)0);                  /* Options (reserved) */

    if(bytes_is_null(request)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Arena OOM for UnregisterSession");
        return PLCTAG_ERR_NO_MEM;
    }

    /* Send request. */
    rc = socket_write_wait(conn->link.socket, &request, 5000, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: UnregisterSession send failed: %d", rc);
        return rc;
    }

    conn->messages_sent++;
    conn->session.established = 0;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: UnregisterSession sent");

    return PLCTAG_STATUS_OK;
}


/* Phase 2: REFACTORED to use enip_txn transaction seam.
 * Consolidates CPF+EIP framing and socket I/O into one place. */
static int32_t enip_connection_get_identity(enip_connection_t *conn) {
    /* GetAttributeAll identity: Service 0x01, Class 0x01 (Identity object), Instance 0x01
     * Response contains vendor_id, device_type, product_code, revision, status, serial, name
     * Sent via unconnected messaging (CPF SendRRData 0x006F) */
    Bytes cip_request, cip_response;
    int32_t rc;
    uint8_t cip_status;
    uint8_t ext_status_size;
    uint64_t used_context;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Fetching device identity");

    if(!conn || !conn->link.socket || !conn->session.established) {
        return PLCTAG_ERR_NULL_PTR;
    }

    /* Step 1: Build CIP GetAttributeAll request for Identity object. */
    arena_reset(&conn->tx_arena);
    cip_request = bytes_pack(&conn->tx_arena, BYTES_LE,
                             (uint8_t)0x01,    /* Service: GetAttributeAll */
                             (uint8_t)0x02,    /* Path size in words */
                             (uint8_t)0x20,    /* Class segment (8-bit) */
                             (uint8_t)0x01,    /* Class 0x01 (Identity) */
                             (uint8_t)0x24,    /* Instance segment (8-bit) */
                             (uint8_t)0x01);   /* Instance 0x01 */

    if(bytes_is_null(cip_request)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Arena OOM for GetIdentity CIP");
        return PLCTAG_ERR_NO_MEM;
    }

    /* Step 2: Execute transaction: wrap, send, receive, unwrap. */
    rc = enip_txn(&conn->link, &conn->session, &conn->tx_arena, &conn->rx_arena,
                  ENIP_MSG_UNCONNECTED, cip_request, &used_context, &cip_response);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: GetIdentity transaction failed: %d", rc);
        return rc;
    }

    conn->messages_sent++;
    conn->messages_received++;

    /* Step 3: Parse CIP response header using enip_cip_parse_response.
     * This correctly handles: reply_service (1) + reserved (1) + status (1) + ext_status_size (1) + data
     * Returns the data portion after the 4-byte header. */
    Bytes cip_data = enip_cip_parse_response(cip_response, &cip_status, &ext_status_size, NULL);
    if(bytes_is_null(cip_data)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: CIP response parsing failed");
        return PLCTAG_ERR_REMOTE_ERR;
    }

    if(cip_status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: GetIdentity CIP status error: 0x%02" PRIx8, cip_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Step 9: Parse identity attributes from CIP data.
     * Structure: vendor_id (2), device_type (2), product_code (2),
     *            revision_major (1), revision_minor (1), status_word (2),
     *            serial_number (4), product_name_len (1), product_name (string) */
    uint16_t vendor_id, device_type, product_code, status_word;
    uint8_t revision_major, revision_minor;
    uint32_t serial_number;
    Bytes remaining = bytes_unpack(cip_data, BYTES_LE,
                                   &vendor_id, &device_type, &product_code,
                                   &revision_major, &revision_minor, &status_word,
                                   &serial_number);

    if(bytes_is_null(remaining)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: GetIdentity data too short");
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Step 10: Parse product name (1 byte length + string). */
    uint8_t name_len = 0;
    char product_name[64] = {'\0'};

    if(remaining.len > 0) {
        remaining = bytes_unpack(remaining, BYTES_LE, &name_len);
        if(!bytes_is_null(remaining) && remaining.len >= name_len) {
            if(name_len > 63) { name_len = 63; }
            if(remaining.data) { memcpy(product_name, remaining.data, name_len); }
            product_name[name_len] = '\0';
        }
    }

    /* Step 11: Select manufacturer strategy based on device identity. */
    enip_identity_t identity = {
        .vendor_id = vendor_id,
        .device_type = device_type,
        .product_code = product_code,
        .revision_major = revision_major,
        .revision_minor = revision_minor,
        .serial_number = serial_number,
        .supports_multiple_services = (status_word & 0x0020) ? 1 : 0
    };
    strncpy(identity.product_name, product_name, sizeof(identity.product_name) - 1);

    conn->mfg_ops = enip_select_mfg_ops(&identity);
    if(!conn->mfg_ops) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: No manufacturer strategy found for vendor=0x%04" PRIx16, vendor_id);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Step 12: Set capability flags from identity.
     * Phase 3: supports_extended_forward_open will be set to 1 if FO_Ex succeeds. */
    conn->session.supports_multi_service = identity.supports_multiple_services;
    conn->session.used_extended_forward_open = 0; /* Phase 3 will detect via FO_Ex attempt+fallback */
    conn->session.cip_size_o_to_t = 2000;      /* Phase 3 will negotiate actual value from FO response */

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "ENIP: Identity fetched (vendor=0x%04" PRIx16 ", device=0x%04" PRIx16 ", product=0x%04" PRIx16
           ", revision=%" PRIu8 ".%" PRIu8 ", mfg=%s)",
           vendor_id, device_type, product_code, revision_major, revision_minor, conn->mfg_ops->name);

    return PLCTAG_STATUS_OK;
}


/* Phase 3: Helper function to attempt ForwardOpen with a given service code.
 *
 * Attempts ForwardOpen (0x54) or ForwardOpen Extended (0x5B) based on service_code.
 * FO_Ex uses 32-bit connection parameters and up to 4002-byte buffers.
 * Standard FO uses 16-bit parameters and 504-byte limit.
 *
 * Returns:
 *   PLCTAG_STATUS_OK: success, connection IDs and buffer size stored in conn
 *   PLCTAG_ERR_REMOTE_ERR: CIP error (status != 0). If status == 0x08, caller can retry.
 *   Other: socket/memory errors */
static int32_t enip_connection_forward_open_attempt(enip_connection_t *conn, uint8_t service_code) {
    if(!conn || !conn->link.socket || !conn->session.established) {
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: ForwardOpen attempt (service=0x%02" PRIx8 ")", service_code);

    arena_reset(&conn->tx_arena);

    /* Build CIP payload with appropriate parameters for service code. */
    uint32_t orig_to_targ_params, targ_to_orig_params;
    if(service_code == 0x5B) {
        /* FO Extended: 32-bit conn params with 12-bit size field. */
        orig_to_targ_params = 0x0FA3u; /* ~4002 bytes with high priority and class 3 bits */
        targ_to_orig_params = 0x0FA3u;
    } else {
        /* Standard FO: 16-bit conn params with 9-bit size field. */
        orig_to_targ_params = 0x43F8u; /* 504 bytes with high priority */
        targ_to_orig_params = 0x43F8u;
    }

    /* Build connection path (reuse from main forward_open) */
    uint8_t conn_path[8];
    uint8_t conn_path_words;

    if(conn->link.cpu_slot >= 0) {
        conn_path[0] = 0x01;
        conn_path[1] = (uint8_t)conn->link.cpu_slot;
        conn_path[2] = 0x20;
        conn_path[3] = 0x02;
        conn_path[4] = 0x24;
        conn_path[5] = 0x01;
        conn_path_words = 3;
    } else {
        conn_path[0] = 0x20;
        conn_path[1] = 0x02;
        conn_path[2] = 0x24;
        conn_path[3] = 0x01;
        conn_path_words = 2;
    }

    /* Assign connection ID if needed */
    if(conn->session.cip_conn_serial == 0) {
        conn->session.cip_conn_serial = 1;
    }
    uint32_t our_conn_id = (uint32_t)(uintptr_t)conn ^ (uint32_t)conn->session.cip_conn_serial;
    if(our_conn_id == 0) { our_conn_id = 0x12345678u; }

    /* Build FO CIP payload */
    Bytes fo_fixed = bytes_pack(&conn->tx_arena, BYTES_LE,
                                service_code,
                                (uint8_t)0x02,        /* path_size: 2 words to CM */
                                (uint8_t)0x20, (uint8_t)0x06, (uint8_t)0x24, (uint8_t)0x01,
                                (uint8_t)0x0A, (uint8_t)0x0E, /* secs_per_tick, timeout_ticks */
                                (uint32_t)0,          /* O->T conn ID (filled by target) */
                                (uint32_t)our_conn_id,/* T->O conn ID (ours) */
                                (uint16_t)conn->session.cip_conn_serial,
                                (uint16_t)0xF33D,     /* vendor ID */
                                (uint32_t)0x21504345u,/* serial number */
                                (uint8_t)0x03, (uint8_t)0x00, (uint8_t)0x00, (uint8_t)0x00,
                                (uint32_t)1000000u,   /* O->T RPI */
                                orig_to_targ_params,
                                (uint32_t)1000000u,   /* T->O RPI */
                                targ_to_orig_params,
                                (uint8_t)0xA3,        /* transport_class */
                                conn_path_words);

    if(bytes_is_null(fo_fixed)) { return PLCTAG_ERR_NO_MEM; }

    /* Append connection path */
    size_t path_bytes = (size_t)conn_path_words * 2u;
    uint8_t *path_buf = arena_alloc(&conn->tx_arena, path_bytes);
    if(!path_buf) { return PLCTAG_ERR_NO_MEM; }
    memcpy(path_buf, conn_path, path_bytes);

    Bytes cip_payload = bytes_concat(&conn->tx_arena, fo_fixed,
                                     (Bytes){path_buf, path_bytes});
    if(bytes_is_null(cip_payload)) { return PLCTAG_ERR_NO_MEM; }

    /* Wrap in CPF+EIP and send */
    Bytes cpf = enip_cpf_build_unconnected(&conn->tx_arena, cip_payload);
    if(bytes_is_null(cpf)) { return PLCTAG_ERR_NO_MEM; }

    Bytes frame = enip_eip_build_request(&conn->tx_arena, ENIP_CMD_UNCONNECTED_SEND,
                                         conn->session.session_handle, &conn->session.sender_context, cpf);
    if(bytes_is_null(frame)) { return PLCTAG_ERR_NO_MEM; }

    socket_wait_state_t io_state = {0};
    int32_t rc = socket_write_wait(conn->link.socket, &frame, 5000, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: ForwardOpen send failed: %d", rc);
        return rc;
    }
    conn->messages_sent++;

    /* Receive response using stream framing */
    arena_reset(&conn->rx_arena);
    Bytes response;
    rc = enip_recv_frame(conn->link.socket, &conn->rx_arena, 5000, &io_state, &response);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: ForwardOpen read failed: %d", rc);
        return rc;
    }
    conn->messages_received++;

    /* Extract and parse CIP response */
    Bytes cpf_response = enip_eip_extract_cpf_payload(response);
    if(bytes_is_null(cpf_response)) { return PLCTAG_ERR_REMOTE_ERR; }

    Bytes cip_response = enip_cpf_extract_udi_payload(cpf_response);
    if(bytes_is_null(cip_response)) { return PLCTAG_ERR_REMOTE_ERR; }

    /* Parse CIP header */
    uint8_t cip_status, ext_sz;
    Bytes cip_data = enip_cip_parse_response(cip_response, &cip_status, &ext_sz, NULL);
    if(bytes_is_null(cip_data)) { return PLCTAG_ERR_REMOTE_ERR; }

    if(cip_status != 0x00) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP: ForwardOpen (0x%02" PRIx8 ") CIP status 0x%02" PRIx8, service_code, cip_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Skip extended status */
    if(ext_sz > 0) {
        size_t skip_bytes = (size_t)ext_sz * 2u;
        if(cip_data.len < skip_bytes) { return PLCTAG_ERR_REMOTE_ERR; }
        cip_data = bytes_slice(cip_data, skip_bytes, cip_data.len - skip_bytes);
        if(bytes_is_null(cip_data)) { return PLCTAG_ERR_REMOTE_ERR; }
    }

    /* Parse success response: conn IDs, serial, vendor, serial, O->T API, T->O API, app data size */
    if(cip_data.len < 24) { return PLCTAG_ERR_REMOTE_ERR; }

    uint32_t o_to_t_conn_id, t_to_o_conn_id;
    uint32_t o_to_t_api, t_to_o_api;
    uint8_t app_data_size;

    bytes_unpack(cip_data, BYTES_LE,
                 &o_to_t_conn_id, &t_to_o_conn_id,
                 BYTES_SKIP(2+2+4), /* skip serial, vendor, serial */
                 &o_to_t_api, &t_to_o_api,
                 &app_data_size);

    conn->session.cip_targ_conn_id    = o_to_t_conn_id;
    conn->session.cip_orig_conn_id    = t_to_o_conn_id;
    conn->session.cip_seq_num    = 0;
    conn->session.cip_connection_open = true;

    uint32_t requested_size = (service_code == 0x5B) ? 4002 : 504;
    conn->session.cip_size_o_to_t = requested_size;
    conn->session.cip_size_t_to_o = requested_size;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "ENIP: ForwardOpen (0x%02" PRIx8 ") OK: O->T=0x%08" PRIx32 " T->O=0x%08" PRIx32 " buffer=%zu",
           service_code, o_to_t_conn_id, t_to_o_conn_id, conn->session.cip_size_o_to_t);

    return PLCTAG_STATUS_OK;
}


/* Phase 3: Try ForwardOpen Extended (0x5B) first, fall back to standard FO (0x54).
 *
 * FO_Ex provides higher capacity (4002 bytes) via 32-bit connection parameters.
 * If the device doesn't support it (CIP status 0x08), retry with standard FO.
 * On success, stores connection IDs and negotiated buffer size in conn. */
static int32_t enip_connection_forward_open(enip_connection_t *conn) {
    if(!conn || !conn->link.socket || !conn->session.established) {
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: ForwardOpen starting");

    /* Phase 3: Try extended ForwardOpen (0x5B) first for better capacity */
    int32_t rc = enip_connection_forward_open_attempt(conn, 0x5B);

    /* If extended not supported, fall back to standard */
    if(rc == PLCTAG_ERR_REMOTE_ERR) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: ForwardOpen Extended not supported, trying standard");
        rc = enip_connection_forward_open_attempt(conn, 0x54);
    }

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
               "ENIP: ForwardOpen OK (O->T=0x%08" PRIx32 " T->O=0x%08" PRIx32 " buffer=%zu)",
               conn->session.cip_targ_conn_id, conn->session.cip_orig_conn_id, conn->session.cip_size_o_to_t);
    }

    return rc;
}


/* Phase 3: Unconnected_Send routing wrapper (service 0x52).
 * For devices accessed through a bridge, wraps the inner CIP request in service 0x52
 * to route it via the Connection Manager using the device's connection path.
 * This is an alternative to ForwardOpen+connected messaging for bridged access.
 *
 * Inner request structure:
 *   service(1)=0x52  (Unconnected_Send)
 *   reserved(1)=0x00
 *   timeout(2)=0x0A00 (10ms in little-endian, arbitrary timeout)
 *   inner_cip_size(2) (length of the wrapped CIP request)
 *   inner_cip(variable) (the CIP request to route)
 *   route_path_size(1) (words)
 *   route_path(variable) (from conn->link.route_path)
 *
 * Returns complete Unconnected_Send CIP request or bytes_null() on error.
 * Phase 6: Used when target device is behind a gateway and unconnected path
 * is desired (fallback from ForwardOpen or for bridged reads).
 */
static Bytes enip_cip_unconnected_send_request(Arena *arena, Bytes inner_cip,
                                               const uint8_t *route_path, uint8_t route_path_words) {
    if(!inner_cip.data || route_path_words == 0) {
        return bytes_null();
    }

    /* Build Unconnected_Send header + inner CIP */
    Bytes uc_fixed = bytes_pack(arena, BYTES_LE,
                                (uint8_t)0x52,        /* service: Unconnected_Send */
                                (uint8_t)0x00,        /* reserved */
                                (uint16_t)0x000A,     /* timeout: 10ms */
                                (uint16_t)inner_cip.len, /* inner CIP length */
                                route_path_words);     /* route path size (words) */

    if(bytes_is_null(uc_fixed)) {
        return bytes_null();
    }

    /* Append inner CIP payload */
    Bytes uc_with_cip = bytes_concat(arena, uc_fixed, inner_cip);
    if(bytes_is_null(uc_with_cip)) {
        return bytes_null();
    }

    /* Append route path */
    uint8_t *path_buf = arena_alloc(arena, (size_t)route_path_words * 2u);
    if(!path_buf) {
        return bytes_null();
    }
    memcpy(path_buf, route_path, (size_t)route_path_words * 2u);

    Bytes complete = bytes_concat(arena, uc_with_cip, (Bytes){path_buf, (size_t)route_path_words * 2u});
    return complete;
}


/* Phase 3: Helper to send and receive a CIP message over unconnected messaging.
 * Wraps CIP payload in unconnected CPF, sends via ENIP_CMD_UNCONNECTED_SEND (0x006F),
 * receives response, and extracts CIP payload.
 * On success, returns CIP response payload (stripped of EIP+CPF headers).
 * On error, returns bytes_null().  Caller must parse CIP status and data.
 *
 * Phase 6: Used for pre-ForwardOpen reads or fallback when connected messaging is unavailable. */
static Bytes enip_connection_send_recv_unconnected(enip_connection_t *conn, Bytes cip_payload,
                                                   int timeout_ms) {
    if(!conn || !conn->link.socket || bytes_is_null(cip_payload) || !conn->session.established) {
        return bytes_null();
    }

    arena_reset(&conn->tx_arena);

    /* Wrap CIP in unconnected CPF */
    Bytes cpf = enip_cpf_build_unconnected(&conn->tx_arena, cip_payload);
    if(bytes_is_null(cpf)) {
        return bytes_null();
    }

    /* Wrap in EIP frame */
    Bytes frame = enip_eip_build_request(&conn->tx_arena, ENIP_CMD_UNCONNECTED_SEND,
                                         conn->session.session_handle, &conn->session.sender_context, cpf);
    if(bytes_is_null(frame)) {
        return bytes_null();
    }

    /* Send */
    socket_wait_state_t io_state = {0};
    int32_t rc = socket_write_wait(conn->link.socket, &frame, timeout_ms, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Unconnected send failed: %d", rc);
        return bytes_null();
    }
    conn->messages_sent++;

    /* Receive response */
    arena_reset(&conn->rx_arena);
    Bytes response;
    rc = enip_recv_frame(conn->link.socket, &conn->rx_arena, timeout_ms, &io_state, &response);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Unconnected recv failed: %d", rc);
        return bytes_null();
    }
    conn->messages_received++;

    /* Extract CIP payload from EIP+CPF response */
    Bytes eip_cpf = enip_eip_extract_cpf_payload(response);
    if(bytes_is_null(eip_cpf)) {
        return bytes_null();
    }

    Bytes cip_response = enip_cpf_extract_udi_payload(eip_cpf);
    if(bytes_is_null(cip_response)) {
        return bytes_null();
    }

    return cip_response;
}


/* Phase 3: Helper to send and receive a CIP message over connected messaging.
 * Wraps CIP payload in connected CPF with current sequence number, sends via
 * ENIP_CMD_CONNECTED_SEND (0x0070), receives response, and extracts CIP payload.
 * On success, returns CIP response payload (stripped of EIP+CPF headers).
 * On error, returns bytes_null().  Caller must parse CIP status and data.
 *
 * Phase 6: This will be called in the main loop for each pending tag I/O. */
static Bytes enip_connection_send_recv_connected(enip_connection_t *conn, Bytes cip_payload,
                                                 int timeout_ms) {
    if(!conn || !conn->link.socket || bytes_is_null(cip_payload) || !conn->session.cip_connection_open) {
        return bytes_null();
    }

    arena_reset(&conn->tx_arena);

    /* Increment sequence number for this request */
    conn->session.cip_seq_num++;

    /* Wrap CIP in connected CPF */
    Bytes cpf = enip_cpf_build_connected(&conn->tx_arena, conn->session.cip_targ_conn_id,
                                         conn->session.cip_seq_num, cip_payload);
    if(bytes_is_null(cpf)) {
        return bytes_null();
    }

    /* Wrap in EIP frame */
    Bytes frame = enip_eip_build_request(&conn->tx_arena, ENIP_CMD_CONNECTED_SEND,
                                         conn->session.session_handle, &conn->session.sender_context, cpf);
    if(bytes_is_null(frame)) {
        return bytes_null();
    }

    /* Send */
    socket_wait_state_t io_state = {0};
    int32_t rc = socket_write_wait(conn->link.socket, &frame, timeout_ms, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Connected send failed: %d", rc);
        return bytes_null();
    }
    conn->messages_sent++;

    /* Receive response */
    arena_reset(&conn->rx_arena);
    Bytes response;
    rc = enip_recv_frame(conn->link.socket, &conn->rx_arena, timeout_ms, &io_state, &response);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Connected recv failed: %d", rc);
        return bytes_null();
    }
    conn->messages_received++;

    /* Extract CIP payload from EIP+CPF response */
    Bytes eip_cpf = enip_eip_extract_cpf_payload(response);
    if(bytes_is_null(eip_cpf)) {
        return bytes_null();
    }

    Bytes cip_response = enip_cpf_extract_cdi_payload(eip_cpf);
    if(bytes_is_null(cip_response)) {
        return bytes_null();
    }

    return cip_response;
}


/* Phase 3: Close the open connection and release buffer resources on target.
 * Sends ForwardClose (service 0x4E) to Connection Manager.
 * Does not fail the overall disconnect if ForwardClose fails; caller must clean up. */
static int32_t enip_connection_forward_close(enip_connection_t *conn) {
    if(!conn || !conn->link.socket || !conn->session.established || !conn->session.cip_connection_open) {
        return PLCTAG_STATUS_OK; /* Not open, nothing to close */
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "ENIP: ForwardClose starting (O->T=0x%08" PRIx32 " T->O=0x%08" PRIx32 ")",
           conn->session.cip_targ_conn_id, conn->session.cip_orig_conn_id);

    arena_reset(&conn->tx_arena);

    /* Build connection path (same as ForwardOpen) */
    uint8_t conn_path[8];
    uint8_t conn_path_words;

    if(conn->link.cpu_slot >= 0) {
        conn_path[0] = 0x01;
        conn_path[1] = (uint8_t)conn->link.cpu_slot;
        conn_path[2] = 0x20; /* class segment */
        conn_path[3] = 0x02; /* Message Router */
        conn_path[4] = 0x24; /* instance segment */
        conn_path[5] = 0x01; /* instance 1 */
        conn_path_words = 3;
    } else {
        conn_path[0] = 0x20;
        conn_path[1] = 0x02;
        conn_path[2] = 0x24;
        conn_path[3] = 0x01;
        conn_path_words = 2;
    }

    /* ForwardClose (0x4E) request:
     *   service(1)=0x4E
     *   path_size(1)=2 (CM)
     *   path(4)={0x20,0x06,0x24,0x01}
     *   priority_and_reserved(1)=0x00
     *   timeout_ticks(1)=0x0E
     *   conn_serial_number(2)
     *   orig_vendor_id(2)=0xF33D
     *   orig_serial_number(4)=0x21504345
     *   conn_path_size(1) (words)
     *   conn_path(variable)
     */
    Bytes fc_fixed = bytes_pack(&conn->tx_arena, BYTES_LE,
                                (uint8_t)0x4E,        /* service: ForwardClose */
                                (uint8_t)0x02,        /* path_size: 2 words to CM */
                                (uint8_t)0x20, (uint8_t)0x06, (uint8_t)0x24, (uint8_t)0x01,
                                (uint8_t)0x00,        /* priority_and_reserved */
                                (uint8_t)0x0E,        /* timeout_ticks */
                                (uint16_t)conn->session.cip_conn_serial,
                                (uint16_t)0xF33D,     /* vendor ID */
                                (uint32_t)0x21504345u,/* serial number */
                                conn_path_words);

    if(bytes_is_null(fc_fixed)) { return PLCTAG_ERR_NO_MEM; }

    /* Append connection path */
    size_t path_bytes = (size_t)conn_path_words * 2u;
    uint8_t *path_buf = arena_alloc(&conn->tx_arena, path_bytes);
    if(!path_buf) { return PLCTAG_ERR_NO_MEM; }
    memcpy(path_buf, conn_path, path_bytes);

    Bytes cip_payload = bytes_concat(&conn->tx_arena, fc_fixed,
                                     (Bytes){path_buf, path_bytes});
    if(bytes_is_null(cip_payload)) { return PLCTAG_ERR_NO_MEM; }

    /* Wrap in CPF+EIP and send */
    Bytes cpf = enip_cpf_build_unconnected(&conn->tx_arena, cip_payload);
    if(bytes_is_null(cpf)) { return PLCTAG_ERR_NO_MEM; }

    Bytes frame = enip_eip_build_request(&conn->tx_arena, ENIP_CMD_UNCONNECTED_SEND,
                                         conn->session.session_handle, &conn->session.sender_context, cpf);
    if(bytes_is_null(frame)) { return PLCTAG_ERR_NO_MEM; }

    socket_wait_state_t io_state = {0};
    int32_t rc = socket_write_wait(conn->link.socket, &frame, 5000, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: ForwardClose send failed: %d", rc);
        return rc;
    }
    conn->messages_sent++;

    /* Receive response */
    arena_reset(&conn->rx_arena);
    Bytes response;
    rc = enip_recv_frame(conn->link.socket, &conn->rx_arena, 5000, &io_state, &response);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: ForwardClose read failed: %d", rc);
        return rc;
    }
    conn->messages_received++;

    /* Extract and parse CIP response (ignore status; we're closing anyway) */
    Bytes cpf_response = enip_eip_extract_cpf_payload(response);
    if(!bytes_is_null(cpf_response)) {
        Bytes cip_response = enip_cpf_extract_udi_payload(cpf_response);
        if(!bytes_is_null(cip_response)) {
            uint8_t cip_status;
            enip_cip_parse_response(cip_response, &cip_status, NULL, NULL);
            if(cip_status == 0x00) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: ForwardClose OK");
            } else {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                       "ENIP: ForwardClose CIP status 0x%02" PRIx8 " (non-fatal)", cip_status);
            }
        }
    }

    conn->session.cip_connection_open = false;
    return PLCTAG_STATUS_OK;
}


/* ============================================================================
 * §13.7 Connection Loop Helpers (Blocking Style)
 * ============================================================================ */

/* Update reported connection status and log the transition. */
static void enip_set_conn_status(enip_connection_t *conn, int32_t status) {
    if(conn->state == status) { return; }
    conn->state = status;
    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Connection status -> %d", status);
}


/* Returns 0 if the front tag's op_time has expired (due now), else milliseconds
 * until it is due, or ENIP_WORK_WAIT_MS if active_tags is empty. */
#define ENIP_WORK_WAIT_MS ((int64_t)1000)

static int64_t enip_next_due_wait(enip_connection_t *conn, int64_t now_ms) {
    int64_t wait_ms = ENIP_WORK_WAIT_MS;

    critical_block(conn->active_tags_mutex) {
        if(vector_length(conn->active_tags) == 0) { break; }
        enip_tag_t *front = (enip_tag_t *)vector_get(conn->active_tags, 0);
        if(!front) { break; }
        int64_t until_due = front->op.op_time - now_ms;
        wait_ms = (until_due <= 0) ? 0 : until_due;
    }

    return wait_ms;
}


/* True if no message has been sent/received for ENIP_IDLE_TIMEOUT_MS. */
#define ENIP_IDLE_TIMEOUT_MS ((int64_t)60000)

static bool enip_idle_expired(enip_connection_t *conn, int64_t now_ms) {
    return (now_ms - conn->last_message_time_ms) > ENIP_IDLE_TIMEOUT_MS;
}


/* Send all bytes of *req, resuming on PENDING (wake absorbed, I/O continues).
 * Increments conn->messages_sent on success. */
static int32_t enip_send_all(enip_connection_t *conn, Bytes *req) {
    int32_t rc;
    do {
        rc = socket_write_wait(conn->link.socket, req, 5000, &conn->link.io);
    } while(rc == PLCTAG_STATUS_PENDING && !conn->shutdown_requested);
    if(rc == PLCTAG_STATUS_OK) { conn->messages_sent++; }
    return rc;
}


/* Reset all RESPONSE-state tags back to REQUEST so they retry on reconnect.
 * Called from enip_connection_graceful_close before the socket is torn down. */
static void enip_reset_inflight_tags(enip_connection_t *conn) {
    critical_block(conn->active_tags_mutex) {
        int n = vector_length(conn->active_tags);
        for(int i = 0; i < n; i++) {
            enip_tag_t *tag = (enip_tag_t *)vector_get(conn->active_tags, i);
            if(tag && tag->op.op_state == ENIP_OP_INFLIGHT) {
                tag->op.op_state = ENIP_OP_REQUEST;
            }
        }
    }
}


/* Receive one EIP frame and dispatch CIP response(s) to in-flight tag(s).
 *
 * Tags in RESPONSE state in active_tags are the in-flight set.  Two cases:
 *
 *   0x8A  Multiple Service Response — slots map 1:1 to RESPONSE-state tags
 *         in active_tags order; batch sends do not fragment.
 *   other Single-tag response — find the RESPONSE-state tag whose
 *         transaction_id matches sender_context; on PLCTAG_ERR_PARTIAL encode
 *         + send the next chunk and loop.
 *
 * Returns PLCTAG_STATUS_OK, or an error code that should trigger err_backoff. */
static int32_t enip_recv_dispatch(enip_connection_t *conn) {
    int32_t rc;

    while(!conn->shutdown_requested) {
        Bytes response;
        arena_reset(&conn->rx_arena);

        do {
            rc = enip_recv_frame(conn->link.socket, &conn->rx_arena, 5000, &conn->link.io, &response);
        } while(rc == PLCTAG_STATUS_PENDING && !conn->shutdown_requested);

        if(rc != PLCTAG_STATUS_OK) { return rc; }

        conn->messages_received++;
        conn->last_message_time_ms = time_ms();

        /* Extract CIP payload (connected or unconnected framing) */
        Bytes cpf_payload = enip_eip_extract_cpf_payload(response);
        if(bytes_is_null(cpf_payload)) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to extract CPF payload");
            return PLCTAG_ERR_REMOTE_ERR;
        }

        Bytes cip_payload = conn->session.cip_connection_open
                            ? enip_cpf_extract_cdi_payload(cpf_payload)
                            : enip_cpf_extract_udi_payload(cpf_payload);
        if(bytes_is_null(cip_payload)) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to extract CIP payload");
            return PLCTAG_ERR_REMOTE_ERR;
        }

        uint8_t service_reply = (cip_payload.len > 0) ? cip_payload.data[0] : 0;

        if(service_reply == 0x8A) {
            /* ── Multiple Service Response (0x0A batch) ─────────────────────
             * Walk active_tags in order; each RESPONSE-state tag maps to the
             * next slot in the offset table.  No fragmentation for batches. */
            if(cip_payload.len < 8) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Batch response too short (%zu)", cip_payload.len);
                return PLCTAG_ERR_REMOTE_ERR;
            }

            uint8_t  ext_sz      = cip_payload.data[3];
            size_t   count_pos   = 4 + (size_t)ext_sz * 2;
            if(cip_payload.len < count_pos + 2) { return PLCTAG_ERR_REMOTE_ERR; }

            uint16_t slot_count = 0;
            bytes_unpack(bytes_slice(cip_payload, count_pos, 2), BYTES_LE, &slot_count);
            size_t offsets_base = count_pos + 2;

            critical_block(conn->active_tags_mutex) {
                int n = vector_length(conn->active_tags);
                uint16_t slot = 0;

                for(int i = 0; i < n && slot < slot_count; i++) {
                    enip_tag_t *tag = (enip_tag_t *)vector_get(conn->active_tags, i);
                    if(!tag || tag->op.op_state != ENIP_OP_INFLIGHT) { continue; }

                    uint16_t slot_off = 0;
                    bytes_unpack(bytes_slice(cip_payload, offsets_base + slot * 2u, 2),
                                 BYTES_LE, &slot_off);
                    uint16_t next_off = (uint16_t)cip_payload.len;
                    if(slot + 1 < slot_count) {
                        bytes_unpack(bytes_slice(cip_payload, offsets_base + (slot + 1u) * 2u, 2),
                                     BYTES_LE, &next_off);
                    }

                    if(slot_off < cip_payload.len && (size_t)next_off <= cip_payload.len) {
                        Bytes slot_resp = bytes_slice(cip_payload, slot_off,
                                                      (size_t)(next_off - slot_off));
                        int32_t rc_accept = conn->mfg_ops->accept_chunk(tag, slot_resp);
                        tag->status = (int8_t)(rc_accept == PLCTAG_STATUS_OK ? 0 : rc_accept);
                        
                        
                        tag->op.op_state = ENIP_OP_IDLE;
                        if(rc_accept != PLCTAG_STATUS_OK) {
                            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                                   "ENIP: Batch slot %u accept_chunk error: %d",
                                   (unsigned)slot, rc_accept);
                        }
                    }
                    slot++;
                }
            }
            return PLCTAG_STATUS_OK;

        } else {
            /* ── Single-tag response ─────────────────────────────────────────
             * Find the RESPONSE-state tag matching sender_context; on PARTIAL
             * encode + send the next chunk and loop to receive again.         */
            uint64_t ctx = enip_connection_extract_sender_context(response.data);
            enip_tag_t *tag = NULL;

            critical_block(conn->active_tags_mutex) {
                int n = vector_length(conn->active_tags);
                for(int i = 0; i < n; i++) {
                    enip_tag_t *candidate = (enip_tag_t *)vector_get(conn->active_tags, i);
                    if(candidate && candidate->op.op_state == ENIP_OP_INFLIGHT
                       && candidate->op.transaction_id == ctx) {
                        /* rc_inc returns NULL if the tag is already being destroyed */
                        tag = (enip_tag_t *)rc_inc(candidate);
                        break;
                    }
                }
            }

            if(!tag) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                       "ENIP: No RESPONSE-state tag for context=0x%016" PRIx64 " (stale?)", ctx);
                return PLCTAG_STATUS_OK;
            }

            int32_t rc_accept = conn->mfg_ops->accept_chunk(tag, cip_payload);

            if(rc_accept == PLCTAG_ERR_PARTIAL) {
                /* Encode the next chunk and send it; tag stays RESPONSE-state */
                arena_reset(&conn->tx_arena);
                size_t req_avail = enip_packetizer_cip_budget(conn->session.cip_size_o_to_t, true);

                Bytes next_req = conn->mfg_ops->encode_chunk(tag, &conn->tx_arena,
                                                             req_avail, req_avail);
                if(bytes_is_null(next_req)) {
                    critical_block(conn->active_tags_mutex) {
                        tag->status = PLCTAG_ERR_REMOTE_ERR;
                        
                        
                        tag->op.op_state = ENIP_OP_IDLE;
                    }
                    rc_dec(tag);
                    return PLCTAG_ERR_REMOTE_ERR;
                }

                /* Stamp new transaction_id before enip_eip_build_request increments it */
                critical_block(conn->active_tags_mutex) {
                    tag->op.transaction_id = conn->session.sender_context;
                }

                conn->session.cip_seq_num++;
                Bytes cpf = enip_cpf_build_connected(&conn->tx_arena,
                                                      conn->session.cip_targ_conn_id,
                                                      conn->session.cip_seq_num, next_req);
                Bytes frame = bytes_is_null(cpf) ? bytes_null()
                            : enip_eip_build_request(&conn->tx_arena,
                                                      ENIP_CMD_CONNECTED_SEND,
                                                      conn->session.session_handle,
                                                      &conn->session.sender_context, cpf);
                if(bytes_is_null(frame)) { rc_dec(tag); return PLCTAG_ERR_NO_MEM; }

                rc = enip_send_all(conn, &frame);
                if(rc != PLCTAG_STATUS_OK) { rc_dec(tag); return rc; }
                continue; /* receive next fragment response */
            }

            /* Done — OK or terminal error */
            critical_block(conn->active_tags_mutex) {
                tag->status = (int8_t)(rc_accept == PLCTAG_STATUS_OK ? 0 : rc_accept);
                
                
                tag->op.op_state = ENIP_OP_IDLE;
            }
            rc_dec(tag); /* release the ref acquired before the mutex gap */
            if(rc_accept != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: accept_chunk failed: %d", rc_accept);
            }
            return PLCTAG_STATUS_OK;
        }
    }

    return PLCTAG_STATUS_PENDING; /* shutdown interrupted */
}


/* ForwardClose (if open) → UnregisterSession → socket_close (data fd only).
 * Resets any in-flight (RESPONSE-state) tags back to REQUEST so they retry.
 * Best-effort: errors are logged but never prevent cleanup. */
static void enip_connection_graceful_close(enip_connection_t *conn) {
    enip_reset_inflight_tags(conn);
    if(conn->session.cip_connection_open) {
        enip_connection_forward_close(conn);
    }
    if(conn->session.established) {
        enip_connection_unregister_session(conn);
    }
    socket_close(conn->link.socket); /* data fd only; wake pipe survives */
    conn->session.established = false;
    conn->session.cip_connection_open = false;
}


/* Block (wake-interruptible) until at least one tag in active_tags is due.
 * Returns immediately if work is already pending or shutdown is requested. */
static void enip_wait_for_work(enip_connection_t *conn) {
    while(!conn->shutdown_requested) {
        int64_t now_ms  = time_ms();
        int64_t wait_ms = enip_next_due_wait(conn, now_ms);
        if(wait_ms == 0) { return; }
        int wait_int = (wait_ms > (int64_t)INT32_MAX) ? INT32_MAX : (int)wait_ms;
        socket_wait_event(conn->link.socket, SOCK_EVENT_DEFAULT_MASK, wait_int);
    }
}


/* Exponential backoff with simple jitter after a connection failure. */
#define ENIP_MAX_BACKOFF_MS ((int32_t)30000)

static void enip_backoff_with_jitter(enip_connection_t *conn) {
    conn->connect_attempt_count++;
    int32_t shift   = conn->connect_attempt_count < 13
                      ? conn->connect_attempt_count : 13;
    int32_t base_ms = 1 << shift;
    int32_t jitter  = (int32_t)(time_ms() & 0xFF); /* 0–255 ms */
    int32_t wait_ms = base_ms + jitter;
    if(wait_ms > ENIP_MAX_BACKOFF_MS) { wait_ms = ENIP_MAX_BACKOFF_MS; }

    int64_t deadline = time_ms() + wait_ms;
    while(!conn->shutdown_requested) {
        int64_t remaining = deadline - time_ms();
        if(remaining <= 0) { break; }
        int chunk = (int)(remaining > 1000 ? 1000 : remaining);
        socket_wait_event(conn->link.socket, SOCK_EVENT_WAKE_UP | SOCK_EVENT_TIMEOUT, chunk);
    }
}


/* ── (dead code removed) ──────────────────────────────────────────────────────
 * enip_connection_match_response  — replaced by enip_recv_dispatch
 * enip_connection_decode_response — replaced by enip_recv_dispatch
 * Both are deleted; their broadcast-to-all-tags approach was incorrect.
 * ─────────────────────────────────────────────────────────────────────────── */


/* Build requests with multi-service 0x0A batching support.
 * Collects multiple REQUEST-state tags, uses packetizer to determine batching strategy:
 * - If single tag fits alone: send as single CIP request
 * - If multiple tags fit with 0x0A: batch them together
 * - Otherwise: send first tag, defer remainder to next cycle
 *
 * Returns: PLCTAG_STATUS_OK if request built and returned,
 *          PLCTAG_ERR_NO_DATA if no pending work,
 *          PLCTAG_ERR_* on failure. */
static int enip_connection_build_requests(enip_connection_t *conn, Bytes *out_request) {
    if(!conn || !out_request) { return PLCTAG_ERR_NULL_PTR; }

    /* Only proceed if we have an open connected path */
    if(!conn->session.cip_connection_open) {
        return PLCTAG_ERR_NO_DATA;
    }

    arena_reset(&conn->tx_arena);

    /* Collect REQUEST-state tags; compute per-slot budgets per §15.5 dual-budget model.
     * req_used / resp_used track the running CIP-level bytes on each side. */
    enip_tag_t *tags[ENIP_PKT_MAX_SLOTS];
    Bytes cip_requests[ENIP_PKT_MAX_SLOTS];
    uint32_t tag_count = 0;

    size_t cip_budget = enip_packetizer_cip_budget(conn->session.cip_size_o_to_t, true);
    size_t req_used   = ENIP_PKT_MULTI_REQ_FIXED;
    size_t resp_used  = ENIP_PKT_MULTI_RESP_FIXED;

    critical_block(conn->active_tags_mutex) {
        int total_tags = vector_length((vector_p)conn->active_tags);
        for(int i = 0; i < total_tags && tag_count < ENIP_PKT_MAX_SLOTS; i++) {
            enip_tag_t *candidate = (enip_tag_t *)vector_get((vector_p)conn->active_tags, i);
            if(!candidate || candidate->op.op_state != ENIP_OP_REQUEST) { continue; }

            /* Per-slot available body bytes after offset-table entry (§15.4, §15.5) */
            size_t req_avail  = (cip_budget > req_used  + ENIP_PKT_MULTI_SLOT_OVERHEAD)
                                ? (cip_budget - req_used  - ENIP_PKT_MULTI_SLOT_OVERHEAD) : 0;
            size_t resp_avail = (cip_budget > resp_used + ENIP_PKT_MULTI_SLOT_OVERHEAD)
                                ? (cip_budget - resp_used - ENIP_PKT_MULTI_SLOT_OVERHEAD) : 0;

            if(req_avail == 0 || resp_avail == 0) { break; } /* frame full */

            Bytes cip_req = conn->mfg_ops->encode_chunk(candidate, &conn->tx_arena,
                                                        req_avail, resp_avail);
            if(bytes_is_null(cip_req)) {
                candidate->op.op_state = ENIP_OP_INFLIGHT;
                continue;
            }

            /* rc_inc returns NULL if the tag is already being destroyed; skip it */
            enip_tag_t *held = (enip_tag_t *)rc_inc(candidate);
            if(!held) { continue; }

            tags[tag_count] = held;
            cip_requests[tag_count] = cip_req;
            tag_count++;

            req_used  += ENIP_PKT_MULTI_SLOT_OVERHEAD + cip_req.len;
            /* resp_used estimate: encode_chunk doesn't yet return resp_body size (§15.6 TODO).
             * Use req size as a conservative proxy; enip_packetizer_plan() is the authoritative check. */
            resp_used += ENIP_PKT_MULTI_SLOT_OVERHEAD + cip_req.len;
        }
    }

    if(tag_count == 0) {
        return PLCTAG_ERR_NO_DATA;
    }

/* Helper macro: release all held refs and return an error from this function. */
#define BUILD_ERR_RETURN(code) \
    do { for(uint32_t _i = 0; _i < tag_count; _i++) { rc_dec(tags[_i]); } return (code); } while(0)

    /* Build final CIP payload (single or 0x0A batched) */
    Bytes cip_payload = bytes_null();

    if(tag_count == 1) {
        /* Single request - use as-is */
        cip_payload = cip_requests[0];
    } else if(conn->session.supports_multi_service) {
        /* Multiple requests - try 0x0A batching if supported */
        enip_pkt_plan_t plan;
        plan.slot_count = tag_count;
        plan.use_connected = true;
        plan.max_buffer = conn->session.cip_size_o_to_t;

        for(uint32_t i = 0; i < tag_count; i++) {
            plan.slot_req_sizes[i]  = (uint16_t)cip_requests[i].len;
            plan.slot_resp_sizes[i] = (uint16_t)cip_requests[i].len; /* §15.6 TODO */
        }

        int32_t plan_rc = enip_packetizer_plan(&plan);
        if(plan_rc == PLCTAG_STATUS_OK) {
            Bytes multi_header = bytes_pack(&conn->tx_arena, BYTES_LE,
                                           (uint8_t)0x0A,
                                           (uint8_t)0x02,
                                           (uint8_t)0x20, (uint8_t)0x06,
                                           (uint8_t)0x00,
                                           (uint16_t)tag_count);
            if(bytes_is_null(multi_header)) { BUILD_ERR_RETURN(PLCTAG_ERR_NO_MEM); }

            Bytes offset_table = bytes_null();
            uint16_t current_offset = (uint16_t)(tag_count * 2);

            for(uint32_t i = 0; i < tag_count; i++) {
                Bytes ob = bytes_pack(&conn->tx_arena, BYTES_LE, current_offset);
                if(bytes_is_null(ob)) { BUILD_ERR_RETURN(PLCTAG_ERR_NO_MEM); }
                offset_table = (i == 0) ? ob : bytes_concat(&conn->tx_arena, offset_table, ob);
                if(bytes_is_null(offset_table)) { BUILD_ERR_RETURN(PLCTAG_ERR_NO_MEM); }
                current_offset += (uint16_t)cip_requests[i].len;
            }

            Bytes temp = bytes_concat(&conn->tx_arena, multi_header, offset_table);
            if(bytes_is_null(temp)) { BUILD_ERR_RETURN(PLCTAG_ERR_NO_MEM); }

            for(uint32_t i = 0; i < tag_count; i++) {
                temp = bytes_concat(&conn->tx_arena, temp, cip_requests[i]);
                if(bytes_is_null(temp)) { BUILD_ERR_RETURN(PLCTAG_ERR_NO_MEM); }
            }
            cip_payload = temp;
        } else {
            /* 0x0A doesn't fit - send first tag only; release refs for tags we won't send */
            for(uint32_t i = 1; i < tag_count; i++) { rc_dec(tags[i]); }
            tag_count = 1;
            cip_payload = cip_requests[0];
        }
    } else {
        for(uint32_t i = 1; i < tag_count; i++) { rc_dec(tags[i]); }
        tag_count = 1;
        cip_payload = cip_requests[0];
    }

    if(bytes_is_null(cip_payload)) { BUILD_ERR_RETURN(PLCTAG_ERR_NO_MEM); }

    /* Capture context value before enip_eip_build_request stamps and increments it. */
    uint64_t used_ctx = conn->session.sender_context;

    Bytes cpf = enip_cpf_build_connected(&conn->tx_arena, conn->session.cip_targ_conn_id,
                                         conn->session.cip_seq_num, cip_payload);
    if(bytes_is_null(cpf)) { BUILD_ERR_RETURN(PLCTAG_ERR_NO_MEM); }

    Bytes frame = enip_eip_build_request(&conn->tx_arena, ENIP_CMD_CONNECTED_SEND,
                                         conn->session.session_handle, &conn->session.sender_context, cpf);
    if(bytes_is_null(frame)) { BUILD_ERR_RETURN(PLCTAG_ERR_NO_MEM); }

    conn->session.cip_seq_num++;

    /* Flip batch tags to RESPONSE and stamp transaction_id; then release build refs.
     * recv_dispatch will acquire its own ref when it processes each tag. */
    critical_block(conn->active_tags_mutex) {
        for(uint32_t i = 0; i < tag_count; i++) {
            tags[i]->op.transaction_id = used_ctx;
            tags[i]->op.op_state = ENIP_OP_INFLIGHT;
        }
    }
    for(uint32_t i = 0; i < tag_count; i++) { rc_dec(tags[i]); }

#undef BUILD_ERR_RETURN

    *out_request = frame;
    return PLCTAG_STATUS_OK;
}


/* Phase 6: Check if any tag in active_tags has work due now (op_time <= now_ms).
 * Used to decide whether to reconnect after idle disconnect.
 * Returns true if at least one tag is ready for processing. */
static bool enip_connection_has_due_tag(enip_connection_t *conn, int64_t now_ms) {
    if(!conn || !conn->active_tags) { return false; }

    bool has_due = false;

    critical_block(conn->active_tags_mutex) {
        int tag_count = vector_length((vector_p)conn->active_tags);
        for(int i = 0; i < tag_count; i++) {
            enip_tag_t *tag = (enip_tag_t *)vector_get((vector_p)conn->active_tags, i);
            if(tag && tag->op.op_time <= now_ms) {
                has_due = true;
                break;
            }
        }
    }

    return has_due;
}


/* Phase 7: correct dispatch logic.
 * One fix: change to call enip_metadata_fetch_root_symbols directly for AB devices
 * (after enip_mfg_ab_fetch_phase1_metadata is rewritten in Phase 7 to delegate there).
 * No changes to this function needed independently. */
static int enip_connection_phase1_metadata(enip_connection_t *conn) {
    /* Phase-1 metadata fetch (manufacturer-specific)
     *
     * Delegate to manufacturer strategy callback. Each manufacturer has its own
     * way of querying device metadata:
     * - AB: GetInstanceAttributeList on Class 0x6B (Symbol inventory)
     * - OMRON: Manufacturer-specific queries
     * - PCCC: Bridge queries through EtherNet/IP gateway
     */
    int rc;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Fetching phase-1 metadata (manufacturer-specific)");

    if(!conn || !conn->link.socket || !conn->session.established) { return PLCTAG_ERR_NULL_PTR; }

    if(!conn->mfg_ops || !conn->mfg_ops->fetch_phase1_metadata) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: No phase-1 metadata callback available");
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* Reset arena for manufacturer-specific queries */
    arena_reset(&conn->tx_arena);

    /* Call manufacturer-specific phase-1 metadata fetch */
    rc = conn->mfg_ops->fetch_phase1_metadata(conn, &conn->tx_arena);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Phase-1 metadata fetch failed: %d", rc);
        return rc;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Phase-1 metadata fetched successfully");
    return PLCTAG_STATUS_OK;
}


/* Ensure every due REQUEST-state tag has resolved metadata before encoding.
 * For each unresolved tag:
 *   1. Look up tag_name in root_symbol_cache (phase-1) → get instance_id.
 *   2. Call mfg_ops->fetch_tag_metadata (phase-2 I/O) → get elem_size/data_type.
 *   3. Allocate tag->data and stamp metadata_generation.
 * Returns PLCTAG_ERR_BAD_CONNECTION if the socket died during a phase-2 fetch. */
static int32_t enip_ensure_due_tags_metadata(enip_connection_t *conn, int64_t now_ms) {
    if(!conn || !conn->mfg_ops) { return PLCTAG_STATUS_OK; }

    for(int i = 0; !conn->shutdown_requested; i++) {
        enip_tag_t *tag = NULL;
        bool done = false;

        critical_block(conn->active_tags_mutex) {
            int n = vector_length(conn->active_tags);
            if(i >= n) { done = true; break; }
            enip_tag_t *c = (enip_tag_t *)vector_get(conn->active_tags, i);
            if(c && c->op.op_state == ENIP_OP_REQUEST
               && c->op.op_time <= now_ms
               && c->meta.generation != conn->metadata_generation) {
                tag = (enip_tag_t *)rc_inc(c); /* NULL if tag is being destroyed */
            }
        }

        if(done) { break; }
        if(!tag) { continue; } /* this slot is either not due or already resolved */

        /* Phase-1: name → instance_id via root_symbol_cache */
        enip_root_symbol_entry_t *sym = enip_metadata_find_root_symbol(conn, tag->tag_name);
        if(!sym) {
            critical_block(conn->active_tags_mutex) {
                tag->status = PLCTAG_ERR_NOT_FOUND;
                tag->op.op_state = ENIP_OP_IDLE;
                
                
            }
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: tag '%s' not in root symbol cache", tag->tag_name);
            rc_dec(tag);
            continue;
        }
        tag->meta.instance_id = sym->instance_id;

        /* Phase-2: instance_id → elem_size / data_type / dims (I/O, no mutex held) */
        if(conn->mfg_ops->fetch_tag_metadata) {
            int32_t rc2 = conn->mfg_ops->fetch_tag_metadata(tag);
            if(rc2 == PLCTAG_ERR_BAD_CONNECTION) {
                rc_dec(tag);
                return PLCTAG_ERR_BAD_CONNECTION;
            }
            if(rc2 != PLCTAG_STATUS_OK) {
                critical_block(conn->active_tags_mutex) {
                    tag->status = (int8_t)rc2;
                    tag->op.op_state = ENIP_OP_IDLE;
                    
                    
                }
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                       "ENIP: fetch_tag_metadata failed for '%s': %d", tag->tag_name, rc2);
                rc_dec(tag);
                continue;
            }
        }

        /* Allocate data buffer and stamp generation so build_requests can encode this tag */
        critical_block(conn->active_tags_mutex) {
            if(!tag->data && tag->meta.elem_size > 0 && tag->meta.elem_count > 0) {
                tag->size = tag->meta.elem_count * tag->meta.elem_size;
                tag->data = (uint8_t *)mem_alloc(tag->size);
            }
            if(tag->data) {
                tag->meta.generation = conn->metadata_generation;
            } else if(!tag->data) {
                tag->status = PLCTAG_ERR_NO_MEM;
                tag->op.op_state = ENIP_OP_IDLE;
                
                
            }
        }

        rc_dec(tag);
    }

    return PLCTAG_STATUS_OK;
}


static void *enip_connection_thread_entry(void *arg) {
    enip_connection_t *conn = (enip_connection_t *)arg;
    int32_t rc;

    if(!conn) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: NULL connection passed to thread");
        return NULL;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Connection thread started");

    enip_set_conn_status(conn, PLCTAG_CONN_STATUS_DOWN);

    while(!conn->shutdown_requested) {

        /* Wait for work before spending resources on the bootstrap sequence */
        enip_wait_for_work(conn);
        if(conn->shutdown_requested) { break; }

        enip_set_conn_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

        /* ── Bootstrap (steps 1–6) ── each step is blocking; failure jumps to err_backoff */

        const char *host = conn->link.host[0] ? conn->link.host : "192.168.1.100";
        int port = conn->link.port ? (int)conn->link.port : 44818;

        rc = enip_connection_tcp_connect(conn, host, port, 5000);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: TCP connect failed: %d", rc);
            goto err_backoff;
        }

        rc = enip_connection_register_session(conn);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: RegisterSession failed: %d", rc);
            goto err_backoff;
        }

        rc = enip_connection_get_identity(conn);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: GetIdentity failed: %d", rc);
            goto err_backoff;
        }

        rc = enip_connection_forward_open(conn);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: ForwardOpen failed: %d", rc);
            goto err_backoff;
        }

        rc = enip_connection_phase1_metadata(conn);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Phase-1 metadata failed: %d", rc);
            goto err_backoff;
        }

        /* Bump generation — every tag must re-resolve its metadata on this connection */
        conn->metadata_generation++;
        conn->connect_attempt_count = 0;
        conn->last_message_time_ms = time_ms();

        enip_set_conn_status(conn, PLCTAG_CONN_STATUS_UP);

        /* ── Inner loop: serve tags until idle timeout, shutdown, or socket error ── */
        while(!conn->shutdown_requested) {
            int64_t now_ms  = time_ms();
            int64_t wait_ms = enip_next_due_wait(conn, now_ms);

            if(wait_ms > 0) {
                int ev = socket_wait_event(conn->link.socket, SOCK_EVENT_DEFAULT_MASK, (int)wait_ms);
                if(ev & (SOCK_EVENT_ERROR | SOCK_EVENT_DISCONNECT)) { goto err_backoff; }
                if(enip_idle_expired(conn, time_ms())) { break; }
                continue;
            }

            /* Resolve metadata for every due tag before encoding (§13.5) */
            rc = enip_ensure_due_tags_metadata(conn, now_ms);
            if(rc == PLCTAG_ERR_BAD_CONNECTION) { goto err_backoff; }

            Bytes request = {NULL, 0};
            rc = enip_connection_build_requests(conn, &request);
            if(rc != PLCTAG_STATUS_OK || !request.data) { continue; }

            if(enip_send_all(conn, &request)  != PLCTAG_STATUS_OK) { goto err_backoff; }
            if(enip_recv_dispatch(conn)       != PLCTAG_STATUS_OK) { goto err_backoff; }
        }

        /* ── Graceful close (idle disconnect or shutdown) ── */
        enip_connection_graceful_close(conn);
        enip_set_conn_status(conn, PLCTAG_CONN_STATUS_DOWN);
        if(conn->shutdown_requested) { break; }

        enip_set_conn_status(conn, PLCTAG_CONN_STATUS_IDLE_WAIT);
        continue; /* back to top: enip_wait_for_work */

err_backoff:
        enip_connection_graceful_close(conn);
        enip_set_conn_status(conn, PLCTAG_CONN_STATUS_ERR_WAIT);
        enip_backoff_with_jitter(conn);
        /* continue outer loop: enip_wait_for_work */
    }

    /* ── Final teardown ── */
    enip_set_conn_status(conn, PLCTAG_CONN_STATUS_DISCONNECTING);
    enip_connection_graceful_close(conn);

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Connection thread exited");
    return NULL;
}


enip_connection_t *enip_connection_create(attr attribs) {
    enip_connection_t *conn = (enip_connection_t *)rc_alloc(sizeof(enip_connection_t), enip_connection_destructor);
    int32_t rc;

    if(!conn) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to allocate connection");
        return NULL;
    }

    rc = arena_init(&conn->tx_arena, 32768);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to init TX arena: %d", rc);
        rc_dec(conn);
        return NULL;
    }

    rc = arena_init(&conn->rx_arena, 32768);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to init RX arena: %d", rc);
        rc_dec(conn);
        return NULL;
    }

    /* Socket created once for the connection lifetime; close() only shuts the data fd. */
    rc = socket_create(&conn->link.socket);
    if(rc != PLCTAG_STATUS_OK || !conn->link.socket) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to create socket: %d", rc);
        rc_dec(conn);
        return NULL;
    }

    conn->active_tags = vector_create(16, 16);
    if(!conn->active_tags) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to create active_tags vector");
        rc_dec(conn);
        return NULL;
    }

    rc = mutex_create(&conn->active_tags_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to create active_tags_mutex: %d", rc);
        rc_dec(conn);
        return NULL;
    }

    rc = mutex_create(&conn->root_symbol_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to create root_symbol_mutex: %d", rc);
        rc_dec(conn);
        return NULL;
    }

    /* Parse gateway: "host" or "host:port" */
    conn->link.port = 44818;
    const char *gw = attr_get_str(attribs, "gateway", "");
    if(gw && gw[0]) {
        char **parts = str_split(gw, ":");
        if(parts && parts[0]) {
            str_copy(conn->link.host, (int)sizeof(conn->link.host), parts[0]);
            if(parts[1]) {
                int parsed_port = 44818;
                str_to_int(parts[1], &parsed_port);
                conn->link.port = (parsed_port > 0 && parsed_port < 65536) ? (uint16_t)parsed_port : 44818;
            }
        }
        if(parts) { mem_free(parts); }
    }

    /* Parse path: comma-separated integers e.g. "1,0" (port 1, slot 0 for ControlLogix).
     * Absent or empty path means no backplane routing (CompactLogix, Micro800, OMRON). */
    conn->link.cpu_slot       = -1;
    conn->link.route_path_words = 0;
    const char *path_str = attr_get_str(attribs, "path", NULL);
    if(path_str && path_str[0]) {
        char **segs = str_split(path_str, ",");
        int byte_idx = 0;
        bool parse_ok = (segs != NULL);

        if(segs) {
            for(int s = 0; segs[s] && byte_idx < (int)sizeof(conn->link.route_path); s++) {
                int val = 0;
                if(str_to_int(segs[s], &val) != PLCTAG_STATUS_OK || val < 0 || val > 255) {
                    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Invalid path segment '%s'", segs[s]);
                    parse_ok = false;
                    break;
                }
                conn->link.route_path[byte_idx++] = (uint8_t)val;
            }
            mem_free(segs);
        }

        if(!parse_ok || byte_idx % 2 != 0) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Bad path '%s' (must be even-count integers)", path_str);
            rc_dec(conn);
            return NULL;
        }

        conn->link.route_path_words = (uint8_t)(byte_idx / 2); /* words */
        if(byte_idx >= 2) {
            uint8_t raw_slot = conn->link.route_path[1];
            /* slot must fit in 0–127: values ≥128 cast to negative int8_t and
             * would alias the -1 "no slot" sentinel. Real CIP slots are 0–14. */
            if(raw_slot > 127) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                       "ENIP: path slot byte 0x%02" PRIx8 " out of valid range (0-127)", raw_slot);
                rc_dec(conn);
                return NULL;
            }
            conn->link.cpu_slot = (int8_t)raw_slot;
        }
    }

    conn->state = PLCTAG_CONN_STATUS_DOWN;

    /* Start the background connection thread. The thread holds no rc_inc on conn;
     * tags hold the refs.  The destructor joins the thread before freeing conn. */
    rc = thread_create(&conn->thread, enip_connection_thread_entry,
                       32768, (void *)conn);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to start connection thread: %d", rc);
        rc_dec(conn);
        return NULL;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Connection created (host=%s port=%u slot=%d)",
           conn->link.host, conn->link.port, conn->link.cpu_slot);
    return conn;
}


/* Placeholder symbol to ensure file links */
int enip_conn_placeholder_symbol = 0;
