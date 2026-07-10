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

#include <stddef.h>
#include <stdint.h>

#include <libplctag/lib/libplctag.h>
#include "platform.h"
#include "utils/debug.h"
#include "utils/arena.h"
#include "utils/bytes.h"
#include "device_sim.h"
#include <libplctag/protocols/enip/common/eip.h>
#include "server.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

#define DEBUG_MOD   DEBUG_MODULE_UTILS
#define EIP_HEADER_SIZE ((size_t)24)
#define CLIENT_ARENA_SIZE ((size_t)65536)
#define ACCEPT_TIMEOUT_MS ((int32_t)1000)
#define IO_TIMEOUT_MS     ((int32_t)30000)
#define CONN_STACK_SIZE   ((int)131072)

/* ============================================================================
 * Per-connection context (heap-allocated by listener, freed by conn_handler)
 * ============================================================================ */

typedef struct {
    device_t   *device;
    registry_t *registry;
    sock_p      sock;
} conn_ctx_t;

/* ============================================================================
 * Registry
 * ============================================================================ */

extern registry_t *registry_create(void) {
    registry_t *reg = (registry_t *)mem_alloc((int)sizeof(registry_t));
    if(!reg) { return NULL; }
    mem_set(reg, 0, (int)sizeof(registry_t));
    if(mutex_create(&reg->mutex) != PLCTAG_STATUS_OK) {
        mem_free(reg);
        return NULL;
    }
    return reg;
}


extern void registry_destroy(registry_t *reg) {
    if(!reg) { return; }
    mutex_destroy(&reg->mutex);
    mem_free(reg);
}


extern void registry_add(registry_t *reg, sock_p sock) {
    if(!reg || !sock) { return; }
    critical_block(reg->mutex) {
        for(int32_t i = 0; i < REGISTRY_MAX; i++) {
            if(!reg->socks[i]) {
                reg->socks[i] = sock;
                break;
            }
        }
    }
}


extern void registry_remove(registry_t *reg, sock_p sock) {
    if(!reg || !sock) { return; }
    critical_block(reg->mutex) {
        for(int32_t i = 0; i < REGISTRY_MAX; i++) {
            if(reg->socks[i] == sock) {
                reg->socks[i] = NULL;
                break;
            }
        }
    }
}


extern void registry_wake_all(registry_t *reg) {
    if(!reg) { return; }
    critical_block(reg->mutex) {
        for(int32_t i = 0; i < REGISTRY_MAX; i++) {
            if(reg->socks[i]) { socket_wake(reg->socks[i]); }
        }
    }
}

/* ============================================================================
 * recv_exact / send_all — blocking helpers using socket_wait_event for
 * wake-pipe integration.  Return PLCTAG_STATUS_OK or a negative error code.
 * ============================================================================ */

static int32_t recv_exact(sock_p sock, device_t *dev, uint8_t *buf, int32_t len) {
    int32_t total = 0;
    while(total < len) {
        int32_t events = socket_wait_event(sock,
            SOCK_EVENT_CAN_READ | SOCK_EVENT_WAKE_UP | SOCK_EVENT_DISCONNECT,
            IO_TIMEOUT_MS);

        if(events & SOCK_EVENT_WAKE_UP)    { return PLCTAG_ERR_ABORT; }
        if(events & SOCK_EVENT_DISCONNECT) { return PLCTAG_ERR_BAD_CONNECTION; }
        if(events & SOCK_EVENT_ERROR)      { return PLCTAG_ERR_READ; }
        if(events & SOCK_EVENT_TIMEOUT) {
            if(atomic_get_bool(&dev->terminate)) { return PLCTAG_ERR_ABORT; }
            continue;
        }
        if(!(events & SOCK_EVENT_CAN_READ)) { return PLCTAG_ERR_BAD_STATUS; }

        int32_t rc = socket_read(sock, buf + total, (int)(len - total), 100);
        if(rc == PLCTAG_ERR_TIMEOUT) { continue; }
        if(rc < 0)  { return rc; }
        if(rc == 0) { return PLCTAG_ERR_BAD_CONNECTION; }
        total += rc;
    }
    return PLCTAG_STATUS_OK;
}


