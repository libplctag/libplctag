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
 * A failed tag create must leave no connection behind.
 *
 * Today the tag is allocated before the connection is resolved, so nothing here can leak
 * and every case passes trivially.  That is the point: Phase L inverts the order so the
 * connection is resolved first and the tag is built from it, and from then on each of these
 * cases exercises a path that can leak.  The detector has to predate the code it guards.
 *
 * No PLC and no network: port 1 refuses, and a connection object plus its handler thread
 * exist either way.  Nothing here waits for a connect to succeed.
 */

#include "mini_mock.h"

#include <libplctag/api/libplctag.h>
#include <libplctag/modules/ab/ab_common.h>
#include <libplctag/modules/omron/omron_common.h>
#include <libplctag/modules/cip/tag.h>
#include <platform.h>
#include <stdio.h>
#include <stdint.h>


/* nothing listens here, so a connect attempt fails without reaching a device */
#define DEAD_GATEWAY "127.0.0.1:1"

/* Logix-class PLCs insist on a path, and that check runs before the connection. */
#define LOGIX_TAG "protocol=ab-eip&gateway=" DEAD_GATEWAY "&path=1,0&plc=controllogix"

/* a connection is torn down on the refcount cleanup thread, and its handler thread has to
 * be joined, so the count falls some time after the create returns rather than at once. */
#define TEARDOWN_WAIT_MS (5000)
#define POLL_INTERVAL_MS (10)


static int32_t total_live_conns(void) { return session_live_count() + conn_live_count(); }


/* true if every connection went away inside the deadline. */
static bool conns_drained(void) {
    int64_t deadline = time_ms() + TEARDOWN_WAIT_MS;

    while(time_ms() < deadline) {
        if(total_live_conns() == 0) { return true; }

        sleep_ms(POLL_INTERVAL_MS);
    }

    return total_live_conns() == 0;
}


/*
 * The counter has to be able to read one, or every other case in this file passes without
 * proving anything.  An unreachable gateway still produces a connection object.
 */
static void test_a_live_tag_holds_a_connection(void **state) {
    int32_t tag = 0;

    (void)state;

    assert_true(conns_drained());

    tag = plc_tag_create(LOGIX_TAG "&elem_size=4&elem_count=1&name=Dummy", 0);
    assert_true(tag > 0);
    assert_int_equal(session_live_count(), 1);

    plc_tag_destroy(tag);
    assert_true(conns_drained());
}


/*
 * elem_count past what the size field can carry.  This is checked after the connection is
 * resolved, which is what makes it the interesting case rather than the obvious one.
 */
static void test_an_oversized_element_count_leaks_nothing(void **state) {
    (void)state;

    assert_true(conns_drained());

    assert_true(plc_tag_create(LOGIX_TAG "&elem_count=70000&name=Dummy", 0) < 0);

    assert_true(conns_drained());
}


/* a name past what the encoder can hold, likewise checked after the connection is resolved. */
static void test_a_bad_tag_name_leaks_nothing(void **state) {
    char name[MAX_TAG_NAME * 2] = {0};
    char tag_string[sizeof(name) + 128] = {0};

    (void)state;

    assert_true(conns_drained());

    for(size_t i = 0; i < sizeof(name) - 1; i++) { name[i] = 'a'; }

    snprintf(tag_string, sizeof(tag_string), LOGIX_TAG "&elem_size=4&elem_count=1&name=%s", name);

    assert_true(plc_tag_create(tag_string, 0) < 0);

    assert_true(conns_drained());
}


/* the OMRON list is a separate list and needs its own case. */
static void test_an_omron_failure_leaks_nothing(void **state) {
    (void)state;

    assert_true(conns_drained());

    assert_true(
        plc_tag_create("protocol=ab-eip&gateway=" DEAD_GATEWAY "&path=1,0&plc=omron-njnx&elem_count=70000&name=Dummy", 0) < 0);

    assert_true(conns_drained());
}


int main(void) {
    int32_t rc = 0;

    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_a_live_tag_holds_a_connection),
        cmocka_unit_test(test_an_oversized_element_count_leaks_nothing),
        cmocka_unit_test(test_a_bad_tag_name_leaks_nothing),
        cmocka_unit_test(test_an_omron_failure_leaks_nothing),
    };

    rc = cmocka_run_group_tests(tests, NULL, NULL);

    plc_tag_shutdown();

    return rc;
}
