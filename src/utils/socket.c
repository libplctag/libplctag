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

/*
 * TCP client sockets.  See utils/socket.h for what this API provides.
 *
 * Unlike the other utils/ modules, the two platform implementations are kept
 * whole and separate rather than folded into shared public functions over a
 * thin platform seam.  Roughly half the lines differ, and the divergence runs
 * through the middle of every function: select()/fd_set against WSAPoll,
 * errno against WSAGetLastError, socketpair() against a self-connected TCP
 * pair.  Forcing them together would be a rewrite, not a move.
 *
 * The merge happens instead in the layering described by
 * docs/socket_layering_design.md, where a non-blocking socket_fd layer
 * becomes the single home of every #ifdef in the socket stack.  Until then
 * these two bodies are what was in the platform shims, unchanged.
 */

#ifdef _WIN32

/* KEEP THE SPACES BETWEEN THE INCLUDES.  The order is required! */
#    include <winsock2.h>

#    include <windows.h>

#    include <ws2tcpip.h>

/*
 * WinSock has neither MSG_NOSIGNAL nor signals to suppress.  Every other socket
 * implementation in the tree defines this the same way locally; this one used to
 * get it from the Windows platform shim.
 */
#    define MSG_NOSIGNAL 0

#else

#    include <arpa/inet.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <netdb.h>
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#    include <sys/socket.h>
#    include <sys/time.h>
#    include <sys/types.h>
#    include <unistd.h>

/*
 * The *BSD family, macOS included, suppresses SIGPIPE with a socket option
 * rather than a per-call flag.  This is the same test platform.c used.
 */
#    if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__bsdi__) \
        || defined(__DragonFly__)
#        define BSD_OS_TYPE
#    endif

#endif

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libplctag/lib/libplctag.h>
#include <utils/mem.h>
#include <utils/debug.h>
#include <utils/socket.h>


#ifdef _WIN32

struct sock_t {
    SOCKET fd;
    int port;
};


#define MAX_IPS (8)



/*
 * Windows needs to have the Winsock library initialized
 * before use. Does it need to be static?
 *
 * Also set the timer period to handle the newer Windows 10 case
 * where it gets set fairly large (>15ms).
 */

static WSADATA wsaData = {0};

static int socket_lib_init(void) {
    // MMRESULT rc = 0;

    /*
    rc = timeBeginPeriod(WINDOWS_REQUESTED_TIMER_PERIOD_MS);
    if(rc != TIMERR_NOERROR) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, "Unable to set timer period to %ums!", WINDOWS_REQUESTED_TIMER_PERIOD_MS);
    }
    */

    return WSAStartup(MAKEWORD(2, 2), &wsaData) == NO_ERROR;
}


extern int socket_create(sock_p *s) {
    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Starting.");

    if(!socket_lib_init()) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "error initializing Windows Sockets.");
        return PLCTAG_ERR_WINSOCK;
    }

    if(!s) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "null socket pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    *s = (sock_p)mem_alloc(sizeof(struct sock_t));

    if(!*s) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_ERROR, 0, "Unable to allocate memory for socket!");
        return PLCTAG_ERR_NO_MEM;
    }

    (*s)->fd = INVALID_SOCKET;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


