/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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
#    include <windows.h>
#    include <winsock2.h>
#    include <ws2tcpip.h>
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
#include "slice.h"
#include "socket.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* lengths for socket read and write. */
#ifdef IS_MSVC
typedef int sock_io_len_t;
#else
typedef size_t sock_io_len_t;
typedef struct timeval TIMEVAL;
#endif


#define LISTEN_QUEUE (10)


int socket_open_tcp_client(const char *remote_host, const char *remote_port) {
    int sock = -1;
    int rc = 0;
    struct sockaddr_in serv_addr = {0};
    struct timeval timeout = {0};
    struct linger so_linger = {0};

#ifdef IS_WINDOWS
    /* Windows needs special initialization. */
    static WSADATA winsock_data;
    rc = WSAStartup(MAKEWORD(2, 2), &winsock_data);

    if(rc != NO_ERROR) {
        info("WSAStartup failed with error: %d\n", rc);
        return SOCKET_ERR_STARTUP;
    }
#endif

    /* create the socket */
    sock = (int)socket(AF_INET, SOCK_STREAM, 0 /* IP protocol */);
    if(sock < 0) {
        info("ERROR: socket() failed: %s\n", gai_strerror(sock));
        return SOCKET_ERR_CREATE;
    }

#ifdef SO_NOSIGPIPE
    int sock_opt = 1;

    /* On *BSD and macOS, set the socket option to prevent SIGPIPE. */
    rc = setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, (char *)&sock_opt, sizeof(sock_opt));
    if(rc) {
        socket_close(sock);
        info("ERROR: Setting SO_NOSIGPIPE on socket failed: %s\n", gai_strerror(rc));
        return SOCKET_ERR_SETOPT;
    }
#endif

    timeout.tv_sec = 10;
    timeout.tv_usec = 0;

    rc = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    if(rc) {
        socket_close(sock);
        info("ERROR: Setting SO_RCVTIMEO on socket failed: %s\n", gai_strerror(rc));
        return SOCKET_ERR_SETOPT;
    }

    rc = setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));
    if(rc) {
        socket_close(sock);
        info("ERROR: Setting SO_SNDTIMEO on socket failed: %s\n", gai_strerror(rc));
        return SOCKET_ERR_SETOPT;
    }

    /* abort the connection on close. */
    so_linger.l_onoff = 1;
    so_linger.l_linger = 0;

    rc = setsockopt(sock, SOL_SOCKET, SO_LINGER, (char *)&so_linger, sizeof(so_linger));
    if(rc) {
        socket_close(sock);
        info("ERROR: Setting SO_LINGER on socket failed: %s\n", gai_strerror(rc));
        return SOCKET_ERR_SETOPT;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((uint16_t)atoi(remote_port));

    if((rc = inet_pton(AF_INET, remote_host, &serv_addr.sin_addr)) <= 0) {
        socket_close(sock);
        info("ERROR: Getting IP address for remote server, %s, failed: %d\n", remote_host, rc);
        return SOCKET_ERR_CREATE;
    }

    /* now connect to the remote server */
    if(connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr))) {
        socket_close(sock);
        info("ERROR: Connecting to remote server, %s, failed: %d\n", remote_host, rc);
        return SOCKET_ERR_CONNECT;
    }

    return sock;
}


int socket_open_tcp_server(const char *listening_port) {
    struct sockaddr_in address = {0};
    int sock = -1;
    int sock_opt = 0;
    int rc = 0;

#ifdef IS_WINDOWS
    /* Windows needs special initialization. */
    static WSADATA winsock_data;
    rc = WSAStartup(MAKEWORD(2, 2), &winsock_data);

    if(rc != NO_ERROR) {
        info("WSAStartup failed with error: %d\n", rc);
        return SOCKET_ERR_STARTUP;
    }
#endif

    /* create the socket */
    sock = (int)socket(AF_INET, SOCK_STREAM, 0 /* IP protocol */);
    if(sock < 0) {
        info("ERROR: socket() failed: %s\n", gai_strerror(sock));
        return SOCKET_ERR_CREATE;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons((uint16_t)atoi(listening_port));

    info("socket_open() setting up server socket. Binding to address 0.0.0.0.");

    rc = bind(sock, (struct sockaddr *)&address, (socklen_t)sizeof(address));
    if(rc < 0) {
        perror("Error from bind(): ");
        printf("ERROR: Unable to bind() socket: %d\n", rc);
        return SOCKET_ERR_BIND;
    }

    rc = listen(sock, LISTEN_QUEUE);
    if(rc < 0) {
        info("ERROR: Unable to call listen() on socket: %d\n", rc);
        return SOCKET_ERR_LISTEN;
    }

    /* set up our socket to allow reuse if we crash suddenly. */
    sock_opt = 1;
    rc = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&sock_opt, sizeof(sock_opt));
    if(rc) {
        socket_close(sock);
        info("ERROR: Setting SO_REUSEADDR on socket failed: %s\n", gai_strerror(rc));
        return SOCKET_ERR_SETOPT;
    }

    // }

    return sock;
}


