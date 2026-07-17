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
 * eip_server.c -- the EIP-specific plug-in for net/tcp_server.c (item 3.4/3.5,
 * ENIP-UPDATES-PLAN.md). Everything protocol-agnostic (listener, accept loop,
 * registry, blocking recv/send) now lives in net/tcp_server.c; this file
 * keeps only the two things that are actually EIP: how to read one framed
 * request off the wire, and how to turn it into a response via eip_dispatch.
 */

#include <stddef.h>
#include <stdint.h>

#include <libplctag/lib/libplctag.h>
#include "platform.h"
#include "utils/debug.h"
#include "utils/arena.h"
#include "utils/bytes.h"
#include "net/tcp_server.h"
#include "device_sim.h"
#include <libplctag/protocols/enip/common/eip.h>
#include "server.h"

#define EIP_HEADER_SIZE ((size_t)24)
#define CLIENT_ARENA_SIZE ((size_t)65536)
#define ACCEPT_TIMEOUT_MS ((int32_t)1000)
#define IO_TIMEOUT_MS     ((int32_t)30000)
#define CONN_STACK_SIZE   ((int)131072)

/* ============================================================================
 * Per-connection session: device_t (shared, read-mostly) + the per-connection
 * eip_session_t the wire codec threads through every request on this socket.
 * ============================================================================ */

typedef struct {
    device_t      *device;
    eip_session_t  sess;
} eip_conn_session_t;


static void eip_session_init(void *session, void *shared_ctx, sock_p sock) {
    eip_conn_session_t *s = (eip_conn_session_t *)session;
    s->device = (device_t *)shared_ctx;

    eip_session_set_unconnected_sizes(&s->sess, s->device->server_to_client_max_packet);

    /* Local address of this accepted socket -- the IP the client reached us
     * on, reported back in any TCP List Identity reply. */
    s->sess.local_ipv4 = s->device->local_ipv4;
    socket_local_ipv4(sock, &s->sess.local_ipv4);

    if(s->device->connect_cb) {
        s->device->connect_cb(s->device->sim, s->device->connect_user_data);
    }
}


static void eip_session_destroy(void *session) {
    eip_conn_session_t *s = (eip_conn_session_t *)session;
    if(s->device->disconnect_cb) {
        s->device->disconnect_cb(s->device->sim, s->device->disconnect_user_data);
    }
}


/* Read the 24-byte EIP header, then its length-prefixed payload, into one
 * contiguous arena buffer: [0:24) header, [24:24+payload_len) payload. */
static Bytes eip_read_request(Arena *a, sock_p sock, atomic_bool *terminate, void *session) {
    eip_conn_session_t *s = (eip_conn_session_t *)session;

    Bytes hdr = bytes_alloc(a, EIP_HEADER_SIZE);
    if(bytes_is_null(hdr)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_ERROR, 0, "Arena OOM allocating EIP header buffer.");
        return bytes_null();
    }

    int32_t rc = tcp_recv_exact(sock, terminate, hdr.data, (int32_t)hdr.len, IO_TIMEOUT_MS);
    if(rc != PLCTAG_STATUS_OK) {
        if(rc != PLCTAG_ERR_ABORT && rc != PLCTAG_ERR_BAD_CONNECTION) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "EIP header recv error %d.", rc);
        }
        return bytes_null();
    }

    uint16_t payload_len = 0;
    if(bytes_is_null(bytes_unpack(hdr, BYTES_LE, BYTES_SKIP(2), &payload_len))) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Failed to unpack EIP payload length.");
        return bytes_null();
    }

    if(s->sess.max_eip_packet_size > 0 && (size_t)payload_len > s->sess.max_eip_packet_size) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "EIP payload_len=%u exceeds negotiated max %zu -- closing.",
               (unsigned)payload_len, s->sess.max_eip_packet_size);
        return bytes_null();
    }

    if(payload_len == 0) { return hdr; }

    if((size_t)payload_len > arena_remaining(a)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "EIP payload_len=%u exceeds arena space -- closing.", (unsigned)payload_len);
        return bytes_null();
    }

    Bytes payload = bytes_alloc(a, payload_len);
    if(bytes_is_null(payload)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_ERROR, 0, "Arena OOM allocating payload buffer.");
        return bytes_null();
    }
    rc = tcp_recv_exact(sock, terminate, payload.data, (int32_t)payload.len, IO_TIMEOUT_MS);
    if(rc != PLCTAG_STATUS_OK) {
        if(rc != PLCTAG_ERR_ABORT && rc != PLCTAG_ERR_BAD_CONNECTION) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "EIP payload recv error %d.", rc);
        }
        return bytes_null();
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "EIP request: payload=%u bytes.", (unsigned)payload_len);

    /* header and payload are sequential arena allocations -- contiguous by
     * construction (bump allocator, no intervening alloc) -- so the pair can
     * be handed back as a single Bytes spanning both. */
    return (Bytes){hdr.data, hdr.len + payload.len};
}


static Bytes eip_dispatch_request(Arena *a, Bytes request, void *session) {
    eip_conn_session_t *s = (eip_conn_session_t *)session;
    Bytes hdr     = bytes_slice(request, 0, EIP_HEADER_SIZE);
    Bytes payload = bytes_slice(request, EIP_HEADER_SIZE, request.len - EIP_HEADER_SIZE);

    Bytes resp = eip_dispatch(a, hdr, payload, &s->sess, s->device);

    if(s->device->response_delay_ms > 0) { sleep_ms((int)s->device->response_delay_ms); }

    return resp;
}


static const tcp_conn_ops_t eip_server_ops = {
    .read_request    = eip_read_request,
    .dispatch        = eip_dispatch_request,
    .session_size    = sizeof(eip_conn_session_t),
    .session_init    = eip_session_init,
    .session_destroy = eip_session_destroy,
};


extern int32_t eip_server_start_listener(device_t *device, tcp_registry_t *registry, thread_p *out_thread) {
    tcp_server_config_t *cfg = (tcp_server_config_t *)mem_alloc((int)sizeof(tcp_server_config_t));
    if(!cfg) { return PLCTAG_ERR_NO_MEM; }

    cfg->bind_addr         = device->bind_addr;
    cfg->port              = device->port;
    cfg->registry          = registry;
    cfg->terminate         = &device->terminate;
    cfg->ops               = &eip_server_ops;
    cfg->shared_ctx        = device;
    cfg->conn_arena_size   = CLIENT_ARENA_SIZE;
    cfg->io_timeout_ms     = IO_TIMEOUT_MS;
    cfg->accept_timeout_ms = ACCEPT_TIMEOUT_MS;
    cfg->conn_stack_size   = CONN_STACK_SIZE;

    if(thread_create(out_thread, tcp_server_listener, CONN_STACK_SIZE, cfg) != PLCTAG_STATUS_OK) {
        mem_free(cfg);
        return PLCTAG_ERR_THREAD_CREATE;
    }
    return PLCTAG_STATUS_OK;
}
