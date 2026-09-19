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
 * L0 of the socket stack.  See socket_fd.h for the contract and
 * docs/socket_layering_design.md for why the layer exists.
 *
 * The structure here is deliberately different from src/utils/socket.c.  That
 * file carries two whole implementations under one top-level #ifdef _WIN32
 * because only about half of its two bodies matched and the divergence ran
 * through the middle of every function.  These functions are small enough
 * that the divergence is down to individual statements, so the #ifdefs are
 * local: a handful of one-line helpers at the top absorb the differences and
 * the bodies below are written once.
 */

#include <utils/socket_fd.h>

#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <string.h>
#include <utils/debug.h>

#ifdef _WIN32
/* socket_fd.h has already pulled in winsock2.h, windows.h and ws2tcpip.h in that order. */
#else
#    include <arpa/inet.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <netdb.h>
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#    include <poll.h>
#    include <sys/socket.h>
#    include <sys/types.h>
#    include <unistd.h>
#endif


/*
 * The *BSD family, macOS included, suppresses SIGPIPE with a socket option
 * rather than a send() flag.  Linux does the opposite.  Windows has neither
 * signals nor MSG_NOSIGNAL.
 */
#ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL (0)
#endif


/* ========================================================================
 * Platform seams.  Everything below this block is written once.
 * ======================================================================== */

/* the platform's "the call did not fail" sentinel for a socket handle */
#ifdef _WIN32
#    define SOCK_CALL_FAILED (SOCKET_ERROR)
#else
#    define SOCK_CALL_FAILED (-1)
#endif


/* last error from a socket call, as the platform reports it */
static int32_t sock_last_error(void) {
#ifdef _WIN32
    return (int32_t)WSAGetLastError();
#else
    return (int32_t)errno;
#endif
}


/* is that error code the platform's "try again later"? */
static bool sock_err_is_would_block(int32_t err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}


/* is that error code the platform's "non-blocking connect is under way"? */
static bool sock_err_is_in_progress(int32_t err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY;
#else
    return err == EINPROGRESS || err == EALREADY;
#endif
}


/* is that error code an interrupted call worth retrying? */
static bool sock_err_is_interrupted(int32_t err) {
#ifdef _WIN32
    return err == WSAEINTR;
#else
    return err == EINTR;
#endif
}


/*
 * Map a platform error onto a libplctag status.  Deliberately coarse: a
 * caller that needs the exact code is asking L0 the wrong question.
 */
static int32_t sock_translate_error(int32_t err) {
    if(sock_err_is_would_block(err)) { return PLCTAG_STATUS_PENDING; }

#ifdef _WIN32
    switch(err) {
        case WSAECONNRESET:
        case WSAECONNABORTED:
        case WSAENETRESET:
        case WSAESHUTDOWN:
        case WSAENOTCONN: return PLCTAG_ERR_BAD_CONNECTION;

        case WSAECONNREFUSED:
        case WSAEHOSTUNREACH:
        case WSAENETUNREACH:
        case WSAENETDOWN: return PLCTAG_ERR_OPEN;

        case WSAETIMEDOUT: return PLCTAG_ERR_TIMEOUT;

        case WSAEACCES: return PLCTAG_ERR_NOT_ALLOWED;

        case WSAEMFILE:
        case WSAENOBUFS: return PLCTAG_ERR_NO_RESOURCES;

        default: return PLCTAG_ERR_BAD_STATUS;
    }
#else
    switch(err) {
        case ECONNRESET:
        case ECONNABORTED:
        case EPIPE:
        case ENOTCONN: return PLCTAG_ERR_BAD_CONNECTION;

        case ECONNREFUSED:
        case EHOSTUNREACH:
        case ENETUNREACH:
        case ENETDOWN: return PLCTAG_ERR_OPEN;

        case ETIMEDOUT: return PLCTAG_ERR_TIMEOUT;

        case EACCES:
        case EPERM: return PLCTAG_ERR_NOT_ALLOWED;

        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM: return PLCTAG_ERR_NO_RESOURCES;

        default: return PLCTAG_ERR_BAD_STATUS;
    }
#endif
}


/* ========================================================================
 * Library startup
 * ======================================================================== */

