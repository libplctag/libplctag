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


#define MAX_RECALC_FD_SET_PRESSURE (10)


#define SET_STATUS(status_field, status) atomic_int8_store(&(status_field), (int8_t)(status))
#define GET_STATUS(status_field) (int)atomic_int8_load(&(status_field))

enum {
    CONNECTION_THREAD_NOT_RUNNING,
    CONNECTION_THREAD_RUNNING,
    CONNECTION_THREAD_TERMINATE,
    CONNECTION_THREAD_DEAD
};

struct sock_t {
    /* next in the list of sockets. */
    struct sock_t *next;

    socket_t fd;

    int port;
    const char *host;

    atomic_int8 status;

    /* callbacks for events. */
    thread_p connection_thread;
    atomic_bool connection_thread_done;
    void (*connection_ready_callback)(void *context);
    void *connection_ready_context;

    void (*read_done_callback)(void *context);
    void *read_done_context;
    uint8_t *read_buffer;
    int read_buffer_capacity;
    int *read_amount;

    void (*write_done_callback)(void *context);
    void *write_done_context;
    uint8_t *write_buffer;
    int *write_amount;
};


static mutex_p socket_event_mutex = NULL;
static sock_p socket_list = NULL;
static sock_p global_socket_iterator = NULL;

// static THREAD_LOCAL atomic_bool socket_event_mutex_held_in_this_thread = ATOMIC_BOOL_STATIC_INIT(FALSE);

static int wake_fds[2] = { INVALID_SOCKET, INVALID_SOCKET};
static fd_set global_read_fds;
static fd_set global_write_fds;
static int max_socket_fd = -1;
static atomic_int32 recalculate_fd_set_pressure = ATOMIC_BOOL_STATIC_INIT(1);
static atomic_int32 connect_thread_complete_count = ATOMIC_INT_STATIC_INIT(0);

#define MAX_IPS (8)

static int create_event_wakeup_channel(void);
static void destroy_event_wakeup_channel(void);

static THREAD_FUNC(socket_connection_thread_func);


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

    /* set up connection callback support */
    (*sock)->connection_thread = NULL;
    atomic_bool_store(&((*sock)->connection_thread_done), FALSE);
    (*sock)->connection_ready_callback = NULL;
    (*sock)->connection_ready_context = NULL;

    /* read callback support */
    (*sock)->read_done_callback = NULL;
    (*sock)->read_done_context = NULL;

    /* write callback support */
    (*sock)->write_done_callback = NULL;
    (*sock)->write_done_context = NULL;

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

    timeout.tv_sec = 10; /* MAGIC */
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
    thread_p connection_thread = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    if(!sock) {
        return PLCTAG_ERR_NULL_PTR;
    }

    /*
     * remove it from the list and clear the FD sets.
     * These are changed by the event thread too, so
     * all this has to be in the mutex.
     */
    critical_block(socket_event_mutex) {
        sock_p *walker = &socket_list;

        while(*walker && *walker != sock) {
            walker = &((*walker)->next);
        }

        if(*walker && *walker == sock) {
            *walker = sock->next;
        } /* else not on the list. */

        /* fix up the iterator. */
        if(global_socket_iterator == sock) {
            pdebug(DEBUG_DETAIL, "Iterator is processing this socket, moving iterator.");
            global_socket_iterator = sock->next;
        }

        /* clear the FD sets */
        FD_CLR(sock->fd, &global_read_fds);
        FD_CLR(sock->fd, &global_write_fds);

        /* force a recalculation of the FD set and max socket if we are the top one. */
        if(sock->fd >= max_socket_fd) {
            atomic_int32_add(&recalculate_fd_set_pressure, (int32_t)MAX_RECALC_FD_SET_PRESSURE);
        } else {
            /* not as urgent, recalculate eventually. */
            atomic_int32_add(&recalculate_fd_set_pressure, (int32_t)1);
        }

        /* close the socket fd */
        if(sock->fd != INVALID_SOCKET) {
            if(!socketclose(sock->fd)) {
                pdebug(DEBUG_WARN, "Error while closing socket: %d!", errno);
                rc = PLCTAG_ERR_CLOSE;
            }

            sock->fd = INVALID_SOCKET;
        }

        /* make sure the event thread will not try to handle this. */
        connection_thread = sock->connection_thread;
        sock->connection_thread = NULL;

        /* clear any link left in the socket object. */
        sock->next = NULL;
    }

    /*
     * now that the socket is removed from the list, the event thread
     * will not trigger it.
     *
     * This is racy if another call to socket_callback_when_connection_ready() is made by
     * another thread when this is running.  Don't do that.
     */

    /*
     * check the connection thread.  If it is still running, then we need to terminate it.
     * But we need to do this outside the mutex because it could hang for a very long time.
     */

    if(connection_thread) {
        pdebug(DEBUG_INFO, "Waiting for connection thread to terminate.");

        thread_join(connection_thread);
        thread_destroy(&connection_thread);

        pdebug(DEBUG_INFO, "Connection thread terminated.");
    }

    /* now remove the callbacks. */
    sock->connection_ready_callback = NULL;
    sock->connection_ready_context = NULL;

    sock->read_done_callback = NULL;
    sock->read_done_context = NULL;

    sock->write_done_callback = NULL;
    sock->write_done_context = NULL;

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

    /* free anything left over. */
    if((*sock)->host) {
        mem_free((*sock)->host);
        (*sock)->host = NULL;
    }

    /* Free any memory. */
    mem_free(*sock);

    *sock = NULL;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int socket_status(sock_p sock)
{
    if(sock) {
        return GET_STATUS(sock->status);
    } else {
        return PLCTAG_ERR_NULL_PTR;
    }
}


