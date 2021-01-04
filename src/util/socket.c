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

#if PLATFORM_IS_WINDOWS
    #include <winsock2.h>
    #include <ws2tcpip.h>

    #ifndef IPPROTO_TCP
        #define IPPROTO_TCP (0)
    #endif

    typedef SOCKET socket_t;
#else
    #include <arpa/inet.h>
    #include <errno.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <sys/types.h>
    #include <unistd.h>

    typedef struct timeval TIMEVAL;
#endif

#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>

#include <lib/libplctag.h>
#include <util/atomic_int.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/mutex.h>
#include <util/platform.h>
#include <util/socket.h>
#include <util/thread.h>


#ifdef PLATFORM_IS_WINDOWS
    #define errno WSAGetLastError()
    #define socketclose(s) closesocket(s)
#endif

#ifdef PLATFORM_IS_POSIX
    #define socketclose(s) close(s)
#endif


/* maximum time to wait for a socket to be ready. */
#define TICKLER_MAX_WAIT_TIME_MS (50)

struct sock_t {
    socket_t fd;
    int port;

    /* next in the list of sockets. */
    struct sock_t *next;

    /* callbacks for events. */
    void (*read_ready_callback)(void *context);
    void *read_ready_context;

    void (*write_ready_callback)(void *context);
    void *write_ready_context;
};


static mutex_p socket_event_mutex = NULL;
static sock_p socket_list = NULL;
static sock_p global_socket_iterator = NULL;

// static THREAD_LOCAL atomic_bool socket_event_mutex_held_in_this_thread = ATOMIC_BOOL_STATIC_INIT(FALSE);

static int wake_fds[2] = { INVALID_SOCKET, INVALID_SOCKET};
static fd_set global_read_fds;
static fd_set global_write_fds;
static int max_socket_fd = -1;
static atomic_bool need_recalculate_socket_fd_sets = ATOMIC_BOOL_STATIC_INIT(1);

#define MAX_IPS (8)

static int create_event_wakeup_channel(void);
static void destroy_event_wakeup_channel(void);




int socket_create(sock_p *sock)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    if(!sock) {
        pdebug(DEBUG_WARN, "Null socket location pointer.");
        return PLCTAG_ERR_NULL_PTR;
    }

    *sock = (sock_p)mem_alloc(sizeof(**sock));
    if(! *sock) {
        pdebug(DEBUG_ERROR, "Failed to allocate memory for socket.");
        return PLCTAG_ERR_NO_MEM;
    }

    (*sock)->read_ready_callback = NULL;
    (*sock)->read_ready_context = NULL;
    (*sock)->write_ready_callback = NULL;
    (*sock)->write_ready_context = NULL;
    (*sock)->fd = INVALID_SOCKET;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int socket_tcp_connect(sock_p s, const char *host, int port)
{
    struct in_addr ips[MAX_IPS];
    int num_ips = 0;
    struct sockaddr_in gw_addr;
    int sock_opt = 1;
    int i = 0;
    int done = 0;
    int fd;
    int flags;
    TIMEVAL timeout;  /* used for timing out connections etc. */
    struct linger so_linger; /* used to set up short/no lingering after connections are close()ed. */

    pdebug(DEBUG_DETAIL,"Starting.");

    /* Open a socket for communication with the gateway. */
    fd = (socket_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    /* check the size of the fd set. */
    if(fd >= FD_SETSIZE) {
        pdebug(DEBUG_WARN, "Socket fd, %d, is too large to fit in a fd set!");

        socketclose(fd);

        return PLCTAG_ERR_TOO_LARGE;
    }

    /* check for errors */
    if(fd < 0) {
        pdebug(DEBUG_ERROR,"Socket creation failed, err: %d", errno);
        return PLCTAG_ERR_OPEN;
    }

    /* set up our socket to allow reuse if we crash suddenly. */
    sock_opt = 1;

    if(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,(char*)&sock_opt,sizeof(sock_opt))) {
        pdebug(DEBUG_ERROR, "Error setting socket reuse option, err: %d",errno);

        socketclose(fd);

        return PLCTAG_ERR_OPEN;
    }

#ifdef PLATFORM_IS_BSD
    /* The *BSD family has a different way to suppress SIGPIPE on sockets. */
    if(setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (char*)&sock_opt, sizeof(sock_opt))) {
        socketclose(fd);
        pdebug(DEBUG_ERROR, "Error setting socket SIGPIPE suppression option, errno: %d", errno);
        return PLCTAG_ERR_OPEN;
    }