extern int32_t socket_fd_lib_startup(void) {
#ifdef _WIN32
    /*
     * WSAStartup() is reference counted by the OS: each call must be matched
     * by a WSACleanup(), and only the last one tears anything down.  So there
     * is no need to track this ourselves.
     */
    WSADATA wsa_data;

    if(WSAStartup(MAKEWORD(2, 2), &wsa_data) != NO_ERROR) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to start Windows Sockets!");
        return PLCTAG_ERR_WINSOCK;
    }
#endif

    return PLCTAG_STATUS_OK;
}


extern int32_t socket_fd_lib_shutdown(void) {
#ifdef _WIN32
    WSACleanup();
#endif

    return PLCTAG_STATUS_OK;
}


/* ========================================================================
 * Handles and options
 * ======================================================================== */

static int32_t sock_open(socket_fd_t *fd, int32_t sock_type, int32_t protocol) {
    int32_t rc = PLCTAG_STATUS_OK;

    if(!fd) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer to socket handle!");
        return PLCTAG_ERR_NULL_PTR;
    }

    *fd = SOCKET_FD_INVALID;

    rc = socket_fd_lib_startup();
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    *fd = (socket_fd_t)socket(AF_INET, sock_type, protocol);

    if(*fd == SOCKET_FD_INVALID) {
        int32_t err = sock_last_error();

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to create socket, error %" PRId32 "!", err);

        return sock_translate_error(err);
    }

    /*
     * Every socket this layer hands out is non-blocking.  That is the whole
     * point of the layer, so it is not left to the caller to remember.
     */
    rc = socket_fd_set_nonblocking(*fd, true);
    if(rc != PLCTAG_STATUS_OK) {
        socket_fd_close(fd);
        return rc;
    }

    return PLCTAG_STATUS_OK;
}


extern int32_t socket_fd_open_tcp(socket_fd_t *fd) { return sock_open(fd, SOCK_STREAM, IPPROTO_TCP); }


extern int32_t socket_fd_open_udp(socket_fd_t *fd) { return sock_open(fd, SOCK_DGRAM, IPPROTO_UDP); }


extern int32_t socket_fd_close(socket_fd_t *fd) {
    int32_t rc = PLCTAG_STATUS_OK;

    if(!fd) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer to socket handle!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(*fd == SOCKET_FD_INVALID) { return PLCTAG_STATUS_OK; }

#ifdef _WIN32
    if(closesocket(*fd) == SOCK_CALL_FAILED) {
#else
    if(close(*fd) == SOCK_CALL_FAILED) {
#endif
        int32_t err = sock_last_error();

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error closing socket, error %" PRId32 "!", err);
        rc = sock_translate_error(err);
    }

    *fd = SOCKET_FD_INVALID;

    return rc;
}


extern int32_t socket_fd_set_nonblocking(socket_fd_t fd, bool on) {
#ifdef _WIN32
    u_long mode = on ? 1UL : 0UL;

    if(ioctlsocket(fd, (long)FIONBIO, &mode) == SOCK_CALL_FAILED) {
        int32_t err = sock_last_error();

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to set non-blocking mode, error %" PRId32 "!", err);

        return sock_translate_error(err);
    }
#else
    int32_t flags = (int32_t)fcntl(fd, F_GETFL, 0);

    if(flags == SOCK_CALL_FAILED) {
        int32_t err = sock_last_error();

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to read socket flags, error %" PRId32 "!", err);

        return sock_translate_error(err);
    }

    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);

    if(fcntl(fd, F_SETFL, flags) == SOCK_CALL_FAILED) {
        int32_t err = sock_last_error();

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to set non-blocking mode, error %" PRId32 "!", err);

        return sock_translate_error(err);
    }
#endif

    return PLCTAG_STATUS_OK;
}


/*
 * All four boolean options go through here.  setsockopt() wants a char * on
 * Windows and a void * on POSIX, which is the only difference between them.
 */
