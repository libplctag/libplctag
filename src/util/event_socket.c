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
#include <util/protocol_socket.h>

#define SELECT_WAIT_TIME_MS (20)


struct event_socket_s {
    sock_p sock;
    atomic_int event_mask;
    struct event_socket_s *next;
    void *context;
    void (*open_callback)(struct event_socket_s *psock, int64_t current_time, void *context);
    int (*can_read_callback)(struct event_socket_s *psock, int64_t current_time, void *context);
    int (*can_write_callback)(struct event_socket_s *psock, int64_t current_time, void *context);
    void (*close_callback)(struct event_socket_s *psock, int64_t current_time, void *context);
};



static mutex_p protocol_socket_mutex = NULL;
static thread_p protocol_socket_thread = NULL;
static volatile struct protocol_socket_s *socket_list = NULL;
static atomic_int termination_flag = ATOMIC_INT_STATIC_INIT(0);
static atomic_int recalc_fd_set_flag = ATOMIC_INT_STATIC_INIT(0);
static FD_SET protocol_socket_read_fds;
static FD_SET protocol_socket_write_fds;
static socket_t protocol_socket_max_fds = 0;

static THREAD_FUNC(protocol_socket_handler);
static void recalc_fd_sets(void);


int event_socket_create(event_socket_p *ps,
                        const char *host,
                        void *context,
                        event_socket_event_t event_mask,
                        void (*open_callback)(event_socket_p *psock, int64_t current_time, void *context),
                        int (*can_read_callback)(event_socket_p *psock, int64_t current_time, void *context),
                        int (*can_write_callback)(event_socket_p *psock, int64_t current_time, void *context),
                        void (*close_callback)(event_socket_p *psock, int64_t current_time, void *context))
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");


    pdebug(DEBUG_INFO, "Done.");

    return rc;
}

extern int event_socket_destroy(event_socket_p *ps);
extern int event_socket_enable_event(event_socket_p *ps, event_socket_event_t event);
extern int event_socket_disable_event(event_socket_p *ps, event_socket_event_t event);


typedef struct timer_s *timer_p;

extern int timer_create(timer_p *timer,
                        void *context,
                        int(*timer_callback)(timer_p timer, int64_t current_time, void *context));
extern int timer_destroy(timer_p *timer);
extern int timer_set(timer_p timer, int64_t absolute_time);
extern int timer_abort(timer_p timer);


extern int event_socket_tickler(void);

extern int event_socket_init(void);
extern void event_socket_teardown(void);


int protocol_socket_open(struct protocol_socket_s *ps, const char *host, protocol_socket_event_t event_mask);
int protocol_socket_close(struct protocol_socket_s *ps);
int protocol_socket_enable_event(struct protocol_socket_s *ps, protocol_socket_event_t event);
int protocol_socket_disable_event(struct protocol_socket_s *ps, protocol_socket_event_t event);




int protocol_socket_tickler(void)
{
    struct timeval tv;

    tv.tv_sec = 0;
    tv.tv_nsec = SELECT_WAIT_TIME_MS * 1000000; /* convert to nanoseconds */

    if(atomic_int_get(&recalc_fd_set_flag) > 0) {
        recalc_fd_sets();

        /* we cannot just clear the flag, it could have been tweaked by another thread. */
        atomic_int_add(&recalc_fd_set_flag), -1);
    }

    /* wait for something to happen. */
    rc = select(protocol_socket_max_fds+1, &protocol_socket_read_fds, &protocol_socket_write_fds, NULL, &tv);
    if(rc >= 0) {
        int64_t current_time = time_ms();

        pdebug(DEBUG_SPEW, "some socket is ready or we timed out!");

        critical_block(protocol_socket_mutex) {
            protocol_socket_p ps = protocol_socket_list;

            while(ps) {
                if(rc > 0) {
                    if((ps->event_mask & PROTOCOL_SOCKET_READ) && FD_ISSET(socket_get_fd(ps->sock), &protocol_socket_read_fds)) {
                        int rc = 0;
                        int data_capacity = ps->buffer_len - slice_len(ps->packet);
                        uint8_t *data_start = slice_data(ps->packet) + slice_len(ps->packet);

                        /* read in some data. */
                        rc = socket_read(ps->sock, data_start, data_capacity);
                        if(rc >= 0) {
                            ps->packet = slice_make(slice_data(ps->packet), rc + slice_len(ps->packet));

                            ps->callback(ps, current_time, PROTOCOL_SOCKET_READ);
                        } else {
                            /* error reading the socket! */
                            ps->callback(ps, current_time, PROTOCOL_SOCKET_READ | PROTOCOL_SOCKET_ERROR);
                        }

                        if(rc == PLCTAG_STATUS_OK) {
                            /* packet is ready!
                             *
                             * This function will
                             */
                            ps->callback(ps, current_time, PROTOCOL_SOCKET_READ);
                        }
                    }

                    if((ps->event_mask & PROTOCOL_SOCKET_WRITE) && FD_ISSET(socket_get_fd(ps->sock), &protocol_socket_write_fds)) {
                        if(slice_len(ps->packet) == 0) {
                            /* the packet buffer is empty, so call the callback. */
                            ps->callback(ps, current_time, PROTOCOL_SOCKET_WRITE);
                        }

                        if(slice_len(ps->output_packet) > 0) {
                            /* there is something to write. */
                            int amount_to_write = slice_len(ps->output_packet);
                            uint8_t data_ptr = slice_data(ps->output_packet);
                            int rc = socket_write(ps->sock, data_ptr, amount_to_write);

                            if(rc >= 0) {
                                int amount_left = amount_to_write - rc;

                                if(amount_left >= 0) {
                                    ps->output_packet = slice_from_slice(ps->output_packet, rc, amount_left);
                                }
                            }

                        }





                        int rc = 0;
                        int data_capacity = ps->buffer_len - slice_len(ps->packet);
                        uint8_t *data_start = slice_data(ps->packet) + slice_len(ps->packet);

                        /* read in some data. */
                        rc = socket_read(ps->sock, data_start, data_capacity);
                        if(rc >= 0) {
                            ps->packet = slice_make(slice_data(ps->packet), rc + slice_len(ps->packet));

                            ps->callback(ps, current_time, PROTOCOL_SOCKET_READ);
                        } else {
                            /* error reading the socket! */
                            ps->callback(ps, current_time, PROTOCOL_SOCKET_READ | PROTOCOL_SOCKET_ERROR);
                        }

                        if(rc == PLCTAG_STATUS_OK) {
                            /* packet is ready!
                             *
                             * This function will
                             */
                            ps->callback(ps, current_time, PROTOCOL_SOCKET_READ);
                        }
                    }
                }

                /* does this protocol socket client want to get timer ticks? */
                if(ps->event_mask & PROTOCOL_SOCKET_TICK)) {
                    ps->callback(ps, current_time, PROTOCOL_SOCKET_TICK);
                }

                ps = ps->next;
            }
        }

    } else if
        /* error! */

        pdebug(DEBUG_WARN, "Error from select()!");
    } else {

    }
}

