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
#include <platform.h>
#include <utils/debug.h>
#include <utils/socket.h>


#ifdef _WIN32

struct sock_t {
    SOCKET fd;
    SOCKET wake_read_fd;
    SOCKET wake_write_fd;
    int port;
};


#define MAX_IPS (8)

static int sock_create_event_wakeup_channel(sock_p sock);


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
    int rc = PLCTAG_STATUS_OK;

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
    (*s)->wake_read_fd = INVALID_SOCKET;
    (*s)->wake_write_fd = INVALID_SOCKET;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Setting up wake pipe.");
    rc = sock_create_event_wakeup_channel((*s));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to create wake channel, error %s!", plc_tag_decode_error(rc));

        /*
         * NOTE: this used to return leaving the allocation orphaned and *s
         * pointing at it.  The caller has no socket to destroy on a failed
         * create, so clean up here and hand back a NULL.
         */
        mem_free(*s);
        *s = NULL;

        return rc;
    }

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


int socket_wait_event(sock_p sock, int events, int timeout_ms) {
    int result = SOCK_EVENT_NONE;
    fd_set read_set;
    fd_set write_set;
    fd_set err_set;
    int num_sockets = 0;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Starting.");

    if(!sock) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null socket pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(timeout_ms < 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Timeout must be zero or positive!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* check if the mask is empty */
    if(events == 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Passed event mask is empty!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* set up fd sets */
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&err_set);

    /* add the wake fd - defensive check for valid socket */
    if(sock->wake_read_fd != INVALID_SOCKET) {
        FD_SET(sock->wake_read_fd, &read_set);
    } else {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Wake socket is invalid, cannot wait for events!");
        return PLCTAG_ERR_BAD_CONFIG;
    }

    /* Only monitor main socket if it's valid (it may be closed during reconnection) */
    if(sock->fd != INVALID_SOCKET) {
        /* we always want to know about errors. */
        FD_SET(sock->fd, &err_set);

        /* add more depending on the mask. */
        if(events & SOCK_EVENT_CAN_READ) { FD_SET(sock->fd, &read_set); }

        if((events & SOCK_EVENT_CONNECT) || (events & SOCK_EVENT_CAN_WRITE)) { FD_SET(sock->fd, &write_set); }
    }
    /* else: main socket invalid - only wake socket will be monitored, which is valid for reconnection */

    /* calculate the timeout. */
    if(timeout_ms > 0) {
        struct timeval tv;

        tv.tv_sec = (long)(timeout_ms / 1000);
        tv.tv_usec = (long)(timeout_ms % 1000) * (long)(1000);

        /* Note: On Windows, the first parameter (nfds) to select() is ignored.
         * Windows select() determines which sockets to check from the fd_sets themselves.
         * The value 0 is used here since it's ignored anyway. */
        num_sockets = select(0, &read_set, &write_set, &err_set, &tv);
    } else {
        struct timeval tv = {0, 0};
        num_sockets = select(0, &read_set, &write_set, &err_set, &tv);
    }

    if(num_sockets == 0) {
        result |= (events & SOCK_EVENT_TIMEOUT);
    } else if(num_sockets > 0) {
        /* was there a wake up? */
        if(FD_ISSET(sock->wake_read_fd, &read_set)) {
            int bytes_read = 0;
            char buf[32];

            /* empty the socket. */
            while((bytes_read = (int)recv(sock->wake_read_fd, (char *)&buf[0], sizeof(buf), 0)) > 0) {}

            pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket woken up.");

            result |= (events & SOCK_EVENT_WAKE_UP);
        }

        /* is read ready for the main fd? Guard against INVALID_SOCKET */
        if(sock->fd != INVALID_SOCKET && FD_ISSET(sock->fd, &read_set)) {
            char buf;
            int byte_read = 0;

            byte_read = (int)recv(sock->fd, &buf, sizeof(buf), MSG_PEEK);

            if(byte_read > 0) {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket can read.");
                result |= (events & SOCK_EVENT_CAN_READ);
            } else if(byte_read == 0) {
                /* recv() returned 0 - this means the connection was closed by the remote peer.
                 * A healthy TCP socket that's just idle will not show as readable in select()
                 * unless there's actual data waiting. If select() says readable but recv() gets 0,
                 * the connection is truly closed. */
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket disconnected (recv returned 0).");
                result |= (events & SOCK_EVENT_DISCONNECT);
            } else {
                /* recv() returned -1, check the specific error */
                int recv_err = WSAGetLastError();
                if(recv_err == WSAEWOULDBLOCK) {
                    /* This is a spurious wakeup - socket showed as readable but no data.
                     * Don't report an error, just return no events. */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket readable but no data available (WSAEWOULDBLOCK).");
                } else {
                    /* Some other error occurred on the socket */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "recv() with MSG_PEEK error %d on socket.", recv_err);
                    result |= (events & SOCK_EVENT_ERROR);
                }
            }
        }

        /* is write ready for the main fd? Guard against INVALID_SOCKET */
        if(sock->fd != INVALID_SOCKET && FD_ISSET(sock->fd, &write_set)) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket can write or just connected.");
            result |= ((events & SOCK_EVENT_CAN_WRITE) | (events & SOCK_EVENT_CONNECT));
        }

        /* is there an error? Guard against INVALID_SOCKET */
        if(sock->fd != INVALID_SOCKET && FD_ISSET(sock->fd, &err_set)) {
            /* On Windows, FD_ISSET on err_set can return true spuriously for idle sockets.
             * We need to verify the error is real by checking the actual socket error state.
             * Use getsockopt(SO_ERROR) to get the actual error code. */
            int sock_error = 0;
            socklen_t sock_error_len = sizeof(sock_error);

            if(getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, (char *)&sock_error, &sock_error_len) == 0) {
                if(sock_error != 0) {
                    /* There's a real socket error */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket has real error %d!", sock_error);
                    result |= (events & SOCK_EVENT_ERROR);
                } else {
                    /* FD_ISSET was spurious - there's no actual error */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0,
                           "FD_ISSET indicated error but SO_ERROR is 0 (spurious error flag).");
                }
            } else {
                /* Failed to get socket error state, assume there's an error */
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Failed to check socket error state, treating as error.");
                result |= (events & SOCK_EVENT_ERROR);
            }
        }
    } else {
        int err = WSAGetLastError();

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "select() returned status %d!", num_sockets);

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

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Done.");

    return result;
}


