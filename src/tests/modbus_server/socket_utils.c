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

#include "socket_utils.h"
#include "logger.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
    #pragma comment(lib, "Ws2_32.lib")
    static bool wsa_initialized = false;
#endif

bool socket_init(void) {
#ifdef _WIN32
    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        log_error("WSAStartup failed with error: %d", result);
        return false;
    }
    wsa_initialized = true;
    log_debug("WSAStartup succeeded");
    return true;
#else
    /* No initialization needed on Unix-like systems */
    return true;
#endif
}

void socket_cleanup(void) {
#ifdef _WIN32
    if (wsa_initialized) {
        WSACleanup();
        wsa_initialized = false;
        log_debug("WSACleanup completed");
    }
#endif
}

int socket_get_last_error(void) {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

const char* socket_get_error_string(int error_code) {
#ifdef _WIN32
    static char error_buffer[256];
    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        error_buffer,
        sizeof(error_buffer),
        NULL
    );
    return error_buffer;
#else
    return strerror(error_code);
#endif
}

bool socket_is_valid(socket_t sock) {
    return sock != INVALID_SOCKET_VALUE;
}

bool socket_set_nonblocking(socket_t sock) {
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) {
        log_error("Failed to set non-blocking mode: %s", 
                  socket_get_error_string(socket_get_last_error()));
        return false;
    }
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) {
        log_error("fcntl F_GETFL failed: %s", strerror(errno));
        return false;
    }
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        log_error("fcntl F_SETFL failed: %s", strerror(errno));
        return false;
    }
#endif
    log_debug("Socket set to non-blocking mode");
    return true;
}

bool socket_set_reuseaddr(socket_t sock) {
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, 
                   (const char*)&reuse, sizeof(reuse)) < 0) {
        log_error("setsockopt SO_REUSEADDR failed: %s",
                  socket_get_error_string(socket_get_last_error()));
        return false;
    }
    log_debug("SO_REUSEADDR enabled");
    return true;
}

socket_t socket_create_listener(const char *ip, int port) {
    socket_t sock;
    struct sockaddr_in addr;
    
    /* Create socket */
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!socket_is_valid(sock)) {
        log_error("Failed to create socket: %s",
                  socket_get_error_string(socket_get_last_error()));
        return INVALID_SOCKET_VALUE;
    }
    
    /* Set socket options */
    if (!socket_set_reuseaddr(sock)) {
        closesocket(sock);
        return INVALID_SOCKET_VALUE;
    }
    
    if (!socket_set_nonblocking(sock)) {
        closesocket(sock);
        return INVALID_SOCKET_VALUE;
    }
    
    /* Bind to address */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        log_error("Invalid IP address: %s", ip);
        closesocket(sock);
        return INVALID_SOCKET_VALUE;
    }
    
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_error("Failed to bind to %s:%d: %s", ip, port,
                  socket_get_error_string(socket_get_last_error()));
        closesocket(sock);
        return INVALID_SOCKET_VALUE;
    }
    
    /* Listen */
    if (listen(sock, SOMAXCONN) < 0) {
        log_error("Failed to listen: %s",
                  socket_get_error_string(socket_get_last_error()));
        closesocket(sock);
        return INVALID_SOCKET_VALUE;
    }
    
    log_debug("Listening on %s:%d", ip, port);
    return sock;
}

socket_t socket_accept(socket_t listener, char *client_info, size_t info_len) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    socket_t client_sock;
    char ip_str[INET_ADDRSTRLEN];
    
    client_sock = accept(listener, (struct sockaddr*)&client_addr, &addr_len);
    if (!socket_is_valid(client_sock)) {
        int err = socket_get_last_error();
#ifdef _WIN32
        if (err != WSAEWOULDBLOCK)
#else
        if (err != EWOULDBLOCK && err != EAGAIN)
#endif
        {
            log_error("accept() failed: %s", socket_get_error_string(err));
        }
        return INVALID_SOCKET_VALUE;
    }
    
    /* Set client socket to non-blocking */
    if (!socket_set_nonblocking(client_sock)) {
        closesocket(client_sock);
        return INVALID_SOCKET_VALUE;
    }
    
    /* Format client info string */
    if (client_info && info_len > 0) {
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        snprintf(client_info, info_len, "%s:%d", ip_str, ntohs(client_addr.sin_port));
        log_debug("Accepted connection from %s", client_info);
    }
    
    return client_sock;
}

void socket_close(socket_t sock) {
    if (socket_is_valid(sock)) {
        closesocket(sock);
    }
}

int socket_send(socket_t sock, const void *buffer, int length) {
#ifdef _WIN32
    int n = send(sock, (const char*)buffer, length, 0);
#else
    int n = (int)send(sock, buffer, (size_t)length, 0);
#endif
    if (n < 0) {
        int err = socket_get_last_error();
#ifdef _WIN32
        if (err == WSAEWOULDBLOCK) {
            return 0; /* Would block, return 0 */
        }
#else
        if (err == EWOULDBLOCK || err == EAGAIN) {
            return 0; /* Would block, return 0 */
        }
#endif
        /* Actual error */
        return -1;
    }
    return n;
}

int socket_recv(socket_t sock, void *buffer, int length) {
#ifdef _WIN32
    int n = recv(sock, (char*)buffer, length, 0);
#else
    int n = (int)recv(sock, buffer, (size_t)length, 0);
#endif
    if (n < 0) {
        int err = socket_get_last_error();
#ifdef _WIN32
        if (err == WSAEWOULDBLOCK) {
            return 0; /* Would block, return 0 */
        }
#else
        if (err == EWOULDBLOCK || err == EAGAIN) {
            return 0; /* Would block, return 0 */
        }
#endif
        /* Actual error */
        return -1;
    }
    return n;
}