int socket_connect_tcp_start(sock_p s, const char *host, int port) {
    int rc = PLCTAG_STATUS_OK;
    IN_ADDR ips[MAX_IPS];
    int num_ips = 0;
    struct sockaddr_in gw_addr;
    int sock_opt = 1;
    u_long non_blocking = 1;
    int i = 0;
    int done = 0;
    SOCKET fd;
    struct timeval timeout; /* used for timing out connections etc. */

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Starting.");

    /* Open a socket for communication with the gateway. */
    fd = socket(AF_INET, SOCK_STREAM, 0 /*IPPROTO_TCP*/);

    /* check for errors */
    if(fd == INVALID_SOCKET) {
        /*pdebug("Socket creation failed, errno: %d",errno);*/
        return PLCTAG_ERR_OPEN;
    }

    /* set up our socket to allow reuse if we crash suddenly. */
    sock_opt = 1;

    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&sock_opt, (int)sizeof(sock_opt))) {
        /*
         * Winsock reports through WSAGetLastError(), not errno, and closesocket()
         * overwrites the thread's last-error value -- so capture it first.
         */
        int sock_err = WSAGetLastError();

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error setting socket reuse option, WSA error: %d", sock_err);
        closesocket(fd);
        return PLCTAG_ERR_OPEN;
    }

    timeout.tv_sec = 10;
    timeout.tv_usec = 0;

    if(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, (int)sizeof(timeout))) {
        /*
         * Winsock reports through WSAGetLastError(), not errno, and closesocket()
         * overwrites the thread's last-error value -- so capture it first.
         */
        int sock_err = WSAGetLastError();

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error setting socket receive timeout option, WSA error: %d", sock_err);
        closesocket(fd);
        return PLCTAG_ERR_OPEN;
    }

    if(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, (int)sizeof(timeout))) {
        /*
         * Winsock reports through WSAGetLastError(), not errno, and closesocket()
         * overwrites the thread's last-error value -- so capture it first.
         */
        int sock_err = WSAGetLastError();

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error setting socket send timeout option, WSA error: %d", sock_err);
        closesocket(fd);
        return PLCTAG_ERR_OPEN;
    }

    /* figure out what address we are connecting to. */

    /* try a numeric IP address conversion first. */
    if(inet_pton(AF_INET, host, (struct in_addr *)ips) > 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Found numeric IP address: %s", host);
        num_ips = 1;
    } else {
        struct addrinfo hints;
        struct addrinfo *res_head = NULL;
        struct addrinfo *res = NULL;

        mem_set(&ips, 0, sizeof(ips));
        mem_set(&hints, 0, sizeof(hints));

        hints.ai_socktype = SOCK_STREAM; /* TCP */
        hints.ai_family = AF_INET;       /* IP V4 only */

        if((rc = getaddrinfo(host, NULL, &hints, &res_head)) != 0) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error looking up PLC IP address %s, error = %d\n", host, rc);

            if(res_head) { freeaddrinfo(res_head); }

            closesocket(fd);
            return PLCTAG_ERR_BAD_GATEWAY;
        }

        res = res_head;
        for(num_ips = 0; res && num_ips < MAX_IPS; num_ips++) {
            ips[num_ips].s_addr = ((struct sockaddr_in *)(res->ai_addr))->sin_addr.s_addr;
            res = res->ai_next;
        }

        freeaddrinfo(res_head);
    }

    /* set no delay for TCP connections.  Send immediately. */
    sock_opt = 1;
    if(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&sock_opt, sizeof(sock_opt))) {
        /*
         * Winsock reports through WSAGetLastError(), not errno, and closesocket()
         * overwrites the thread's last-error value -- so capture it first.
         */
        int sock_err = WSAGetLastError();

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_ERROR, 0, "Error setting TCP_NODELAY option, WSA error: %d", sock_err);
        closesocket(fd);
        return PLCTAG_ERR_OPEN;
    }

    /* set the socket to non-blocking. */
    if(ioctlsocket(fd, (long)FIONBIO, &non_blocking)) {
        /* Winsock reports through WSAGetLastError(), not errno. */
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error setting socket to non-blocking, WSA error: %d", WSAGetLastError());
        closesocket(fd);
        return PLCTAG_ERR_OPEN;
    }

    /*
     * now try to connect to the remote gateway.  We may need to
     * try several of the IPs we have.
     */

    i = 0;
    done = 0;

    memset((void *)&gw_addr, 0, sizeof(gw_addr));
    gw_addr.sin_family = AF_INET;
    gw_addr.sin_port = htons((uint16_t)(int16_t)port);

    do {
        /* try each IP until we run out or get a connection. */
        gw_addr.sin_addr.s_addr = ips[i].s_addr;

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Attempting to connect to %s", inet_ntoa(*((struct in_addr *)&ips[i])));

        rc = connect(fd, (struct sockaddr *)&gw_addr, sizeof(gw_addr));

        /* connect returns SOCKET_ERROR and a code of WSAEWOULDBLOCK on non-blocking sockets. */
        if(rc == SOCKET_ERROR) {
            int sock_err = WSAGetLastError();
            if(sock_err == WSAEWOULDBLOCK) {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Socket connection attempt %d started successfully.", i);
                rc = PLCTAG_STATUS_PENDING;
                done = 1;
            } else {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0,
                       "Error %d trying to start connection attempt %d process!  Trying next IP address.", sock_err, i);
                i++;
            }
        } else {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Socket connection attempt %d succeeded immediately.", i);
            rc = PLCTAG_STATUS_OK;
            done = 1;
        }
    } while(!done && i < num_ips);

    if(!done) {
        closesocket(fd);
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to connect to any gateway host IP address!");
        return PLCTAG_ERR_OPEN;
    }

    /* save the values */
    s->fd = fd;
    s->port = port;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