int socket_wake(sock_p sock) {
    int rc = PLCTAG_STATUS_OK;
    const char dummy_data[] = "Dummy data.";

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Starting.");

    if(!sock) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null socket pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* The wake pipe is independent of the TCP connection state.
     * Write to the wake pipe to interrupt the handler thread's select() call.
     * This works regardless of whether the TCP connection is open.
     * Check that the wake pipe is valid before writing to it. */
    if(sock->wake_write_fd == INVALID_SOCKET) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Wake pipe not yet initialized, skipping wake.");
        return PLCTAG_STATUS_OK;
    }

    rc = send(sock->wake_write_fd, (const char *)dummy_data, sizeof(dummy_data), 0);
    if(rc < 0) {
        int err = WSAGetLastError();

        if(err == WSAEWOULDBLOCK) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Write wrote no data.");

            rc = PLCTAG_STATUS_OK;
        } else if(err == WSAEBADF) {
            /* If the write failed with WSAEBADF (bad socket), the wake pipe
             * has been closed. Mark it as invalid and return success so the system
             * can proceed. The next wake attempt will skip due to INVALID_SOCKET check. */
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Wake pipe closed (WSAEBADF), marking as invalid.");
            sock->wake_write_fd = INVALID_SOCKET;
            rc = PLCTAG_STATUS_OK;
        } else {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "socket write error rc=%d, errno=%d", rc, err);
            return PLCTAG_ERR_WRITE;
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

    if((*s)->wake_read_fd != INVALID_SOCKET) {
        if(closesocket((*s)->wake_read_fd)) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error closing wake read socket!");
            rc = PLCTAG_ERR_CLOSE;
        }

        (*s)->wake_read_fd = INVALID_SOCKET;
    }

    if((*s)->wake_write_fd != INVALID_SOCKET) {
        if(closesocket((*s)->wake_write_fd)) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error closing wake write socket!");
            rc = PLCTAG_ERR_CLOSE;
        }

        (*s)->wake_write_fd = INVALID_SOCKET;
    }

    socket_close(*s);

    mem_free(*s);

    *s = 0;

    if(WSACleanup() != NO_ERROR) { return PLCTAG_ERR_WINSOCK; }

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Done.");

    return rc;
}