static int32_t send_all(sock_p sock, device_t *dev, uint8_t *buf, int32_t len) {
    int32_t total = 0;
    while(total < len) {
        int32_t events = socket_wait_event(sock,
            SOCK_EVENT_CAN_WRITE | SOCK_EVENT_WAKE_UP | SOCK_EVENT_DISCONNECT,
            IO_TIMEOUT_MS);

        if(events & SOCK_EVENT_WAKE_UP)    { return PLCTAG_ERR_ABORT; }
        if(events & SOCK_EVENT_DISCONNECT) { return PLCTAG_ERR_BAD_CONNECTION; }
        if(events & SOCK_EVENT_ERROR)      { return PLCTAG_ERR_WRITE; }
        if(events & SOCK_EVENT_TIMEOUT) {
            if(atomic_get_bool(&dev->terminate)) { return PLCTAG_ERR_ABORT; }
            continue;
        }
        if(!(events & SOCK_EVENT_CAN_WRITE)) { return PLCTAG_ERR_BAD_STATUS; }

        int32_t rc = socket_write(sock, buf + total, (int)(len - total), 100);
        if(rc == PLCTAG_ERR_TIMEOUT) { continue; }
        if(rc < 0)  { return rc; }
        total += rc;
    }
    return PLCTAG_STATUS_OK;
}

/* ============================================================================
 * Per-connection thread
 * ============================================================================ */

static THREAD_FUNC(conn_handler) {
    conn_ctx_t *c = (conn_ctx_t *)arg;
    Arena arena;

    thread_detach();

    if(arena_init(&arena, CLIENT_ARENA_SIZE) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MOD, DEBUG_ERROR, 0, "Failed to allocate client arena.");
        registry_remove(c->registry, c->sock);
        socket_close(c->sock);
        socket_destroy(&c->sock);
        mem_free(c);
        THREAD_RETURN(0);
    }

    eip_session_t sess;
    mem_set(&sess, 0, (int)sizeof(sess));
    eip_session_set_unconnected_sizes(&sess, c->device->server_to_client_max_packet);

    /* Local address of this accepted socket — the IP the client reached us on,
     * reported back in any TCP List Identity reply. */
    sess.local_ipv4 = c->device->local_ipv4;
    socket_local_ipv4(c->sock, &sess.local_ipv4);

    pdebug(DEBUG_MOD, DEBUG_DETAIL, 0, "Connection handler started.");

    if(c->device->connect_cb) {
        c->device->connect_cb(c->device->sim, c->device->connect_user_data);
    }

    while(!atomic_get_bool(&c->device->terminate)) {
        arena_reset(&arena);

        /* Phase 1: read 24-byte EIP header. */
        Bytes hdr = bytes_alloc(&arena, EIP_HEADER_SIZE);
        if(bytes_is_null(hdr)) {
            pdebug(DEBUG_MOD, DEBUG_ERROR, 0, "Arena OOM allocating EIP header buffer.");
            break;
        }

        int32_t rc = recv_exact(c->sock, c->device, hdr.data, (int32_t)hdr.len);
        if(rc != PLCTAG_STATUS_OK) {
            if(rc != PLCTAG_ERR_ABORT && rc != PLCTAG_ERR_BAD_CONNECTION) {
                pdebug(DEBUG_MOD, DEBUG_WARN, 0, "EIP header recv error %d.", rc);
            }
            break;
        }

        /* Phase 2: extract and validate payload length. */
        uint16_t payload_len = 0;
        if(bytes_is_null(bytes_unpack(hdr, BYTES_LE, BYTES_SKIP(2), &payload_len))) {
            pdebug(DEBUG_MOD, DEBUG_WARN, 0, "Failed to unpack EIP payload length.");
            break;
        }

        if(sess.max_eip_packet_size > 0 && (size_t)payload_len > sess.max_eip_packet_size) {
            pdebug(DEBUG_MOD, DEBUG_WARN, 0,
                   "EIP payload_len=%u exceeds negotiated max %zu — closing.",
                   (unsigned)payload_len, sess.max_eip_packet_size);
            break;
        }

        if((size_t)payload_len > arena_remaining(&arena)) {
            pdebug(DEBUG_MOD, DEBUG_WARN, 0,
                   "EIP payload_len=%u exceeds arena space — closing.", (unsigned)payload_len);
            break;
        }

        /* Phase 3: read payload. */
        Bytes payload = (Bytes){NULL, 0};
        if(payload_len > 0) {
            payload = bytes_alloc(&arena, payload_len);
            if(bytes_is_null(payload)) {
                pdebug(DEBUG_MOD, DEBUG_ERROR, 0, "Arena OOM allocating payload buffer.");
                break;
            }
            rc = recv_exact(c->sock, c->device, payload.data, (int32_t)payload.len);
            if(rc != PLCTAG_STATUS_OK) {
                if(rc != PLCTAG_ERR_ABORT && rc != PLCTAG_ERR_BAD_CONNECTION) {
                    pdebug(DEBUG_MOD, DEBUG_WARN, 0, "EIP payload recv error %d.", rc);
                }
                break;
            }
        }

        pdebug(DEBUG_MOD, DEBUG_DETAIL, 0,
               "EIP request: payload=%u bytes.", (unsigned)payload_len);

        /* Phase 4: dispatch. */
        Bytes resp = eip_dispatch(&arena, hdr, payload, &sess, c->device);

        /* Optional artificial delay. */
        if(c->device->response_delay_ms > 0) {
            sleep_ms((int)c->device->response_delay_ms);
        }

        /* Null response means UnregisterSession or fatal error. */
        if(bytes_is_null(resp)) { break; }

        /* Phase 5: send response. */
        rc = send_all(c->sock, c->device, resp.data, (int32_t)resp.len);
        if(rc != PLCTAG_STATUS_OK) {
            if(rc != PLCTAG_ERR_ABORT && rc != PLCTAG_ERR_BAD_CONNECTION) {
                pdebug(DEBUG_MOD, DEBUG_WARN, 0, "EIP response send error %d.", rc);
            }
            break;
        }
    }

    pdebug(DEBUG_MOD, DEBUG_DETAIL, 0, "Connection handler closing.");

    if(c->device->disconnect_cb) {
        c->device->disconnect_cb(c->device->sim, c->device->disconnect_user_data);
    }

    registry_remove(c->registry, c->sock);
    socket_close(c->sock);
    socket_destroy(&c->sock);
    arena_free(&arena);
    mem_free(c);

    THREAD_RETURN(0);
}