static int32_t sock_set_bool_option(socket_fd_t fd, int32_t level, int32_t option, bool on, const char *name) {
    int32_t value = on ? 1 : 0;

#ifdef _WIN32
    if(setsockopt(fd, level, option, (const char *)&value, (int)sizeof(value)) == SOCK_CALL_FAILED) {
#else
    if(setsockopt(fd, level, option, &value, (socklen_t)sizeof(value)) == SOCK_CALL_FAILED) {
#endif
        int32_t err = sock_last_error();

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to set %s, error %" PRId32 "!", name, err);

        return sock_translate_error(err);
    }

    return PLCTAG_STATUS_OK;
}


extern int32_t socket_fd_set_nodelay(socket_fd_t fd, bool on) {
    return sock_set_bool_option(fd, IPPROTO_TCP, TCP_NODELAY, on, "TCP_NODELAY");
}


extern int32_t socket_fd_set_reuseaddr(socket_fd_t fd, bool on) {
    return sock_set_bool_option(fd, SOL_SOCKET, SO_REUSEADDR, on, "SO_REUSEADDR");
}


extern int32_t socket_fd_set_broadcast(socket_fd_t fd, bool on) {
    return sock_set_bool_option(fd, SOL_SOCKET, SO_BROADCAST, on, "SO_BROADCAST");
}


extern int32_t socket_fd_get_error(socket_fd_t fd) {
    int32_t sock_error = 0;

#ifdef _WIN32
    int sock_error_len = (int)sizeof(sock_error);

    if(getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&sock_error, &sock_error_len) == SOCK_CALL_FAILED) {
#else
    socklen_t sock_error_len = (socklen_t)sizeof(sock_error);

    if(getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_error, &sock_error_len) == SOCK_CALL_FAILED) {
#endif
        return sock_translate_error(sock_last_error());
    }

    if(sock_error != 0) { return sock_translate_error(sock_error); }

    return PLCTAG_STATUS_OK;
}


/* ========================================================================
 * Addresses
 * ======================================================================== */

extern int32_t socket_fd_addr_init(socket_fd_addr_t *addr, const char *host, int32_t port) {
    struct sockaddr_in *sin = NULL;

    if(!addr || !host) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer passed for address or host!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(port < 0 || port > 65535) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Port %" PRId32 " is out of range!", port);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    memset(addr, 0, sizeof(*addr));

    sin = (struct sockaddr_in *)&(addr->storage[0]);
    sin->sin_family = AF_INET;
    sin->sin_port = htons((uint16_t)port);
    addr->length = (uint32_t)sizeof(struct sockaddr_in);

    /*
     * A dotted-quad goes straight in.  Anything else is a name, and that
     * needs the resolver, which is the one call in this file that can block.
     * That is a property of DNS, not of the socket, and there is nowhere
     * better to put it.
     */
    if(inet_pton(AF_INET, host, &(sin->sin_addr)) == 1) { return PLCTAG_STATUS_OK; }

    {
        struct addrinfo hints;
        struct addrinfo *result = NULL;
        int32_t rc = 0;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        rc = (int32_t)getaddrinfo(host, NULL, &hints, &result);
        if(rc != 0 || !result) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to resolve host name \"%s\"!", host);

            if(result) { freeaddrinfo(result); }

            return PLCTAG_ERR_BAD_GATEWAY;
        }

        sin->sin_addr = ((struct sockaddr_in *)(result->ai_addr))->sin_addr;

        freeaddrinfo(result);
    }

    return PLCTAG_STATUS_OK;
}


extern int32_t socket_fd_addr_str(const socket_fd_addr_t *addr, char *buf, int32_t buf_capacity) {
    const struct sockaddr_in *sin = NULL;

    if(!addr || !buf) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer passed for address or buffer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(buf_capacity < INET_ADDRSTRLEN) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Buffer of %" PRId32 " bytes is too small for an address!", buf_capacity);
        return PLCTAG_ERR_TOO_SMALL;
    }

    sin = (const struct sockaddr_in *)&(addr->storage[0]);

    /* inet_ntop()'s size argument is a socklen_t on POSIX and a size_t on Windows */
#ifdef _WIN32
    if(!inet_ntop(AF_INET, &(sin->sin_addr), buf, (size_t)buf_capacity)) {
#else
    if(!inet_ntop(AF_INET, &(sin->sin_addr), buf, (socklen_t)buf_capacity)) {
#endif
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to format address!");
        return PLCTAG_ERR_BAD_DATA;
    }

    return PLCTAG_STATUS_OK;
}


extern int32_t socket_fd_addr_port(const socket_fd_addr_t *addr) {
    const struct sockaddr_in *sin = NULL;

    if(!addr) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer passed for address!");
        return PLCTAG_ERR_NULL_PTR;
    }

    sin = (const struct sockaddr_in *)&(addr->storage[0]);

    return (int32_t)ntohs(sin->sin_port);
}


