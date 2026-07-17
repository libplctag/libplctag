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
#include "utils/debug.h"
#include "net/tcp_server.h"

/* ============================================================================
 * Registry
 * ============================================================================ */

extern tcp_registry_t *tcp_registry_create(void) {
    tcp_registry_t *reg = (tcp_registry_t *)mem_alloc((int)sizeof(tcp_registry_t));
    if(!reg) { return NULL; }
    mem_set(reg, 0, (int)sizeof(tcp_registry_t));
    if(mutex_create(&reg->mutex) != PLCTAG_STATUS_OK) {
        mem_free(reg);
        return NULL;
    }
    return reg;
}


extern void tcp_registry_destroy(tcp_registry_t *reg) {
    if(!reg) { return; }
    mutex_destroy(&reg->mutex);
    mem_free(reg);
}


extern void tcp_registry_add(tcp_registry_t *reg, sock_p sock) {
    if(!reg || !sock) { return; }
    critical_block(reg->mutex) {
        for(int32_t i = 0; i < TCP_SERVER_REGISTRY_MAX; i++) {
            if(!reg->socks[i]) {
                reg->socks[i] = sock;
                break;
            }
        }
    }
}


extern void tcp_registry_remove(tcp_registry_t *reg, sock_p sock) {
    if(!reg || !sock) { return; }
    critical_block(reg->mutex) {
        for(int32_t i = 0; i < TCP_SERVER_REGISTRY_MAX; i++) {
            if(reg->socks[i] == sock) {
                reg->socks[i] = NULL;
                break;
            }
        }
    }
}


extern void tcp_registry_wake_all(tcp_registry_t *reg) {
    if(!reg) { return; }
    critical_block(reg->mutex) {
        for(int32_t i = 0; i < TCP_SERVER_REGISTRY_MAX; i++) {
            if(reg->socks[i]) { socket_wake(reg->socks[i]); }
        }
    }
}

/* ============================================================================
 * tcp_recv_exact / tcp_send_all -- blocking helpers using socket_wait_event
 * for wake-pipe integration.
 * ============================================================================ */

