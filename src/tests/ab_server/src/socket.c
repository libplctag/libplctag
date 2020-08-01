/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <errno.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slice.h"
#include "socket.h"
#include "utils.h"


/* lengths for socket read and write. */
#ifdef IS_MSVC
    typedef int sock_io_len_t;
#else
    typedef ssize_t sock_io_len_t;
    typedef struct timeval TIMEVAL;
#endif


#define LISTEN_QUEUE (10)

int socket_open(const char *host, const char *port)
{
	//int status;
	struct addrinfo addr_hints;
    struct addrinfo *addr_info = NULL;
	int sock;
    int sock_opt = 0;
    int rc;

#ifdef IS_WINDOWS
	/* Windows needs special initialization. */
	static WSADATA winsock_data;
	rc = WSAStartup(MAKEWORD(2, 2), &winsock_data);

	if (rc != NO_ERROR) {
		info("WSAStartup failed with error: %d\n", rc);
		return SOCKET_ERR_STARTUP;
	}
#endif

    /*
     * Set up the hints for the type of socket that we want.
     */

    /* make sure the whole struct is set to 0 bytes */
	memset(&addr_hints, 0, sizeof addr_hints);

    /*
     * From the man page (node == host name here):
     *
     * "If the AI_PASSIVE flag is specified in hints.ai_flags, and node is NULL, then
     * the returned socket addresses will be suitable for bind(2)ing a socket that
     * will  accept(2) connections.   The returned  socket  address  will  contain
     * the  "wildcard  address" (INADDR_ANY for IPv4 addresses, IN6ADDR_ANY_INIT for
     * IPv6 address).  The wildcard address is used by applications (typically servers)
     * that intend to accept connections on any of the host's network addresses.  If
     * node is not NULL, then the AI_PASSIVE flag is ignored.
     *
     * If the AI_PASSIVE flag is not set in hints.ai_flags, then the returned socket
     * addresses will be suitable for use with connect(2), sendto(2), or sendmsg(2).
     * If node is NULL, then the  network address  will  be  set  to the loopback
     * interface address (INADDR_LOOPBACK for IPv4 addresses, IN6ADDR_LOOPBACK_INIT for
     * IPv6 address); this is used by applications that intend to communicate with
     * peers running on the same host."
     *
     * So we can get away with just setting AI_PASSIVE.
     */
    addr_hints.ai_flags = AI_PASSIVE;

    /* this allows for both IPv4 and IPv6 */
    addr_hints.ai_family = AF_UNSPEC;

    /* we want a TCP socket.  And we want it now! */
    addr_hints.ai_socktype = SOCK_STREAM;

    /* Get the address info about the local system, to be used later. */
    rc = getaddrinfo(host, port, &addr_hints, &addr_info);
    if (rc != 0) {
        info("ERROR: getaddrinfo() failed: %s\n", gai_strerror(rc));
        return SOCKET_ERR_OPEN;
    }

    /* finally, finally, finally, we get to open a socket! */
    sock = (int)socket(addr_info->ai_family, addr_info->ai_socktype, addr_info->ai_protocol);

    if (sock < 0) {
        info("ERROR: socket() failed: %s\n", gai_strerror(sock));
        return SOCKET_ERR_CREATE;
    }

    /* if this is going to be a server socket, bind it. */
    if(strcmp(host,"0.0.0.0") == 0) {
        info("socket_open() setting up server socket.   Binding to address 0.0.0.0.");

        rc = bind(sock, addr_info->ai_addr, (socklen_t)(unsigned int)addr_info->ai_addrlen);
        if (rc < 0)	{
            printf("ERROR: Unable to bind() socket: %s\n", gai_strerror(rc));
            return SOCKET_ERR_BIND;
        }

        rc = listen(sock, LISTEN_QUEUE);
        if(rc < 0) {
            info("ERROR: Unable to call listen() on socket: %s\n", gai_strerror(rc));
            return SOCKET_ERR_LISTEN;
        }

        /* set up our socket to allow reuse if we crash suddenly. */
        sock_opt = 1;
        rc = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&sock_opt, sizeof(sock_opt));
        if(rc) {
            socket_close(sock);
            info("ERROR: Setting SO_REUSEADDR on socket failed: %s\n", gai_strerror(rc));
            return SOCKET_ERR_SETOPT;
        }
    } else {
        struct timeval timeout; /* used for timing out connections etc. */
        struct linger so_linger; /* used to set up short/no lingering after connections are close()ed. */

#ifdef SO_NOSIGPIPE
        /* On *BSD and macOS, set the socket option to prevent SIGPIPE. */
        rc = setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, (char*)&sock_opt, sizeof(sock_opt));
        if(rc) {
            socket_close(sock);
            info ("ERROR: Setting SO_REUSEADDR on socket failed: %s\n", gai_strerror(rc));
            return SOCKET_ERR_SETOPT;
        }
#endif

        timeout.tv_sec = 10;
        timeout.tv_usec = 0;

        rc = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        if(rc) {
            socket_close(sock);
            info("ERROR: Setting SO_RCVTIMEO on socket failed: %s\n", gai_strerror(rc));
            return SOCKET_ERR_SETOPT;
        }

        rc = setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
        if(rc) {
            socket_close(sock);
            info("ERROR: Setting SO_SNDTIMEO on socket failed: %s\n", gai_strerror(rc));
            return SOCKET_ERR_SETOPT;
        }

        /* abort the connection on close. */
        so_linger.l_onoff = 1;
        so_linger.l_linger = 0;

        rc = setsockopt(sock, SOL_SOCKET, SO_LINGER,(char*)&so_linger,sizeof(so_linger));
        if(rc) {
            socket_close(sock);
            info("ERROR: Setting SO_LINGER on socket failed: %s\n", gai_strerror(rc));
            return SOCKET_ERR_SETOPT;
        }
    }

    /* free the memory for the address info struct. */
    freeaddrinfo(addr_info);

    return sock;
}



void socket_close(int sock)
{
    if(sock >= 0) {
#ifdef IS_WINDOWS
        closesocket(sock);
#else
        close(sock);
#endif
    }
}


int socket_accept(int sock)
{
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
    num_accept_ready = select(sock+1, &accept_fd_set, NULL, NULL, &timeout);
    if (num_accept_ready > 0) {
        info("Ready to accept on %d sockets.", num_accept_ready);
        if (FD_ISSET(sock, &accept_fd_set)) {
            return (int)accept(sock, NULL, NULL);
        }
    } else if (num_accept_ready < 0) {
        info("Error selecting the listen socket!");
        return SOCKET_ERR_SELECT;
    } 

    return SOCKET_STATUS_OK;
}


slice_s socket_read(int sock, slice_s in_buf)
{
    int rc = (int)recv(sock, (char *)in_buf.data, (sock_io_len_t)(unsigned int)(int)in_buf.len, 0);

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

    return ((rc>=0) ? slice_from_slice(in_buf, 0, (size_t)(unsigned int)rc) : slice_make_err(rc));
}


/* this blocks until all the data is written or there is an error. */
int socket_write(int sock, slice_s out_buf)
{
    size_t total_bytes_written = 0;
    int rc = 0;
    slice_s tmp_out_buf = out_buf;

    info("socket_write(): writing packet:");
    slice_dump(out_buf);

    do {
        rc = (int)send(sock, (char *)tmp_out_buf.data, (sock_io_len_t)(unsigned int)(int)tmp_out_buf.len, 0);

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

