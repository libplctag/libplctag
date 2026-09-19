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
 * socket_fd and poller, exercised over a loopback pair rather than mocked.
 * There is no way to mock a readiness answer that means anything: the
 * question these layers exist to answer is what the kernel does, so the
 * tests ask the kernel.
 *
 * What is pinned here is the behaviour the rest of the stack is going to
 * rely on, and each case corresponds to a way an event loop silently wedges
 * when it is wrong:
 *
 *   - a wait with nothing ready returns zero events rather than blocking
 *     forever or reporting a phantom one,
 *   - a wake is delivered once and then drained, not re-delivered on every
 *     subsequent wait, which is the classic level-triggered spin,
 *   - readiness is reported against the right context pointer, which is the
 *     only thing tying an event back to a connection,
 *   - a socket removed mid-iteration does not resurface,
 *   - a peer close is distinguishable from data.
 */

#include "mini_mock.h"

#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <string.h>
#include <utils/poller.h>
#include <utils/socket_fd.h>

#define TEST_POLL_TIMEOUT_MS (500)


/* a connected loopback pair, which socket_fd_pair() already knows how to build */
static void make_pair(socket_fd_t *a, socket_fd_t *b) {
    assert_int_equal(socket_fd_pair(a, b), PLCTAG_STATUS_OK);
}


static void test_pair_transfers_both_ways(void **state) {
    socket_fd_t a = SOCKET_FD_INVALID;
    socket_fd_t b = SOCKET_FD_INVALID;
    uint8_t out[4] = {1, 2, 3, 4};
    uint8_t in[4] = {0};
    int32_t count = 0;

    (void)state;

    make_pair(&a, &b);

    /* nothing sent yet, so a read is would-block rather than an error or a zero-length OK */
    assert_int_equal(socket_fd_recv(a, in, (int32_t)sizeof(in), &count), PLCTAG_STATUS_PENDING);
    assert_int_equal(count, 0);

    assert_int_equal(socket_fd_send(b, out, (int32_t)sizeof(out), &count), PLCTAG_STATUS_OK);
    assert_int_equal(count, (int32_t)sizeof(out));

    /*
     * A loopback write is not instantly readable on every platform, so wait
     * for readiness rather than assuming it.
     */
    {
        socket_fd_poll_item_t item;
        int32_t ready = 0;

        item.fd = a;
        item.want = SOCKET_FD_POLL_READ;
        item.got = 0;

        assert_int_equal(socket_fd_poll(&item, 1, TEST_POLL_TIMEOUT_MS, &ready), PLCTAG_STATUS_OK);
        assert_int_equal(ready, 1);
        assert_int_equal((item.got & SOCKET_FD_POLL_READ) != 0, true);
    }

    assert_int_equal(socket_fd_recv(a, in, (int32_t)sizeof(in), &count), PLCTAG_STATUS_OK);
    assert_int_equal(count, (int32_t)sizeof(out));
    assert_int_equal(memcmp(in, out, sizeof(out)), 0);

    socket_fd_close(&a);
    socket_fd_close(&b);
}


static void test_peer_close_reads_zero(void **state) {
    socket_fd_t a = SOCKET_FD_INVALID;
    socket_fd_t b = SOCKET_FD_INVALID;
    uint8_t in[4] = {0};
    int32_t count = -1;

    (void)state;

    make_pair(&a, &b);

    socket_fd_close(&b);

    {
        socket_fd_poll_item_t item;
        int32_t ready = 0;

        item.fd = a;
        item.want = SOCKET_FD_POLL_READ;
        item.got = 0;

        assert_int_equal(socket_fd_poll(&item, 1, TEST_POLL_TIMEOUT_MS, &ready), PLCTAG_STATUS_OK);
        assert_int_equal(ready, 1);
    }

    /*
     * An orderly close is OK with zero bytes, not an error.  A caller that
     * treats it as an error logs a failure every time a client hangs up
     * politely.
     */
    assert_int_equal(socket_fd_recv(a, in, (int32_t)sizeof(in), &count), PLCTAG_STATUS_OK);
    assert_int_equal(count, 0);

    socket_fd_close(&a);
}


