#pragma once

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


/* need platform specific typedef for socket_t */
#ifdef _WIN32
#    include <winsock2.h>
typedef SOCKET socket_t;
#else
#    include <sys/socket.h>
typedef int socket_t;
#endif


#include <libplctag/lib/libplctag.h>

/* set up any necessary socket libraries and global state */
libplctag_error_code_t async_socket_init(void);
void async_socket_cleanup(void);


typedef struct async_event_loop_s async_event_loop_t;

/* Event types */
typedef enum {
    ASYNC_EVENT_NONE = 0,
    ASYNC_EVENT_READ = 1 << 0,   /* Data available to read */
    ASYNC_EVENT_WRITE = 1 << 1,  /* Ready to write data */
    ASYNC_EVENT_TICK = 1 << 2,   /* Tick event */
    ASYNC_EVENT_WAKE = 1 << 3,   /* Wake event */
    ASYNC_EVENT_TIMEOUT = 1 << 4 /* Timer expiration */
} async_event_t;


/* Buffer structure for handling partial I/O */
typedef struct async_buf_s {
    uint8_t *data;
    size_t capacity;
    size_t read_index;             /* Current read cursor position */
    size_t write_index;            /* Current write cursor position */
    libplctag_error_code_t status; /* Last status code for buffer operations */
} async_buf_t;

/* Helper to initialize a buffer wrapper */
void async_buf_init(async_buf_t *buf, uint8_t *data, size_t capacity);

/* Helper to move unconsumed data (from read_index to write_index) to the beginning of the buffer */
void async_buf_compact(async_buf_t *buf);

/* read data functions */
uint8_t *async_buf_get_read_ptr(async_buf_t *buf);
size_t async_buf_get_read_available(async_buf_t *buf);
libplctag_error_code_t async_buf_advance_read_index(async_buf_t *buf, size_t bytes_consumed);

/* we do not need a status value because it is set in the buffer, these increment the read index */
uint8_t async_buf_read_u8(async_buf_t *buf);
uint16_t async_buf_read_u16_be(async_buf_t *buf);
uint16_t async_buf_read_u16_le(async_buf_t *buf);
uint32_t async_buf_read_u32_be(async_buf_t *buf);
uint32_t async_buf_read_u32_le(async_buf_t *buf);
uint64_t async_buf_read_u64_be(async_buf_t *buf);
uint64_t async_buf_read_u64_le(async_buf_t *buf);

int8_t async_buf_read_i8(async_buf_t *buf);
int16_t async_buf_read_i16_be(async_buf_t *buf);
int16_t async_buf_read_i16_le(async_buf_t *buf);
int32_t async_buf_read_i32_be(async_buf_t *buf);
int32_t async_buf_read_i32_le(async_buf_t *buf);
int64_t async_buf_read_i64_be(async_buf_t *buf);
int64_t async_buf_read_i64_le(async_buf_t *buf);

libplctag_error_code_t async_buf_read_bytes(async_buf_t *buf, uint8_t *dest, size_t len);


uint8_t *async_buf_get_write_ptr(async_buf_t *buf);
size_t async_buf_get_write_capacity(async_buf_t *buf);
libplctag_error_code_t async_buf_advance_write_index(async_buf_t *buf, size_t bytes_written);

/* these increment the write index */
libplctag_error_code_t async_buf_write_u8(async_buf_t *buf, uint8_t val);
libplctag_error_code_t async_buf_write_u16_be(async_buf_t *buf, uint16_t val);
libplctag_error_code_t async_buf_write_u16_le(async_buf_t *buf, uint16_t val);
libplctag_error_code_t async_buf_write_u32_be(async_buf_t *buf, uint32_t val);
libplctag_error_code_t async_buf_write_u32_le(async_buf_t *buf, uint32_t val);
libplctag_error_code_t async_buf_write_u64_be(async_buf_t *buf, uint64_t val);
libplctag_error_code_t async_buf_write_u64_le(async_buf_t *buf, uint64_t val);