/* ========================================================================
 * Client
 * ======================================================================== */

extern int32_t socket_fd_connect_start(socket_fd_t fd, const char *host, int32_t port) {
    socket_fd_addr_t addr;
    int32_t rc = PLCTAG_STATUS_OK;

    rc = socket_fd_addr_init(&addr, host, port);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    /* a TCP client wants Nagle off; a failure here is not fatal to the connect. */
    socket_fd_set_nodelay(fd, true);

    if(connect(fd, (struct sockaddr *)&(addr.storage[0]), (socklen_t)addr.length) == SOCK_CALL_FAILED) {
        int32_t err = sock_last_error();

        if(sock_err_is_in_progress(err)) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Connection to %s:%" PRId32 " is in progress.", host, port);
            return PLCTAG_STATUS_PENDING;
        }

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to connect to %s:%" PRId32 ", error %" PRId32 "!", host, port, err);

        return sock_translate_error(err);
    }

    /* connected immediately, which happens for loopback */
    return PLCTAG_STATUS_OK;
}


extern int32_t socket_fd_connect_check(socket_fd_t fd) {
    /*
     * A socket in the middle of a non-blocking connect becomes writable when
     * the attempt resolves, either way.  SO_ERROR is what says which.
     */
    return socket_fd_get_error(fd);
}


/* ========================================================================
 * Server
 * ======================================================================== */

extern int32_t socket_fd_bind_listen(socket_fd_t fd, const char *host, int32_t port, int32_t backlog) {
    socket_fd_addr_t addr;
    int32_t rc = PLCTAG_STATUS_OK;

    /*
     * An empty host means "every interface".  Callers that want a specific
     * one pass it, and it goes through the same resolution as a client.
     */
    if(!host || *host == '\0') {
        struct sockaddr_in *sin = NULL;

        memset(&addr, 0, sizeof(addr));

        sin = (struct sockaddr_in *)&(addr.storage[0]);
        sin->sin_family = AF_INET;
        sin->sin_port = htons((uint16_t)port);
        sin->sin_addr.s_addr = htonl(INADDR_ANY);
        addr.length = (uint32_t)sizeof(struct sockaddr_in);
    } else {
        rc = socket_fd_addr_init(&addr, host, port);
        if(rc != PLCTAG_STATUS_OK) { return rc; }
    }

    /*
     * Without SO_REUSEADDR a server that just exited leaves its port in
     * TIME_WAIT and the next start fails.  A test suite restarts these
     * constantly, so this is not optional.
     */
    rc = socket_fd_set_reuseaddr(fd, true);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(bind(fd, (struct sockaddr *)&(addr.storage[0]), (socklen_t)addr.length) == SOCK_CALL_FAILED) {
        int32_t err = sock_last_error();

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to bind to port %" PRId32 ", error %" PRId32 "!", port, err);

        return sock_translate_error(err);
    }

    if(listen(fd, (int)backlog) == SOCK_CALL_FAILED) {
        int32_t err = sock_last_error();

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to listen on port %" PRId32 ", error %" PRId32 "!", port, err);

        return sock_translate_error(err);
    }

    return PLCTAG_STATUS_OK;
}


extern int32_t socket_fd_accept(socket_fd_t listener, socket_fd_t *client_fd, socket_fd_addr_t *client_addr) {
    socket_fd_addr_t addr;
    socklen_t addr_len = (socklen_t)sizeof(addr.storage);
    socket_fd_t new_fd = SOCKET_FD_INVALID;
    int32_t rc = PLCTAG_STATUS_OK;

    if(!client_fd) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer to client socket handle!");
        return PLCTAG_ERR_NULL_PTR;
    }

    *client_fd = SOCKET_FD_INVALID;

    memset(&addr, 0, sizeof(addr));

    new_fd = (socket_fd_t)accept(listener, (struct sockaddr *)&(addr.storage[0]), &addr_len);

    if(new_fd == SOCKET_FD_INVALID) {
        int32_t err = sock_last_error();

        if(sock_err_is_would_block(err) || sock_err_is_interrupted(err)) { return PLCTAG_STATUS_PENDING; }

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to accept connection, error %" PRId32 "!", err);

        return sock_translate_error(err);
    }

    addr.length = (uint32_t)addr_len;

    /*
     * accept() does not inherit O_NONBLOCK from the listener on every
     * platform -- POSIX explicitly leaves it unspecified and Linux does not
     * inherit it -- so the new handle is set here rather than assumed.
     */
    rc = socket_fd_set_nonblocking(new_fd, true);
    if(rc != PLCTAG_STATUS_OK) {
        socket_fd_close(&new_fd);
        return rc;
    }

    socket_fd_set_nodelay(new_fd, true);

    *client_fd = new_fd;

    if(client_addr) { *client_addr = addr; }

    return PLCTAG_STATUS_OK;
}


