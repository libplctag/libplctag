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

#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

#include <stdbool.h>
#include <stdint.h>

/* Cross-platform socket type and definitions */
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET socket_t;
    typedef unsigned int nfds_t;
    #define INVALID_SOCKET_VALUE INVALID_SOCKET
    #define SOCKET_ERROR_VALUE SOCKET_ERROR
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <errno.h>
    typedef int socket_t;
    #define INVALID_SOCKET_VALUE (-1)
    #define SOCKET_ERROR_VALUE (-1)
    #define closesocket close
#endif

/* Unified invalid socket value */
#define LIBPLCTAG_INVALID_SOCKET INVALID_SOCKET_VALUE

/* Poll events wrapper */
#ifdef _WIN32
    typedef WSAPOLLFD pollfd_t;
    #define poll_wrapper WSAPoll
#else
    typedef struct pollfd pollfd_t;
    #define poll_wrapper poll
#endif

/* Initialize socket library (Windows WSAStartup) */
bool socket_init(void);

/* Cleanup socket library (Windows WSACleanup) */
void socket_cleanup(void);

/* Create a listening socket */
socket_t socket_create_listener(const char *ip, int port);

/* Set socket to non-blocking mode */
bool socket_set_nonblocking(socket_t sock);

/* Set SO_REUSEADDR option */
bool socket_set_reuseaddr(socket_t sock);

/* Accept a connection and format client info as "IP:PORT" */
socket_t socket_accept(socket_t listener, char *client_info, size_t info_len);

/* Close a socket */
void socket_close(socket_t sock);

/* Send data on socket */
int socket_send(socket_t sock, const void *buffer, int length);

/* Receive data from socket */
int socket_recv(socket_t sock, void *buffer, int length);

/* Get last socket error */
int socket_get_last_error(void);

/* Get error string for error code */
const char* socket_get_error_string(int error_code);

/* Check if socket is valid */
bool socket_is_valid(socket_t sock);

#endif /* SOCKET_UTILS_H */