static int sock_create_event_wakeup_channel(sock_p sock) {
    int rc = PLCTAG_STATUS_OK;
    SOCKET listener = INVALID_SOCKET;
    struct sockaddr_in listener_addr_info;
    socklen_t addr_info_size = sizeof(struct sockaddr_in);
    u_long non_blocking = 1;
    SOCKET wake_fds[2];

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Starting.");

    wake_fds[0] = INVALID_SOCKET;
    wake_fds[1] = INVALID_SOCKET;

    /*
     * This is a bit convoluted.
     *
     * First we open a listening socket on the loopback interface.
     * We do not care what port so we let the OS decide.
     *
     * Then we connect to that socket.   The connection becomes
     * the reader side of the wake up fds.
     *
     * Then we accept and that becomes the writer side of the
     * wake up fds.
     *
     * Then we close the listener because we do not want to keep
     * it open as it might be a problem.  Probably more for DOS
     * purposes than any security, but you never know!
     *
     * And the reader and writer have to be set up as non-blocking!
     *
     * This was cobbled together from various sources including
     * StackExchange and MSDN.   I did not take notes, so I am unable
     * to properly credit the original sources :-(
     */

    do {
        /*
         * Set up our listening socket.
         */

        listener = (SOCKET)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if(listener == INVALID_SOCKET) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error %d creating the listener socket!", WSAGetLastError());
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }

        /* clear the listener address info */
        mem_set(&listener_addr_info, 0, addr_info_size);

        /* standard IPv4 for the win! */
        listener_addr_info.sin_family = AF_INET;

        /* we do not care what port. */
        listener_addr_info.sin_port = 0;

        /* we want to connect on the loopback address. */
        listener_addr_info.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        /* now comes the part where we could fail. */

        /* first we bind the listener to the loopback and let the OS choose the port. */
        if(bind(listener, (struct sockaddr *)&listener_addr_info, addr_info_size)) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error %d binding the listener socket!", WSAGetLastError());
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }

        /*
         * we need to get the address and port of the listener for later steps.
         * Notice that this _sets_ the address size!.
         */
        if(getsockname(listener, (struct sockaddr *)&listener_addr_info, &addr_info_size)) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error %d getting the listener socket address info!", WSAGetLastError());
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }

        /* Phwew.   We can actually listen now. Notice that this is blocking! */
        if(listen(listener, 1)) { /* MAGIC constant - We do not want any real queue! */
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error %d listening on the listener socket!", WSAGetLastError());
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }

        /*
         * Set up our wake read side socket.
         */

        wake_fds[0] = (SOCKET)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if(wake_fds[0] <= 0) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error %d creating the wake channel read side socket!",
                   WSAGetLastError());
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }

        /*
         * now we start the next phase.   We need to connect to our own listener.
         * This will be the reader side of the wake up socket.
         */

        if(connect(wake_fds[0], (struct sockaddr *)&listener_addr_info, addr_info_size)) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error %d connecting to the listener socket!", WSAGetLastError());
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }

        /* now we accept our own connection. This becomes the writer side. */
        wake_fds[1] = accept(listener, 0, 0);
        if(wake_fds[1] == INVALID_SOCKET) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error %d connecting to the listener socket!", WSAGetLastError());
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }

        /* now we need to set these to non-blocking. */

        /* reader */
        if(ioctlsocket(wake_fds[0], (long)FIONBIO, &non_blocking)) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error %d setting reader socket to non-blocking!", WSAGetLastError());
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }

        /* writer */
        if(ioctlsocket(wake_fds[1], (long)FIONBIO, &non_blocking)) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error %d setting writer socket to non-blocking!", WSAGetLastError());
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }

        /* set TCP no delay on both sides to avoid delays */
        int flag = 1;
        if(setsockopt(wake_fds[0], IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(int)) < 0) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error %d setting TCP_NODELAY on wake read socket!", WSAGetLastError());
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }
        if(setsockopt(wake_fds[1], IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(int)) < 0) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error %d setting TCP_NODELAY on wake write socket!", WSAGetLastError());
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }
    } while(0);

    /* do some clean up */
    if(listener != INVALID_SOCKET) { closesocket(listener); }

    /* check the result */
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to set up wakeup socket!");

        if(wake_fds[0] != INVALID_SOCKET) {
            closesocket(wake_fds[0]);
            wake_fds[0] = INVALID_SOCKET;
        }

        if(wake_fds[1] != INVALID_SOCKET) {
            closesocket(wake_fds[1]);
            wake_fds[1] = INVALID_SOCKET;
        }
    } else {
        sock->wake_read_fd = wake_fds[0];
        sock->wake_write_fd = wake_fds[1];

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Done.");
    }

    return rc;
}

