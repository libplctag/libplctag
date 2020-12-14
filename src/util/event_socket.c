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


#include <lib/libplctag.h>
#include <platform.h>
#include <util/atomic_int.h>
#include <util/debug.h>
#include <util/event_socket.h>

#ifdef POSIX_PLATFORM
    #include <sys/select.h>
#endif


#define SELECT_WAIT_TIME_uS (20000)


struct event_socket_s {
    sock_p sock;
    atomic_int event_mask;
    struct event_socket_s *next;
    void *context;
    void (*connect_callback)(struct event_socket_s *psock, int64_t current_time, void *context);
    int (*can_read_callback)(struct event_socket_s *psock, int64_t current_time, void *context, sock_p sock);
    int (*can_write_callback)(struct event_socket_s *psock, int64_t current_time, void *context, sock_p sock);
    void (*close_callback)(struct event_socket_s *psock, int64_t current_time, void *context);
};



static mutex_p event_socket_mutex = NULL;
static thread_p event_socket_thread = NULL;
static event_socket_p event_socket_list = NULL;
static atomic_int termination_flag = ATOMIC_INT_STATIC_INIT(0);
static atomic_int recalc_fd_set_flag = ATOMIC_INT_STATIC_INIT(0);
static fd_set event_socket_read_fds;
static fd_set event_socket_write_fds;
static socket_t event_socket_max_fds = 0;

static THREAD_FUNC(event_socket_handler);
static void recalc_fd_sets(void);


int event_socket_create(event_socket_p *event_sock,
                        void *context,
                        event_socket_event_t event_mask,
                        void (*connect_callback)(event_socket_p esock, int64_t current_time, void *context),
                        int (*can_read_callback)(event_socket_p esock, int64_t current_time, void *context, sock_p sock),
                        int (*can_write_callback)(event_socket_p esock, int64_t current_time, void *context), sock_p sock,
                        void (*close_callback)(event_socket_p esock, int64_t current_time, void *context))
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(!event_sock) {
        pdebug(DEBUG_WARN, "Pointer to event socket location is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(*event_sock) {
        pdebug(DEBUG_WARN, "Event socket pointer points to valid memory!");
        return PLCTAG_ERR_BAD_DATA;
    }

    *event_sock = mem_alloc(sizeof(**event_sock));
    if(!*event_sock) {
        pdebug(DEBUG_WARN, "Unable to allocate memory for event socket!");
        return PLCTAG_ERR_NO_MEM;
    }

    /* create the socket */
    rc = socket_create(&((*event_sock)->sock));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create socket!");
        mem_free(*event_sock);
        *event_sock = NULL;
        return rc;
    }

    /* set what we can */
    (*event_sock)->next = NULL;
    (*event_sock)->context = context;
    atomic_int_set(&((*event_sock)->event_mask), event_mask);
    (*event_sock)->connect_callback = connect_callback;
    (*event_sock)->can_read_callback = can_read_callback;
    (*event_sock)->can_write_callback = can_write_callback;
    (*event_sock)->close_callback = close_callback;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