THREAD_FUNC(socket_connection_thread_func)
{
    sock_p sock = (sock_p)arg;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for %s:%d.", sock->host, sock->port);

    rc = socket_tcp_connect(sock, sock->host, sock->port);

    /* save our status for other callers. */
    SET_STATUS(sock->status, rc);

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Blocking TCP connect call to %s:%d complete.", sock->host, sock->port);
    } else {
        pdebug(DEBUG_INFO, "Blocking TCP connect call to %s:%d failed with error %s.", sock->host, sock->port, plc_tag_decode_error(rc));
    }

    /* call the callback if there is one. */
    if(sock->connection_ready_callback) {
        pdebug(DEBUG_INFO, "Calling connection callback for socket %s:%d.", sock->host, sock->port);
        sock->connection_ready_callback(sock->connection_ready_context);
    }

    /* set a flag so that the system knows we are done. */
    atomic_bool_store(&(sock->connection_thread_done), TRUE);
    atomic_int32_add(&connect_thread_complete_count, (int32_t)1);

    /* make sure the event thread cleans up this thread soon. */
    socket_event_loop_wake();

    pdebug(DEBUG_INFO, "Done for %s:%d.", sock->host, sock->port);

    THREAD_RETURN(0);
}




int socket_callback_when_connection_ready(sock_p sock, void (*callback)(void *context), void *context, const char *host, int port)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for %s:%d.", host, port);

    if(!sock) {
        pdebug(DEBUG_WARN, "Called with null socket pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    critical_block(socket_event_mutex) {
        /* clean up anything left over. */
        if(sock->host) {
            mem_free(sock->host);
            sock->host = NULL;
        }

        sock->host = str_dup(host);
        if(!sock->host) {
            pdebug(DEBUG_WARN, "Unable to allocate memory for host name copy!");
            rc = PLCTAG_ERR_NO_MEM;
            break;
        }

        sock->port = port;

        /* set up the callback info before we create the thread. */
        sock->connection_ready_callback = callback;
        sock->connection_ready_context = context;

        /* set the flag so that we know the connection thread is running. */
        atomic_bool_store(&(sock->connection_thread_done), FALSE);

        /* set the socket status that we are in flight. */
        SET_STATUS(sock->status, PLCTAG_STATUS_PENDING);

        /* set up the connection thread. */
        rc = thread_create(&(sock->connection_thread), socket_connection_thread_func, 32768, sock);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to create background connection thread!");

            sock->connection_thread = NULL;
            SET_STATUS(sock->status, rc);
            atomic_bool_store(&(sock->connection_thread_done), TRUE);

            break;
        }
    }

    pdebug(DEBUG_INFO, "Done for %s:%d.", host, port);

    return rc;
}




