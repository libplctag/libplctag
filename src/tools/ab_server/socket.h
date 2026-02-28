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

#pragma once

#include "compat.h"
#include "err.h"
#include "slice.h"

#include <stdint.h>

#ifndef IS_WINDOWS
typedef int SOCKET;
#    define INVALID_SOCKET (-1)
#else
#    include <winsock2.h>
#endif

/* ===== SOCKET API ===== */

/* Open a TCP client connection
 * Returns: Valid SOCKET file descriptor (>= 0) on success
 *          Negative error code (from err_t) on failure */
extern SOCKET socket_open_tcp_client(const char *remote_host, const char *remote_port);

/* Open a TCP server socket
 * Returns: Valid SOCKET file descriptor (>= 0) on success
 *          Negative error code (from err_t) on failure */
extern SOCKET socket_open_tcp_server(const char *listening_port);

/* Close a socket */
extern void socket_close(SOCKET sock);

/* Accept an incoming connection
 * Returns: 0 on success, error code on failure
 *          On success, *out_client_fd contains the accepted socket
 *          On failure, *out_client_fd is set to INVALID_SOCKET */
extern int socket_accept(SOCKET sock, uint32_t timeout_ms, SOCKET *out_client_fd);

/* Read from socket into buffer
 * Returns: slice_s with data read from socket
 *          On error: slice_has_err() is true, slice_get_err() returns negative error code */
extern slice_s socket_read(SOCKET sock, slice_s in_buf, uint32_t timeout_ms);

/* Write to socket from buffer
 * Returns: slice_s with length set to bytes written
 *          On error: slice_has_err() is true, slice_get_err() returns negative error code */
extern slice_s socket_write(SOCKET sock, slice_s out_buf, uint32_t timeout_ms);
