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

#include <platform.h>
#include <lib/libplctag.h>
#include <util/atomic_int.h>
#include <util/debug.h>
#include <util/socket.h>

#include <sys/select.h>


static mutex_p socket_mutex = NULL;
static sock_p socket_list = NULL;
static sock_p *socket_list_iter = NULL;
static fd_set global_read_fds;
static fd_set global_write_fds;
static int pipe_fds[2] = { -1, -1};
static int max_socket_fd = -1;
static atomic_int need_recalculate_fd_sets = ATOMIC_INT_STATIC_INIT(1);


void socket_event_wake(void)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    uint8_t dummy = 0;
    write(pipe_fds[1], &dummy, 1);

    pdebug(DEBUG_DETAIL, "Done.");
}


/*

NOTES:
 * need to copy remaining socket functions from platform.c here.
 * need to finish socket_event_tickler by copying from platform tickler code.
 */

void socket_event_tickler(int64_t next_wake_time, int64_t current_time)
{

}


int socket_event_init(void)
{
    int rc = PLCTAG_STATUS_OK;
    int flags = 0;

    pdebug(DEBUG_INFO, "Starting.");

    /* open the pipe for waking the select wait. */
    if(pipe(pipe_fds)) {
        pdebug(DEBUG_WARN, "Unable to open waker pipe!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    /* make the read pipe fd non-blocking. */
    if(fcntl(pipe_fds[0], F_GETFL) < 0) {
        pdebug(DEBUG_WARN, "Unable to get flags of read pipe fd!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    /* set read fd non-blocking */
    flags |= O_NONBLOCK;

    if(fcntl(pipe_fds[0], F_SETFL, flags) < 0) {
        pdebug(DEBUG_WARN, "Unable to set flags of read pipe fd!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    /* make the write pipe fd non-blocking. */
    if(fcntl(pipe_fds[1], F_GETFL) < 0) {
        pdebug(DEBUG_WARN, "Unable to get flags of write pipe fd!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    /* set write fd non-blocking */
    flags |= O_NONBLOCK;

    if(fcntl(pipe_fds[1], F_SETFL, flags) < 0) {
        pdebug(DEBUG_WARN, "Unable to set flags of write pipe fd!");
        return PLCTAG_ERR_BAD_REPLY;
    }

    /* create the socket mutex. */
    rc = mutex_create(&socket_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to create socket mutex!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



void socket_event_teardown(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /* destroy the socket mutex. */
    rc = mutex_destroy(&socket_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to destroy socket mutex!");
        return;
    }

    socket_mutex = NULL;

    close(pipe_fds[0]);
    close(pipe_fds[1]);

    pipe_fds[0] = -1;
    pipe_fds[1] = -1;

    FD_ZERO(&global_read_fds);
    FD_ZERO(&global_write_fds);

    /* if there were sockets left, oh well, leak away! */
    socket_list = NULL;

    atomic_int_set(&need_recalculate_fd_sets, 0);

    pdebug(DEBUG_INFO, "Done.");
}