int socket_connect_tcp_check(sock_p sock, int timeout_ms) {
    int rc = PLCTAG_STATUS_OK;
    fd_set write_set;
    fd_set err_set;
    struct timeval tv;
    int select_rc = 0;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Starting.");

    if(!sock) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null socket pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* wait for the socket to be ready. */
    tv.tv_sec = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)(timeout_ms % 1000) * (long)(1000);

    /* Windows reports connection errors on the exception/error socket set. */
    FD_ZERO(&write_set);
    FD_SET(sock->fd, &write_set);
    FD_ZERO(&err_set);
    FD_SET(sock->fd, &err_set);

    select_rc = select((int)(sock->fd) + 1, NULL, &write_set, &err_set, &tv);
    if(select_rc == 1) {
        if(FD_ISSET(sock->fd, &write_set)) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket is connected.");
            rc = PLCTAG_STATUS_OK;
        } else if(FD_ISSET(sock->fd, &err_set)) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error connecting!");
            return PLCTAG_ERR_OPEN;
        } else {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "select() returned a 1, but no sockets are selected!");
            return PLCTAG_ERR_OPEN;
        }
    } else if(select_rc == 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket connection not done yet.");
        rc = PLCTAG_ERR_TIMEOUT;
    } else {
        int err = WSAGetLastError();

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "select() has error %d!", err);

        switch(err) {
            case WSAENETDOWN: /* The network subsystem is down */
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "The network subsystem is down!");
                return PLCTAG_ERR_OPEN;
                break;

            case WSANOTINITIALISED: /*Winsock was not initialized. */
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "WSAStartup() was not called to initialize the Winsock subsystem.!");
                return PLCTAG_ERR_OPEN;
                break;

            case WSAEINVAL: /* The arguments to select() were bad. */
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "One or more of the arguments to select() were invalid!");
                return PLCTAG_ERR_OPEN;
                break;

            case WSAEFAULT: /* No mem/resources for select. */
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Insufficient memory or resources for select() to run!");
                return PLCTAG_ERR_NO_MEM;
                break;

            case WSAEINTR: /* A blocking Windows Socket 1.1 call was canceled through WSACancelBlockingCall.  */
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "A blocking Winsock call was canceled!");
                return PLCTAG_ERR_OPEN;
                break;

            case WSAEINPROGRESS: /* A blocking Windows Socket 1.1 call is in progress.  */
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "A blocking Winsock call is in progress!");
                return PLCTAG_ERR_OPEN;
                break;

            case WSAENOTSOCK: /* One or more of the FDs in the set is not a socket. */
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "The fd in the FD set is not a socket!");
                return PLCTAG_ERR_OPEN;
                break;

            default:
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unexpected err %d from select()!", err);
                return PLCTAG_ERR_OPEN;
                break;
        }
    }

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Done.");

    return rc;
}






int socket_read(sock_p s, uint8_t *buf, int size, int timeout_ms) {
    int rc;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Starting.");

    if(!s) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!buf) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Buffer pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(s->fd == INVALID_SOCKET) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket is not open!");
        return PLCTAG_ERR_READ;
    }

    if(timeout_ms < 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Timeout must be zero or positive!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* try to read without waiting.   Saves a system call if it works. */
    rc = recv(s->fd, (char *)buf, size, 0);
    if(rc < 0) {
        int err = WSAGetLastError();

        if(err == WSAEWOULDBLOCK) {
            if(timeout_ms > 0) {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Immediate read attempt did not succeed, now wait for select().");
            } else {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Read resulted in no data.");
            }

            rc = 0;
        } else {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "socket read error rc=%d, errno=%d", rc, err);
            return PLCTAG_ERR_READ;
        }
    }

    /* only wait if we have a timeout and no data and no error. */
    if(rc == 0 && timeout_ms > 0) {
        fd_set read_set;
        TIMEVAL tv;
        int select_rc = 0;

        tv.tv_sec = (long)(timeout_ms / 1000);
        tv.tv_usec = (long)(timeout_ms % 1000) * (long)(1000);

        FD_ZERO(&read_set);

        FD_SET(s->fd, &read_set);

        select_rc = select(1, &read_set, NULL, NULL, &tv);
        if(select_rc == 1) {
            if(FD_ISSET(s->fd, &read_set)) {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket can read data.");
            } else {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "select() returned but socket is not ready to read data!");
                return PLCTAG_ERR_BAD_REPLY;
            }
        } else if(select_rc == 0) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket read timed out.");
            return PLCTAG_ERR_TIMEOUT;
        } else {
            int err = WSAGetLastError();

            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "select() returned status %d!", select_rc);

            switch(err) {
                case WSANOTINITIALISED: /* WSAStartup() not called first. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "WSAStartUp() not called before calling Winsock functions!");
                    return PLCTAG_ERR_BAD_CONFIG;
                    break;

                case WSAEFAULT: /* No mem for internal tables. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Insufficient resources for select() to run!");
                    return PLCTAG_ERR_NO_MEM;
                    break;

                case WSAENETDOWN: /* network subsystem is down. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "The network subsystem is down!");
                    return PLCTAG_ERR_BAD_DEVICE;
                    break;

                case WSAEINVAL: /* timeout is invalid. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "The timeout is invalid!");
                    return PLCTAG_ERR_BAD_PARAM;
                    break;

                case WSAEINTR: /* A blocking call wss cancelled. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "A blocking call was cancelled!");
                    return PLCTAG_ERR_BAD_CONFIG;
                    break;

                case WSAEINPROGRESS: /* A blocking call is already in progress. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "A blocking call is already in progress!");
                    return PLCTAG_ERR_BAD_CONFIG;
                    break;

                case WSAENOTSOCK: /* The descriptor set contains something other than a socket. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "The fd set contains something other than a socket!");
                    return PLCTAG_ERR_BAD_DATA;
                    break;

                default:
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unexpected socket err %d!", err);
                    return PLCTAG_ERR_BAD_STATUS;
                    break;
            }
        }

        /* select() returned saying we can read, so read. */
        rc = recv(s->fd, (char *)buf, size, 0);
        if(rc < 0) {
            int err = WSAGetLastError();

            if(err == WSAEWOULDBLOCK) {
                rc = 0;
            } else {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "socket read error rc=%d, errno=%d", rc, err);
                return PLCTAG_ERR_READ;
            }
        }
    }

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Done: result = %d.", rc);

    return rc;
}