int protocol_socket_init(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    FD_ZERO(&protocol_socket_read_fds);
    FD_ZERO(&protocol_socket_write_fds);
    protocol_socket_max_fds = 0;

    /* create the mutex. */
    rc = mutex_create(&protocol_socket_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Mutex creation failed with %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* start the tickler thread. */
    rc = thread_create(&protocol_socket_thread, protocol_socket_handler, 32768, NULL);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Timer thread creation failed with %s!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}


void protocol_socket_teardown(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    /* first shut down the thread. */
    atomic_int_set(&termination_flag,1);

    if(protocol_socket_thread) {
        thread_join(protocol_socket_thread);
        protocol_socket_thread = NULL;
    }

    /* reset to valid state. */
    atomic_int_set(&termination_flag, 0);

    FD_ZERO(&protocol_socket_read_fds);
    FD_ZERO(&protocol_socket_write_fds);

    /* check the callbacks */
    if(protocol_socket_mutex) {
        int done = 1;
        int64_t current_time = time_ms();

        do {
            protocol_socket_p protocol_socket = NULL;

            critical_block(protocol_socket_mutex) {
                if(protocol_socket_list) {
                    /* unlink the head callback */
                    protocol_socket = protocol_socket_list;
                    protocol_socket_list = protocol_socket->next;
                }

                /* any left? if so, keep going. */
                done = !protocol_socket_list;
            }

            if(protocol_socket) {
                protocol_socket->callback(protocol_socket, protocol_socket->context, PROTOCOL_SOCKET_CLOSE, current_time, slice_make(NULL, 0), slice_make(NULL, 0));
                protocol_socket = NULL;

                /* close the socket */
                close_socket(protocol_socket->sock_fd);

                protocol_socket->is_open = 0;
            }
        } while(!done);

        /* destroy the mutex. */
        rc = mutex_destroy(&protocol_socket_mutex);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Mutex destruction failed with %s!", plc_tag_decode_error(rc));
        }
    } else {
        pdebug(DEBUG_WARN, "Found null protocol socket list mutex during clean up!");
    }

    pdebug(DEBUG_INFO, "Done");
}



/***************** Helper Functions ******************/



THREAD_FUNC(protocol_socket_handler)
{
    (void)arg;

    pdebug(DEBUG_INFO, "Starting.");

    while(!atomic_int_get(&termination_flag)) {
        int rc = protocol_socket_tickler();
    }

    pdebug(DEBUG_INFO, "Protocol socket handler thread shutting down.");

    pdebug(DEBUG_INFO, "Done.");

    THREAD_RETURN((void*)0);
}





void recalc_fd_sets(void)
{
    /* scan through the socket and recreate the fd set. */

    FD_ZERO(&protocol_socket_read_fds);
    FD_ZERO(&protocol_socket_write_fds);

    protocol_socket_max_fds = 0;

    critical_block(protocol_socket_mutex) {
        struct protocol_socket_s *ps = socket_list;

        if(ps) {
            if(ps->is_open) {
                socket_t sock_fd = socket_get_fd(ps->sock);

                if(ps->event_mask & PROTOCOL_SOCKET_READ) {
                    protocol_socket_max_fds = (protocol_socket_max_fds < sock_fd ? sock_fd : protocol_socket_max_fds);
                    FD_SET(&protocol_socket_read_fds, sock_fd);
                }

                if((ps->event_mask & PROTOCOL_SOCKET_WRITE) || (ps->event_mask & PROTOCOL_SOCKET_OPEN)) {
                    protocol_socket_max_fds = (protocol_socket_max_fds < sock_fd ? sock_fd : protocol_socket_max_fds);
                    FD_SET(&protocol_socket_write_fds, sock_fd);
                }
            }
        }
    }
}