extern int32_t tcp_recv_exact(sock_p sock, atomic_bool *terminate, uint8_t *buf, int32_t len, int32_t io_timeout_ms) {
    int32_t total = 0;
    while(total < len) {
        int32_t events = socket_wait_event(sock,
            SOCK_EVENT_CAN_READ | SOCK_EVENT_WAKE_UP | SOCK_EVENT_DISCONNECT,
            io_timeout_ms);

        if(events & SOCK_EVENT_WAKE_UP)    { return PLCTAG_ERR_ABORT; }
        if(events & SOCK_EVENT_DISCONNECT) { return PLCTAG_ERR_BAD_CONNECTION; }
        if(events & SOCK_EVENT_ERROR)      { return PLCTAG_ERR_READ; }
        if(events & SOCK_EVENT_TIMEOUT) {
            if(atomic_get_bool(terminate)) { return PLCTAG_ERR_ABORT; }
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


extern int32_t tcp_send_all(sock_p sock, atomic_bool *terminate, uint8_t *buf, int32_t len, int32_t io_timeout_ms) {
    int32_t total = 0;
    while(total < len) {
        int32_t events = socket_wait_event(sock,
            SOCK_EVENT_CAN_WRITE | SOCK_EVENT_WAKE_UP | SOCK_EVENT_DISCONNECT,
            io_timeout_ms);

        if(events & SOCK_EVENT_WAKE_UP)    { return PLCTAG_ERR_ABORT; }
        if(events & SOCK_EVENT_DISCONNECT) { return PLCTAG_ERR_BAD_CONNECTION; }
        if(events & SOCK_EVENT_ERROR)      { return PLCTAG_ERR_WRITE; }
        if(events & SOCK_EVENT_TIMEOUT) {
            if(atomic_get_bool(terminate)) { return PLCTAG_ERR_ABORT; }
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
 * Per-connection thread -- generic over tcp_conn_ops_t.
 * ============================================================================ */

typedef struct {
    tcp_server_config_t cfg;   /* copied out of the listener's config */
    sock_p               sock;
} conn_ctx_t;

static THREAD_FUNC(conn_handler) {
    conn_ctx_t *c = (conn_ctx_t *)arg;
    const tcp_conn_ops_t *ops = c->cfg.ops;
    Arena arena;
    void *session = NULL;

    thread_detach();

    if(arena_init(&arena, c->cfg.conn_arena_size) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "Failed to allocate connection arena.");
        goto done_no_arena;
    }

    session = mem_alloc((int)ops->session_size);
    if(!session) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "Failed to allocate connection session.");
        goto done;
    }
    mem_set(session, 0, (int)ops->session_size);
    ops->session_init(session, c->cfg.shared_ctx, c->sock);

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Connection handler started.");

    while(!atomic_get_bool(c->cfg.terminate)) {
        arena_reset(&arena);

        Bytes request = ops->read_request(&arena, c->sock, c->cfg.terminate, session);
        if(bytes_is_null(request)) { break; }

        Bytes resp = ops->dispatch(&arena, request, session);

        /* Null response means protocol-level session teardown or fatal error. */
        if(bytes_is_null(resp)) { break; }

        int32_t rc = tcp_send_all(c->sock, c->cfg.terminate, resp.data, (int32_t)resp.len, c->cfg.io_timeout_ms);
        if(rc != PLCTAG_STATUS_OK) {
            if(rc != PLCTAG_ERR_ABORT && rc != PLCTAG_ERR_BAD_CONNECTION) {
                pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "Response send error %d.", rc);
            }
            break;
        }
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_DETAIL, 0, "Connection handler closing.");

    if(ops->session_destroy) { ops->session_destroy(session); }
    mem_free(session);

done:
    arena_free(&arena);
done_no_arena:
    tcp_registry_remove(c->cfg.registry, c->sock);
    socket_close(c->sock);
    socket_destroy(&c->sock);
    mem_free(c);

    THREAD_RETURN(0);
}

/* ============================================================================
 * Listener thread
 * ============================================================================ */

extern THREAD_FUNC(tcp_server_listener) {
    tcp_server_config_t cfg = *(tcp_server_config_t *)arg;
    sock_p listen_sock = NULL;
    int32_t rc;

    mem_free(arg);   /* config only needed to pass arguments; copied above */

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0,
           "Listener starting on %s:%u.",
           cfg.bind_addr ? cfg.bind_addr : "0.0.0.0",
           (unsigned)cfg.port);

    rc = socket_create(&listen_sock);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "socket_create failed: %d.", rc);
        THREAD_RETURN(0);
    }

    rc = socket_listen_tcp(listen_sock, cfg.bind_addr, cfg.port, 128);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "socket_listen_tcp failed: %d.", rc);
        socket_destroy(&listen_sock);
        THREAD_RETURN(0);
    }

    tcp_registry_add(cfg.registry, listen_sock);

    while(!atomic_get_bool(cfg.terminate)) {
        sock_p client = NULL;
        rc = socket_accept(listen_sock, &client, cfg.accept_timeout_ms);

        if(rc == PLCTAG_ERR_TIMEOUT) { continue; }
        if(rc == PLCTAG_ERR_ABORT)   { break; }
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_WARN, 0, "socket_accept error %d.", rc);
            break;
        }

        conn_ctx_t *c = (conn_ctx_t *)mem_alloc((int)sizeof(conn_ctx_t));
        if(!c) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "Failed to allocate connection context.");
            socket_close(client);
            socket_destroy(&client);
            continue;
        }
        c->cfg  = cfg;
        c->sock = client;

        tcp_registry_add(cfg.registry, client);

        thread_p t;
        if(thread_create(&t, conn_handler, cfg.conn_stack_size, c) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_UTILS, DEBUG_ERROR, 0, "thread_create failed for connection.");
            tcp_registry_remove(cfg.registry, client);
            socket_close(client);
            socket_destroy(&client);
            mem_free(c);
        }
        /* thread detaches itself; do not join t */
    }

    pdebug(DEBUG_MODULE_UTILS, DEBUG_INFO, 0, "Listener stopping.");

    tcp_registry_remove(cfg.registry, listen_sock);
    socket_close(listen_sock);
    socket_destroy(&listen_sock);

    THREAD_RETURN(0);
}