int socket_write(sock_p s, uint8_t *buf, int size, int timeout_ms) {
    int rc;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Starting.");

    if(!s) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!buf) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Buffer pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(s->fd == INVALID_SOCKET) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket is not open!");
        return PLCTAG_ERR_READ;
    }

    if(timeout_ms < 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Timeout must be zero or positive!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    rc = send(s->fd, (const char *)buf, size, 0);
    if(rc < 0) {
        int err = WSAGetLastError();

        if(err == WSAEWOULDBLOCK) {
            if(timeout_ms > 0) {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Immediate write attempt did not succeed, now wait for select().");
            } else {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Write wrote no data.");
            }

            rc = 0;
        } else {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "socket write error rc=%d, errno=%d", rc, err);
            return PLCTAG_ERR_WRITE;
        }
    }

    /* only wait if we have a timeout and no data. */
    if(rc == 0 && timeout_ms > 0) {
        fd_set write_set;
        TIMEVAL tv;
        int select_rc = 0;

        tv.tv_sec = (long)(timeout_ms / 1000);
        tv.tv_usec = (long)(timeout_ms % 1000) * (long)(1000);

        FD_ZERO(&write_set);

        FD_SET(s->fd, &write_set);

        select_rc = select(1, NULL, &write_set, NULL, &tv);
        if(select_rc == 1) {
            if(FD_ISSET(s->fd, &write_set)) {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket can write data.");
            } else {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "select() returned but socket is not ready to write data!");
                return PLCTAG_ERR_BAD_REPLY;
            }
        } else if(select_rc == 0) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket write timed out.");
            return PLCTAG_ERR_TIMEOUT;
        } else {
            int err = WSAGetLastError();

            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "select() returned status %d!", select_rc);

            switch(err) {
                case WSANOTINITIALISED: /* WSAStartup() not called first. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "WSAStartUp() not called before calling Winsock functions!");
                    return PLCTAG_ERR_BAD_CONFIG;
                    break;

                case WSAEFAULT: /* No mem for internal tables. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Insufficient resources for select() to run!");
                    return PLCTAG_ERR_NO_MEM;
                    break;

                case WSAENETDOWN: /* network subsystem is down. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "The network subsystem is down!");
                    return PLCTAG_ERR_BAD_DEVICE;
                    break;

                case WSAEINVAL: /* timeout is invalid. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "The timeout is invalid!");
                    return PLCTAG_ERR_BAD_PARAM;
                    break;

                case WSAEINTR: /* A blocking call wss cancelled. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "A blocking call was cancelled!");
                    return PLCTAG_ERR_BAD_CONFIG;
                    break;

                case WSAEINPROGRESS: /* A blocking call is already in progress. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "A blocking call is already in progress!");
                    return PLCTAG_ERR_BAD_CONFIG;
                    break;

                case WSAENOTSOCK: /* The descriptor set contains something other than a socket. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "The fd set contains something other than a socket!");
                    return PLCTAG_ERR_BAD_DATA;
                    break;

                default:
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unexpected socket err %d!", err);
                    return PLCTAG_ERR_BAD_STATUS;
                    break;
            }
        }

        /* try to write since select() said we could. */
        rc = send(s->fd, (const char *)buf, size, 0);
        if(rc < 0) {
            int err = WSAGetLastError();

            if(err == WSAEWOULDBLOCK) {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "No data written.");
                rc = 0;
            } else {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "socket write error rc=%d, errno=%d", rc, err);
                return PLCTAG_ERR_WRITE;
            }
        }
    }

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Done: result = %d.", rc);

    return rc;
}


int socket_close(sock_p s) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Starting.");

    if(!s) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket pointer or pointer to socket pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(s->fd != INVALID_SOCKET) {
        if(closesocket(s->fd)) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error closing socket!");
            rc = PLCTAG_ERR_CLOSE;
        }

        s->fd = INVALID_SOCKET;
    }

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Done.");

    return rc;
}


int socket_destroy(sock_p *s) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Starting.");

    if(!s || !*s) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket pointer or pointer to socket pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

