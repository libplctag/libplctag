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
 * A POSIX fd_set is a bitmap indexed by descriptor value, so FD_SET() with a descriptor
 * at or above FD_SETSIZE writes past the object.  The socket shim used to do exactly that
 * in socket_connect_tcp_check(), socket_wait_event(), socket_read() and socket_write().
 *
 * This test holds enough descriptors open that every descriptor the shim then opens is
 * above FD_SETSIZE, and drives all four of those calls against a loopback peer.  Under the
 * sanitizer build the old code aborts with a stack buffer overflow; poll() has no such
 * limit and the exchange completes.
 *
 * The descriptor limit is a property of the process, so the padding has to come first and
 * stay open for the whole exchange -- descriptors are handed out lowest-free-first.
 *
 * POSIX sockets and the platform shim are both internal to the library, so the unit test
 * CMakeLists.txt leaves this test out on Windows.
 */

#include "mini_mock.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libplctag/api/libplctag.h>
#include <netinet/in.h>
#include <platform.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>


/* enough headroom above the fd_set bound that the shim's own descriptors clear it too. */
#define FD_PAD_TARGET (FD_SETSIZE + 16)

/* the shim opens the socket plus both ends of its wake channel. */
#define SHIM_FDS_PER_SOCKET (3)

#define TEST_TIMEOUT_MS (5000)

#define PING "PING"
#define PONG "PONG"
#define MSG_LEN (4)


