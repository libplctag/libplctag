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
 *   - Replace conn->sender_context_base with conn->sender_context everywhere
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
 *     (a) try ForwardOpen Extended (0x5B) first if conn->supports_extended_forward_open
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
 *     the route path from conn->conn_path.  This is needed once ForwardOpen provides
 *     a connected path and the unconnected path goes through a bridge.
 *   - In enip_connection_thread_entry, switch Phase C-G to use
 *     enip_cpf_build_connected + ENIP_CMD_CONNECTED_SEND (0x0070) after ForwardOpen
 *     succeeds, incrementing conn->cip_conn_seq_num each request.
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
#include <libplctag/protocols/enip/enip_mfg_ops.h>
#include <libplctag/protocols/enip/enip_stream.h>
#include <libplctag/protocols/enip/tag.h>
#include <platform.h>
#include <stdbool.h>
#include <string.h>
#include <utils/arena.h>
#include <utils/bytes.h>
#include <utils/debug.h>
#include <utils/enip_wait.h>
#include <utils/hashtable.h>
#include <utils/rc.h>
#include <utils/vector.h>

/* ============================================================================
 * Connection Loop (Stub - Phase Implementation)
 * ============================================================================ */


/* ============================================================================
 * Connection Loop (Stub - Phase Implementation)
 * ============================================================================ */

/* Phase 6: mostly correct; add cleanup for the condvar conn->wake and conn->io
 * when those fields are added.  Also add enip_root_symbol_cache_clear(conn). */
static void enip_connection_destructor(void *ptr) {
    enip_connection_t *conn = (enip_connection_t *)ptr;

    if(!conn) { return; }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: Destroying connection");

    conn->shutdown_requested = 1;

    /* Clear and destroy metadata cache (enip_metadata_cache_clear destroys hashtable internally) */
    if(conn->metadata_cache) { enip_metadata_cache_clear(conn); }

    if(conn->metadata_cache_mutex) { mutex_destroy(&conn->metadata_cache_mutex); }

    if(conn->socket) {
        socket_close(conn->socket);
        conn->socket = NULL;
    }
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

/* Find pending request by sender_context (for response matching)
 * Returns tag pointer if found, NULL if not found or error
 * The tag must be rc_inc'd by caller
 */
/* Phase 6: correct as-is — no changes needed to this function.
 * Currently never called (decode_response broadcasts instead).  Wire it in
 * as the first step of the Phase 6 receive path. */
enip_tag_t *enip_connection_find_pending_request(enip_connection_t *conn, uint64_t sender_context) {
    if(!conn || !conn->pending_requests) { return NULL; }

    mutex_lock(conn->pending_requests_mutex);

    /* Linear search through pending requests
     * TODO: Use hashtable for O(1) lookup if pending_requests grows large
     */
    for(int i = 0; i < vector_length(conn->pending_requests); i++) {
        enip_tag_t **tag_ptr = (enip_tag_t **)vector_get(conn->pending_requests, i);
        if(tag_ptr && *tag_ptr) {
            enip_tag_t *tag = *tag_ptr;

            /* Match by sender_context value stored in tag during encode_request */
            if((uint64_t)tag->transaction_id == sender_context) {
                /* Found match - increment refcount before releasing mutex */
                rc_inc(tag);

                mutex_unlock(conn->pending_requests_mutex);

                pdebug(DEBUG_MODULE_ENIP, DEBUG_SPEW, 0, "ENIP: Matched response sender_context=0x%016llx to tag",
                       (unsigned long long)sender_context);

                return tag;
            }
        }
    }

    mutex_unlock(conn->pending_requests_mutex);

    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: No pending request found for sender_context=0x%016llx",
           (unsigned long long)sender_context);

    return NULL;
}

/* Remove tag from pending_requests after response matched
 * Call after processing response data
 */
/* Phase 6: correct as-is — no changes needed.
 * Call after accept_chunk returns PLCTAG_STATUS_OK or an error to remove the tag
 * from the pending list and decrement its refcount. */
int enip_connection_remove_pending_request(enip_connection_t *conn, enip_tag_t *tag) {
    if(!conn || !conn->pending_requests || !tag) { return PLCTAG_ERR_NULL_PTR; }

    mutex_lock(conn->pending_requests_mutex);

    for(int i = 0; i < vector_length(conn->pending_requests); i++) {
        enip_tag_t **tag_ptr = (enip_tag_t **)vector_get(conn->pending_requests, i);
        if(tag_ptr && *tag_ptr == tag) {
            /* Remove from vector and decrement refcount */
            vector_remove(conn->pending_requests, i);
            rc_dec(tag);

            mutex_unlock(conn->pending_requests_mutex);

            pdebug(DEBUG_MODULE_ENIP, DEBUG_SPEW, 0, "ENIP: Removed tag from pending_requests");

            return PLCTAG_STATUS_OK;
        }
    }

    mutex_unlock(conn->pending_requests_mutex);

    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Tag not found in pending_requests");

    return PLCTAG_ERR_NOT_FOUND;
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

    /* Close any existing socket */
    if(conn->socket) {
        socket_close(conn->socket);
        conn->socket = NULL;
    }

    /* Create TCP socket */
    rc = socket_create(&conn->socket);
    if(rc != PLCTAG_STATUS_OK || !conn->socket) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to create TCP socket: %d", rc);
        return PLCTAG_ERR_OPEN;
    }

    /* Connect with restart state support */
    rc = socket_connect_wait(conn->socket, host, port, timeout_ms, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: TCP connect failed: %d", rc);
        socket_close(conn->socket);
        conn->socket = NULL;
        return rc;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: TCP connected");
    return PLCTAG_STATUS_OK;
}