socket_close(*s);

    mem_free(*s);

    *s = 0;

    if(WSACleanup() != NO_ERROR) { return PLCTAG_ERR_WINSOCK; }

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Done.");

    return rc;
}



#else

#ifndef INVALID_SOCKET
#    define INVALID_SOCKET (-1)
#endif

struct sock_t {
    int fd;
    int port;
};



#define MAX_IPS (8)

extern int socket_create(sock_p *s) {
    pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Starting.");

    if(!s) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "null socket pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    *s = (sock_p)mem_alloc(sizeof(struct sock_t));

    if(!*s) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_ERROR, 0, "Failed to allocate memory for socket.");
        return PLCTAG_ERR_NO_MEM;
    }

    (*s)->fd = INVALID_SOCKET;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


int socket_connect_tcp_start(sock_p s, const char *host, int port) {
    int rc = PLCTAG_STATUS_OK;
    struct in_addr ips[MAX_IPS];
    int num_ips = 0;
    struct sockaddr_in gw_addr;
    int sock_opt = 1;
    int i = 0;
    int done = 0;
    int fd;
    int flags;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Starting.");

    /* Open a socket for communication with the gateway. */
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "socket() created fd=%d", fd);

    /* check for errors */
    if(fd < 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_ERROR, 0, "Socket creation failed, errno: %d", errno);
        return PLCTAG_ERR_OPEN;
    }

    /* set up our socket to allow reuse if we crash suddenly. */
    sock_opt = 1;

    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&sock_opt, sizeof(sock_opt))) {
        /* report before close(), which sets errno of its own on failure. */
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_ERROR, 0, "Error setting socket reuse option, errno: %d", errno);
        close(fd);
        return PLCTAG_ERR_OPEN;
    }

#ifdef BSD_OS_TYPE
    /* The *BSD family has a different way to suppress SIGPIPE on sockets. */
    if(setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (char *)&sock_opt, sizeof(sock_opt))) {
        /* report before close(), which sets errno of its own on failure. */
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_ERROR, 0, "Error setting socket SIGPIPE suppression option, errno: %d", errno);
        close(fd);
        return PLCTAG_ERR_OPEN;
    }
#endif

    /* NOTE: We do NOT set SO_RCVTIMEO or SO_SNDTIMEO here because:
     * 1. We use non-blocking mode with select() for timeout handling
     * 2. SO_RCVTIMEO + non-blocking mode can cause select() to not report readability correctly
     * 3. select() provides finer control over timeout behavior than SO_RCVTIMEO/SO_SNDTIMEO
     * Instead, timeout handling is done via select() in socket_read()
     */

    /* make the socket non-blocking. */
    flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_ERROR, 0, "Error getting socket options, errno: %d", errno);
        close(fd);
        return PLCTAG_ERR_OPEN;
    }

    /* set the non-blocking flag. */
    flags |= O_NONBLOCK;

    if(fcntl(fd, F_SETFL, flags) < 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_ERROR, 0, "Error setting socket to non-blocking, errno: %d", errno);
        close(fd);
        return PLCTAG_ERR_OPEN;
    }

    /* set no delay for TCP connections.  Send immediately. */
    sock_opt = 1;
    if(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&sock_opt, sizeof(sock_opt))) {
        /* report before close(), which sets errno of its own on failure. */
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_ERROR, 0, "Error setting TCP_NODELAY option, errno: %d", errno);
        close(fd);
        return PLCTAG_ERR_OPEN;
    }

    /* figure out what address we are connecting to. */

    /* try a numeric IP address conversion first. */
    if(inet_pton(AF_INET, host, (struct in_addr *)ips) > 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Found numeric IP address: %s", host);
        num_ips = 1;
    } else {
        struct addrinfo hints;
        struct addrinfo *res_head = NULL;
        struct addrinfo *res = NULL;
        int rc = 0;

        mem_set(&ips, 0, sizeof(ips));
        mem_set(&hints, 0, sizeof(hints));

        hints.ai_socktype = SOCK_STREAM; /* TCP */
        hints.ai_family = AF_INET;       /* IP V4 only */

        if((rc = getaddrinfo(host, NULL, &hints, &res_head)) != 0) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error looking up PLC IP address %s, error = %d\n", host, rc);

            if(res_head) { freeaddrinfo(res_head); }

            close(fd);
            return PLCTAG_ERR_BAD_GATEWAY;
        }

        res = res_head;
        for(num_ips = 0; res && num_ips < MAX_IPS; num_ips++) {
            ips[num_ips].s_addr = ((struct sockaddr_in *)(res->ai_addr))->sin_addr.s_addr;
            res = res->ai_next;
        }

        freeaddrinfo(res_head);
    }

    // pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Setting up wake pipe.");
    // if(rc != PLCTAG_STATUS_OK) {
    //     pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to create wake pipe, error %s!", plc_tag_decode_error(rc));
    //     return rc;
    // }

    /* now try to connect to the remote gateway.  We may need to
     * try several of the IPs we have.
     */

    i = 0;
    done = 0;

    // NOLINTNEXTLINE
    memset((void *)&gw_addr, 0, sizeof(gw_addr));

    gw_addr.sin_family = AF_INET;
    gw_addr.sin_port = htons((uint16_t)port);

    do {
        /* try each IP until we run out or get a connection started. */
        gw_addr.sin_addr.s_addr = ips[i].s_addr;

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Attempting to connect to %s:%d", inet_ntoa(*((struct in_addr *)&ips[i])),
               port);

        /* this is done non-blocking. Could be interrupted, so restart if needed.*/
        do { rc = connect(fd, (struct sockaddr *)&gw_addr, sizeof(gw_addr)); } while(rc < 0 && errno == EINTR);

        if(rc == 0) {
            /* instantly connected. */
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Connected instantly to %s:%d.",
                   inet_ntoa(*((struct in_addr *)&ips[i])), port);
            done = 1;
            rc = PLCTAG_STATUS_OK;
        } else if(rc < 0 && (errno == EINPROGRESS)) {
            /* the connection has started. */
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Started connecting to %s:%d successfully.",
                   inet_ntoa(*((struct in_addr *)&ips[i])), port);
            done = 1;
            rc = PLCTAG_STATUS_PENDING;
        } else {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Attempt to connect to %s:%d failed, errno: %d",
                   inet_ntoa(*((struct in_addr *)&ips[i])), port, errno);
            i++;
        }
    } while(!done && i < num_ips);

    if(!done) {
        close(fd);
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_ERROR, 0, "Unable to connect to any gateway host IP address!");
        return PLCTAG_ERR_OPEN;
    }

    /* save the values */
    s->fd = fd;
    s->port = port;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Done with status %s.", plc_tag_decode_error(rc));

    return rc;
}


