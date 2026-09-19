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
 * LGPL 2:                                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 ***************************************************************************/

/*
 * L0b of the socket stack: one thread's readiness loop over many sockets.
 *
 * A poller owns exactly one wake pair for the whole thread, not one per
 * socket.  That is the point.  The old sock_p carries its own wake channel,
 * so N connections cost N wake pairs whether or not anything ever signals
 * them -- and in the library only the Modbus sockets ever do.  One thread
 * driving 200 sockets needs one.
 *
 * `context` is the integration point with per-connection state machines: it
 * is the connection object.  poller_wait() answers "these connections are
 * ready, here they are", and the loop calls each one's step function.  The
 * poller never learns whether that step function is a switch/case, a
 * protothread, or a coroutine.
 *
 * Backed by poll()/WSAPoll() rather than select(): select()'s FD_SETSIZE
 * ceiling of 1024 is a wall that would be hit precisely in the many-sockets
 * model this exists for.  epoll and kqueue can replace the backend later
 * without touching this interface.
 *
 * Not thread safe, with one deliberate exception: poller_wake() may be
 * called from any thread.  Everything else belongs to the thread that owns
 * the poller.
 *
 * See docs/socket_layering_design.md.
 */

#include <stdbool.h>
#include <stdint.h>
#include <utils/socket_fd.h>

typedef struct poller_t *poller_p;

/*
 * Readiness bits.  The same vocabulary sock_event_t uses, so that callers
 * moving off socket_wait_event() do not have to re-learn it.
 *
 * Pass CAN_READ, CAN_WRITE and CONNECT to poller_add() and poller_modify()
 * to say what a socket is waiting for.  DISCONNECT, ERROR, WAKE_UP and
 * TIMEOUT are never requested; they are only ever reported.
 */
typedef enum {
    POLLER_EVENT_NONE = 0,
    POLLER_EVENT_TIMEOUT = (1 << 0),
    POLLER_EVENT_DISCONNECT = (1 << 1),
    POLLER_EVENT_ERROR = (1 << 2),
    POLLER_EVENT_CAN_READ = (1 << 3),
    POLLER_EVENT_CAN_WRITE = (1 << 4),
    POLLER_EVENT_WAKE_UP = (1 << 5),
    POLLER_EVENT_CONNECT = (1 << 6)
} poller_event_type_t;

typedef struct {
    socket_fd_t fd;
    int32_t events;
    void *context;
} poller_event_t;


extern int32_t poller_create(poller_p *poller, int32_t max_sockets);
extern int32_t poller_destroy(poller_p *poller);

extern int32_t poller_add(poller_p poller, socket_fd_t fd, int32_t events, void *context);
extern int32_t poller_modify(poller_p poller, socket_fd_t fd, int32_t events);
extern int32_t poller_remove(poller_p poller, socket_fd_t fd);

/*
 * Waits up to timeout_ms for any registered socket to become ready and fills
 * in up to max_events entries.  A timeout_ms of zero polls and returns
 * immediately; a negative value waits until something happens.
 *
 * Returns PLCTAG_STATUS_OK with *event_count set, which is zero if the wait
 * timed out.  Being woken by poller_wake() reports one event carrying
 * POLLER_EVENT_WAKE_UP, an invalid fd and a NULL context.
 */
extern int32_t poller_wait(poller_p poller, poller_event_t *events, int32_t max_events, int32_t timeout_ms,
                           int32_t *event_count);

/* the one call that is safe from another thread */
extern int32_t poller_wake(poller_p poller);

extern int32_t poller_socket_count(poller_p poller);
