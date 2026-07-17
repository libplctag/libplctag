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

#pragma once

/*
 * tcp_server.{h,c} -- protocol-agnostic blocking thread-per-connection TCP
 * server (ENIP-UPDATES-PLAN.md items 3.4/3.5). Everything here is generic:
 * listener + accept loop, live-socket registry (for wake-on-shutdown), and
 * blocking recv/send helpers. The two things that differ per wire protocol
 * -- how to read one complete request off the socket, and how to turn a
 * request into a response -- plug in through tcp_conn_ops_t.
 *
 * Today's only caller is protocols/enip/server/eip_server.c. A Modbus-TCP or
 * S7 server would need only its own tcp_conn_ops_t (framing + dispatch),
 * reusing everything else in this file.
 */

#include <stddef.h>
#include <stdint.h>

#include "platform.h"
#include "utils/arena.h"
#include "utils/atomic_utils.h"
#include "utils/bytes.h"

/* ============================================================================
 * Live-socket registry -- mutex-protected array of all active sockets for a
 * server instance (listener + accepted connections). registry_wake_all()
 * unblocks every blocking socket_wait_event() call so shutdown doesn't have
 * to wait out an IO timeout.
 * ============================================================================ */

#define TCP_SERVER_REGISTRY_MAX 64

typedef struct {
    mutex_p mutex;
    sock_p  socks[TCP_SERVER_REGISTRY_MAX];
} tcp_registry_t;

extern tcp_registry_t *tcp_registry_create(void);
extern void            tcp_registry_destroy(tcp_registry_t *reg);
extern void            tcp_registry_add(tcp_registry_t *reg, sock_p sock);
extern void            tcp_registry_remove(tcp_registry_t *reg, sock_p sock);
extern void            tcp_registry_wake_all(tcp_registry_t *reg);

/* Blocking read/write of exactly len bytes, honoring *terminate and the
 * registry wake pipe via socket_wait_event(). Return PLCTAG_STATUS_OK or a
 * negative PLCTAG_ERR_*. */
extern int32_t tcp_recv_exact(sock_p sock, atomic_bool *terminate, uint8_t *buf, int32_t len, int32_t io_timeout_ms);
extern int32_t tcp_send_all(sock_p sock, atomic_bool *terminate, uint8_t *buf, int32_t len, int32_t io_timeout_ms);

/* ============================================================================
 * Per-connection protocol plug-in.
 * ============================================================================ */

typedef struct {
    /*
     * Read exactly one complete request into the arena (owns "how many
     * bytes is one message" -- e.g. EIP's 24-byte header + length-prefixed
     * payload, Modbus's MBAP header + length field). Return a null Bytes on
     * clean close, timeout-terminate, or fatal error; the connection is then
     * closed without calling dispatch.
     */
    Bytes (*read_request)(Arena *a, sock_p sock, atomic_bool *terminate, void *session);

    /*
     * Produce a response for one request. A null response ends the
     * connection cleanly after this request (e.g. protocol-level session
     * teardown) without it being an error.
     */
    Bytes (*dispatch)(Arena *a, Bytes request, void *session);

    /* Per-connection session state, opaque to this module. Zero-initialized,
     * then session_init() runs once after accept; session_destroy() (may be
     * NULL) runs once before the socket is closed. */
    size_t session_size;
    void (*session_init)(void *session, void *shared_ctx, sock_p sock);
    void (*session_destroy)(void *session);
} tcp_conn_ops_t;

typedef struct {
    const char           *bind_addr;   /* NULL = any */
    uint16_t              port;
    tcp_registry_t        *registry;
    atomic_bool           *terminate;
    const tcp_conn_ops_t  *ops;
    void                  *shared_ctx; /* passed to ops->session_init, e.g. device_t* */
    size_t                 conn_arena_size;
    int32_t                io_timeout_ms;
    int32_t                accept_timeout_ms;
    int                    conn_stack_size;
} tcp_server_config_t;

/*
 * Listener thread entry point. arg must be a heap-allocated
 * tcp_server_config_t* (mem_alloc'd by the caller); this function frees it
 * after copying out the fields it needs, same lifetime contract the old
 * listener_ctx_t had.
 */
extern THREAD_FUNC(tcp_server_listener);