/* ========================================================================
 * Transfer
 * ======================================================================== */

extern int32_t socket_fd_recv(socket_fd_t fd, uint8_t *buf, int32_t len, int32_t *count) {
    int32_t bytes = 0;

    if(!buf || !count) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer passed for buffer or count!");
        return PLCTAG_ERR_NULL_PTR;
    }

    *count = 0;

    if(len < 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Negative length %" PRId32 "!", len);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(len == 0) { return PLCTAG_STATUS_OK; }

#ifdef _WIN32
    bytes = (int32_t)recv(fd, (char *)buf, (int)len, 0);
#else
    bytes = (int32_t)recv(fd, buf, (size_t)len, 0);
#endif

    if(bytes == SOCK_CALL_FAILED) {
        int32_t err = sock_last_error();

        if(sock_err_is_would_block(err) || sock_err_is_interrupted(err)) { return PLCTAG_STATUS_PENDING; }

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket read error %" PRId32 "!", err);

        return sock_translate_error(err);
    }

    /*
     * Zero bytes from a stream socket means the peer sent FIN.  It is
     * reported as OK with a count of zero and the caller decides what an
     * orderly close means to it -- that is a protocol question, not a socket
     * one.
     */
    *count = bytes;

    return PLCTAG_STATUS_OK;
}


extern int32_t socket_fd_send(socket_fd_t fd, const uint8_t *buf, int32_t len, int32_t *count) {
    int32_t bytes = 0;

    if(!buf || !count) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer passed for buffer or count!");
        return PLCTAG_ERR_NULL_PTR;
    }

    *count = 0;

    if(len < 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Negative length %" PRId32 "!", len);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(len == 0) { return PLCTAG_STATUS_OK; }

#ifdef _WIN32
    bytes = (int32_t)send(fd, (const char *)buf, (int)len, 0);
#else
    bytes = (int32_t)send(fd, buf, (size_t)len, MSG_NOSIGNAL);
#endif

    if(bytes == SOCK_CALL_FAILED) {
        int32_t err = sock_last_error();

        if(sock_err_is_would_block(err) || sock_err_is_interrupted(err)) { return PLCTAG_STATUS_PENDING; }

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket write error %" PRId32 "!", err);

        return sock_translate_error(err);
    }

    *count = bytes;

    return PLCTAG_STATUS_OK;
}


/* ========================================================================
 * UDP
 * ======================================================================== */

extern int32_t socket_fd_recvfrom(socket_fd_t fd, uint8_t *buf, int32_t len, socket_fd_addr_t *from, int32_t *count) {
    socket_fd_addr_t addr;
    socklen_t addr_len = (socklen_t)sizeof(addr.storage);
    int32_t bytes = 0;

    if(!buf || !count) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer passed for buffer or count!");
        return PLCTAG_ERR_NULL_PTR;
    }

    *count = 0;

    if(len <= 0) { return PLCTAG_ERR_OUT_OF_BOUNDS; }

    memset(&addr, 0, sizeof(addr));

#ifdef _WIN32
    bytes = (int32_t)recvfrom(fd, (char *)buf, (int)len, 0, (struct sockaddr *)&(addr.storage[0]), &addr_len);
#else
    bytes = (int32_t)recvfrom(fd, buf, (size_t)len, 0, (struct sockaddr *)&(addr.storage[0]), &addr_len);
#endif

    if(bytes == SOCK_CALL_FAILED) {
        int32_t err = sock_last_error();

        if(sock_err_is_would_block(err) || sock_err_is_interrupted(err)) { return PLCTAG_STATUS_PENDING; }

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket recvfrom error %" PRId32 "!", err);

        return sock_translate_error(err);
    }

    addr.length = (uint32_t)addr_len;

    *count = bytes;

    if(from) { *from = addr; }

    return PLCTAG_STATUS_OK;
}