void socket_close(int sock) {
    if(sock >= 0) {
#ifdef IS_WINDOWS
        closesocket(sock);
#else
        close(sock);
#endif
    }
}


int socket_accept(int sock) {
    fd_set accept_fd_set;
    TIMEVAL timeout;
    int num_accept_ready = 0;

    /* set the timeout to zero */
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    /* zero out the file descriptor set. */
    FD_ZERO(&accept_fd_set);

    /* set our socket's bit in the set. */
    FD_SET(sock, &accept_fd_set);

    /* do a select to see if anything is ready to accept. */
    num_accept_ready = select(sock + 1, &accept_fd_set, NULL, NULL, &timeout);
    if(num_accept_ready > 0) {
        info("Ready to accept on %d sockets.", num_accept_ready);
        if(FD_ISSET(sock, &accept_fd_set)) { return (int)accept(sock, NULL, NULL); }
    } else if(num_accept_ready < 0) {
        info("Error selecting the listen socket! Errno=%d.", errno);
        return SOCKET_ERR_SELECT;
    }

    return SOCKET_STATUS_OK;
}


slice_s socket_read(int sock, slice_s in_buf) {
#ifdef IS_WINDOWS
    int rc = (int)recv(sock, (char *)in_buf.data, (int)in_buf.len, 0);
#else
    int rc = (int)recv(sock, (char *)in_buf.data, (size_t)in_buf.len, 0);
#endif

    if(rc < 0) {
#ifdef IS_WINDOWS
        rc = WSAGetLastError();
        if(rc == WSAEWOULDBLOCK) {
#else
        rc = errno;
        if(rc == EAGAIN || rc == EWOULDBLOCK) {
#endif
            rc = 0;
        } else {
            info("Socket read error rc=%d.\n", rc);
            rc = SOCKET_ERR_READ;
        }
    }

    return ((rc >= 0) ? slice_from_slice(in_buf, 0, (size_t)(unsigned int)rc) : slice_make_err(rc));
}


/* this blocks until all the data is written or there is an error. */
int socket_write(int sock, slice_s out_buf) {
    size_t total_bytes_written = 0;
    int rc = 0;
    slice_s tmp_out_buf = out_buf;

    info("socket_write(): writing packet:");
    slice_dump(out_buf);

    do {
#ifdef IS_WINDOWS
        rc = (int)send(sock, (char *)tmp_out_buf.data, (int)tmp_out_buf.len, 0);
#else
        rc = (int)send(sock, (char *)tmp_out_buf.data, (size_t)tmp_out_buf.len, 0);
#endif

        /* was there an error? */
        if(rc < 0) {
            /*
             * check the return value.  If it is an interrupted system call
             * or would block, just keep looping.
             */
#ifdef IS_WINDOWS
            rc = WSAGetLastError();
            if(rc != WSAEWOULDBLOCK) {
#else
            rc = errno;
            if(rc != EAGAIN && rc != EWOULDBLOCK) {
#endif
                info("Socket write error rc=%d.\n", rc);
                return SOCKET_ERR_WRITE;
            }
        } else {
            total_bytes_written += (size_t)rc;
            tmp_out_buf = slice_from_slice(out_buf, total_bytes_written, slice_len(out_buf) - total_bytes_written);
        }
    } while(total_bytes_written < slice_len(out_buf));

    return (int)(unsigned int)total_bytes_written;
}