/* Phase 1: REWRITE the receive path.
 * Current bug: reads a flat 24-byte buffer assuming the RegisterSession response
 * body is already included.  EIP over TCP is a stream: must read exactly 24 bytes
 * first (the EIP header), then parse header.length, then read that many more bytes.
 * Also: remove conn->sender_context_base = 1 (Phase 0 fix — use conn->sender_context).
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

    if(!conn || !conn->socket) { return PLCTAG_ERR_NULL_PTR; }

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
    rc = socket_write_wait(conn->socket, &request, 5000, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: RegisterSession send failed: %d", rc);
        return rc;
    }

    conn->messages_sent++;

    /* Receive response using stream framing (24-byte header + payload). */
    arena_reset(&conn->rx_arena);
    Bytes response;
    rc = enip_recv_frame(conn->socket, &conn->rx_arena, 5000, &io_state, &response);
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
    bytes_unpack(response, BYTES_LE, BYTES_SKIP(4), &conn->session_handle, &resp_status);

    if(resp_status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: RegisterSession response status: 0x%08x", resp_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    if(conn->session_handle == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: RegisterSession returned zero handle");
        return PLCTAG_ERR_REMOTE_ERR;
    }

    conn->session_established = 1;
    conn->sender_context = 1;

    /* Initialize metadata cache after session established */
    conn->metadata_cache = hashtable_create(128);
    if(!conn->metadata_cache) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to create metadata cache hashtable");
        /* Non-fatal: continue without cache */
    }

    rc = mutex_create(&conn->metadata_cache_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to create metadata cache mutex: %d", rc);
        if(conn->metadata_cache) {
            hashtable_destroy(conn->metadata_cache);
            conn->metadata_cache = NULL;
        }
        /* Non-fatal: continue without cache */
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Session established (handle=0x%08x)", conn->session_handle);

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

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Unregistering session (handle=0x%08x)", conn->session_handle);

    if(!conn || !conn->socket) {
        return PLCTAG_ERR_NULL_PTR;
    }

    /* Build UnregisterSession request using Bytes API. */
    arena_reset(&conn->tx_arena);
    request = bytes_pack(&conn->tx_arena, BYTES_LE,
                         (uint16_t)0x0066,              /* Command: UnregisterSession */
                         (uint16_t)0,                   /* Length: no payload */
                         conn->session_handle,          /* Session handle to unregister */
                         (uint32_t)0,                   /* Status */
                         (uint64_t)1,                   /* Sender context */
                         (uint32_t)0);                  /* Options (reserved) */

    if(bytes_is_null(request)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Arena OOM for UnregisterSession");
        return PLCTAG_ERR_NO_MEM;
    }

    /* Send request. */
    rc = socket_write_wait(conn->socket, &request, 5000, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: UnregisterSession send failed: %d", rc);
        return rc;
    }

    conn->messages_sent++;
    conn->session_established = 0;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: UnregisterSession sent");

    return PLCTAG_STATUS_OK;
}


/* Phase 2: REWRITTEN using layer helpers (enip_cpf, enip_eip, stream framing).
 * Fixes:
 *   (a) Uses enip_cpf_build_unconnected + enip_eip_build_request for framing
 *   (b) Uses enip_recv_frame for stream framing (24-byte header + length-based body read)
 *   (c) Uses enip_cip_parse_response which correctly handles 4-byte CIP header
 *       (reply_service + reserved + status + ext_sz)
 *   (d) Sets supports_extended_forward_open = 0 initially (Phase 3 will detect via FO_Ex attempt)
 *   (e) Calls enip_select_mfg_ops for device strategy selection */