#else

#ifndef INVALID_SOCKET
#    define INVALID_SOCKET (-1)
#endif

struct sock_t {
    int fd;
    int wake_read_fd;
    int wake_write_fd;
    int port;
};


static int sock_create_event_wakeup_channel(sock_p sock);

#define MAX_IPS (8)

extern int socket_create(sock_p *s) {
    int32_t rc = PLCTAG_STATUS_OK;

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
    (*s)->wake_read_fd = INVALID_SOCKET;
    (*s)->wake_write_fd = INVALID_SOCKET;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Setting up wake pipe.");
    rc = sock_create_event_wakeup_channel((*s));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to create wake pipe, error %s!", plc_tag_decode_error(rc));

        /*
         * NOTE: this used to return leaving the allocation orphaned and *s
         * pointing at it.  The caller has no socket to destroy on a failed
         * create, so clean up here and hand back a NULL.
         */
        mem_free(*s);
        *s = NULL;

        return rc;
    }

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
     * Instead, timeout handling is done via select() in socket_wait_event() and socket_read()
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
    // rc = sock_create_event_wakeup_channel(s);
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


static inline void log_event_bits(const char *prefix, int bits) {
    pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "%s events=0x%x: %s%s%s%s%s%s", prefix, bits,
           (bits & SOCK_EVENT_CAN_READ) ? "CAN_READ " : "", (bits & SOCK_EVENT_CAN_WRITE) ? "CAN_WRITE " : "",
           (bits & SOCK_EVENT_CONNECT) ? "CONNECT " : "", (bits & SOCK_EVENT_DISCONNECT) ? "DISCONNECT " : "",
           (bits & SOCK_EVENT_ERROR) ? "ERROR " : "", (bits & SOCK_EVENT_TIMEOUT) ? "TIMEOUT " : "");
}