int socket_connect_tcp_check(sock_p sock, int timeout_ms) {
    int rc = PLCTAG_STATUS_OK;
    fd_set write_set;
    struct timeval tv;
    int select_rc = 0;
    int sock_err = 0;
    socklen_t sock_err_len = (socklen_t)(sizeof(sock_err));


    pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Starting.");

    if(!sock) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null socket pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* wait for the socket to be ready. */
    tv.tv_sec = (time_t)(timeout_ms / 1000);
    tv.tv_usec = (suseconds_t)(timeout_ms % 1000) * (suseconds_t)(1000);

    FD_ZERO(&write_set);

    FD_SET(sock->fd, &write_set);

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "socket_connect_tcp_check: calling select() on fd=%d with timeout_ms=%d",
           sock->fd, timeout_ms);
    select_rc = select(sock->fd + 1, NULL, &write_set, NULL, &tv);

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "socket_connect_tcp_check: select() returned %d, write_set fd_isset=%d",
           select_rc, FD_ISSET(sock->fd, &write_set));

    if(select_rc == 1) {
        if(FD_ISSET(sock->fd, &write_set)) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Socket is probably connected.");
        } else {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "select() returned but socket is not connected!");
            return PLCTAG_ERR_BAD_REPLY;
        }
    } else if(select_rc == 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Socket connection not done yet.");
        return PLCTAG_ERR_TIMEOUT;
    } else {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "select() returned status %d!", select_rc);

        switch(errno) {
            case EBADF: /* bad file descriptor */
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Bad file descriptor used in select()!");
                return PLCTAG_ERR_OPEN;
                break;

            case EINTR: /* signal was caught, this should not happen! */
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "A signal was caught in select() and this should not happen!");
                return PLCTAG_ERR_OPEN;
                break;

            case EINVAL: /* number of FDs was negative or exceeded the max allowed. */
                pdebug(
                    DEBUG_MODULE_SOCKET, DEBUG_WARN, 0,
                    "The number of fds passed to select() was negative or exceeded the allowed limit or the timeout is invalid!");
                return PLCTAG_ERR_OPEN;
                break;

            case ENOMEM: /* No mem for internal tables. */
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Insufficient memory for select() to run!");
                return PLCTAG_ERR_NO_MEM;
                break;

            default:
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unexpected socket err %d!", errno);
                return PLCTAG_ERR_OPEN;
                break;
        }
    }

    /* now make absolutely sure that the connection is ready. */
    /* Use getsockopt to check socket connection status (standard POSIX method) */
    rc = getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &sock_err, &sock_err_len);
    if(rc == 0) {
        /* sock_err has the error. */
        switch(sock_err) {
            case 0: pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "No error, socket is connected."); break;

            case EBADF:
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket fd is not valid!");
                return PLCTAG_ERR_OPEN;
                break;

            case EFAULT:
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "The address passed to getsockopt() is not a valid user address!");
                return PLCTAG_ERR_OPEN;
                break;

            case EINVAL:
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "The size of the socket error result is invalid!");
                return PLCTAG_ERR_OPEN;
                break;

            case ENOPROTOOPT:
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "The option SO_ERROR is not understood at the SOL_SOCKET level!");
                return PLCTAG_ERR_OPEN;
                break;

            case ENOTSOCK:
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "The FD is not a socket!");
                return PLCTAG_ERR_OPEN;
                break;

            case ECONNREFUSED:
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Connection refused!");
                return PLCTAG_ERR_OPEN;
                break;

            default:
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unexpected error %d returned!", sock_err);
                return PLCTAG_ERR_OPEN;
                break;
        }
    } else {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error %d getting socket connection status!", errno);
        return PLCTAG_ERR_OPEN;
    }

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}