static int32_t enip_connection_get_identity(enip_connection_t *conn) {
    /* GetAttributeAll identity: Service 0x01, Class 0x01 (Identity object), Instance 0x01
     * Response contains vendor_id, device_type, product_code, revision, status, serial, name
     * Sent via unconnected messaging (CPF SendRRData 0x006F) */
    Bytes cip_request, cpf_frame, eip_frame, response, cip_response;
    socket_wait_state_t io_state = {0};
    int32_t rc;
    uint16_t cip_status;
    uint8_t ext_status_size;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Fetching device identity");

    if(!conn || !conn->socket || !conn->session_established) {
        return PLCTAG_ERR_NULL_PTR;
    }

    /* Step 1: Build CIP GetAttributeAll request.
     * Service 0x01, path to Class 0x01 Instance 0x01 */
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

    /* Step 2: Wrap in CPF unconnected frame. */
    cpf_frame = enip_cpf_build_unconnected(&conn->tx_arena, cip_request);
    if(bytes_is_null(cpf_frame)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to build CPF frame");
        return PLCTAG_ERR_NO_MEM;
    }

    /* Step 3: Wrap in EIP frame and build complete request. */
    eip_frame = enip_eip_build_request(&conn->tx_arena, ENIP_CMD_UNCONNECTED_SEND,
                                       conn->session_handle, &conn->sender_context, cpf_frame);
    if(bytes_is_null(eip_frame)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to build EIP frame");
        return PLCTAG_ERR_NO_MEM;
    }

    /* Step 4: Send request. */
    rc = socket_write_wait(conn->socket, &eip_frame, 5000, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: GetIdentity send failed: %d", rc);
        return rc;
    }

    conn->messages_sent++;

    /* Step 5: Receive response using stream framing. */
    arena_reset(&conn->rx_arena);
    rc = enip_recv_frame(conn->socket, &conn->rx_arena, 5000, &io_state, &response);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: GetIdentity read failed: %d", rc);
        return rc;
    }

    conn->messages_received++;

    /* Step 6: Extract CPF payload from EIP response. */
    Bytes cpf_response = enip_eip_extract_cpf_payload(response);
    if(bytes_is_null(cpf_response)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to extract CPF from EIP response");
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Step 7: Extract CIP payload from CPF response (UDI item). */
    cip_response = enip_cpf_extract_udi_payload(cpf_response);
    if(bytes_is_null(cip_response)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to extract CIP from CPF response");
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Step 8: Parse CIP response header using enip_cip_parse_response.
     * This correctly handles: reply_service (1) + reserved (1) + status (1) + ext_status_size (1) + data
     * Returns the data portion after the 4-byte header. */
    Bytes cip_data = enip_cip_parse_response(cip_response, &cip_status, &ext_status_size, NULL);
    if(bytes_is_null(cip_data)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: CIP response parsing failed");
        return PLCTAG_ERR_REMOTE_ERR;
    }

    if(cip_status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: GetIdentity CIP status error: 0x%04x", cip_status);
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
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: No manufacturer strategy found for vendor=0x%04x", vendor_id);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Step 12: Set capability flags from identity.
     * Phase 3: supports_extended_forward_open will be set to 1 if FO_Ex succeeds. */
    conn->supports_0x0a = identity.supports_multiple_services;
    conn->supports_extended_forward_open = 0; /* Phase 3 will detect via FO_Ex attempt+fallback */
    conn->max_packet_buffer_size = 2000;      /* Phase 3 will negotiate actual value from FO response */

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "ENIP: Identity fetched (vendor=0x%04x, device=0x%04x, product=0x%04x, revision=%u.%u, mfg=%s)",
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
    if(!conn || !conn->socket || !conn->session_established) {
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: ForwardOpen attempt (service=0x%02x)", service_code);

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

    if(conn->cpu_slot >= 0) {
        conn_path[0] = 0x01;
        conn_path[1] = (uint8_t)conn->cpu_slot;
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
    if(conn->cip_conn_serial == 0) {
        conn->cip_conn_serial = 1;
    }
    uint32_t our_conn_id = (uint32_t)(uintptr_t)conn ^ (uint32_t)conn->cip_conn_serial;
    if(our_conn_id == 0) { our_conn_id = 0x12345678u; }

    /* Build FO CIP payload */
    Bytes fo_fixed = bytes_pack(&conn->tx_arena, BYTES_LE,
                                service_code,
                                (uint8_t)0x02,        /* path_size: 2 words to CM */
                                (uint8_t)0x20, (uint8_t)0x06, (uint8_t)0x24, (uint8_t)0x01,
                                (uint8_t)0x0A, (uint8_t)0x0E, /* secs_per_tick, timeout_ticks */
                                (uint32_t)0,          /* O->T conn ID (filled by target) */
                                (uint32_t)our_conn_id,/* T->O conn ID (ours) */
                                (uint16_t)conn->cip_conn_serial,
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
                                         conn->session_handle, &conn->sender_context, cpf);
    if(bytes_is_null(frame)) { return PLCTAG_ERR_NO_MEM; }

    socket_wait_state_t io_state = {0};
    int32_t rc = socket_write_wait(conn->socket, &frame, 5000, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: ForwardOpen send failed: %d", rc);
        return rc;
    }
    conn->messages_sent++;

    /* Receive response using stream framing */
    arena_reset(&conn->rx_arena);
    Bytes response;
    rc = enip_recv_frame(conn->socket, &conn->rx_arena, 5000, &io_state, &response);
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
               "ENIP: ForwardOpen (0x%02x) CIP status 0x%02x", service_code, cip_status);
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

    conn->cip_targ_conn_id    = o_to_t_conn_id;
    conn->cip_orig_conn_id    = t_to_o_conn_id;
    conn->cip_conn_seq_num    = 0;
    conn->cip_connection_open = true;

    /* Phase 3: Parse actual O->T RPI to compute CIP budget.
     * Formula: CIP budget = O->T RPI size - 6 bytes (CDI header + seq number)
     * For now, use negotiated size from params; Phase 3 could extract from API value. */
    uint32_t requested_size = (service_code == 0x5B) ? 4002 : 504;
    conn->max_packet_buffer_size = (requested_size > 6) ? (requested_size - 6) : 0;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "ENIP: ForwardOpen (0x%02x) OK: O->T=0x%08x T->O=0x%08x buffer=%zu",
           service_code, o_to_t_conn_id, t_to_o_conn_id, conn->max_packet_buffer_size);

    return PLCTAG_STATUS_OK;
}


/* Phase 3: Try ForwardOpen Extended (0x5B) first, fall back to standard FO (0x54).
 *
 * FO_Ex provides higher capacity (4002 bytes) via 32-bit connection parameters.
 * If the device doesn't support it (CIP status 0x08), retry with standard FO.
 * On success, stores connection IDs and negotiated buffer size in conn. */
static int32_t enip_connection_forward_open(enip_connection_t *conn) {
    if(!conn || !conn->socket || !conn->session_established) {
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
               "ENIP: ForwardOpen OK (O->T=0x%08x T->O=0x%08x buffer=%zu)",
               conn->cip_targ_conn_id, conn->cip_orig_conn_id, conn->max_packet_buffer_size);
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
 *   route_path(variable) (from conn->conn_path)
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
    if(!conn || !conn->socket || bytes_is_null(cip_payload) || !conn->session_established) {
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
                                         conn->session_handle, &conn->sender_context, cpf);
    if(bytes_is_null(frame)) {
        return bytes_null();
    }

    /* Send */
    socket_wait_state_t io_state = {0};
    int32_t rc = socket_write_wait(conn->socket, &frame, timeout_ms, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Unconnected send failed: %d", rc);
        return bytes_null();
    }
    conn->messages_sent++;

    /* Receive response */
    arena_reset(&conn->rx_arena);
    Bytes response;
    rc = enip_recv_frame(conn->socket, &conn->rx_arena, timeout_ms, &io_state, &response);
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
    if(!conn || !conn->socket || bytes_is_null(cip_payload) || !conn->cip_connection_open) {
        return bytes_null();
    }

    arena_reset(&conn->tx_arena);

    /* Increment sequence number for this request */
    conn->cip_conn_seq_num++;

    /* Wrap CIP in connected CPF */
    Bytes cpf = enip_cpf_build_connected(&conn->tx_arena, conn->cip_targ_conn_id,
                                         conn->cip_conn_seq_num, cip_payload);
    if(bytes_is_null(cpf)) {
        return bytes_null();
    }

    /* Wrap in EIP frame */
    Bytes frame = enip_eip_build_request(&conn->tx_arena, ENIP_CMD_CONNECTED_SEND,
                                         conn->session_handle, &conn->sender_context, cpf);
    if(bytes_is_null(frame)) {
        return bytes_null();
    }

    /* Send */
    socket_wait_state_t io_state = {0};
    int32_t rc = socket_write_wait(conn->socket, &frame, timeout_ms, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Connected send failed: %d", rc);
        return bytes_null();
    }
    conn->messages_sent++;

    /* Receive response */
    arena_reset(&conn->rx_arena);
    Bytes response;
    rc = enip_recv_frame(conn->socket, &conn->rx_arena, timeout_ms, &io_state, &response);
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
    if(!conn || !conn->socket || !conn->session_established || !conn->cip_connection_open) {
        return PLCTAG_STATUS_OK; /* Not open, nothing to close */
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0,
           "ENIP: ForwardClose starting (O->T=0x%08x T->O=0x%08x)",
           conn->cip_targ_conn_id, conn->cip_orig_conn_id);

    arena_reset(&conn->tx_arena);

    /* Build connection path (same as ForwardOpen) */
    uint8_t conn_path[8];
    uint8_t conn_path_words;

    if(conn->cpu_slot >= 0) {
        conn_path[0] = 0x01;
        conn_path[1] = (uint8_t)conn->cpu_slot;
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
                                (uint16_t)conn->cip_conn_serial,
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
                                         conn->session_handle, &conn->sender_context, cpf);
    if(bytes_is_null(frame)) { return PLCTAG_ERR_NO_MEM; }

    socket_wait_state_t io_state = {0};
    int32_t rc = socket_write_wait(conn->socket, &frame, 5000, &io_state);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: ForwardClose send failed: %d", rc);
        return rc;
    }
    conn->messages_sent++;

    /* Receive response */
    arena_reset(&conn->rx_arena);
    Bytes response;
    rc = enip_recv_frame(conn->socket, &conn->rx_arena, 5000, &io_state, &response);
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
                       "ENIP: ForwardClose CIP status 0x%02x (non-fatal)", cip_status);
            }
        }
    }

    conn->cip_connection_open = false;
    return PLCTAG_STATUS_OK;
}


