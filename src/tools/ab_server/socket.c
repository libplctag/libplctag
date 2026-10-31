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

#if IS_WINDOWS
#    define WIN32_LEAN_AND_MEAN

#    include <winsock2.h>

#    include <windows.h>

#    include <ws2tcpip.h>

#    define suseconds_t long

#else
#    include <arpa/inet.h>
#    include <errno.h>
#    include <netdb.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <sys/time.h>
#    include <sys/types.h>
#    include <unistd.h>
#endif


#include "socket.h"
#include "err.h"
#include <libplctag/api/libplctag.h>
#include <utils/byte_slice.h>
#include <stdbool.h>
#include "utils.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* lengths for socket read and write. */
#ifdef IS_WINDOWS
typedef int sock_io_len_t;
#    define MSG_NOSIGNAL (0)
#else
typedef int SOCKET;
typedef size_t sock_io_len_t;
typedef struct timeval TIMEVAL;
#endif


#define LISTEN_QUEUE (10)


static SOCKET open_bound_socket(const char *listening_port, int32_t type);
static int32_t wait_ready(SOCKET sock, uint32_t timeout_ms, bool for_write);
static bool last_error_would_block(void);


/* the OS code of the last socket error: WSAGetLastError() and errno are separate namespaces. */
static int32_t socket_last_error(void) {
#ifdef IS_WINDOWS
    return (int32_t)WSAGetLastError();
#else
    return (int32_t)errno;
#endif
}


/*
 * Returns INVALID_SOCKET on every failure, never an error code.
 *
 * Error codes used to be POSITIVE while SOCKET is a plain
 * int, so returning one produced a "socket" that sailed through the caller's
 * `sock >= 0` check and was stored as a live fd. A failed bind then left the
 * server running its accept loop against a descriptor that was never opened:
 * alive, logging "Timed out waiting for new client connection" forever, and never
 * accepting anything. The specific reason is logged here instead.
 */
SOCKET socket_open_tcp_server(const char *listening_port) {
    SOCKET sock = open_bound_socket(listening_port, SOCK_STREAM);
    int32_t rc = 0;

    if(sock == INVALID_SOCKET) { return INVALID_SOCKET; }

    rc = listen(sock, LISTEN_QUEUE);
    if(rc < 0) {
        int32_t listen_err = socket_last_error();

        log_error("ERROR: Unable to listen() on port %s: OS error %d", listening_port, listen_err);
        socket_close(sock);
        return INVALID_SOCKET;
    }

    return sock;
}


SOCKET socket_open_udp_server(const char *listening_port) { return open_bound_socket(listening_port, SOCK_DGRAM); }


/* a socket of the given type bound to the port on every interface, or INVALID_SOCKET. */
static SOCKET open_bound_socket(const char *listening_port, int32_t type) {
    struct sockaddr_in address = {0};
    SOCKET sock = INVALID_SOCKET;
    SOCKET sock_opt = 0;
    int32_t rc = 0;

#ifdef IS_WINDOWS
    /* Windows needs special initialization.  It counts calls, so a second one is harmless. */
    static WSADATA winsock_data;
    rc = WSAStartup(MAKEWORD(2, 2), &winsock_data);

    if(rc != NO_ERROR) {
        log_error("ERROR: WSAStartup failed with error %d!", rc);
        return INVALID_SOCKET;
    }
#endif

    /* create the socket */
    sock = socket(AF_INET, type, 0 /* IP protocol */);
    if(sock == INVALID_SOCKET) {
        log_error("ERROR: socket() failed for port %s: OS error %d", listening_port, socket_last_error());
        return INVALID_SOCKET;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons((uint16_t)atoi(listening_port));

    /* set up our socket to allow reuse if we crash suddenly — must be before bind(). */
    sock_opt = 1;
    rc = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&sock_opt, sizeof(sock_opt));
    if(rc) {
        socket_close(sock);
        log_error("ERROR: Setting SO_REUSEADDR failed for port %s: OS error %d", listening_port, socket_last_error());
        return INVALID_SOCKET;
    }

    log_info("socket_open() setting up server socket. Binding to address 0.0.0.0 port %s.", listening_port);

    rc = bind(sock, (struct sockaddr *)&address, (socklen_t)sizeof(address));
    if(rc < 0) {
        /* capture the error before socket_close() can overwrite it. */
        int32_t bind_err = socket_last_error();

        log_error("ERROR: Unable to bind() to port %s: OS error %d", listening_port, bind_err);
        socket_close(sock);
        return INVALID_SOCKET;
    }

    return sock;
}


void socket_close(SOCKET sock) {
    if(sock > 0) {
#ifdef IS_WINDOWS
        closesocket(sock);
#else
        close(sock);
#endif
    }
}


int32_t socket_accept(SOCKET sock, uint32_t timeout_ms, SOCKET *out_client_fd) {
    int32_t rc = PLCTAG_STATUS_OK;

    if(!out_client_fd) { return PLCTAG_ERR_NULL_PTR; }

    *out_client_fd = INVALID_SOCKET;

    if((rc = wait_ready(sock, timeout_ms, false)) != PLCTAG_STATUS_OK) { return (rc == PLCTAG_ERR_TIMEOUT) ? rc : PLCTAG_ERR_OPEN; }

    *out_client_fd = accept(sock, NULL, NULL);

    return (*out_client_fd == INVALID_SOCKET) ? PLCTAG_ERR_OPEN : PLCTAG_STATUS_OK;
}