int socket_read(sock_p s, uint8_t *buf, int size, int timeout_ms) {
    int rc;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Starting.");

    if(!s) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!buf) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Buffer pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(s->fd == INVALID_SOCKET) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket is not open!");
        return PLCTAG_ERR_READ;
    }

    if(timeout_ms < 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Timeout must be zero or positive!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * Try to read immediately.   If we get data, we skip any other
     * delays.   If we do not, then see if we have a timeout.
     */

    /* The socket is non-blocking. */
    rc = (int)read(s->fd, buf, (size_t)size);
    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "read() returned %d for fd=%d (requested %d bytes)", rc, s->fd, size);

    if(rc < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            if(timeout_ms > 0) {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Would block, will wait for select() on fd=%d", s->fd);
            } else {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Read resulted in no data.");
            }

            rc = 0;
        } else {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket read error: rc=%d, errno=%d (%s)", rc, errno, strerror(errno));
            return PLCTAG_ERR_READ;
        }
    } else if(rc == 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Connection closed by peer (read returned 0) on fd=%d", s->fd);
    } else if(rc > 0 && rc < size) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Partial read on fd=%d: read %d of %d bytes", s->fd, rc, size);
    } else if(rc == size) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Complete read on fd=%d: read all %d bytes", s->fd, size);
    }

    /* only wait if we have a timeout and no error and no data. */
    if(rc == 0 && timeout_ms > 0) {
        fd_set read_set;
        struct timeval tv;
        int select_rc = 0;

        tv.tv_sec = (time_t)(timeout_ms / 1000);
        tv.tv_usec = (suseconds_t)(timeout_ms % 1000) * (suseconds_t)(1000);

        FD_ZERO(&read_set);

        FD_SET(s->fd, &read_set);

        select_rc = select(s->fd + 1, &read_set, NULL, NULL, &tv);
        if(select_rc == 1) {
            if(FD_ISSET(s->fd, &read_set)) {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket can read data.");
            } else {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "select() returned but socket is not ready to read data!");
                return PLCTAG_ERR_BAD_REPLY;
            }
        } else if(select_rc == 0) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket read timed out.");
            return PLCTAG_ERR_TIMEOUT;
        } else {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "select() returned status %d!", select_rc);

            switch(errno) {
                case EBADF: /* bad file descriptor */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Bad file descriptor used in select()!");
                    return PLCTAG_ERR_BAD_PARAM;
                    break;

                case EINTR: /* signal was caught, this should not happen! */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "A signal was caught in select() and this should not happen!");
                    return PLCTAG_ERR_BAD_CONFIG;
                    break;

                case EINVAL: /* number of FDs was negative or exceeded the max allowed. */
                    pdebug(
                        DEBUG_MODULE_SOCKET, DEBUG_WARN, 0,
                        "The number of fds passed to select() was negative or exceeded the allowed limit or the timeout is invalid!");
                    return PLCTAG_ERR_BAD_PARAM;
                    break;

                case ENOMEM: /* No mem for internal tables. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Insufficient memory for select() to run!");
                    return PLCTAG_ERR_NO_MEM;
                    break;

                default:
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unexpected socket err %d!", errno);
                    return PLCTAG_ERR_BAD_STATUS;
                    break;
            }
        }

        /* try to read again. */
        rc = (int)read(s->fd, buf, (size_t)size);
        if(rc < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "No data read.");
                rc = 0;
            } else {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket read error: rc=%d, errno=%d", rc, errno);
                return PLCTAG_ERR_READ;
            }
        }
    }

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Done: result %d.", rc);

    return rc;
}


int socket_write(sock_p s, uint8_t *buf, int size, int timeout_ms) {
    int rc;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Starting.");

    if(!s) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!buf) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Buffer pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(s->fd == INVALID_SOCKET) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket is not open!");
        return PLCTAG_ERR_WRITE;
    }

    if(timeout_ms < 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Timeout must be zero or positive!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * Try to write without waiting.
     *
     * In the case that we can immediately write, then we skip a
     * system call to select().   If we cannot, then we will
     * call select().
     */

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "About to write %d bytes on fd=%d", size, s->fd);