/* Phase 6: REPLACE with proper correlation.
 * Currently stores context in conn->last_message_time_ms (wrong field) and does
 * nothing else with it.  Phase 6 replacement:
 *   uint64_t ctx = enip_connection_extract_sender_context(response.data);
 *   enip_tag_t *tag = enip_connection_find_pending_request(conn, ctx);
 *   if(!tag) { return PLCTAG_ERR_NOT_FOUND; }
 *   // extract CIP payload and call accept_chunk on that tag
 *   rc = conn->mfg_ops->accept_chunk(tag, cip_payload);
 *   if(rc != PLCTAG_ERR_PARTIAL) { enip_connection_remove_pending_request(conn, tag); }
 *   return rc;
 * This function can be renamed enip_connection_dispatch_response in Phase 6. */
static int enip_connection_match_response(enip_connection_t *conn, Bytes response) {
    /* Phase G: Match response to pending requests via sender_context correlation
     *
     * EIP header contains sender_context that identifies which tags the response
     * corresponds to. We use this to route response data to manufacturer decode
     * callbacks and then trigger tag callbacks.
     */

    if(!conn || !response.data) { return PLCTAG_ERR_NULL_PTR; }

    /* Parse EIP response header to extract sender_context */
    if(response.len < 24) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Response header too short (%zu bytes)", response.len);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Extract fields from EIP header (all little-endian):
     * Offset 0-1: command (should be 0x006F for SendRRData)
     * Offset 2-3: length
     * Offset 4-7: session_handle
     * Offset 8-11: status
     * Offset 12-19: sender_context (8 bytes)
     * Offset 20-23: options
     */
    uint16_t resp_command;
    uint16_t resp_length;
    uint32_t resp_session;
    uint32_t resp_status;
    uint64_t resp_context;
    uint32_t resp_options;

    Bytes remaining =
        bytes_unpack(response, BYTES_LE, &resp_command, &resp_length, &resp_session, &resp_status, &resp_context, &resp_options);

    if(bytes_is_null(remaining)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to parse EIP response header");
        return PLCTAG_ERR_REMOTE_ERR;
    }

    if(resp_status != 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Response EIP status: 0x%08x (error)", resp_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Store context in conn for Phase H to extract and route */
    conn->last_message_time_ms = time_ms();
    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: Response matched (context=0x%016llx, length=%u)",
           (unsigned long long)resp_context, resp_length);

    return PLCTAG_STATUS_OK;
}


