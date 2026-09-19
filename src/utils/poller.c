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
 * L0b of the socket stack.  See poller.h for the contract.
 *
 * There is no platform code in this file.  Every difference between Windows
 * and POSIX lives in socket_fd.c, which is the point of having an L0 at all.
 *
 * The registration table is a flat array scanned linearly.  That is the
 * right shape for the sizes involved: poll() itself is linear in the number
 * of descriptors, so an index would speed up the lookups and leave the
 * syscall exactly as slow.  If the descriptor count ever gets large enough
 * for that to matter, the backend wants to be epoll or kqueue, and those
 * replace this table rather than add an index to it.
 */

#include <utils/poller.h>

#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <string.h>
#include <utils/debug.h>
#include <utils/mem.h>
#include <utils/socket_fd.h>

struct poller_t {
    int32_t capacity;
    int32_t count;

    /* the thread's one wake pair; see poller.h */
    socket_fd_t wake_read_fd;
    socket_fd_t wake_write_fd;

    socket_fd_t *fds;
    int32_t *wanted_events;
    void **contexts;

    /* scratch for socket_fd_poll(); sized capacity + 1 for the wake end */
    socket_fd_poll_item_t *poll_items;
};


/* index of fd in the table, or -1 */
static int32_t poller_find(poller_p poller, socket_fd_t fd) {
    int32_t index = 0;

    for(index = 0; index < poller->count; index++) {
        if(poller->fds[index] == fd) { return index; }
    }

    return -1;
}


extern int32_t poller_create(poller_p *poller, int32_t max_sockets) {
    poller_p result = NULL;
    int32_t rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Starting.");

    if(!poller) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer to poller!");
        return PLCTAG_ERR_NULL_PTR;
    }

    *poller = NULL;

    if(max_sockets <= 0 || max_sockets > (SOCKET_FD_POLL_MAX - 1)) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket count %" PRId32 " is out of range!", max_sockets);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    result = (poller_p)mem_alloc((int)sizeof(struct poller_t));
    if(!result) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_ERROR, 0, "Unable to allocate poller!");
        return PLCTAG_ERR_NO_MEM;
    }

    result->capacity = max_sockets;
    result->count = 0;
    result->wake_read_fd = SOCKET_FD_INVALID;
    result->wake_write_fd = SOCKET_FD_INVALID;

    result->fds = (socket_fd_t *)mem_alloc((int)(sizeof(socket_fd_t) * (size_t)max_sockets));
    result->wanted_events = (int32_t *)mem_alloc((int)(sizeof(int32_t) * (size_t)max_sockets));
    result->contexts = (void **)mem_alloc((int)(sizeof(void *) * (size_t)max_sockets));
    result->poll_items = (socket_fd_poll_item_t *)mem_alloc((int)(sizeof(socket_fd_poll_item_t) * (size_t)(max_sockets + 1)));

    if(!result->fds || !result->wanted_events || !result->contexts || !result->poll_items) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_ERROR, 0, "Unable to allocate poller tables!");
        poller_destroy(&result);
        return PLCTAG_ERR_NO_MEM;
    }

    rc = socket_fd_pair(&(result->wake_read_fd), &(result->wake_write_fd));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Unable to create wake pair, error %s!", plc_tag_decode_error(rc));
        poller_destroy(&result);
        return rc;
    }

    *poller = result;

    pdebug(DEBUG_MODULE_SOCKET, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


extern int32_t poller_destroy(poller_p *poller) {
    poller_p target = NULL;

    if(!poller || !*poller) { return PLCTAG_STATUS_OK; }

    target = *poller;

    socket_fd_close(&(target->wake_read_fd));
    socket_fd_close(&(target->wake_write_fd));

    /*
     * The registered sockets are not closed here.  They belong to whoever
     * added them -- a poller that closed them would be guessing about a
     * lifetime it does not own.
     */
    if(target->fds) { mem_free(target->fds); }
    if(target->wanted_events) { mem_free(target->wanted_events); }
    if(target->contexts) { mem_free(target->contexts); }
    if(target->poll_items) { mem_free(target->poll_items); }

    mem_free(target);

    *poller = NULL;

    return PLCTAG_STATUS_OK;
}


extern int32_t poller_add(poller_p poller, socket_fd_t fd, int32_t events, void *context) {
    if(!poller) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer to poller!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(fd == SOCKET_FD_INVALID) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Attempt to add an invalid socket!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(poller_find(poller, fd) >= 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket is already registered!");
        return PLCTAG_ERR_DUPLICATE;
    }

    if(poller->count >= poller->capacity) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Poller is full at %" PRId32 " sockets!", poller->capacity);
        return PLCTAG_ERR_NO_RESOURCES;
    }

    poller->fds[poller->count] = fd;
    poller->wanted_events[poller->count] = events;
    poller->contexts[poller->count] = context;
    poller->count++;

    return PLCTAG_STATUS_OK;
}


extern int32_t poller_modify(poller_p poller, socket_fd_t fd, int32_t events) {
    int32_t index = 0;

    if(!poller) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer to poller!");
        return PLCTAG_ERR_NULL_PTR;
    }

    index = poller_find(poller, fd);
    if(index < 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket is not registered!");
        return PLCTAG_ERR_NOT_FOUND;
    }

    poller->wanted_events[index] = events;

    return PLCTAG_STATUS_OK;
}