int socket_callback_when_read_done(sock_p sock, void (*callback)(void *context), void *context, uint8_t *buffer, int buffer_capacity, int *amount)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(!sock) {
        pdebug(DEBUG_WARN, "Called with null socket pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    critical_block(socket_event_mutex) {
        /* if we are enabling the event, set the FD and raise the recalc count. */
        if(callback) {
            pdebug(DEBUG_DETAIL, "Setting up new read complete callback.");

            /* set the socket status that we are in flight. */
            SET_STATUS(sock->status, PLCTAG_STATUS_PENDING);

            /* set the FD set and max socket level. */
            FD_SET(sock->fd, &global_read_fds);
            max_socket_fd = (sock->fd > max_socket_fd ? sock->fd : max_socket_fd);

            atomic_int32_add(&recalculate_fd_set_pressure, (int32_t)1);
        } else {
            pdebug(DEBUG_DETAIL, "Clearing read complete callback.");

            /* set the socket status that we are NOT in flight. */
            SET_STATUS(sock->status, PLCTAG_STATUS_OK);

            FD_CLR(sock->fd, &global_read_fds);
            if(sock->fd >= max_socket_fd) {
                /* the top FD has possibly changed, force a recalculation. */
                atomic_int32_add(&recalculate_fd_set_pressure, (int32_t)MAX_RECALC_FD_SET_PRESSURE);
            } else {
                /* there was a change, so raise the need for recalculation. */
                atomic_int32_add(&recalculate_fd_set_pressure, (int32_t)1);
            }
        }

        sock->read_done_callback = callback;
        sock->read_done_context = context;
        sock->read_buffer = buffer;
        sock->read_buffer_capacity = buffer_capacity;
        sock->read_amount = amount;

        /* wake up the socket select() that could be waiting. */
        socket_event_loop_wake();
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


int socket_callback_when_write_done(sock_p sock, void (*callback)(void *context), void *context, uint8_t *buffer, int *amount)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(!sock) {
        pdebug(DEBUG_WARN, "Called with null socket pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    critical_block(socket_event_mutex) {
        /* if we are enabling the event, set the FD and raise the recalc count. */
        if(callback) {
            /* set the socket status that we are in flight. */
            SET_STATUS(sock->status, PLCTAG_STATUS_PENDING);

            FD_SET(sock->fd, &global_write_fds);
            max_socket_fd = (sock->fd > max_socket_fd ? sock->fd : max_socket_fd);

            atomic_int32_add(&recalculate_fd_set_pressure, (int32_t)1);
        } else {
            /* set the socket status that we are NOT in flight. */
            SET_STATUS(sock->status, PLCTAG_STATUS_OK);

            FD_CLR(sock->fd, &global_write_fds);
            if(sock->fd >= max_socket_fd) {
                /* the top FD has possibly changed, force a recalculation. */
                atomic_int32_add(&recalculate_fd_set_pressure, (int32_t)MAX_RECALC_FD_SET_PRESSURE);
            } else {
                /* there was a change, so raise the need for recalculation. */
                atomic_int32_add(&recalculate_fd_set_pressure, (int32_t)1);
            }
        }

        sock->write_done_callback = callback;
        sock->write_done_context = context;
        sock->write_buffer = buffer;
        sock->write_amount = amount;

        /* wake up the socket select() that could be waiting. */
        socket_event_loop_wake();
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
    int bytes_written = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    uint8_t dummy[16] = {0};
    bytes_written = (int)write(wake_fds[1], &dummy[0], sizeof(dummy));

    if(bytes_written >= 0) {
        pdebug(DEBUG_DETAIL, "Wrote %d bytes to the wake channel.", bytes_written);
    } else {
        pdebug(DEBUG_WARN, "Error writing to wake channel!");
    }

    pdebug(DEBUG_DETAIL, "Done.");
}



void socket_event_loop_tickler(int64_t next_wake_time, int64_t current_time)
{
    int32_t recalc_count = 0;
    int32_t connection_thread_cleanup_count = 0;
    TIMEVAL timeval_wait;
    int64_t wait_time_ms = next_wake_time - current_time;
    fd_set local_read_fds;
    fd_set local_write_fds;
    int num_signaled_fds = 0;
    int byte_count = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    /* do we need to recalculate fd sets for the sockets? */
    recalc_count = atomic_int32_load(&recalculate_fd_set_pressure);
    // DEBUG FIXME
    if(recalc_count >= MAX_RECALC_FD_SET_PRESSURE) {
        pdebug(DEBUG_DETAIL, "Recalculating the select fd sets.");

        critical_block(socket_event_mutex) {

            max_socket_fd = -1;

            FD_ZERO(&global_read_fds);
            FD_ZERO(&global_write_fds);

            for(sock_p tmp_sock = socket_list; tmp_sock; tmp_sock = tmp_sock->next) {
                if(tmp_sock->read_done_callback) {
                    pdebug(DEBUG_DETAIL, "Setting select() read fd set for fd %d.", tmp_sock->fd);

                    FD_SET(tmp_sock->fd, &global_read_fds);

                    /* only do this inside the if.   Some events are not part of the fd set. */
                    max_socket_fd = (max_socket_fd < tmp_sock->fd ? tmp_sock->fd : max_socket_fd);
                }

                if(tmp_sock->write_done_callback) {
                    pdebug(DEBUG_DETAIL, "Setting select() write fd set for fd %d.", tmp_sock->fd);

                    FD_SET(tmp_sock->fd, &global_write_fds);

                    /* only do this inside the if.   Some events are not part of the fd set. */
                    max_socket_fd = (max_socket_fd < tmp_sock->fd ? tmp_sock->fd : max_socket_fd);
                }
            }

            /* make sure the wake channel is in the read set. */
            pdebug(DEBUG_DETAIL, "Setting select() read fd set for wake fd %d.", wake_fds[0]);
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
        atomic_int32_sub(&recalculate_fd_set_pressure, recalc_count);
    }

    /* do we need to clean up any dead connection threads? */
    connection_thread_cleanup_count = atomic_int32_load(&connect_thread_complete_count);
    if(connection_thread_cleanup_count > 0) {
        critical_block(socket_event_mutex) {
            sock_p sock = socket_list;

            while(sock) {
                /* does this socket have a completed connection thread? */
                if(sock->connection_thread && (atomic_bool_load(&(sock->connection_thread_done)) == TRUE)) {
                    pdebug(DEBUG_INFO, "Cleaning up connection thread for socket %s:%d.", sock->host, sock->port);
                    thread_join(sock->connection_thread);
                    thread_destroy(&(sock->connection_thread));
                    sock->connection_thread = NULL;
                }
                sock = sock->next;
            }
        }

        /*
         * subtract the number we had before.   Other threads may have
         * incremented the counter since we started this if statement.
         * If so, then we will trigger again.
         *
         * This handles the race condition that the counter changes
         * after we recalculate and before we reset the counter.
         */
        atomic_int32_sub(&connect_thread_complete_count, connection_thread_cleanup_count);
    }

    /* copy the fd sets safely */
    pdebug(DEBUG_DETAIL, "Copy the FD sets for safety.");
    critical_block(socket_event_mutex) {
        local_read_fds = global_read_fds;
        local_write_fds = global_write_fds;

        FD_SET(wake_fds[0], &local_read_fds);

        max_socket_fd = (wake_fds[0] > max_socket_fd ? wake_fds[0] : max_socket_fd);
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

    pdebug(DEBUG_DETAIL, "Calling select().");
    num_signaled_fds = select(max_socket_fd + 1, &local_read_fds, &local_write_fds, NULL, &timeval_wait);
    pdebug(DEBUG_DETAIL, "select() returned with status %d.", num_signaled_fds);

    // for(int index=0; index < (max_socket_fd+1); index++) {
    //     if(FD_ISSET(index, &local_read_fds)) {
    //         pdebug(DEBUG_SPEW, "fd %d is set for reading.", index);
    //     } else {
    //         pdebug(DEBUG_SPEW, "fd %d is NOT set for reading.", index);
    //     }
    //     if(FD_ISSET(index, &local_write_fds)) {
    //         pdebug(DEBUG_SPEW, "fd %d is set for writing.", index);
    //     } else {
    //         pdebug(DEBUG_SPEW, "fd %d is NOT set for writing.", index);
    //     }
    // }

    // FIXME DEBUG
    if(num_signaled_fds > 0) {
        /* Were we woken up by the waker channel? */
        if(FD_ISSET(wake_fds[0], &local_read_fds)) {
            uint8_t dummy[48];
            int bytes_read = 0;

            pdebug(DEBUG_DETAIL, "Wake channel is active.");

            /* pull all the data out to clear the status. */
            while((bytes_read = (int)read(wake_fds[0], &dummy[0], sizeof(dummy))) > 0) {
                pdebug(DEBUG_SPEW, "Read %d bytes from the wake channel.", bytes_read);
            }

            num_signaled_fds--;
        }

        /* is there still work to do? */
        if(num_signaled_fds > 0) {
            /* the callbacks can mutate the socket list and the global FD sets. */
            critical_block(socket_event_mutex) {
                /* loop, but use an iterator so that sockets can be closed while in the mutex. */
                for(global_socket_iterator = socket_list; global_socket_iterator; (global_socket_iterator ? global_socket_iterator = global_socket_iterator->next : NULL)) {
                    sock_p sock = global_socket_iterator;
                    int rc = PLCTAG_STATUS_OK;

                    if(sock->read_done_callback && FD_ISSET(sock->fd, &local_read_fds)) {
                        pdebug(DEBUG_DETAIL, "Socket %d has data to read.", sock->fd);

                        byte_count = socket_tcp_read(sock, sock->read_buffer + *(sock->read_amount), sock->read_buffer_capacity - *(sock->read_amount));
                        if(byte_count > 0) {
                            /* we got data! */
                            pdebug(DEBUG_DETAIL, "IO thread read data:");
                            pdebug_dump_bytes(DEBUG_DETAIL, sock->read_buffer + *(sock->read_amount), byte_count);

                            *(sock->read_amount) += byte_count;

                            rc = PLCTAG_STATUS_OK;
                        } else if(byte_count < 0) {
                            /* set the status if we had an error. */
                            pdebug(DEBUG_WARN, "Got error %s trying to read from socket %s:%d!", plc_tag_decode_error(rc), sock->host, sock->port);
                            rc = byte_count;
                        }

                        if(byte_count != 0) {
                            void (*callback)(void *) = sock->read_done_callback;

                            if(byte_count > 0) {
                                pdebug(DEBUG_DETAIL, "Read had %d bytes of data.", byte_count);
                            } else {
                                pdebug(DEBUG_DETAIL, "Read had error %s!", plc_tag_decode_error(rc));
                            }

                            /* do not keep calling this, we got data or an error. */
                            FD_CLR(sock->fd, &global_read_fds);
                            sock->read_done_callback = NULL;

                            /* tickle the recalculation. */
                            atomic_int32_add(&recalculate_fd_set_pressure, (uint32_t)1);

                            /* set up state. */
                            SET_STATUS(sock->status, rc);

                            /* call the callback either way. */
                            if(callback) {
                                callback(sock->read_done_context);
                            } else {
                                pdebug(DEBUG_DETAIL, "Write complete callback is NULL.");
                            }
                        } /* else no data and not an error, just keep waiting. */
                    }

                    if(sock->write_done_callback && FD_ISSET(sock->fd, &local_write_fds)) {
                        pdebug(DEBUG_DETAIL, "Socket %d can write data.", sock->fd);

                        rc = PLCTAG_STATUS_PENDING;

                        byte_count = socket_tcp_write(sock, sock->write_buffer, *(sock->write_amount));
                        if(byte_count > 0) {
                            /* we sent data */
                            pdebug(DEBUG_DETAIL, "IO thread wrote data:");
                            pdebug_dump_bytes(DEBUG_DETAIL, sock->write_buffer, byte_count);

                            /* advance the buffer pointer */
                            sock->write_buffer += byte_count;
                            *(sock->write_amount) -= byte_count;

                            pdebug(DEBUG_DETAIL, "Wrote %d bytes, %d remaining.", byte_count, *(sock->write_amount));

                            rc = PLCTAG_STATUS_OK;
                        } else if(byte_count < 0) {
                            /* error! */
                            pdebug(DEBUG_WARN, "Got error %s trying to write data to socket %d!", plc_tag_decode_error(byte_count), sock->fd);

                            rc = byte_count;
                        }

                        /* are we done writing?   Either all data sent or an error. */
                        if(*(sock->write_amount) <= 0 || rc < 0) {
                            void (*callback)(void *) = sock->write_done_callback;

                            if(rc < 0) {
                                pdebug(DEBUG_DETAIL, "Write error %d.", plc_tag_decode_error(rc));
                            } else {
                                pdebug(DEBUG_DETAIL, "Write done.");
                            }

                            /* prevent another call. */
                            FD_CLR(sock->fd, &global_write_fds);
                            sock->write_done_callback = NULL;

                            atomic_int32_add(&recalculate_fd_set_pressure, (int32_t)1);

                            /* call the callback */
                            SET_STATUS(sock->status, rc);

                            if(callback) {
                                callback(sock->write_done_context);
                            } else {
                                pdebug(DEBUG_DETAIL, "Write complete callback is NULL.");
                            }
                        }

                        /* if the byte_count was zero then we keep trying. */
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
//    FD_SET(wake_fds[1], &global_write_fds);

    socket_list = NULL;
    global_socket_iterator = NULL;

    atomic_int32_store(&connect_thread_complete_count, (uint32_t)0);

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

    atomic_int32_store(&recalculate_fd_set_pressure, (int32_t)0);

    destroy_event_wakeup_channel();

#ifdef PLATFORM_IS_WINDOWS
	/* Windows needs special teardown. */
    WSACleanup();
#endif

    pdebug(DEBUG_INFO, "Done.");
}