/* Phase 6: DELETE this function.
 * The broadcast-to-all-REQUEST-tags approach is wrong: it feeds every tag's
 * decode callback the same response regardless of which tag the response is for.
 * Phase 6 replacement is enip_connection_match_response (above), which correlates
 * by sender_context and calls accept_chunk only on the matching tag. */
static int enip_connection_decode_response(enip_connection_t *conn, Bytes response) {
    /* Phase H: Decode responses and invoke tag callbacks
     *
     * Extract CIP payload from response using CPF headers, then route to
     * each tag's manufacturer decode callback for data extraction.
     * Handle chunking (needs_retry) and tag completion callbacks.
     */

    if(!conn || !conn->mfg_ops || !response.data) { return PLCTAG_ERR_NULL_PTR; }

    /* Parse EIP header first to get sender_context and length */
    if(response.len < 24) { return PLCTAG_ERR_REMOTE_ERR; }

    uint16_t resp_command;
    uint16_t resp_length;
    uint32_t resp_session;
    uint32_t resp_status;
    uint64_t resp_context;
    uint32_t resp_options;

    Bytes remaining =
        bytes_unpack(response, BYTES_LE, &resp_command, &resp_length, &resp_session, &resp_status, &resp_context, &resp_options);

    if(bytes_is_null(remaining) || resp_status != 0) { return PLCTAG_ERR_REMOTE_ERR; }

    /* Skip EIP header (24 bytes), parse CPF header:
     * interface_handle (4 bytes), router_timeout (2), item_count (2),
     * then items: type (2), length (2), data...
     */
    if(response.len < 24 + 8) { return PLCTAG_ERR_REMOTE_ERR; }

    uint32_t interface_handle;
    uint16_t router_timeout;
    uint16_t item_count;

    Bytes cpf_start = bytes_slice(response, 24, response.len - 24);
    if(bytes_is_null(cpf_start)) { return PLCTAG_ERR_REMOTE_ERR; }

    remaining = bytes_unpack(cpf_start, BYTES_LE, &interface_handle, &router_timeout, &item_count);
    if(bytes_is_null(remaining)) { return PLCTAG_ERR_REMOTE_ERR; }

    /* Find UDI (Unconnected Data Item, type 0x00B2) which contains CIP payload */
    size_t cpf_offset = 8; /* After CPF header */
    Bytes cip_payload = {NULL, 0};

    for(uint16_t i = 0; i < item_count && cpf_offset < cpf_start.len; i++) {
        if(cpf_offset + 4 > cpf_start.len) { break; }

        Bytes item_hdr = bytes_slice(cpf_start, cpf_offset, cpf_start.len - cpf_offset);
        if(bytes_is_null(item_hdr)) { break; }

        uint16_t item_type;
        uint16_t item_length;
        Bytes item_data = bytes_unpack(item_hdr, BYTES_LE, &item_type, &item_length);

        if(bytes_is_null(item_data)) { break; }

        if(item_type == 0x00B2) {
            /* Found UDI - extract CIP payload */
            cip_payload = bytes_slice(item_data, 0, item_length);
            break;
        }

        cpf_offset += 4 + item_length;
    }

    if(bytes_is_null(cip_payload)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: No UDI found in CPF response");
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* Route CIP payload to each tag that was in the request batch
     * Identify tags by checking which ones have op_state == ENIP_TAG_OP_REQUEST
     */
    int tags_processed = 0;

    critical_block(conn->active_tags_mutex) {
        int tag_count = vector_length((vector_p)conn->active_tags);

        for(int i = 0; i < tag_count; i++) {
            enip_tag_t *tag = (enip_tag_t *)vector_get((vector_p)conn->active_tags, i);
            if(!tag) { continue; }

            /* Only process tags that are waiting for response */
            if(tag->op_state != ENIP_TAG_OP_REQUEST) { continue; }

            /* Call manufacturer decode_response callback */
            enip_chunk_result_t chunk_result = {0};
            int rc_decode = conn->mfg_ops->decode_response(tag, conn, cip_payload, (uint32_t)resp_context, &chunk_result);

            if(rc_decode == PLCTAG_STATUS_OK) {
                /* Response complete - invoke tag callback */
                if(tag->callback) {
                    tag->op_state = ENIP_TAG_OP_COMPLETE;
                    tag->callback(tag->tag_id, PLCTAG_EVENT_READ_COMPLETED, rc_decode, tag->userdata);
                }
                tags_processed++;
            } else if(rc_decode == PLCTAG_ERR_PARTIAL || chunk_result.needs_retry) {
                /* Partial response - tag needs more data */
                tag->op_state = ENIP_TAG_OP_REQUEST; /* Re-queue for next cycle */
                pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: Tag needs more data (partial response)");
                tags_processed++;
            } else {
                /* Decode error */
                tag->op_state = ENIP_TAG_OP_ERROR;
                if(tag->callback) { tag->callback(tag->tag_id, PLCTAG_EVENT_READ_COMPLETED, rc_decode, tag->userdata); }
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Decode failed for tag: %d", rc_decode);
                tags_processed++;
            }
        }
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Processed %d tags from response", tags_processed);

    return PLCTAG_STATUS_OK;
}


/* Phase 6: REPLACE this function entirely.
 * Current problems:
 *   (a) Allocates cip_payloads vector but never puts anything in it and destroys it
 *       immediately — remove it (Phase 0 compile cleanup).
 *   (b) Uses conn->sender_context_base (undeclared) — Phase 0 fix: use
 *       conn->sender_context.  Before calling enip_eip_build_request, copy the
 *       current counter value into each tag's transaction_id for later correlation.
 *   (c) Hand-rolls EIP/CPF/NAI/UDI headers instead of calling the layer helpers.
 *   (d) The multi-service 0x0A wrapper has a TODO but is never built.
 *
 * Phase 6 replacement: pick one tag from active_tags (front of sorted queue),
 * call encode_chunk to get the CIP bytes, wrap in CPF+EIP (using the layer
 * helpers), and return.  Multi-service batching (0x0A) is Phase 5. */
static int enip_connection_build_requests(enip_connection_t *conn, Bytes *out_request) {
    /* Phase C: Build outgoing requests from active_tags vector
     *
     * Iterate active_tags, call manufacturer encode functions, aggregate requests.
     * Enforces budget (max_packet_buffer_size).
     *
     * Algorithm:
     * 1. Scan active_tags for pending work (read_in_flight, write_in_flight, tag_is_dirty)
     * 2. For each tag, estimate size using mfg_ops->estimate_request_size()
     * 3. If total estimated fits in budget, encode using mfg_ops->encode_request()
     * 4. Accumulate CIP payloads into arena
     * 5. Build EIP header + CPF + CIP aggregate
     * 6. Return full request or PLCTAG_ERR_NO_DATA if no work
     */

    if(!conn || !conn->active_tags || !out_request) { return PLCTAG_ERR_NULL_PTR; }

    if(!conn->mfg_ops) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: No manufacturer strategy available");
        return PLCTAG_ERR_NO_DATA;
    }

    /* Lock active_tags to safely iterate and encode */
    int tags_encoded = 0;
    size_t estimated_request_size = 0;
    size_t estimated_response_size = 0;

    critical_block(conn->active_tags_mutex) {
        int tag_count = vector_length((vector_p)conn->active_tags);
        if(tag_count <= 0) { break; }

        /* Reset arena for this request cycle */
        arena_reset(&conn->tx_arena);

        /* First pass: scan for tags with pending work and estimate total size */
        int tags_with_work = 0;

        for(int i = 0; i < tag_count; i++) {
            plc_tag_p tag = (plc_tag_p)vector_get((vector_p)conn->active_tags, i);
            if(tag && (tag->read_in_flight || tag->write_in_flight || tag->tag_is_dirty)) { tags_with_work++; }
        }

        if(tags_with_work == 0) { break; }

        /* Rough budget check (assume ~64 bytes per tag) before full encoding */
        size_t overhead = 50; /* EIP header + CPF wrapper */
        size_t rough_estimate = ((size_t)tags_with_work * 64U) + overhead;
        if(rough_estimate > (size_t)conn->max_packet_buffer_size) {
            /* Too many tags, will handle in next cycle */
            pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: Estimated %zu bytes exceeds budget %u, splitting batch",
                   rough_estimate, conn->max_packet_buffer_size);
            break;
        }

        /* Second pass: encode tags that fit in budget */
        vector_p cip_payloads = vector_create(10, 5);
        if(!cip_payloads) { break; }

        for(int i = 0; i < tag_count && tags_encoded < 10; i++) { /* Limit to 10 tags per batch */
            plc_tag_p tag = (plc_tag_p)vector_get((vector_p)conn->active_tags, i);
            if(!tag || (!tag->read_in_flight && !tag->write_in_flight && !tag->tag_is_dirty)) { continue; }

            /* Estimate request size for this tag */
            enip_req_desc_t req_desc = {0};
            req_desc.sequence_id = conn->sender_context + (uint32_t)tags_encoded;
            req_desc.is_write = (tag->write_in_flight || tag->tag_is_dirty) ? 1 : 0;
            req_desc.multi_request_index = (uint16_t)tags_encoded;

            size_t tag_req_budget = (size_t)conn->max_packet_buffer_size - overhead - estimated_request_size;
            size_t tag_resp_budget = (size_t)conn->max_packet_buffer_size - estimated_response_size;

            int rc_est = conn->mfg_ops->estimate_request_size((enip_tag_t *)tag, conn, &conn->tx_arena, tag_req_budget,
                                                              tag_resp_budget, &req_desc);
            if(rc_est != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: Tag %d estimation failed: %d, stopping batch", i, rc_est);
                break;
            }

            /* Check if this tag would exceed budget */
            if(estimated_request_size + req_desc.request_size + overhead > (size_t)conn->max_packet_buffer_size
               || estimated_response_size + req_desc.estimated_response_size > (size_t)conn->max_packet_buffer_size) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: Tag %d would exceed budget, stopping batch", i);
                break;
            }

            /* Encode the CIP payload */
            int rc_enc = conn->mfg_ops->encode_request((enip_tag_t *)tag, conn, &conn->tx_arena, &req_desc);
            if(rc_enc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: Tag %d encoding failed: %d, skipping", i, rc_enc);
                continue;
            }

            /* Record payload offset and size in vector for later assembly */
            estimated_request_size += req_desc.request_size;
            estimated_response_size += req_desc.estimated_response_size;
            tags_encoded++;

            /* Clear pending work flags (will be re-set if chunking or error) */
            tag->read_in_flight = 0;
            tag->write_in_flight = 0;
            tag->tag_is_dirty = 0;

            pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: Encoded tag %d (%zu bytes request, %zu bytes response)", i,
                   req_desc.request_size, req_desc.estimated_response_size);
        }

        vector_destroy(cip_payloads);
    }

    if(tags_encoded == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: No tags successfully encoded");
        return PLCTAG_ERR_NO_DATA;
    }

    /* Build full EIP request: header + CPF + CIP aggregate
     * The arena now contains all encoded CIP payloads written sequentially.
     * We'll add wrapper headers and then use bytes_concat to assemble the final request.
     */
    size_t cip_size = conn->tx_arena.length;
    if(cip_size == 0) { return PLCTAG_ERR_NO_MEM; }

    /* Create a Bytes reference to the CIP data already in arena */
    Bytes cip_data = {conn->tx_arena.buffer, cip_size};

    /* Build EIP header */
    uint16_t payload_len = (uint16_t)((8 + 4 + cip_size) & 0xFFFF);
    Bytes eip_hdr = bytes_pack(&conn->tx_arena, BYTES_LE, (uint16_t)0x006F, /* SendRRData command */
                               payload_len,                                 /* Total payload length */
                               (uint32_t)conn->session_handle, (uint32_t)0, /* Status */
                               (uint64_t)conn->sender_context);             /* Sender context */

    if(bytes_is_null(eip_hdr)) { return PLCTAG_ERR_NO_MEM; }

    /* Build CPF header */
    Bytes cpf_hdr = bytes_pack(&conn->tx_arena, BYTES_LE, (uint32_t)0, /* interface_handle */
                               (uint16_t)0,                            /* router_timeout */
                               (uint16_t)2);                           /* item_count: NAI + UDI */

    if(bytes_is_null(cpf_hdr)) { return PLCTAG_ERR_NO_MEM; }

    /* Build NAI item (Null Address Item: type=0x0000, length=0) */
    Bytes nai = bytes_pack(&conn->tx_arena, BYTES_LE, (uint16_t)0x0000, (uint16_t)0x0000);

    if(bytes_is_null(nai)) { return PLCTAG_ERR_NO_MEM; }

    /* Build UDI item (Unconnected Data Item: type=0x00B2, length=cip_size) */
    Bytes udi = bytes_pack(&conn->tx_arena, BYTES_LE, (uint16_t)0x00B2, (uint16_t)(cip_size & 0xFFFF));

    if(bytes_is_null(udi)) { return PLCTAG_ERR_NO_MEM; }

    /* Allocate a new buffer that assembles headers + CIP data in correct order
     * NOTE: This uses a separate allocation, which is intentional to ensure proper
     * framing (EIP + CPF + NAI + UDI headers followed by CIP payload).
     * For now, we'll build a simple aggregate without multi-service wrapper.
     * TODO: Multi-service 0x0A support for supports_0x0a
     */
    Bytes full_request = bytes_concat(&conn->tx_arena, eip_hdr, cpf_hdr, nai, udi, cip_data);
    if(bytes_is_null(full_request)) { return PLCTAG_ERR_NO_MEM; }

    /* Return the request */
    *out_request = full_request;
    conn->sender_context += (uint32_t)tags_encoded;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Built request from %d tags (%zu bytes total request, %zu bytes response)",
           tags_encoded, estimated_request_size, estimated_response_size);

    return PLCTAG_STATUS_OK;
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

    if(!conn || !conn->socket || !conn->session_established) { return PLCTAG_ERR_NULL_PTR; }

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