#ifdef BSD_OS_TYPE
    /* On *BSD and macOS, the socket option is set to prevent SIGPIPE. */
    rc = (int)write(s->fd, buf, (size_t)size);
    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "write() returned %d for fd=%d (requested %d bytes)", rc, s->fd, size);
#else
    /* on Linux, we use MSG_NOSIGNAL */
    rc = (int)send(s->fd, buf, (size_t)size, MSG_NOSIGNAL);
    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "send() returned %d for fd=%d (requested %d bytes)", rc, s->fd, size);
#endif

    if(rc < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Would block, no immediate data written on fd=%d", s->fd);
            rc = 0;
        } else {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket write error: rc=%d, errno=%d (%s)", rc, errno, strerror(errno));
            return PLCTAG_ERR_WRITE;
        }
    } else if(rc > 0 && rc < size) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Partial write on fd=%d: wrote %d of %d bytes", s->fd, rc, size);
    } else if(rc == size) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Complete write on fd=%d: wrote all %d bytes", s->fd, size);
    }

    /* only wait if we have a timeout and no error and wrote no data. */
    if(rc == 0 && timeout_ms > 0) {
        fd_set write_set;
        struct timeval tv;
        int select_rc = 0;

        tv.tv_sec = (time_t)(timeout_ms / 1000);
        tv.tv_usec = (suseconds_t)(timeout_ms % 1000) * (suseconds_t)(1000);

        FD_ZERO(&write_set);

        FD_SET(s->fd, &write_set);

        select_rc = select(s->fd + 1, NULL, &write_set, NULL, &tv);
        if(select_rc == 1) {
            if(FD_ISSET(s->fd, &write_set)) {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket can write data.");
            } else {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "select() returned but socket is not ready to write data!");
                return PLCTAG_ERR_BAD_REPLY;
            }
        } else if(select_rc == 0) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket write timed out.");
            return PLCTAG_ERR_TIMEOUT;
        } else {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "select() returned status %d!", select_rc);

            switch(errno) {
                case EBADF: /* bad file descriptor */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Bad file descriptor used in select()!");
                    return PLCTAG_ERR_BAD_PARAM;
                    break;

                case EINTR: /* signal was caught, this should not happen! */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "A signal was caught in select() and this should not happen!");
                    return PLCTAG_ERR_BAD_CONFIG;
                    break;

                case EINVAL: /* number of FDs was negative or exceeded the max allowed. */
                    pdebug(
                        DEBUG_MODULE_SOCKET, DEBUG_WARN, 0,
                        "The number of fds passed to select() was negative or exceeded the allowed limit or the timeout is invalid!");
                    return PLCTAG_ERR_BAD_PARAM;
                    break;

                case ENOMEM: /* No mem for internal tables. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Insufficient memory for select() to run!");
                    return PLCTAG_ERR_NO_MEM;
                    break;

                default:
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unexpected socket err %d!", errno);
                    return PLCTAG_ERR_BAD_STATUS;
                    break;
            }
        }

        /* select() passed and said we can write, so try. */
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "select() indicated fd=%d ready for write, attempting write", s->fd);
#ifdef BSD_OS_TYPE
        /* On *BSD and macOS, the socket option is set to prevent SIGPIPE. */
        rc = (int)write(s->fd, buf, (size_t)size);
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "After select(), write() returned %d for fd=%d (requested %d bytes)", rc,
               s->fd, size);
#else
        /* on Linux, we use MSG_NOSIGNAL */
        rc = (int)send(s->fd, buf, (size_t)size, MSG_NOSIGNAL);
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "After select(), send() returned %d for fd=%d (requested %d bytes)", rc,
               s->fd, size);
#endif

        if(rc < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "After select(), still would block on fd=%d", s->fd);
                rc = 0;
            } else {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket write error after select: rc=%d, errno=%d (%s)", rc, errno,
                       strerror(errno));
                return PLCTAG_ERR_WRITE;
            }
        } else if(rc > 0 && rc < size) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "After select(), partial write on fd=%d: wrote %d of %d bytes", s->fd,
                   rc, size);
        } else if(rc == size) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "After select(), complete write on fd=%d: wrote all %d bytes", s->fd,
                   size);
        }
    }

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Done: result = %d.", rc);

    return rc;
}


int socket_close(sock_p s) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Starting.");

    if(!s) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket pointer or pointer to socket pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(s->fd != INVALID_SOCKET) {
        if(close(s->fd)) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error closing socket fd=%d!", s->fd);
            rc = PLCTAG_ERR_CLOSE;
        } else {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Closed socket fd=%d", s->fd);
        }

        s->fd = INVALID_SOCKET;
    }

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Done.");

    return rc;
}


int socket_destroy(sock_p *s) {
    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Starting.");

    if(!s || !*s) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket pointer or pointer to socket pointer is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* close the wake sockets */
socket_close(*s);

    mem_free(*s);

    *s = NULL;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}



#endif