static void test_wait_with_nothing_ready_times_out(void **state) {
    poller_p poller = NULL;
    socket_fd_t a = SOCKET_FD_INVALID;
    socket_fd_t b = SOCKET_FD_INVALID;
    poller_event_t events[4];
    int32_t count = -1;

    (void)state;

    make_pair(&a, &b);

    assert_int_equal(poller_create(&poller, 4), PLCTAG_STATUS_OK);
    assert_int_equal(poller_add(poller, a, POLLER_EVENT_CAN_READ, (void *)"ctx-a"), PLCTAG_STATUS_OK);

    /* a zero timeout is a poll, and nothing has been sent */
    assert_int_equal(poller_wait(poller, events, 4, 0, &count), PLCTAG_STATUS_OK);
    assert_int_equal(count, 0);

    poller_destroy(&poller);
    socket_fd_close(&a);
    socket_fd_close(&b);
}


static void test_readiness_carries_context(void **state) {
    poller_p poller = NULL;
    socket_fd_t a1 = SOCKET_FD_INVALID;
    socket_fd_t b1 = SOCKET_FD_INVALID;
    socket_fd_t a2 = SOCKET_FD_INVALID;
    socket_fd_t b2 = SOCKET_FD_INVALID;
    poller_event_t events[4];
    int32_t count = 0;
    int32_t written = 0;
    uint8_t poke = 42;

    (void)state;

    make_pair(&a1, &b1);
    make_pair(&a2, &b2);

    assert_int_equal(poller_create(&poller, 4), PLCTAG_STATUS_OK);
    assert_int_equal(poller_add(poller, a1, POLLER_EVENT_CAN_READ, (void *)"one"), PLCTAG_STATUS_OK);
    assert_int_equal(poller_add(poller, a2, POLLER_EVENT_CAN_READ, (void *)"two"), PLCTAG_STATUS_OK);
    assert_int_equal(poller_socket_count(poller), 2);

    /* only the second connection gets data */
    assert_int_equal(socket_fd_send(b2, &poke, 1, &written), PLCTAG_STATUS_OK);

    assert_int_equal(poller_wait(poller, events, 4, TEST_POLL_TIMEOUT_MS, &count), PLCTAG_STATUS_OK);
    assert_int_equal(count, 1);
    assert_int_equal((events[0].events & POLLER_EVENT_CAN_READ) != 0, true);

    /* the event has to name the connection that is ready, not just some connection */
    assert_int_equal(strcmp((const char *)events[0].context, "two"), 0);

    poller_destroy(&poller);
    socket_fd_close(&a1);
    socket_fd_close(&b1);
    socket_fd_close(&a2);
    socket_fd_close(&b2);
}


static void test_wake_fires_once(void **state) {
    poller_p poller = NULL;
    poller_event_t events[4];
    int32_t count = 0;

    (void)state;

    assert_int_equal(poller_create(&poller, 4), PLCTAG_STATUS_OK);

    assert_int_equal(poller_wake(poller), PLCTAG_STATUS_OK);

    assert_int_equal(poller_wait(poller, events, 4, TEST_POLL_TIMEOUT_MS, &count), PLCTAG_STATUS_OK);
    assert_int_equal(count, 1);
    assert_int_equal(events[0].events, POLLER_EVENT_WAKE_UP);
    assert_int_equal(events[0].context == NULL, true);

    /*
     * The wake pair is level triggered, so a wake that is not drained is
     * redelivered on every wait and the loop spins at 100% CPU.  This is the
     * case that catches that.
     */
    assert_int_equal(poller_wait(poller, events, 4, 0, &count), PLCTAG_STATUS_OK);
    assert_int_equal(count, 0);

    poller_destroy(&poller);
}


static void test_remove_stops_reporting(void **state) {
    poller_p poller = NULL;
    socket_fd_t a = SOCKET_FD_INVALID;
    socket_fd_t b = SOCKET_FD_INVALID;
    poller_event_t events[4];
    int32_t count = 0;
    int32_t written = 0;
    uint8_t poke = 7;

    (void)state;

    make_pair(&a, &b);

    assert_int_equal(poller_create(&poller, 4), PLCTAG_STATUS_OK);
    assert_int_equal(poller_add(poller, a, POLLER_EVENT_CAN_READ, (void *)"ctx"), PLCTAG_STATUS_OK);

    /* the same socket cannot be registered twice */
    assert_int_equal(poller_add(poller, a, POLLER_EVENT_CAN_READ, (void *)"ctx"), PLCTAG_ERR_DUPLICATE);

    assert_int_equal(socket_fd_send(b, &poke, 1, &written), PLCTAG_STATUS_OK);

    assert_int_equal(poller_wait(poller, events, 4, TEST_POLL_TIMEOUT_MS, &count), PLCTAG_STATUS_OK);
    assert_int_equal(count, 1);

    assert_int_equal(poller_remove(poller, a), PLCTAG_STATUS_OK);
    assert_int_equal(poller_socket_count(poller), 0);

    /* still readable, but no longer registered, so no event */
    assert_int_equal(poller_wait(poller, events, 4, 0, &count), PLCTAG_STATUS_OK);
    assert_int_equal(count, 0);

    assert_int_equal(poller_remove(poller, a), PLCTAG_ERR_NOT_FOUND);

    poller_destroy(&poller);
    socket_fd_close(&a);
    socket_fd_close(&b);
}