extern int32_t poller_remove(poller_p poller, socket_fd_t fd) {
    int32_t index = 0;

    if(!poller) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer to poller!");
        return PLCTAG_ERR_NULL_PTR;
    }

    index = poller_find(poller, fd);
    if(index < 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Socket is not registered!");
        return PLCTAG_ERR_NOT_FOUND;
    }

    /*
     * Order is not meaningful here, so the last entry fills the hole.  Note
     * that this moves an entry the caller may be in the middle of iterating:
     * poller_wait() copies what it needs into the caller's array before
     * returning, so a step function is free to call poller_remove().
     */
    poller->count--;

    if(index != poller->count) {
        poller->fds[index] = poller->fds[poller->count];
        poller->wanted_events[index] = poller->wanted_events[poller->count];
        poller->contexts[index] = poller->contexts[poller->count];
    }

    return PLCTAG_STATUS_OK;
}


extern int32_t poller_wait(poller_p poller, poller_event_t *events, int32_t max_events, int32_t timeout_ms,
                           int32_t *event_count) {
    int32_t index = 0;
    int32_t item_count = 0;
    int32_t ready_count = 0;
    int32_t found = 0;
    int32_t wake_index = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    if(!poller || !events || !event_count) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer passed to poller_wait!");
        return PLCTAG_ERR_NULL_PTR;
    }

    *event_count = 0;

    if(max_events <= 0) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Event array size %" PRId32 " is out of range!", max_events);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /* build the poll set: every registered socket, then the wake end */
    for(index = 0; index < poller->count; index++) {
        int16_t want = 0;

        if(poller->wanted_events[index] & POLLER_EVENT_CAN_READ) { want = (int16_t)(want | SOCKET_FD_POLL_READ); }

        /*
         * A connect in flight completes by becoming writable, so CONNECT and
         * CAN_WRITE ask poll() for the same thing.  They are separate bits
         * on the way out, where the caller can tell them apart by what it
         * asked for.
         */
        if(poller->wanted_events[index] & (POLLER_EVENT_CAN_WRITE | POLLER_EVENT_CONNECT)) {
            want = (int16_t)(want | SOCKET_FD_POLL_WRITE);
        }

        poller->poll_items[item_count].fd = poller->fds[index];
        poller->poll_items[item_count].want = want;
        poller->poll_items[item_count].got = 0;
        item_count++;
    }

    wake_index = item_count;
    poller->poll_items[wake_index].fd = poller->wake_read_fd;
    poller->poll_items[wake_index].want = SOCKET_FD_POLL_READ;
    poller->poll_items[wake_index].got = 0;
    item_count++;

    rc = socket_fd_poll(poller->poll_items, item_count, timeout_ms, &ready_count);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(ready_count == 0) { return PLCTAG_STATUS_OK; }

    /* drain the wake end first so that one poke does not wake us twice */
    if(poller->poll_items[wake_index].got & SOCKET_FD_POLL_READ) {
        uint8_t drain[64];
        int32_t drained = 0;

        while(socket_fd_recv(poller->wake_read_fd, drain, (int32_t)sizeof(drain), &drained) == PLCTAG_STATUS_OK
              && drained > 0) {
            /* keep draining */
        }

        if(found < max_events) {
            events[found].fd = SOCKET_FD_INVALID;
            events[found].events = POLLER_EVENT_WAKE_UP;
            events[found].context = NULL;
            found++;
        }
    }

    for(index = 0; index < poller->count && found < max_events; index++) {
        int16_t got = poller->poll_items[index].got;
        int32_t out_events = 0;

        if(got == 0) { continue; }

        if(got & SOCKET_FD_POLL_READ) { out_events |= POLLER_EVENT_CAN_READ; }

        if(got & SOCKET_FD_POLL_WRITE) {
            /*
             * Writable means the connect finished for a socket that asked
             * for CONNECT, and means there is room in the send buffer for
             * one that asked for CAN_WRITE.  Report whichever was asked
             * for, so the caller is never handed an event it does not
             * handle.
             */
            if(poller->wanted_events[index] & POLLER_EVENT_CONNECT) {
                out_events |= POLLER_EVENT_CONNECT;
            } else {
                out_events |= POLLER_EVENT_CAN_WRITE;
            }
        }

        if(got & SOCKET_FD_POLL_HUP) { out_events |= POLLER_EVENT_DISCONNECT; }
        if(got & SOCKET_FD_POLL_ERR) { out_events |= POLLER_EVENT_ERROR; }

        events[found].fd = poller->fds[index];
        events[found].events = out_events;
        events[found].context = poller->contexts[index];
        found++;
    }

    *event_count = found;

    return PLCTAG_STATUS_OK;
}


extern int32_t poller_wake(poller_p poller) {
    uint8_t poke = 1;
    int32_t written = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    if(!poller) {
        pdebug(DEBUG_MODULE_SOCKET, DEBUG_WARN, 0, "Null pointer to poller!");
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = socket_fd_send(poller->wake_write_fd, &poke, (int32_t)sizeof(poke), &written);

    /*
     * A full wake pipe means a wake is already pending and unread, which is
     * exactly the state a wake is trying to produce.  Nothing to report.
     */
    if(rc == PLCTAG_STATUS_PENDING) { return PLCTAG_STATUS_OK; }

    return rc;
}


extern int32_t poller_socket_count(poller_p poller) {
    if(!poller) { return 0; }

    return poller->count;
}
