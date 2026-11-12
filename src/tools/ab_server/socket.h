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

#include "compat.h"
#include "err.h"
#include <utils/byte_slice.h>

#include <stdint.h>

#ifndef IS_WINDOWS
typedef int SOCKET;
#    define INVALID_SOCKET (-1)
#else
#    include <winsock2.h>
#endif

/* ===== SOCKET API ===== */

/* Open a TCP server socket
 * Returns: Valid SOCKET file descriptor (>= 0) on success
 *          Negative error code (from err_t) on failure */
extern SOCKET socket_open_tcp_server(const char *listening_port);

/* Open a UDP socket bound to the port on every interface, or INVALID_SOCKET. */
extern SOCKET socket_open_udp_server(const char *listening_port);

/* Where a datagram came from, so the reply can go back.  Big enough for any sockaddr. */
typedef struct {
    uint64_t addr[16]; /* 128 bytes, aligned for a sockaddr */
    uint32_t len;
} socket_peer_s;

/*
 * Socket errors are PLCTAG_ERR_* codes in an error slice: PLCTAG_ERR_TIMEOUT, PLCTAG_ERR_CLOSE when
 * the peer closed, PLCTAG_ERR_READ or PLCTAG_ERR_WRITE for anything else.
 */

/* Receive one datagram into space, waiting up to timeout_ms.  Returns the filled front of space. */
extern byte_slice_t socket_read_from(SOCKET sock, byte_slice_t space, uint32_t timeout_ms, socket_peer_s *peer);

/* Send data as one datagram to peer.  Returns the empty remainder. */
extern byte_slice_t socket_write_to(SOCKET sock, byte_slice_t data, socket_peer_s *peer);

/* The local IPv4 address, host order, that traffic to peer leaves from, or 0. */
extern uint32_t socket_local_ipv4_toward(socket_peer_s *peer);

/* Close a socket */
extern void socket_close(SOCKET sock);

/* Accept a connection, waiting up to timeout_ms.  PLCTAG_STATUS_OK, PLCTAG_ERR_TIMEOUT or PLCTAG_ERR_OPEN. */
extern int32_t socket_accept(SOCKET sock, uint32_t timeout_ms, SOCKET *out_client_fd);

/* Read what is available into space, waiting up to timeout_ms.  Returns the filled front of space. */
extern byte_slice_t socket_read(SOCKET sock, byte_slice_t space, uint32_t timeout_ms);

/* Write all of data, waiting up to timeout_ms for each chunk.  Returns the empty remainder. */
extern byte_slice_t socket_write(SOCKET sock, byte_slice_t data, uint32_t timeout_ms);