extern int32_t socket_fd_sendto(socket_fd_t fd, const uint8_t *buf, int32_t len, const socket_fd_addr_t *to, int32_t *count) {
    int32_t bytes = 0;

    if(!buf || !to || !count) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer passed for buffer, address or count!");
        return PLCTAG_ERR_NULL_PTR;
    }

    *count = 0;

    if(len < 0) { return PLCTAG_ERR_OUT_OF_BOUNDS; }

#ifdef _WIN32
    bytes = (int32_t)sendto(fd, (const char *)buf, (int)len, 0, (const struct sockaddr *)&(to->storage[0]),
                            (int)to->length);
#else
    bytes = (int32_t)sendto(fd, buf, (size_t)len, MSG_NOSIGNAL, (const struct sockaddr *)&(to->storage[0]),
                            (socklen_t)to->length);
#endif

    if(bytes == SOCK_CALL_FAILED) {
        int32_t err = sock_last_error();

        if(sock_err_is_would_block(err) || sock_err_is_interrupted(err)) { return PLCTAG_STATUS_PENDING; }

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket sendto error %" PRId32 "!", err);

        return sock_translate_error(err);
    }

    *count = bytes;

    return PLCTAG_STATUS_OK;
}


/* ========================================================================
 * Wake pair and readiness
 * ======================================================================== */

extern int32_t socket_fd_pair(socket_fd_t *fd_read, socket_fd_t *fd_write) {
    int32_t rc = PLCTAG_STATUS_OK;

    if(!fd_read || !fd_write) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer to socket handle!");
        return PLCTAG_ERR_NULL_PTR;
    }

    *fd_read = SOCKET_FD_INVALID;
    *fd_write = SOCKET_FD_INVALID;

    rc = socket_fd_lib_startup();
    if(rc != PLCTAG_STATUS_OK) { return rc; }

#ifndef _WIN32
    {
        int32_t fds[2] = {-1, -1};

        if(socketpair(PF_LOCAL, SOCK_STREAM, 0, fds) == SOCK_CALL_FAILED) {
            int32_t err = sock_last_error();

            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to create socket pair, error %" PRId32 "!", err);

            return sock_translate_error(err);
        }

        *fd_read = (socket_fd_t)fds[0];
        *fd_write = (socket_fd_t)fds[1];
    }
#else
    /*
     * Windows has no socketpair(), so the pair is built by hand: listen on
     * an ephemeral loopback port, connect to it, accept, throw the listener
     * away.  The listener is bound to 127.0.0.1 rather than INADDR_ANY so
     * that nothing off the machine can reach it in the window it exists.
     */
    {
        socket_fd_t listener = SOCKET_FD_INVALID;
        struct sockaddr_in listener_addr;
        int listener_addr_len = (int)sizeof(listener_addr);

        rc = socket_fd_open_tcp(&listener);
        if(rc != PLCTAG_STATUS_OK) { return rc; }

        memset(&listener_addr, 0, sizeof(listener_addr));
        listener_addr.sin_family = AF_INET;
        listener_addr.sin_port = htons(0);
        listener_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        do {
            if(bind(listener, (struct sockaddr *)&listener_addr, (int)sizeof(listener_addr)) == SOCK_CALL_FAILED) {
                rc = sock_translate_error(sock_last_error());
                break;
            }

            if(listen(listener, 1) == SOCK_CALL_FAILED) {
                rc = sock_translate_error(sock_last_error());
                break;
            }

            /* find out which port bind() picked */
            if(getsockname(listener, (struct sockaddr *)&listener_addr, &listener_addr_len) == SOCK_CALL_FAILED) {
                rc = sock_translate_error(sock_last_error());
                break;
            }

            rc = socket_fd_open_tcp(fd_write);
            if(rc != PLCTAG_STATUS_OK) { break; }

            /*
             * Both ends are non-blocking, so this connect returns "in
             * progress" and the accept below completes it.  Over loopback
             * with a listener already waiting, the handshake is immediate.
             */
            if(connect(*fd_write, (struct sockaddr *)&listener_addr, (int)sizeof(listener_addr)) == SOCK_CALL_FAILED) {
                int32_t err = sock_last_error();

                if(!sock_err_is_in_progress(err)) {
                    rc = sock_translate_error(err);
                    break;
                }
            }

            {
                int32_t attempts = 0;

                /*
                 * The connect is in flight on the same machine; accept()
                 * may still report would-block for an instant.  Spin a
                 * bounded number of times rather than sleeping, because
                 * nothing in this layer is allowed to sleep.
                 */
                for(attempts = 0; attempts < 1000; attempts++) {
                    rc = socket_fd_accept(listener, fd_read, NULL);

                    if(rc != PLCTAG_STATUS_PENDING) { break; }
                }

                if(rc == PLCTAG_STATUS_PENDING) { rc = PLCTAG_ERR_TIMEOUT; }
            }

            if(rc != PLCTAG_STATUS_OK) { break; }

            rc = socket_fd_connect_check(*fd_write);
        } while(0);

        socket_fd_close(&listener);

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to create wake pair, error %s!", plc_tag_decode_error(rc));

            socket_fd_close(fd_read);
            socket_fd_close(fd_write);

            return rc;
        }
    }