int socket_wait_event(sock_p sock, int events, int timeout_ms) {
    int result = SOCK_EVENT_NONE;
    fd_set read_set;
    fd_set write_set;
    fd_set err_set;
    int max_fd = 0;
    int num_sockets = 0;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Starting.");

    if(!sock) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null socket pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(timeout_ms < 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Timeout must be zero or positive!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* check if the mask is empty */
    if(events == 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Passed event mask is empty!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* set up fd sets */
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&err_set);

    /* add the wake fd - defensive check for valid socket */
    if(sock->wake_read_fd == INVALID_SOCKET) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Wake socket is invalid, cannot wait for events!");
        return PLCTAG_ERR_BAD_CONFIG;
    }

    FD_SET(sock->wake_read_fd, &read_set);

    /* Only monitor main socket if it's valid (it may be closed during reconnection) */
    if(sock->fd != INVALID_SOCKET) {
        /* calculate the maximum fd */
        max_fd = (sock->fd > sock->wake_read_fd ? sock->fd : sock->wake_read_fd);

        /* we always want to know about errors. */
        FD_SET(sock->fd, &err_set);

        /* add more depending on the mask. */
        if(events & SOCK_EVENT_CAN_READ) {
            FD_SET(sock->fd, &read_set);
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Adding sock->fd=%d to read_set for SOCK_EVENT_CAN_READ", sock->fd);
        }

        if((events & SOCK_EVENT_CONNECT) || (events & SOCK_EVENT_CAN_WRITE)) {
            FD_SET(sock->fd, &write_set);
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0,
                   "Adding sock->fd=%d to write_set for SOCK_EVENT_CONNECT or SOCK_EVENT_CAN_WRITE", sock->fd);
        }

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Main socket fd=%d is valid, max_fd=%d, wake_read_fd=%d", sock->fd, max_fd,
               sock->wake_read_fd);
    } else {
        /* Main socket invalid or closed - only wake socket will be monitored (valid for reconnection/idle) */
        max_fd = sock->wake_read_fd;
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Main socket is INVALID, using only wake_read_fd=%d", sock->wake_read_fd);
    }

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "events=0x%x, max_fd=%d, sock->fd=%d, timeout_ms=%d, calling select()", events,
           max_fd, sock->fd, timeout_ms);

    /* calculate the timeout. */
    if(timeout_ms > 0) {
        struct timeval tv;

        tv.tv_sec = (time_t)(timeout_ms / 1000);
        tv.tv_usec = (suseconds_t)(timeout_ms % 1000) * (suseconds_t)(1000);

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "calling select with timeout tv_sec=%ld tv_usec=%ld", tv.tv_sec, tv.tv_usec);
        num_sockets = select(max_fd + 1, &read_set, &write_set, &err_set, &tv);
    } else {
        struct timeval tv = {0, 0};
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "calling select with zero timeout (poll)");
        num_sockets = select(max_fd + 1, &read_set, &write_set, &err_set, &tv);
    }

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "select() returned num_sockets=%d for sock->fd=%d", num_sockets, sock->fd);

    if(num_sockets == 0) {
        result |= (events & SOCK_EVENT_TIMEOUT);
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "select() timed out, returning TIMEOUT event");
    } else if(num_sockets > 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "num_sockets (%d) > 0, checking ready fds. sock->fd=%d, wake_read_fd=%d",
               num_sockets, sock->fd, sock->wake_read_fd);

        /* was there a wake up? */
        int wake_isset = FD_ISSET(sock->wake_read_fd, &read_set);
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "FD_ISSET(wake_read_fd=%d, read_set)=%d", sock->wake_read_fd, wake_isset);
        if(wake_isset) {
            char buf[32];

            /* empty the socket. */
            while((int)read(sock->wake_read_fd, &buf[0], sizeof(buf)) > 0) {}

            pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket woken up.");
            result |= (events & SOCK_EVENT_WAKE_UP);
        }

        /* is read ready for the main fd? Guard against INVALID_SOCKET (-1) which causes undefined behavior in FD_ISSET */
        int read_isset = (sock->fd != INVALID_SOCKET) ? FD_ISSET(sock->fd, &read_set) : 0;
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "FD_ISSET(sock->fd=%d, read_set)=%d", sock->fd, read_isset);

        if(sock->fd != INVALID_SOCKET && read_isset) {
            char buf;
            int byte_read = 0;

            byte_read = (int)recv(sock->fd, &buf, sizeof(buf), MSG_PEEK);

            if(byte_read > 0) {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket can read.");
                result |= (events & SOCK_EVENT_CAN_READ);
            } else if(byte_read == 0) {
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket disconnected (recv returned 0).");
                result |= (events & SOCK_EVENT_DISCONNECT);
            } else {
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Don't report anything, will wait for next event */
                } else {
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket recv error: %d", errno);
                    result |= (events & SOCK_EVENT_DISCONNECT);
                }
            }
        }

        /* is write ready for the main fd? Guard against INVALID_SOCKET (-1) */
        int write_isset = (sock->fd != INVALID_SOCKET) ? FD_ISSET(sock->fd, &write_set) : 0;

        if(sock->fd != INVALID_SOCKET && write_isset) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Socket can write or just connected.");
            result |= ((events & SOCK_EVENT_CAN_WRITE) | (events & SOCK_EVENT_CONNECT));
        }

        /* is there an error? Guard against INVALID_SOCKET (-1) */
        int err_isset = (sock->fd != INVALID_SOCKET) ? FD_ISSET(sock->fd, &err_set) : 0;

        if(sock->fd != INVALID_SOCKET && err_isset) {
            /* On some platforms, FD_ISSET on err_set can return true spuriously.
             * Verify the error is real by checking SO_ERROR. */
            int sock_error = 0;
            socklen_t sock_error_len = sizeof(sock_error);

            if(getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &sock_error, &sock_error_len) == 0) {
                if(sock_error != 0) {
                    /* There's a real socket error */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket has real error %d!", sock_error);
                    result |= (events & SOCK_EVENT_ERROR);
                } else {
                    /* FD_ISSET was spurious - there's no actual error */
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0,
                           "FD_ISSET indicated error but SO_ERROR is 0 (spurious error flag).");
                }
            } else {
                /* Failed to get socket error state, assume there's an error */
                pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Failed to check socket error state, treating as error.");
                result |= (events & SOCK_EVENT_ERROR);
            }
        }

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "After all checks, result=0x%x", result);
    } else {
        /* error */
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "select() returned status %d!", num_sockets);

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

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_SPEW, 0, "Done.");

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0,
           "socket_wait_event: Final result=0x%x before return, TIMEOUT bit=%d, events=0x%x", result,
           (result & SOCK_EVENT_TIMEOUT) ? 1 : 0, events);
    log_event_bits("socket_wait_event: result", result);
    log_event_bits("socket_wait_event: requested_events", events);

    /* Log result at INFO level for visibility */
    if(result != SOCK_EVENT_NONE) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "fd=%d returning events=0x%x (requested=0x%x) %s%s%s%s%s%s", sock->fd,
               result, events, (result & SOCK_EVENT_CAN_READ) ? "CAN_READ " : "",
               (result & SOCK_EVENT_CAN_WRITE) ? "CAN_WRITE " : "", (result & SOCK_EVENT_CONNECT) ? "CONNECT " : "",
               (result & SOCK_EVENT_DISCONNECT) ? "DISCONNECT " : "", (result & SOCK_EVENT_ERROR) ? "ERROR " : "",
               (result & SOCK_EVENT_TIMEOUT) ? "TIMEOUT " : "");
    }

    return result;
}