#define assert_true(cond)                                                                  \
    do {                                                                                   \
        if(!(cond)) {                                                                      \
            fprintf(stderr, "%s:%d: assert_true failed: %s\n", __FILE__, __LINE__, #cond); \
            abort();                                                                       \
        }                                                                                  \
    } while(0)


static int32_t pad_fds[FD_PAD_TARGET + SHIM_FDS_PER_SOCKET];
static int32_t num_pad_fds = 0;


/*
 * Raise the soft descriptor limit as far as the hard limit allows.  macOS ships a soft
 * limit of 256, well under FD_SETSIZE, so without this there is nothing to test.
 */
static bool raise_fd_limit(void) {
    struct rlimit limit;

    if(getrlimit(RLIMIT_NOFILE, &limit) != 0) { return false; }

    if(limit.rlim_cur >= (rlim_t)(FD_PAD_TARGET + SHIM_FDS_PER_SOCKET)) { return true; }

    if(limit.rlim_max != RLIM_INFINITY && limit.rlim_max < (rlim_t)(FD_PAD_TARGET + SHIM_FDS_PER_SOCKET)) { return false; }

    limit.rlim_cur = (rlim_t)(FD_PAD_TARGET + SHIM_FDS_PER_SOCKET);

    return (setrlimit(RLIMIT_NOFILE, &limit) == 0);
}


/* open descriptors until the next one handed out must be above FD_SETSIZE. */
static bool pad_descriptors(void) {
    int32_t fd = 0;

    do {
        fd = (int32_t)open("/dev/null", O_RDONLY);

        if(fd < 0) { return false; }

        pad_fds[num_pad_fds] = fd;
        num_pad_fds++;
    } while(fd < FD_PAD_TARGET && num_pad_fds < (int32_t)(sizeof(pad_fds) / sizeof(pad_fds[0])));

    return (fd >= FD_PAD_TARGET);
}


static void release_descriptors(void) {
    while(num_pad_fds > 0) {
        num_pad_fds--;
        close(pad_fds[num_pad_fds]);
    }
}


/* a loopback listener on an OS-chosen port.  Returns the descriptor, or -1. */
static int32_t start_listener(uint16_t *port) {
    struct sockaddr_in addr;
    socklen_t addr_len = (socklen_t)sizeof(addr);
    int32_t listener = (int32_t)socket(AF_INET, SOCK_STREAM, 0);
    int32_t reuse = 1;

    if(listener < 0) { return -1; }

    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, (socklen_t)sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if(bind(listener, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0 || listen(listener, 1) != 0
       || getsockname(listener, (struct sockaddr *)&addr, &addr_len) != 0) {
        close(listener);
        return -1;
    }

    *port = ntohs(addr.sin_port);

    return listener;
}


static void test_socket_ops_work_above_fd_setsize(void **state) {
    sock_p client = NULL;
    int32_t listener = -1;
    int32_t peer = -1;
    uint16_t port = 0;
    uint8_t buf[MSG_LEN + 1] = {0};
    int32_t rc = 0;
    int32_t events = 0;

    (void)state;

    if(!raise_fd_limit()) {
        printf("    skipped: the descriptor limit cannot be raised above FD_SETSIZE (%d).\n", FD_SETSIZE);
        return;
    }

    listener = start_listener(&port);
    assert_true(listener >= 0);

    assert_true(pad_descriptors());

    /* every descriptor opened from here on is above the fd_set bound. */
    assert_true(pad_fds[num_pad_fds - 1] >= FD_SETSIZE);

    assert_int_equal(socket_create(&client), PLCTAG_STATUS_OK);

    rc = (int32_t)socket_connect_tcp_start(client, "127.0.0.1", (int)port);
    assert_true(rc == PLCTAG_STATUS_OK || rc == PLCTAG_STATUS_PENDING);

    peer = (int32_t)accept(listener, NULL, NULL);
    assert_true(peer >= 0);

    /* socket_connect_tcp_check(): the first of the four waits. */
    assert_int_equal(socket_connect_tcp_check(client, TEST_TIMEOUT_MS), PLCTAG_STATUS_OK);

    /* socket_write(). */
    assert_int_equal(socket_write(client, (uint8_t *)PING, MSG_LEN, TEST_TIMEOUT_MS), MSG_LEN);
    assert_int_equal(read(peer, &buf[0], MSG_LEN), MSG_LEN);
    assert_int_equal(memcmp(&buf[0], PING, MSG_LEN), 0);

    /* socket_wait_event() and socket_read(). */
    assert_int_equal(write(peer, PONG, MSG_LEN), MSG_LEN);

    events = (int32_t)socket_wait_event(client, SOCK_EVENT_CAN_READ | SOCK_EVENT_TIMEOUT, TEST_TIMEOUT_MS);
    assert_true((events & SOCK_EVENT_CAN_READ) != 0);

    memset(&buf[0], 0, sizeof(buf));
    assert_int_equal(socket_read(client, &buf[0], MSG_LEN, TEST_TIMEOUT_MS), MSG_LEN);
    assert_int_equal(memcmp(&buf[0], PONG, MSG_LEN), 0);

    /* the wake channel's descriptors are above the bound too. */
    assert_int_equal(socket_wake(client), PLCTAG_STATUS_OK);

    events = (int32_t)socket_wait_event(client, SOCK_EVENT_WAKE_UP | SOCK_EVENT_TIMEOUT, TEST_TIMEOUT_MS);
    assert_true((events & SOCK_EVENT_WAKE_UP) != 0);

    socket_close(client);
    socket_destroy(&client);

    close(peer);
    close(listener);

    release_descriptors();
}


/*
 * A peer that half-closes leaves the socket readable at end of file and raises no POLLHUP,
 * so the zero-length read is the only notice of it.  A caller that asked about disconnects
 * but not about readable data has to be told, or it waits out its whole timeout against a
 * socket that is already gone.
 */
static void test_peer_half_close_reports_disconnect(void **state) {
    sock_p client = NULL;
    int32_t listener = -1;
    int32_t peer = -1;
    uint16_t port = 0;
    int32_t rc = 0;
    int32_t events = 0;

    (void)state;

    listener = start_listener(&port);
    assert_true(listener >= 0);

    assert_int_equal(socket_create(&client), PLCTAG_STATUS_OK);

    rc = (int32_t)socket_connect_tcp_start(client, "127.0.0.1", (int)port);
    assert_true(rc == PLCTAG_STATUS_OK || rc == PLCTAG_STATUS_PENDING);

    peer = (int32_t)accept(listener, NULL, NULL);
    assert_true(peer >= 0);

    assert_int_equal(socket_connect_tcp_check(client, TEST_TIMEOUT_MS), PLCTAG_STATUS_OK);

    /* send a FIN without closing our read side, so this is a half-close and not a hangup. */
    assert_int_equal(shutdown(peer, SHUT_WR), 0);

    /* the mask deliberately leaves out SOCK_EVENT_CAN_READ. */
    events = (int32_t)socket_wait_event(client, SOCK_EVENT_DISCONNECT | SOCK_EVENT_TIMEOUT, TEST_TIMEOUT_MS);

    assert_true((events & SOCK_EVENT_DISCONNECT) != 0);
    assert_true((events & SOCK_EVENT_TIMEOUT) == 0);

    socket_close(client);
    socket_destroy(&client);

    close(peer);
    close(listener);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_socket_ops_work_above_fd_setsize),
        cmocka_unit_test(test_peer_half_close_reports_disconnect),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