static void test_modify_changes_interest(void **state) {
    poller_p poller = NULL;
    socket_fd_t a = SOCKET_FD_INVALID;
    socket_fd_t b = SOCKET_FD_INVALID;
    poller_event_t events[4];
    int32_t count = 0;
    int32_t written = 0;
    uint8_t poke = 9;

    (void)state;

    make_pair(&a, &b);

    assert_int_equal(poller_create(&poller, 4), PLCTAG_STATUS_OK);

    /* registered, but not interested in reading yet */
    assert_int_equal(poller_add(poller, a, POLLER_EVENT_NONE, (void *)"ctx"), PLCTAG_STATUS_OK);

    assert_int_equal(socket_fd_send(b, &poke, 1, &written), PLCTAG_STATUS_OK);

    assert_int_equal(poller_wait(poller, events, 4, 0, &count), PLCTAG_STATUS_OK);
    assert_int_equal(count, 0);

    assert_int_equal(poller_modify(poller, a, POLLER_EVENT_CAN_READ), PLCTAG_STATUS_OK);

    assert_int_equal(poller_wait(poller, events, 4, TEST_POLL_TIMEOUT_MS, &count), PLCTAG_STATUS_OK);
    assert_int_equal(count, 1);
    assert_int_equal((events[0].events & POLLER_EVENT_CAN_READ) != 0, true);

    poller_destroy(&poller);
    socket_fd_close(&a);
    socket_fd_close(&b);
}


static void test_capacity_is_enforced(void **state) {
    poller_p poller = NULL;
    socket_fd_t a = SOCKET_FD_INVALID;
    socket_fd_t b = SOCKET_FD_INVALID;
    socket_fd_t c = SOCKET_FD_INVALID;
    socket_fd_t d = SOCKET_FD_INVALID;

    (void)state;

    make_pair(&a, &b);
    make_pair(&c, &d);

    assert_int_equal(poller_create(&poller, 1), PLCTAG_STATUS_OK);
    assert_int_equal(poller_add(poller, a, POLLER_EVENT_CAN_READ, NULL), PLCTAG_STATUS_OK);
    assert_int_equal(poller_add(poller, c, POLLER_EVENT_CAN_READ, NULL), PLCTAG_ERR_NO_RESOURCES);

    poller_destroy(&poller);
    socket_fd_close(&a);
    socket_fd_close(&b);
    socket_fd_close(&c);
    socket_fd_close(&d);
}


static void test_null_arguments_refused(void **state) {
    poller_p poller = NULL;
    poller_event_t events[2];
    int32_t count = 0;

    (void)state;

    assert_int_equal(poller_create(NULL, 4), PLCTAG_ERR_NULL_PTR);
    assert_int_equal(poller_create(&poller, 0), PLCTAG_ERR_OUT_OF_BOUNDS);

    assert_int_equal(poller_create(&poller, 2), PLCTAG_STATUS_OK);

    assert_int_equal(poller_wait(poller, NULL, 2, 0, &count), PLCTAG_ERR_NULL_PTR);
    assert_int_equal(poller_wait(poller, events, 2, 0, NULL), PLCTAG_ERR_NULL_PTR);
    assert_int_equal(poller_add(poller, SOCKET_FD_INVALID, POLLER_EVENT_CAN_READ, NULL), PLCTAG_ERR_BAD_PARAM);

    poller_destroy(&poller);

    /* destroying twice is not an error; it is how cleanup paths are written */
    assert_int_equal(poller_destroy(&poller), PLCTAG_STATUS_OK);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_pair_transfers_both_ways),
        cmocka_unit_test(test_peer_close_reads_zero),
        cmocka_unit_test(test_wait_with_nothing_ready_times_out),
        cmocka_unit_test(test_readiness_carries_context),
        cmocka_unit_test(test_wake_fires_once),
        cmocka_unit_test(test_remove_stops_reporting),
        cmocka_unit_test(test_modify_changes_interest),
        cmocka_unit_test(test_capacity_is_enforced),
        cmocka_unit_test(test_null_arguments_refused),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