byte_slice_t socket_read(SOCKET sock, byte_slice_t space, uint32_t timeout_ms) {
    byte_slice_t filled = space;
    int32_t rc = 0;

    if(byte_slice_has_error(space)) { return space; }

    if((rc = wait_ready(sock, timeout_ms, false)) != PLCTAG_STATUS_OK) { return byte_slice_fail(space, rc); }

    rc = (int32_t)recv(sock, (char *)byte_slice_get_data(space), (sock_io_len_t)byte_slice_len(space), 0);

    if(rc == 0) {
        log_info("Unable to read.  The socket is closed.");
        return byte_slice_fail(space, PLCTAG_ERR_CLOSE);
    }

    /* an interrupted read is treated as a timeout. */
    if(rc < 0) { return byte_slice_fail(space, last_error_would_block() ? PLCTAG_ERR_TIMEOUT : PLCTAG_ERR_READ); }

    byte_slice_cut(space, rc, &filled, NULL);

    return filled;
}


/* B8: send() may take only part of the data, so loop until it is all gone. */
byte_slice_t socket_write(SOCKET sock, byte_slice_t data, uint32_t timeout_ms) {
    while(byte_slice_len(data) > 0) {
        int32_t rc = wait_ready(sock, timeout_ms, true);

        if(rc != PLCTAG_STATUS_OK) { return byte_slice_fail(data, rc); }

        rc = (int32_t)send(sock, (const char *)byte_slice_get_data(data), (sock_io_len_t)byte_slice_len(data), MSG_NOSIGNAL);

        if(rc < 0) {
            if(last_error_would_block()) { continue; }

            log_info("Socket write error!");
            return byte_slice_fail(data, PLCTAG_ERR_WRITE);
        }

        data = byte_slice_skip(data, rc);
    }

    return data;
}


byte_slice_t socket_read_from(SOCKET sock, byte_slice_t space, uint32_t timeout_ms, socket_peer_s *peer) {
    byte_slice_t filled = space;
    socklen_t peer_len = (socklen_t)sizeof(peer->addr);
    int32_t rc = 0;

    if(byte_slice_has_error(space)) { return space; }

    if((rc = wait_ready(sock, timeout_ms, false)) != PLCTAG_STATUS_OK) { return byte_slice_fail(space, rc); }

    rc = (int32_t)recvfrom(sock, (char *)byte_slice_get_data(space), (sock_io_len_t)byte_slice_len(space), 0,
                           (struct sockaddr *)peer->addr, &peer_len);
    if(rc < 0) { return byte_slice_fail(space, PLCTAG_ERR_READ); }

    peer->len = (uint32_t)peer_len;

    byte_slice_cut(space, rc, &filled, NULL);

    return filled;
}


byte_slice_t socket_write_to(SOCKET sock, byte_slice_t data, socket_peer_s *peer) {
    int32_t rc = 0;

    if(byte_slice_has_error(data)) { return data; }

    rc = (int32_t)sendto(sock, (const char *)byte_slice_get_data(data), (sock_io_len_t)byte_slice_len(data), 0,
                         (struct sockaddr *)peer->addr, (socklen_t)peer->len);

    /* a datagram goes whole or not at all */
    if(rc != byte_slice_len(data)) { return byte_slice_fail(data, PLCTAG_ERR_WRITE); }

    return byte_slice_skip(data, rc);
}


/* PLCTAG_STATUS_OK when sock is ready, else PLCTAG_ERR_TIMEOUT or the read/write error. */
static int32_t wait_ready(SOCKET sock, uint32_t timeout_ms, bool for_write) {
    fd_set fds;
    TIMEVAL timeout;
    int32_t rc = 0;

    timeout.tv_sec = (time_t)(timeout_ms / 1000);
    timeout.tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000);

    FD_ZERO(&fds);
    FD_SET(sock, &fds);

    rc = (int32_t)select((int)(unsigned int)sock + 1, for_write ? NULL : &fds, for_write ? &fds : NULL, NULL, &timeout);

    if(rc == 0) { return PLCTAG_ERR_TIMEOUT; }

    if(rc < 0) {
        log_info("Socket select error!");
        return for_write ? PLCTAG_ERR_WRITE : PLCTAG_ERR_READ;
    }

    return PLCTAG_STATUS_OK;
}


static bool last_error_would_block(void) {
#ifdef IS_WINDOWS
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}


/*
 * The local IPv4 address, host order, that traffic to peer leaves from, or 0.  Connecting a
 * UDP socket sends nothing; it only makes the kernel pick the route.
 */
uint32_t socket_local_ipv4_toward(socket_peer_s *peer) {
    struct sockaddr_in local = {0};
    socklen_t local_len = (socklen_t)sizeof(local);
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    uint32_t result = 0;

    if(sock == INVALID_SOCKET) { return 0; }

    if(connect(sock, (struct sockaddr *)peer->addr, (socklen_t)peer->len) == 0
       && getsockname(sock, (struct sockaddr *)&local, &local_len) == 0) {
        result = ntohl(local.sin_addr.s_addr);
    }

    socket_close(sock);

    return result;
}