#endif

    /* the pair is only ever poked, so Nagle would only add latency */
    socket_fd_set_nodelay(*fd_read, true);
    socket_fd_set_nodelay(*fd_write, true);

    if((rc = socket_fd_set_nonblocking(*fd_read, true)) != PLCTAG_STATUS_OK
       || (rc = socket_fd_set_nonblocking(*fd_write, true)) != PLCTAG_STATUS_OK) {
        socket_fd_close(fd_read);
        socket_fd_close(fd_write);

        return rc;
    }

    return PLCTAG_STATUS_OK;
}


extern int32_t socket_fd_poll(socket_fd_poll_item_t *items, int32_t item_count, int32_t timeout_ms, int32_t *ready_count) {
#ifdef _WIN32
    WSAPOLLFD native[SOCKET_FD_POLL_MAX];
#else
    struct pollfd native[SOCKET_FD_POLL_MAX];
#endif
    int32_t index = 0;
    int32_t rc = 0;

    if(!items || !ready_count) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer passed for items or count!");
        return PLCTAG_ERR_NULL_PTR;
    }

    *ready_count = 0;

    if(item_count < 0 || item_count > SOCKET_FD_POLL_MAX) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Item count %" PRId32 " is out of range!", item_count);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(item_count == 0) { return PLCTAG_STATUS_OK; }

    for(index = 0; index < item_count; index++) {
        memset(&(native[index]), 0, sizeof(native[index]));

        native[index].fd = items[index].fd;
        native[index].events = 0;

        if(items[index].want & SOCKET_FD_POLL_READ) { native[index].events = (short)(native[index].events | POLLIN); }
        if(items[index].want & SOCKET_FD_POLL_WRITE) { native[index].events = (short)(native[index].events | POLLOUT); }

        items[index].got = 0;
    }

#ifdef _WIN32
    rc = (int32_t)WSAPoll(native, (ULONG)item_count, (INT)timeout_ms);
#else
    rc = (int32_t)poll(native, (nfds_t)item_count, (int)timeout_ms);
#endif

    if(rc == SOCK_CALL_FAILED) {
        int32_t err = sock_last_error();

        /* an interrupted wait is not an error; it is an empty result */
        if(sock_err_is_interrupted(err)) { return PLCTAG_STATUS_OK; }

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Poll failed, error %" PRId32 "!", err);

        return sock_translate_error(err);
    }

    if(rc == 0) { return PLCTAG_STATUS_OK; }

    for(index = 0; index < item_count; index++) {
        int16_t got = 0;

        if(native[index].revents & POLLIN) { got = (int16_t)(got | SOCKET_FD_POLL_READ); }
        if(native[index].revents & POLLOUT) { got = (int16_t)(got | SOCKET_FD_POLL_WRITE); }
        if(native[index].revents & POLLERR) { got = (int16_t)(got | SOCKET_FD_POLL_ERR); }
        if(native[index].revents & POLLHUP) { got = (int16_t)(got | SOCKET_FD_POLL_HUP); }

        /*
         * POLLNVAL means the handle is not open.  Report it as an error so
         * the caller drops the socket rather than spinning on it forever,
         * which is what happens if it is silently ignored.
         */
        if(native[index].revents & POLLNVAL) { got = (int16_t)(got | SOCKET_FD_POLL_ERR); }

        items[index].got = got;
    }

    *ready_count = rc;

    return PLCTAG_STATUS_OK;
}