int event_socket_connect(event_socket_p event_sock, const char *host, int port)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(!event_sock) {
        pdebug(DEBUG_WARN, "Pointer to event socket location is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* open the socket */

    /* FIXME - this will hang in the client/app as it is blocking. */

    rc = socket_connect_tcp(event_sock->sock, host, port);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to open TCP socket connection!");
        socket_close(event_sock->sock);
        return rc;
    }

    /* add the protocol socket to the list */
    critical_block(event_socket_mutex) {
        event_sock->next = event_socket_list;
        event_socket_list = event_sock;
    }

    /* if there is a callback for socket connect, call it. */
    if(atomic_int_get(&(event_sock->event_mask)) & EVENT_SOCKET_CONNECT) {
        if(event_sock->connect_callback) {
            event_sock->connect_callback(event_sock, time_ms(), event_sock->context);
        }
    }

    /* mark for FD set recalculation */
    atomic_int_add(&recalc_fd_set_flag, 1);

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



int event_socket_close(event_socket_p event_sock)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(!event_sock) {
        pdebug(DEBUG_WARN, "Pointer to event socket location is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* if there is a callback for socket close, call it. */
    if(atomic_int_get(&(event_sock->event_mask)) & EVENT_SOCKET_CLOSE) {
        if(event_sock->close_callback) {
            event_sock->close_callback(event_sock, time_ms(), event_sock->context);
        }
    }

    /* remove the event socket from the list. */
    critical_block(event_socket_mutex) {
        event_socket_p *sock_walker = &event_socket_list;

        while(*sock_walker && *sock_walker != event_sock) {
            sock_walker = &((*sock_walker)->next);
        }

        if(*sock_walker && *sock_walker == event_sock) {
            *sock_walker = event_sock->next;
            event_sock->next = NULL;
        }
    }

    /* close the socket. */
    rc = socket_close(event_sock->sock);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, closing socket!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int event_socket_destroy(event_socket_p *event_sock)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(!event_sock) {
        pdebug(DEBUG_WARN, "Pointer to event socket location is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!*event_sock) {
        pdebug(DEBUG_WARN, "Pointer to event socket is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* in case it was not already closed. */
    rc = socket_close((*event_sock)->sock);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, closing socket!", plc_tag_decode_error(rc));
    }

    rc = socket_destroy(&((*event_sock)->sock));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, destroying socket!", plc_tag_decode_error(rc));
    }

    /* free the event socket */
    mem_free(*event_sock);
    *event_sock = NULL;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int event_socket_enable_events(event_socket_p event_sock, event_socket_event_t event)
{
    pdebug(DEBUG_INFO, "Starting.");

    if(!event_sock) {
        pdebug(DEBUG_WARN, "Pointer to event socket location is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* set the event mask */
    atomic_int_or(&(event_sock->event_mask), event);

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int event_socket_disable_events(event_socket_p event_sock, event_socket_event_t event)
{
    pdebug(DEBUG_INFO, "Starting.");

    if(!event_sock) {
        pdebug(DEBUG_WARN, "Pointer to event socket location is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* set the event mask */
    atomic_int_xor(&(event_sock->event_mask), event);

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


/******************************************************************
 ***************************** Timers *****************************
 ******************************************************************/


struct timer_s {
    struct timer_s *next;
    void *context;
    int64_t wake_time;
    int (*timer_callback)(timer_p timer, int64_t current_time, void *context);
};


static mutex_p timer_mutex = NULL;
static timer_p timer_list = NULL;

static int64_t timer_tickler(int64_t);

int timer_create(timer_p *timer,
                 void *context,
                 int (*timer_callback)(timer_p timer, int64_t current_time, void *context))
{
    pdebug(DEBUG_INFO, "Starting.");

    if(!timer) {
        pdebug(DEBUG_WARN, "Pointer to timer location is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(*timer) {
        pdebug(DEBUG_WARN, "Pointer to non-null pointer!");
        return PLCTAG_ERR_BAD_DATA;
    }

    *timer = mem_alloc(sizeof(**timer));
    if(!*timer) {
        pdebug(DEBUG_WARN, "Unable to allocate memory for new timer!");
        return PLCTAG_ERR_NO_MEM;
    }

    (*timer)->next = NULL;
    (*timer)->context = context;
    (*timer)->timer_callback = timer_callback;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int timer_destroy(timer_p *timer)
{
    pdebug(DEBUG_INFO, "Starting.");

    if(!timer) {
        pdebug(DEBUG_WARN, "Pointer to timer location is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!*timer) {
        pdebug(DEBUG_WARN, "Pointer to null pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* make sure the timer is removed. */
    critical_block(timer_mutex) {
        timer_p *timer_walker = &timer_list;

        while(*timer_walker && *timer_walker != *timer) {
            timer_walker = &((*timer_walker)->next);
        }

        if(*timer_walker && *timer_walker == *timer) {
            *timer_walker = (*timer)->next;
        }
    }

    (*timer)->next = NULL;

    /*free the memory */
    mem_free(*timer);

    *timer = NULL;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int timer_set(timer_p timer, int64_t wake_time)
{
    pdebug(DEBUG_INFO, "Starting.");

    if(!timer) {
        pdebug(DEBUG_WARN, "Pointer to timer location is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    timer->wake_time = wake_time;

    /* make sure the timer is removed first, then add it in the right position. */
    critical_block(timer_mutex) {
        timer_p *timer_walker = &timer_list;

        while(*timer_walker && *timer_walker != timer) {
            timer_walker = &((*timer_walker)->next);
        }

        if(*timer_walker && *timer_walker == timer) {
            *timer_walker = timer->next;
        }

        /* now add the timer into the right location. */
        timer_walker = &timer_list;

        while(*timer_walker && (*timer_walker)->wake_time <= wake_time) {
            timer_walker = &((*timer_walker)->next);
        }

        timer->next = *timer_walker;
        *timer_walker = timer;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int timer_abort(timer_p timer)
{
    pdebug(DEBUG_INFO, "Starting.");

    if(!timer) {
        pdebug(DEBUG_WARN, "Pointer to timer location is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* make sure the timer is removed */
    critical_block(timer_mutex) {
        timer_p *timer_walker = &timer_list;

        while(*timer_walker && *timer_walker != timer) {
            timer_walker = &((*timer_walker)->next);
        }

        if(*timer_walker && *timer_walker == timer) {
            *timer_walker = timer->next;
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


void event_socket_tickler(void)
{
    struct timeval tv;
    int rc = PLCTAG_STATUS_OK;
    int64_t current_time = 0;
    int64_t earliest_wake_time = 0;
    int64_t usec_sleep_time = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    /* how long should we sleep? */
    current_time = time_ms();
    earliest_wake_time = timer_tickler(current_time);

    if(earliest_wake_time <= current_time) {
        usec_sleep_time = SELECT_WAIT_TIME_uS;
    } else {
        usec_sleep_time = 1000 * (earliest_wake_time - current_time);
    }

    tv.tv_sec = 0;
    tv.tv_usec = usec_sleep_time;

    if(atomic_int_get(&recalc_fd_set_flag) > 0) {
        recalc_fd_sets();

        /* we cannot just clear the flag, it could have been tweaked by another thread. */
        atomic_int_add(&recalc_fd_set_flag, -1);
    }

    /* wait for something to happen. */
    rc = select(event_socket_max_fds+1, &event_socket_read_fds, &event_socket_write_fds, NULL, &tv);
    if(rc >= 0) {
        pdebug(DEBUG_SPEW, "some socket is ready or we timed out!");

        critical_block(event_socket_mutex) {
            event_socket_p *sock_walker = &event_socket_list;

            while(*sock_walker) {
                event_socket_p event_sock = *sock_walker;

                if(rc > 0) {
                    if((atomic_int_get(&(event_sock->event_mask)) & EVENT_SOCKET_CAN_READ) && FD_ISSET(socket_get_fd(event_sock->sock), &event_socket_read_fds)) {
                        rc = event_sock->can_read_callback(event_sock, current_time, event_sock->context, event_sock->sock);

                        /* if we get an OK, then the event should not be rescheduled. */
                        if(rc == PLCTAG_STATUS_OK) {
                            /* unlink from the list */
                            *sock_walker = event_sock->next;
                        } /* else leave it in place. */
                    } else if((atomic_int_get(&(event_sock->event_mask)) & EVENT_SOCKET_CAN_WRITE) && FD_ISSET(socket_get_fd(event_sock->sock), &event_socket_write_fds)) {
                        rc = event_sock->can_write_callback(event_sock, current_time, event_sock->context, event_sock->sock);

                        /* if we get an OK, then the event should not be rescheduled. */
                        if(rc == PLCTAG_STATUS_OK) {
                            /* unlink from the list */
                            *sock_walker = event_sock->next;
                        } /* else leave it in place. */
                    }
                }

                sock_walker = &(event_sock->next);
            }
        }
    } else {
        /* error! */
        pdebug(DEBUG_WARN, "Error from select()!");
    }
}


int event_socket_init(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    FD_ZERO(&event_socket_read_fds);
    FD_ZERO(&event_socket_write_fds);
    event_socket_max_fds = 0;
    atomic_int_set(&recalc_fd_set_flag, 0);
    atomic_int_set(&termination_flag, 0);

    /* create the event socket mutex. */
    rc = mutex_create(&event_socket_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Event socket mutex creation failed with %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* create the timer mutex. */
    rc = mutex_create(&timer_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Timer mutex creation failed with %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* start the tickler thread. */
    rc = thread_create(&event_socket_thread, event_socket_handler, 32768, NULL);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Timer thread creation failed with %s!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}


void event_socket_teardown(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    /* first shut down the thread. */
    atomic_int_set(&termination_flag,1);

    if(event_socket_thread) {
        thread_join(event_socket_thread);
        event_socket_thread = NULL;
    }

    /* reset to valid state. */
    atomic_int_set(&termination_flag, 0);

    FD_ZERO(&event_socket_read_fds);
    FD_ZERO(&event_socket_write_fds);

    if(event_socket_mutex) {
        /* destroy the mutex. */
        rc = mutex_destroy(&event_socket_mutex);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Event socket mutex destruction failed with %s!", plc_tag_decode_error(rc));
        }
    } else {
        pdebug(DEBUG_WARN, "Found null event socket list mutex during clean up!");
    }

    if(timer_mutex) {
        /* destroy the mutex. */
        rc = mutex_destroy(&timer_mutex);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Timer mutex destruction failed with %s!", plc_tag_decode_error(rc));
        }
    } else {
        pdebug(DEBUG_WARN, "Found null timer list mutex during clean up!");
    }

    pdebug(DEBUG_INFO, "Done");
}



/***************** Helper Functions ******************/



THREAD_FUNC(event_socket_handler)
{
    (void)arg;

    pdebug(DEBUG_INFO, "Starting.");

    while(!atomic_int_get(&termination_flag)) {
        event_socket_tickler();
    }

    pdebug(DEBUG_INFO, "Protocol socket handler thread shutting down.");

    pdebug(DEBUG_INFO, "Done.");

    THREAD_RETURN((void*)0);
}





void recalc_fd_sets(void)
{
    /* scan through the socket and recreate the fd set. */

    FD_ZERO(&event_socket_read_fds);
    FD_ZERO(&event_socket_write_fds);

    event_socket_max_fds = 0;

    critical_block(event_socket_mutex) {
        struct event_socket_s *event_socket = event_socket_list;

        if(event_socket) {
            socket_t sock_fd = socket_get_fd(event_socket->sock);
            event_socket_event_t mask = atomic_int_get(&(event_socket->event_mask));

            if(mask & EVENT_SOCKET_CAN_READ) {
                event_socket_max_fds = (event_socket_max_fds < sock_fd ? sock_fd : event_socket_max_fds);
                FD_SET(sock_fd, &event_socket_read_fds);
            }

            if((mask & EVENT_SOCKET_CAN_WRITE) || (mask & EVENT_SOCKET_CONNECT)) {
                event_socket_max_fds = (event_socket_max_fds < sock_fd ? sock_fd : event_socket_max_fds);
                FD_SET(sock_fd, &event_socket_write_fds);
            }
        }
    }
}


int64_t timer_tickler(int64_t current_time)
{
    int64_t res = 0;

    pdebug(DEBUG_SPEW, "Starting.");

    critical_block(timer_mutex) {
        timer_p *timer_walker = &timer_list;

        while(*timer_walker && (*timer_walker)->wake_time <= current_time) {
            timer_p timer = *timer_walker;
            int rc = PLCTAG_STATUS_OK;

            rc = timer->timer_callback(timer, current_time, timer->context);
            if(rc == PLCTAG_STATUS_OK) {
                /* timer is done. */
                *timer_walker = timer->next;
            }

            if(*timer_walker) {
                timer_walker = &((*timer_walker)->next);
            }
        }

        /* return the next wake time. */
        if(timer_list) {
            res = timer_list->wake_time;
        }
    }


    pdebug(DEBUG_SPEW, "Done.");

    return res;
}