int socket_wake(sock_p sock) {
    int rc = PLCTAG_STATUS_OK;
    const char dummy_data[] = "Dummy data.";

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Starting.");

    if(!sock) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null socket pointer passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* The wake pipe is independent of the TCP connection state.
     * Write to the wake pipe to interrupt the handler thread's select() call.
     * This works regardless of whether the TCP connection is open.
     * Check that the wake pipe is valid before writing to it. */
    if(sock->wake_write_fd == INVALID_SOCKET) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Wake pipe not yet initialized, skipping wake.");
        return PLCTAG_STATUS_OK;
    }

    // rc = (int)write(sock->wake_write_fd, &dummy_data[0], sizeof(dummy_data));
#ifdef BSD_OS_TYPE
    /* On *BSD and macOS, the socket option is set to prevent SIGPIPE. */
    rc = (int)write(sock->wake_write_fd, &dummy_data[0], sizeof(dummy_data));
#else
    /* on Linux, we use MSG_NOSIGNAL */
    rc = (int)send(sock->wake_write_fd, &dummy_data[0], sizeof(dummy_data), MSG_NOSIGNAL);
#endif
    if(rc >= 0) {
        rc = PLCTAG_STATUS_OK;
    } else {
        int err = errno;

        /* If the write failed with EAGAIN/EWOULDBLOCK, the wake pipe is full.
         * This means a wake is already pending, so return success. */
        if(err == EAGAIN || err == EWOULDBLOCK) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Wake pipe full (EAGAIN/EWOULDBLOCK), wake already pending.");
            return PLCTAG_STATUS_OK;
        }

        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket write error: rc=%d, errno=%d", rc, err);

        /* If the write failed with EBADF (bad file descriptor), the wake pipe
         * has been closed. Mark it as invalid and return success so the system
         * can proceed. The next wake attempt will skip due to INVALID_SOCKET check. */
        if(err == EBADF) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Wake pipe closed (EBADF), marking as invalid.");
            sock->wake_write_fd = INVALID_SOCKET;
            return PLCTAG_STATUS_OK;
        }

        return PLCTAG_ERR_WRITE;
    }

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_DETAIL, 0, "Done.");

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
    if((*s)->wake_read_fd != INVALID_SOCKET) {
        if(close((*s)->wake_read_fd)) { pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error closing read wake socket!"); }

        (*s)->wake_read_fd = INVALID_SOCKET;
    }

    if((*s)->wake_write_fd != INVALID_SOCKET) {
        if(close((*s)->wake_write_fd)) { pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Error closing write wake socket!"); }

        (*s)->wake_write_fd = INVALID_SOCKET;
    }

    socket_close(*s);

    mem_free(*s);

    *s = NULL;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


static int sock_create_event_wakeup_channel(sock_p sock) {
    int rc = PLCTAG_STATUS_OK;
    int flags = 0;
    int wake_fds[2] = {0};
#ifdef BSD_OS_TYPE
    int sock_opt = 1;
#endif

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Starting.");

    do {
        /* open the pipe for waking the select wait. */
        // if(pipe(wake_fds)) {
        if((rc = socketpair(PF_LOCAL, SOCK_STREAM, 0, wake_fds))) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to open waker pipe!");
            switch(errno) {
                case EAFNOSUPPORT:
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0,
                           "The specified addresss family is not supported on this machine!");
                    break;

                case EFAULT:
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0,
                           "The address socket_vector does not specify a valid part of the process address space.");
                    break;

                case EMFILE:
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "No more file descriptors are available for this process.");
                    break;

                case ENFILE:
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "No more file descriptors are available for the system.");
                    break;

                case ENOBUFS:
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0,
                           "Insufficient resources were available in the system to perform the operation.");
                    break;

                case ENOMEM:
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Insufficient memory was available to fulfill the request.");
                    break;

                case EOPNOTSUPP:
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0,
                           "The specified protocol does not support creation of socket pairs.");
                    break;

                case EPROTONOSUPPORT:
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "The specified protocol is not supported on this machine.");
                    break;

                case EPROTOTYPE:
                    pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "The socket type is not supported by the protocol.");
                    break;

                default: pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unexpected error %d!", errno); break;
            }

            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