#endif

    timeout.tv_sec = 10;
    timeout.tv_usec = 0;

    if(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout))) {
        pdebug(DEBUG_ERROR, "Error setting socket receive timeout option, err: %d", errno);
        socketclose(fd);
        return PLCTAG_ERR_OPEN;
    }

    if(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout))) {
        pdebug(DEBUG_ERROR, "Error setting socket send timeout option, err: %d", errno);
        socketclose(fd);
        return PLCTAG_ERR_OPEN;
    }

    /* abort the connection immediately upon close. */
    so_linger.l_onoff = 1;
    so_linger.l_linger = 0;

    if(setsockopt(fd, SOL_SOCKET, SO_LINGER,(char*)&so_linger,sizeof(so_linger))) {
        pdebug(DEBUG_ERROR, "Error setting socket close linger option, err: %d", errno);
        socketclose(fd);
        return PLCTAG_ERR_OPEN;
    }

    /* figure out what address we are connecting to. */

    /* try a numeric IP address conversion first. */
    if(inet_pton(AF_INET,host,(struct in_addr *)ips) > 0) {
        pdebug(DEBUG_DETAIL, "Found numeric IP address: %s",host);
        num_ips = 1;
    } else {
        struct addrinfo hints;
        struct addrinfo *res_head = NULL;
        struct addrinfo *res=NULL;
        int rc = 0;

        mem_set(&ips, 0, sizeof(ips));
        mem_set(&hints, 0, sizeof(hints));

        hints.ai_socktype = SOCK_STREAM; /* TCP */
        hints.ai_family = AF_INET; /* IP V4 only */

        if ((rc = getaddrinfo(host, NULL, &hints, &res_head)) != 0) {
            pdebug(DEBUG_WARN,"Error looking up PLC IP address %s, error = %d\n", host, rc);

            if(res_head) {
                freeaddrinfo(res_head);
            }

            socketclose(fd);
            return PLCTAG_ERR_BAD_GATEWAY;
        }

        res = res_head;
        for(num_ips = 0; res && num_ips < MAX_IPS; num_ips++) {
            ips[num_ips].s_addr = ((struct sockaddr_in *)(res->ai_addr))->sin_addr.s_addr;
            res = res->ai_next;
        }

        freeaddrinfo(res_head);
    }


    /* now try to connect to the remote gateway.  We may need to
     * try several of the IPs we have.
     */

    i = 0;
    done = 0;

    memset((void *)&gw_addr,0, sizeof(gw_addr));
    gw_addr.sin_family = AF_INET ;
    gw_addr.sin_port = htons((uint16_t)port);

    do {
        int rc;
        /* try each IP until we run out or get a connection. */
        gw_addr.sin_addr.s_addr = ips[i].s_addr;

        pdebug(DEBUG_DETAIL, "Attempting to connect to %s", inet_ntoa(*((struct in_addr *)&ips[i])));

        rc = connect(fd,(struct sockaddr *)&gw_addr,sizeof(gw_addr));

        if( rc == 0) {
            pdebug(DEBUG_DETAIL, "Attempt to connect to %s succeeded.", inet_ntoa(*((struct in_addr *)&ips[i])));
            done = 1;
        } else {
            int err = errno;
            pdebug(DEBUG_DETAIL, "Attempt to connect to %s failed, err: %d", inet_ntoa(*((struct in_addr *)&ips[i])), err);
            i++;
        }
    } while(!done && i < num_ips);

    if(!done) {
        socketclose(fd);
        pdebug(DEBUG_ERROR, "Unable to connect to any gateway host IP address!");
        return PLCTAG_ERR_OPEN;
    }

    /* FIXME
     * connect() is a little easier to handle in blocking mode, for now
     * we make the socket non-blocking here, after connect(). */