libplctag_error_code_t async_buf_write_i8(async_buf_t *buf, int8_t val);
libplctag_error_code_t async_buf_write_i16_be(async_buf_t *buf, int16_t val);
libplctag_error_code_t async_buf_write_i16_le(async_buf_t *buf, int16_t val);
libplctag_error_code_t async_buf_write_i32_be(async_buf_t *buf, int32_t val);
libplctag_error_code_t async_buf_write_i32_le(async_buf_t *buf, int32_t val);
libplctag_error_code_t async_buf_write_i64_be(async_buf_t *buf, int64_t val);
libplctag_error_code_t async_buf_write_i64_le(async_buf_t *buf, int64_t val);

libplctag_error_code_t async_buf_write_bytes(async_buf_t *buf, const uint8_t *src, size_t len);

/*
 * Callback function signature.
 * loop: the event loop
 * sock: the socket associated with the event (or INVALID_SOCKET for ticks)
 * event: the event type(s) triggered
 * user_data: context pointer provided at registration
 */
typedef void (*async_callback_t)(async_event_loop_t *loop, socket_t sock, async_event_t event, libplctag_error_code_t status,
                                 void *user_data);


async_event_loop_t *async_event_loop_create(void);
void async_event_loop_destroy(async_event_loop_t *loop);

/* a single pass/poll makes more sense for embedded systems that may not have threads. */
libplctag_error_code_t async_event_loop_run_once(async_event_loop_t *loop, uint32_t poll_timeout_ms);

/* event handling */

/* socket can be INVALID_SOCKET for ticks or wake ups */
libplctag_error_code_t async_event_loop_watch_events(async_event_loop_t *loop, async_callback_t cb, socket_t sock,
                                                     async_event_t events, void *user_data, uint32_t timeout_ms);
libplctag_error_code_t async_event_loop_abort_events(async_event_loop_t *loop, async_callback_t cb, socket_t sock,
                                                     void *user_data);

/* this takes care of wake events */
libplctag_error_code_t async_raise_event(async_event_loop_t *loop, async_callback_t cb, async_event_t event, void *user_data);


typedef enum { ASYNC_SOCKET_TYPE_TCP, ASYNC_SOCKET_TYPE_UDP } async_socket_type_t;

/* automatically sets nodelay and nonblocking */
socket_t async_socket_open(async_socket_type_t type);
void async_socket_close(socket_t sock);

/* are these needed? Perhaps for sockets that come from the application and not the above async_socket_open() function? */
libplctag_error_code_t async_socket_set_nodelay(socket_t sock, bool enable);
libplctag_error_code_t async_socket_set_reuseaddr(socket_t sock, bool enable);
libplctag_error_code_t async_socket_set_nonblocking(socket_t sock, bool enable);


/* automatically sets reuseaddr */
libplctag_error_code_t async_socket_bind(socket_t sock, const char *local_address, uint16_t local_port);

/* server TCP sockets */
libplctag_error_code_t async_socket_listen(socket_t sock, int backlog);


/* server TCP sockets */
libplctag_error_code_t async_socket_accept(socket_t server_sock, socket_t *client_sock, char *client_addr, size_t addr_len,
                                           uint16_t *client_port);


/* client TCP sockets */
libplctag_error_code_t async_socket_connect(socket_t sock, const char *address, uint16_t port);

/*
 * Attempt to send data from the buffer.
 * Sends data starting at buf->read_index up to buf->write_index.
 * Updates buf->read_index.
 * Returns PLCTAG_STATUS_PENDING if partial data sent (would block).
 */
libplctag_error_code_t async_socket_send(socket_t sock, async_buf_t *buf);

/*
 * Attempt to receive data into the buffer.
 * Appends data starting at buf->write_index up to buf->capacity.
 * Updates buf->write_index.
 * Returns PLCTAG_STATUS_PENDING if no data available (would block).
 */
libplctag_error_code_t async_socket_recv(socket_t sock, async_buf_t *buf);

/* UDP sockets */

libplctag_error_code_t async_socket_set_broadcast(socket_t sock, bool enable);
libplctag_error_code_t async_socket_join_multicast(socket_t sock, const char *group_addr, const char *interface_addr);
libplctag_error_code_t async_socket_leave_multicast(socket_t sock, const char *group_addr, const char *interface_addr);
libplctag_error_code_t async_socket_sendto(socket_t sock, const char *dest_addr, uint16_t dest_port, async_buf_t *send_buf);
libplctag_error_code_t async_socket_recvfrom(socket_t sock, char *src_addr, size_t addr_len, uint16_t *src_port,
                                             async_buf_t *recv_buf);