/* Phase 6: REWRITE this function as the pseudo-blocking event loop (plan §1.5.4).
 * Current problems:
 *   (a) host/port hardcoded "192.168.1.100:44818" — Phase 1 fix: use conn->host/port.
 *   (b) sleep_ms(10) busy-wait — Phase 6 fix: replace with cond_wait on conn->wake.
 *   (c) No lazy connect logic — Phase 6: only connect when active_tags is non-empty.
 *   (d) Phase B calls enip_connection_phase1_metadata unconditionally; move to
 *       Phase 7 bootstrap (only after identity confirms AB device).
 *   (e) Phase C-G: replace build_requests + send + recv with the per-tag
 *       encode_chunk -> send -> recv -> accept_chunk loop; repeat on PLCTAG_ERR_PARTIAL.
 *   (f) Phase I idle disconnect: 60000ms literal should use a named constant (plan §6).
 *
 * Phase 1: minimal fix — remove hardcoded host/port; use conn->host, conn->port.
 * Phase 3: switch to connected messaging (enip_cpf_build_connected + 0x0070) when
 *          conn->cip_connection_open is true.
 * Phase 6: full rewrite per plan §1.5.4. */
void *enip_connection_thread_entry(void *arg) {
    enip_connection_t *conn = (enip_connection_t *)arg;
    int rc;

    if(!conn) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: NULL connection passed to thread");
        return NULL;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Connection thread started");

    while(!conn->shutdown_requested) {
        /* Phase A: Terminate check */
        if(conn->shutdown_requested) { break; }

        /* Phase B: Session bootstrap if needed */
        if(!conn->session_established) {
            /* Phase 3: Use actual host/port from connection. Phase 6 will populate from attributes. */
            const char *host = conn->host[0] ? conn->host : "192.168.1.100";
            int port = conn->port ? conn->port : 44818;

            pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: Bootstrap sequence starting (host=%s port=%d)", host, port);

            /* TCP connect */
            rc = enip_connection_tcp_connect(conn, host, port, 5000);
            if(rc != PLCTAG_STATUS_OK) {
                conn->connection_attempt_count++;
                int backoff = (1 << (conn->connection_attempt_count < 13 ? conn->connection_attempt_count : 13));
                conn->retry_deadline_ms = time_ms() + backoff;
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: TCP connect failed, retry in %dms", backoff);
                sleep_ms(10);
                continue;
            }

            /* RegisterSession */
            rc = enip_connection_register_session(conn);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: RegisterSession failed, reconnecting");
                socket_close(conn->socket);
                conn->socket = NULL;
                conn->session_established = 0;
                conn->cip_connection_open = 0;
                continue;
            }

            /* Get Identity */
            rc = enip_connection_get_identity(conn);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: GetIdentity failed, reconnecting");
                enip_connection_unregister_session(conn);
                socket_close(conn->socket);
                conn->socket = NULL;
                conn->session_established = 0;
                conn->cip_connection_open = 0;
                continue;
            }

            /* Phase 3: Forward Open (negotiates connected messaging) */
            rc = enip_connection_forward_open(conn);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: ForwardOpen failed, reconnecting");
                enip_connection_unregister_session(conn);
                socket_close(conn->socket);
                conn->socket = NULL;
                conn->session_established = 0;
                conn->cip_connection_open = 0;
                continue;
            }

            /* Phase-1 Metadata */
            rc = enip_connection_phase1_metadata(conn);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Phase-1 metadata failed, reconnecting");
                enip_connection_forward_close(conn);
                enip_connection_unregister_session(conn);
                socket_close(conn->socket);
                conn->socket = NULL;
                conn->session_established = 0;
                conn->cip_connection_open = 0;
                continue;
            }

            conn->connection_attempt_count = 0;
            pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Connection UP");
        }

        /* Phase I: Idle disconnect - check for inactivity timeout (60 seconds) */
        int64_t now_ms = time_ms();
        if(conn->session_established && (now_ms - conn->last_message_time_ms) > 60000) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Idle disconnect after 60s inactivity");
            if(conn->cip_connection_open) {
                enip_connection_forward_close(conn);
            }
            enip_connection_unregister_session(conn);
            socket_close(conn->socket);
            conn->socket = NULL;
            conn->session_established = 0;
            conn->cip_connection_open = 0;
            conn->connection_attempt_count = 0;
        }

        /* Phase C-G: Request/response cycle */
        if(conn->session_established) {
            Bytes request = {NULL, 0};

            /* Phase C: Build requests from active_tags */
            int rc_build = enip_connection_build_requests(conn, &request);

            if(rc_build == PLCTAG_STATUS_OK && request.data) {
                socket_wait_state_t io_state = {0};
                int rc_send;

                /* Phase D: Send request */
                rc_send = socket_write_wait(conn->socket, &request, 5000, &io_state);
                if(rc_send != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Request send failed: %d, reconnecting", rc_send);
                    if(conn->cip_connection_open) {
                        enip_connection_forward_close(conn);
                    }
                    enip_connection_unregister_session(conn);
                    socket_close(conn->socket);
                    conn->socket = NULL;
                    conn->session_established = 0;
                    conn->cip_connection_open = 0;
                } else {
                    conn->messages_sent++;

                    /* Phase E: Wait for I/O (integrated into socket_read_wait) */
                    /* Phase F: Receive response */
                    arena_reset(&conn->rx_arena);
                    Bytes response = bytes_alloc(&conn->rx_arena, conn->max_packet_buffer_size);
                    if(!bytes_is_null(response)) {
                        int rc_recv = socket_read_wait(conn->socket, &response, 5000, &io_state);
                        if(rc_recv == PLCTAG_STATUS_OK) {
                            conn->messages_received++;

                            /* Phase G: Match response to pending requests via sender_context */
                            int rc_match = enip_connection_match_response(conn, response);
                            if(rc_match != PLCTAG_STATUS_OK) {
                                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Response match failed: %d", rc_match);
                            } else {
                                /* Phase H: Decode responses and invoke tag callbacks */
                                int rc_decode = enip_connection_decode_response(conn, response);
                                if(rc_decode != PLCTAG_STATUS_OK) {
                                    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "ENIP: Decode failed: %d", rc_decode);
                                }
                            }
                        } else {
                            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Response receive failed: %d, reconnecting", rc_recv);
                            if(conn->cip_connection_open) {
                                enip_connection_forward_close(conn);
                            }
                            enip_connection_unregister_session(conn);
                            socket_close(conn->socket);
                            conn->socket = NULL;
                            conn->session_established = 0;
                            conn->cip_connection_open = 0;
                        }
                    }
                }
            } else if(rc_build != PLCTAG_ERR_NO_DATA) {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Request build failed: %d", rc_build);
            }
        }

        sleep_ms(10);
    }

    /* Phase J: Shutdown — close connection and session cleanly */
    if(conn->session_established) {
        if(conn->cip_connection_open) {
            enip_connection_forward_close(conn);
        }
        enip_connection_unregister_session(conn);
    }

    if(conn->socket) {
        socket_close(conn->socket);
        conn->socket = NULL;
    }

    conn->session_established = 0;
    conn->cip_connection_open = 0;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Connection thread exited");
    return NULL;
}


/* Phase 6: ADD to this function after arenas:
 *   - Initialize conn->active_tags (vector) and conn->active_tags_mutex.
 *   - Initialize conn->pending_requests (vector) and conn->pending_requests_mutex.
 *   - Initialize conn->root_symbol_cache and conn->root_symbol_mutex.
 *   - Initialize conn->metadata_cache and conn->metadata_cache_mutex
 *     (currently created in register_session — move here).
 *   - Initialize the conn->wake condvar (Phase 6).
 *   - Set conn->state = ENIP_CONN_DISCONNECTED.
 *   - Copy host/port/slot from caller-supplied attributes.
 * Phase 1: at minimum, add host/port storage so thread_entry can use them. */
enip_connection_t *enip_connection_create(void) {
    enip_connection_t *conn = (enip_connection_t *)rc_alloc(sizeof(enip_connection_t), enip_connection_destructor);
    int rc;

    if(!conn) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "ENIP: Failed to allocate connection");
        return NULL;
    }

    /* Initialize arena (32KB for request/response buffers) */
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

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Connection created");
    return conn;
}


/* Placeholder symbol to ensure file links */
int enip_conn_placeholder_symbol = 0;