#ifdef PLATFORM_IS_WINDOWS
    if(ioctlsocket(fd,FIONBIO,&non_blocking)) {
        pdebug(DEBUG_ERROR, "Error getting socket options, err: %d", WSAGetLastError());
        socketclose(fd);
        return PLCTAG_ERR_OPEN;
    }
#else
    flags=fcntl(fd,F_GETFL,0);
    if(flags<0) {
        pdebug(DEBUG_ERROR, "Error getting socket options, errno: %d", errno);
        socketclose(fd);
        return PLCTAG_ERR_OPEN;
    }

    flags |= O_NONBLOCK;

    if(fcntl(fd,F_SETFL,flags)<0) {
        pdebug(DEBUG_ERROR, "Error setting socket to non-blocking, errno: %d", errno);
        socketclose(fd);
        return PLCTAG_ERR_OPEN;
    }
#endif

    /* save the values */
    s->fd = fd;
    s->port = port;

    /* put it on the list. */
    critical_block(socket_event_mutex) {
        s->next = socket_list;
        socket_list = s;
    }

    /* we might need an event. */
    socket_event_loop_wake();

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}




extern int socket_tcp_read(sock_p s, uint8_t *buf, int size)
{
    int rc;
    int err;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!s || !buf) {
        pdebug(DEBUG_WARN, "Socket pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(size <= 0) {
        pdebug(DEBUG_WARN, "Size is zero or negative!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* The socket is non-blocking. */
    rc = (int)read(s->fd, buf, INT_TO_SIZE_T(size));
#ifdef PLATFORM_IS_WINDOWS
    if(rc < 0) {
        err = WSAGetLastError();
        if(err == WSAEWOULDBLOCK) {
            rc = 0;
        } else {
            pdebug(DEBUG_WARN,"Socket read error: rc=%d, err=%d", rc, err);
            rc = PLCTAG_ERR_READ;
        }
    }
#else
    if(rc < 0) {
        err = errno;
        if(err == EAGAIN || err == EWOULDBLOCK) {
            rc = 0;
        } else {
            pdebug(DEBUG_WARN,"Socket read error: rc=%d, err=%d", rc, err);
            rc = PLCTAG_ERR_READ;
        }
    }
#endif

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


extern int socket_tcp_write(sock_p s, uint8_t *buf, int size)
{
    int rc;
    int err;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!s || !buf) {
        pdebug(DEBUG_WARN, "Socket pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(size <= 0) {
        pdebug(DEBUG_WARN, "Buffer size is zero or negative!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* The socket is non-blocking. */
#ifdef PLATFORM_IS_WINDOWS
    /* no signals in Windows. */
    rc = (int)send(s->fd, buf, size, 0);
#else
    #ifdef PLATFORM_IS_BSD
    /* On *BSD and macOS, the socket option is set to prevent SIGPIPE. */
    rc = (int)write(s->fd, buf, (size_t)(unsigned int)size);
    #else
    /* on Linux, we use MSG_NOSIGNAL */
    rc = (int)send(s->fd, buf, (size_t)(unsigned int)size, MSG_NOSIGNAL);
    #endif
#endif

#ifdef PLATFORM_IS_WINDOWS
    if(rc < 0) {
        err = WSAGetLastError();
        if(err == WSAWOULDBLOCK) {
            return PLCTAG_ERR_NO_DATA;
        } else {
            pdebug(DEBUG_WARN, "Socket write error: rc=%d, err=%d", rc, err);
            return PLCTAG_ERR_WRITE;
        }
    }
#else
    if(rc < 0) {
        err = errno;
        if(err == EAGAIN || err == EWOULDBLOCK) {
            return PLCTAG_ERR_NO_DATA;
        } else {
            pdebug(DEBUG_WARN, "Socket write error: rc=%d, err=%d", rc, err);
            return PLCTAG_ERR_WRITE;
        }
    }
#endif

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



extern int socket_close(sock_p sock)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(!sock) {
        return PLCTAG_ERR_NULL_PTR;
    }

    /* remove it from the list and clear the FD sets */
    critical_block(socket_event_mutex) {
        sock_p *walker = &socket_list;

        while(*walker && *walker != sock) {
            walker = &((*walker)->next);
        }

        if(*walker && *walker == sock) {
            *walker = sock->next;

            /* fix up the iterator. */
            if(global_socket_iterator == sock) {
                global_socket_iterator = sock->next;
            }
        } /* else not on the list. */

        sock->next = NULL;

        /* remove the callbacks */
        sock->read_ready_callback = NULL;
        sock->read_ready_context = NULL;
        sock->write_ready_callback = NULL;
        sock->write_ready_context = NULL;

        /* clear the FD sets */
        FD_CLR(sock->fd, &global_read_fds);
        FD_CLR(sock->fd, &global_write_fds);

        /* force a recalculation of the FD set and max socket if we are the top one. */
        if(sock->fd >= max_socket_fd) {
            atomic_int_add(&need_recalculate_socket_fd_sets, 10);
        } else {
            /* not as urgent, recalculate eventually. */
            atomic_int_add(&need_recalculate_socket_fd_sets, 1);
        }
    }

    /* close the socket fd. */
    if(sock->fd != INVALID_SOCKET) {
        if(!socketclose(sock->fd)) {
            pdebug(DEBUG_WARN, "Error while closing socket: %d!", errno);
            rc = PLCTAG_ERR_CLOSE;
        }
    }

    sock->fd = INVALID_SOCKET;

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



int socket_destroy(sock_p *sock)
{
    pdebug(DEBUG_INFO, "Starting.");

    if(!sock || !*sock) {
        pdebug(DEBUG_ERROR, "Called with null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* make sure the socket is closed. */
    socket_close(*sock);

    /* Free any memory. */
    mem_free(*sock);

    *sock = NULL;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



int socket_callback_when_read_ready(sock_p sock, void (*callback)(void *context), void *context)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    critical_block(socket_event_mutex) {
        /* if we are enabling the event, set the FD and raise the recalc count. */
        if(callback) {
            FD_SET(sock->fd, &global_read_fds);
            max_socket_fd = (sock->fd > max_socket_fd ? sock->fd : max_socket_fd);
            atomic_int_add(&need_recalculate_socket_fd_sets, 1);
        } else {
            FD_CLR(sock->fd, &global_read_fds);
            if(sock->fd >= max_socket_fd) {
                /* the top FD has possibly changed, force a recalculation. */
                atomic_int_add(&need_recalculate_socket_fd_sets, 10);
            } else {
                /* there was a change, so raise the need for recalculation. */
                atomic_int_add(&need_recalculate_socket_fd_sets, 1);
            }
        }

        sock->read_ready_callback = callback;
        sock->read_ready_context = context;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


int socket_callback_when_write_ready(sock_p sock, void (*callback)(void *context), void *context)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    critical_block(socket_event_mutex) {
        /* if we are enabling the event, set the FD and raise the recalc count. */
        if(callback) {
            FD_SET(sock->fd, &global_write_fds);
            max_socket_fd = (sock->fd > max_socket_fd ? sock->fd : max_socket_fd);
            atomic_int_add(&need_recalculate_socket_fd_sets, 1);
        } else {
            FD_CLR(sock->fd, &global_write_fds);
            if(sock->fd >= max_socket_fd) {
                /* the top FD has possibly changed, force a recalculation. */
                atomic_int_add(&need_recalculate_socket_fd_sets, 10);
            } else {
                /* there was a change, so raise the need for recalculation. */
                atomic_int_add(&need_recalculate_socket_fd_sets, 1);
            }
        }

        sock->write_ready_callback = callback;
        sock->write_ready_context = context;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



#ifdef PLATFORM_IS_WINDOWS

/* Windows has no pipes, so open a socket to ourselves. Hax! Hax! */

int create_event_wakeup_channel(void)
{
    int rc = PLCTAG_STATUS_OK;
    SOCKET listener = INVALID_SOCKET;
    struct sockaddr_in listener_addr_info;
    struct sockaddr_in reader_addr_info;
    struct sockaddr_in writer_addr_info;
    socklen_t addr_info_size = sizeof(struct sockaddr_in);
    int non_blocking = 1;

    pdebug(DEBUG_INFO, "Starting.");

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
        if(bind(listener, (struct sockaddr *)&listener_addr_info, addr_info_size)){
            pdebug(DEBUG_WARN, "Error %d binding the listener socker!", WSAGetLastError())
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }

        /*
         * we need to get the address and port of the listener for later steps.
         * Notice that this _sets_ the address size!.
         */
        if(getsockname(listener, (struct sockaddr *)&listener_addr_info, &addr_info_size)) {
            pdebug(DEBUG_WARN, "Error %d getting the listener socker address info!", WSAGetLastError())
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }

        /* Phwew.   We can actually listen now. Notice that this is blocking! */
        if(listen(listener, 1)) { /* MAGIC constant - We do not want any real queue! */
            pdebug(DEBUG_WARN, "Error %d listening on the listener socket!", WSAGetLastError())
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }

        /*
         * now we start the next phase.   We need to connect to our own listener.
         * This will be the reader side of the wake up socket.
         */

        if(connect(wake_fds[0], (struct sockaddr *)&listener_addr_info, addr_info_size)) {
            pdebug(DEBUG_WARN, "Error %d connecting to the listener socket!", WSAGetLastError())
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }

        /* now we accept our own connection. This becomes the writer side. */
        wake_fds[1] = accept(listener, 0, 0);
        if (wake_fds[1] == INVALID_SOCKET) {
            pdebug(DEBUG_WARN, "Error %d connecting to the listener socket!", WSAGetLastError())
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }

        /* now we need to set these to non-blocking. */

        /* reader */
        if(ioctlsocket(wake_fds[0], FIONBIO, &non_blocking)) {
            pdebug(DEBUG_WARN, "Error %d setting reader socket to non-blocking!", WSAGetLastError())
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }

        /* writer */
        if(ioctlsocket(wake_fds[1], FIONBIO, &non_blocking)) {
            pdebug(DEBUG_WARN, "Error %d setting reader socket to non-blocking!", WSAGetLastError())
            rc = PLCTAG_ERR_WINSOCK;
            break;
        }
    } while(0);

    /* do some clean up */
    if(listener != INVALID_SOCKET) {
        closesocket(listener);
    }

    /* check the result */
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to set up wakeup socket!");

        if(wake_fds[0] != INVALID_SOCKET) {
            closesocket(wake_fds[0]);
            wake_fds[0] = INVALID_SOCKET;
        }

        if(wake_fds[1] != INVALID_SOCKET) {
            closesocket(wake_fds[1]);
            wake_fds[1] = INVALID_SOCKET;
        }
    } else {
        pdebug(DEBUG_INFO, "Done.");
    }


    return rc;
}



#else

/* standard POSIX */

int create_event_wakeup_channel(void)
{
    int rc = PLCTAG_STATUS_OK;
    int flags = 0;

    pdebug(DEBUG_INFO, "Starting.");

    do {
        /* open the pipe for waking the select wait. */
        if(pipe(wake_fds)) {
            pdebug(DEBUG_WARN, "Unable to open waker pipe!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* make the read pipe fd non-blocking. */
        if((flags = fcntl(wake_fds[0], F_GETFL)) < 0) {
            pdebug(DEBUG_WARN, "Unable to get flags of read pipe fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* set read fd non-blocking */
        flags |= O_NONBLOCK;

        if(fcntl(wake_fds[0], F_SETFL, flags) < 0) {
            pdebug(DEBUG_WARN, "Unable to set flags of read pipe fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* make the write pipe fd non-blocking. */
        if((flags = fcntl(wake_fds[1], F_GETFL)) < 0) {
            pdebug(DEBUG_WARN, "Unable to get flags of write pipe fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* set write fd non-blocking */
        flags |= O_NONBLOCK;

        if(fcntl(wake_fds[1], F_SETFL, flags) < 0) {
            pdebug(DEBUG_WARN, "Unable to set flags of write pipe fd!");
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to open waker pipe!");

        if(wake_fds[0] != INVALID_SOCKET) {
            close(wake_fds[0]);
            wake_fds[0] = INVALID_SOCKET;
        }

        if(wake_fds[1] != INVALID_SOCKET) {
            close(wake_fds[1]);
            wake_fds[1] = INVALID_SOCKET;
        }
    } else {
        pdebug(DEBUG_INFO, "Done.");
    }

    return rc;
}


#endif


void destroy_event_wakeup_channel(void)
{
    pdebug(DEBUG_INFO, "Starting.");

    if(wake_fds[0] != INVALID_SOCKET) {
        socketclose(wake_fds[0]);
        wake_fds[0] = INVALID_SOCKET;
    }

    if(wake_fds[1] != INVALID_SOCKET) {
        socketclose(wake_fds[1]);
        wake_fds[1] = INVALID_SOCKET;
    }

    pdebug(DEBUG_INFO, "Done.");
}




void socket_event_loop_wake(void)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    uint8_t dummy[32] = {0};
    write(wake_fds[1], &dummy[0], sizeof(dummy));

    pdebug(DEBUG_DETAIL, "Done.");
}



void socket_event_loop_tickler(int64_t next_wake_time, int64_t current_time)
{
    int recalc_count = 0;
    TIMEVAL timeval_wait;
    int64_t wait_time_ms = next_wake_time - current_time;
    fd_set local_read_fds;
    fd_set local_write_fds;
    int num_signaled_fds = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    /* do we need to recalculate fd sets for the sockets? */
    recalc_count = atomic_bool_get(&need_recalculate_socket_fd_sets);
    if(recalc_count >= 10) {
        pdebug(DEBUG_DETAIL, "Recalculating the select fd sets.");

        critical_block(socket_event_mutex) {

            max_socket_fd = -1;

            FD_ZERO(&global_read_fds);
            FD_ZERO(&global_write_fds);

            for(sock_p tmp_sock = socket_list; tmp_sock; tmp_sock = tmp_sock->next) {
                if(tmp_sock->read_ready_callback) {
                    FD_SET(tmp_sock->fd, &global_read_fds);

                    /* only do this inside the if.   Some events are not part of the fd set. */
                    max_socket_fd = (max_socket_fd < tmp_sock->fd ? tmp_sock->fd : max_socket_fd);
                }

                if(tmp_sock->write_ready_callback) {
                    FD_SET(tmp_sock->fd, &global_write_fds);

                    /* only do this inside the if.   Some events are not part of the fd set. */
                    max_socket_fd = (max_socket_fd < tmp_sock->fd ? tmp_sock->fd : max_socket_fd);
                }
            }

            /* make sure the wake channel is in the read set. */
            FD_SET(wake_fds[0], &global_read_fds);
            max_socket_fd = (max_socket_fd < wake_fds[0] ? wake_fds[0] : max_socket_fd);
        }

        /*
         * subtract the number we had before.   Other threads may have
         * incremented the counter since we started this if statement.
         * If so, then we will trigger again.
         *
         * This handles the race condition that the counter changes
         * after we recalculate and before we reset the counter.
         */
        atomic_int_add(&need_recalculate_socket_fd_sets, -recalc_count);
    }

    /* copy the fd sets safely */
    pdebug(DEBUG_DETAIL, "Code the FD sets for safety.");
    critical_block(socket_event_mutex) {
        local_read_fds = global_read_fds;
        local_write_fds = global_write_fds;
    }

    pdebug(DEBUG_DETAIL, "Calculating the wait time.");

    if(wait_time_ms < 0) {
        pdebug(DEBUG_WARN, "Wait time was less than zero!");
        wait_time_ms = 0;
    }

    if(wait_time_ms > TICKLER_MAX_WAIT_TIME_MS) {
        pdebug(DEBUG_DETAIL, "Clamping wait time to %d milliseconds.", TICKLER_MAX_WAIT_TIME_MS);
        wait_time_ms = TICKLER_MAX_WAIT_TIME_MS;
    }

    timeval_wait.tv_sec = 0;
    timeval_wait.tv_usec = wait_time_ms * 1000;

    num_signaled_fds = select(max_socket_fd + 1, &local_read_fds, &local_write_fds, NULL, &timeval_wait);
    if(num_signaled_fds > 0) {
        /* Were we woken up by the waker channel? */
        if(FD_ISSET(wake_fds[0], &local_read_fds)) {
            uint8_t dummy[48];

            /* pull all the data out to clear the status. */
            while(read(wake_fds[0], &dummy[0], sizeof(dummy)) > 0) { /* nothing */ }

            num_signaled_fds--;
        }

        /* is there still work to do? */
        if(num_signaled_fds > 0) {
            /* the callbacks can mutate the socket list and the global FD sets. */
            critical_block(socket_event_mutex) {
                /* loop, but use an iterator so that sockets can be closed while in the mutex. */
                for(global_socket_iterator = socket_list; global_socket_iterator; global_socket_iterator = global_socket_iterator->next) {
                    sock_p sock = global_socket_iterator;

                    if(sock->read_ready_callback && FD_ISSET(sock->fd, &local_read_fds)) {
                        FD_CLR(sock->fd, &global_read_fds);
                        atomic_int_add(&need_recalculate_socket_fd_sets, 1);
                        pdebug(DEBUG_DETAIL, "Socket %d has data to read.", sock->fd);
                        sock->read_ready_callback(sock->read_ready_context);
                    }

                    if(sock->write_ready_callback && FD_ISSET(sock->fd, &local_write_fds)) {
                        FD_CLR(sock->fd, &global_write_fds);
                        atomic_int_add(&need_recalculate_socket_fd_sets, 1);
                        pdebug(DEBUG_DETAIL, "Socket %d can write data.", sock->fd);
                        sock->write_ready_callback(sock->write_ready_context);
                    }
                }
            }
        }
    }
}



int socket_event_loop_init(void)
{
    int rc = PLCTAG_STATUS_OK;

#ifdef PLATFORM_IS_WINDOWS
	static WSADATA winsock_data;
#endif

    pdebug(DEBUG_DETAIL,"Starting.");

#ifdef PLATFORM_IS_WINDOWS
	/* Windows needs special initialization. */
	rc = WSAStartup(MAKEWORD(2, 2), &winsock_data);
	if (rc != NO_ERROR) {
		info("WSAStartup failed with error: %d\n", rc);
		return SOCKET_ERR_STARTUP;
	}

#endif

    /* set up the waker channel, platform specific. */
    rc = create_event_wakeup_channel();
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to create socket wake up!");
        return rc;
    }

    /* clear out the fd sets */
    FD_ZERO(&global_read_fds);
    FD_ZERO(&global_write_fds);

    /* set the fds for the waker channel */
    FD_SET(wake_fds[0], &global_read_fds);
    FD_SET(wake_fds[1], &global_write_fds);

    socket_list = NULL;
    global_socket_iterator = NULL;

    /* create the socket mutex. */
    rc = mutex_create(&socket_event_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to create socket mutex!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



void socket_event_loop_teardown(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /* destroy the socket mutex. */
    rc = mutex_destroy(&socket_event_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to destroy socket mutex!");
        return;
    }

    socket_event_mutex = NULL;

    FD_ZERO(&global_read_fds);
    FD_ZERO(&global_write_fds);

    /* if there were sockets left, oh well, leak away! */
    socket_list = NULL;
    global_socket_iterator = NULL;

    atomic_bool_set(&need_recalculate_socket_fd_sets, 0);

    destroy_event_wakeup_channel();

#ifdef PLATFORM_IS_WINDOWS
	/* Windows needs special teardown. */
    WSACleanup();
#endif

    pdebug(DEBUG_INFO, "Done.");
}