/* ============================================================================
 * Listener thread
 * ============================================================================ */

extern THREAD_FUNC(server_listener) {
    listener_ctx_t *lctx = (listener_ctx_t *)arg;
    device_t   *device   = lctx->device;
    registry_t *registry = lctx->registry;
    sock_p      listen_sock = NULL;
    int32_t     rc;

    mem_free(lctx);   /* context only needed to pass arguments; free now */

    pdebug(DEBUG_MOD, DEBUG_INFO, 0,
           "Listener starting on %s:%u.",
           device->bind_addr ? device->bind_addr : "0.0.0.0",
           (unsigned)device->port);

    rc = socket_create(&listen_sock);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MOD, DEBUG_ERROR, 0, "socket_create failed: %d.", rc);
        THREAD_RETURN(0);
    }

    rc = socket_listen_tcp(listen_sock, device->bind_addr, device->port, 128);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MOD, DEBUG_ERROR, 0, "socket_listen_tcp failed: %d.", rc);
        socket_destroy(&listen_sock);
        THREAD_RETURN(0);
    }

    registry_add(registry, listen_sock);

    while(!atomic_get_bool(&device->terminate)) {
        sock_p client = NULL;
        rc = socket_accept(listen_sock, &client, ACCEPT_TIMEOUT_MS);

        if(rc == PLCTAG_ERR_TIMEOUT) { continue; }
        if(rc == PLCTAG_ERR_ABORT)   { break; }
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MOD, DEBUG_WARN, 0, "socket_accept error %d.", rc);
            break;
        }

        conn_ctx_t *c = (conn_ctx_t *)mem_alloc((int)sizeof(conn_ctx_t));
        if(!c) {
            pdebug(DEBUG_MOD, DEBUG_ERROR, 0, "Failed to allocate connection context.");
            socket_close(client);
            socket_destroy(&client);
            continue;
        }
        c->device   = device;
        c->registry = registry;
        c->sock     = client;

        registry_add(registry, client);

        thread_p t;
        if(thread_create(&t, conn_handler, CONN_STACK_SIZE, c) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MOD, DEBUG_ERROR, 0, "thread_create failed for connection.");
            registry_remove(registry, client);
            socket_close(client);
            socket_destroy(&client);
            mem_free(c);
        }
        /* thread detaches itself; do not join t */
    }

    pdebug(DEBUG_MOD, DEBUG_INFO, 0, "Listener stopping.");

    registry_remove(registry, listen_sock);
    socket_close(listen_sock);
    socket_destroy(&listen_sock);

    THREAD_RETURN(0);
}