#ifdef BSD_OS_TYPE
        /* The *BSD family has a different way to suppress SIGPIPE on sockets. */
        if(setsockopt(wake_fds[0], SOL_SOCKET, SO_NOSIGPIPE, (char *)&sock_opt, sizeof(sock_opt))) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_ERROR, 0,
                   "Error setting wake fd read socket SIGPIPE suppression option, errno: %d", errno);
            rc = PLCTAG_ERR_OPEN;
            break;
        }

        if(setsockopt(wake_fds[1], SOL_SOCKET, SO_NOSIGPIPE, (char *)&sock_opt, sizeof(sock_opt))) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_ERROR, 0,
                   "Error setting wake fd write socket SIGPIPE suppression option, errno: %d", errno);
            rc = PLCTAG_ERR_OPEN;
            break;
        }
#endif

        /* make the read pipe fd non-blocking. */
        if((flags = fcntl(wake_fds[0], F_GETFL)) < 0) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to get flags of read socket fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* set read fd non-blocking */
        flags |= O_NONBLOCK;

        if(fcntl(wake_fds[0], F_SETFL, flags) < 0) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to set flags of read socket fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* make the write pipe fd non-blocking. */
        if((flags = fcntl(wake_fds[1], F_GETFL)) < 0) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to get flags of write socket fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* set write fd non-blocking */
        flags |= O_NONBLOCK;

        if(fcntl(wake_fds[1], F_SETFL, flags) < 0) {
            pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to set flags of write socket fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        sock->wake_read_fd = wake_fds[0];
        sock->wake_write_fd = wake_fds[1];
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to open waker socket!");

        if(wake_fds[0] != INVALID_SOCKET) {
            close(wake_fds[0]);
            wake_fds[0] = INVALID_SOCKET;
        }

        if(wake_fds[1] != INVALID_SOCKET) {
            close(wake_fds[1]);
            wake_fds[1] = INVALID_SOCKET;
        }
    } else {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Done.");
    }

    return rc;
}

#endif
