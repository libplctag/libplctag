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
 * Verify destroy_tag_after_idle_sec.
 *
 * Default mode runs tests 1-4 of ephemeral_docs/auto_destroy_idle_tags.md on one timeline
 * (about 42 s), one tag per case:
 *   A  destroy=10, never used: alive at 8 s, gone by 12 s, callback saw DESTROYED.
 *   B  destroy=10, read every 5 s to 30 s: alive at 35 s, gone by 42 s.
 *   C  destroy=20, set to 10 at 8 s: alive at 15 s, gone by 20 s.
 *   D  destroy=10, set to 0 at 5 s: alive at 42 s.
 *   Refusals: auto-sync with destroy at create, 9 at create, auto-sync set at runtime.
 *
 * With --conn-tag=, runs test 5 instead: the connection idles out, the tag is still destroyed
 * by 12 s, and the connection never comes back up.
 *
 * Usage:
 *   test_auto_destroy --tag=BASE [--conn-tag=CONN]
 * BASE is a tag string without destroy_tag_after_idle_sec; the tool appends it.
 * CONN is an @connection tag string for the same connection as BASE.
 */

#include "compat_utils.h"
#include <libplctag/api/libplctag.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 6, 0

#define DATA_TIMEOUT (5000)
#define TAG_STR_SIZE (1024)
#define POLL_MS (50)
#define IDLE_TIMEOUT_MS (1000)
#define ATTR_NAME "destroy_tag_after_idle_sec"

/* TAG_SCRATCH takes the events of the tags that are expected to fail create. */
enum { TAG_A = 0, TAG_B, TAG_C, TAG_D, NUM_TAGS, TAG_SCRATCH = NUM_TAGS };

static const char *tag_names[NUM_TAGS] = {"A", "B", "C", "D"};

static const char *base_tag = NULL;
static const char *conn_tag_str = NULL;
static int64_t start_ms = 0;
static int32_t failures = 0;
static compat_atomic_int32_t destroyed_events[NUM_TAGS + 1];


static void tag_callback(int32_t tag_id, int event, int status, void *userdata) {
    (void)tag_id;
    (void)status;

    if(event == PLCTAG_EVENT_DESTROYED) { compat_atomic_inc_int32(&destroyed_events[(intptr_t)userdata]); }
}


static void fail(const char *msg, const char *name) {
    printf("FAIL at %.1f s: tag %s: %s\n", (double)(compat_time_ms() - start_ms) / 1000.0, name, msg);
    failures++;
}


static void wait_until(int64_t offset_ms) {
    int64_t now = compat_time_ms();

    if(start_ms + offset_ms > now) { compat_sleep_ms((uint32_t)(start_ms + offset_ms - now), NULL); }
}


static int32_t make_tag(const char *extra, intptr_t index) {
    char tag_str[TAG_STR_SIZE];

    compat_snprintf(tag_str, sizeof(tag_str), "%s&%s", base_tag, extra);

    return plc_tag_create_ex(tag_str, tag_callback, (void *)index, DATA_TIMEOUT);
}


static void expect_alive(int32_t tag, const char *name) {
    int32_t rc = plc_tag_status(tag);

    if(rc != PLCTAG_STATUS_OK) { fail(plc_tag_decode_error(rc), name); }
}


static void expect_gone(int32_t tag, const char *name) {
    int32_t rc = plc_tag_status(tag);

    if(rc != PLCTAG_ERR_NOT_FOUND) { fail("still alive, expected PLCTAG_ERR_NOT_FOUND", name); }
}


static void expect_rc(int32_t rc, int32_t expected, const char *name) {
    if(rc != expected) {
        printf("    got %s, expected %s\n", plc_tag_decode_error(rc), plc_tag_decode_error(expected));
        fail("wrong status", name);
    }
}


static void read_tag(int32_t tag, const char *name) {
    int32_t rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) { fail(plc_tag_decode_error(rc), name); }
}


static int32_t run_timeline(void) {
    int32_t tags[NUM_TAGS];
    int32_t rc = 0;

    start_ms = compat_time_ms();

    tags[TAG_A] = make_tag(ATTR_NAME "=10", TAG_A);
    tags[TAG_B] = make_tag(ATTR_NAME "=10", TAG_B);
    tags[TAG_C] = make_tag(ATTR_NAME "=20", TAG_C);
    tags[TAG_D] = make_tag(ATTR_NAME "=10", TAG_D);

    for(size_t i = 0; i < NUM_TAGS; i++) {
        if(tags[i] < 0) {
            fail(plc_tag_decode_error(tags[i]), tag_names[i]);
            return 1;
        }
    }

    printf("Attribute reads back as set.\n");
    expect_rc(plc_tag_get_int_attribute(tags[TAG_A], ATTR_NAME, -1), 10, "A");
    expect_rc(plc_tag_get_int_attribute(tags[TAG_C], ATTR_NAME, -1), 20, "C");

    printf("Refusals at create.\n");
    rc = make_tag(ATTR_NAME "=10&auto_sync_read_ms=200", TAG_SCRATCH);
    expect_rc(rc, PLCTAG_ERR_BAD_PARAM, "auto_sync_read_ms with destroy");
    if(rc >= 0) { plc_tag_destroy(rc); }

    rc = make_tag(ATTR_NAME "=9", TAG_SCRATCH);
    expect_rc(rc, PLCTAG_ERR_OUT_OF_BOUNDS, "destroy=9");
    if(rc >= 0) { plc_tag_destroy(rc); }

    wait_until(5000);
    printf("5 s: read B, D refuses auto-sync, D set to 0.\n");
    read_tag(tags[TAG_B], "B");
    expect_rc(plc_tag_set_int_attribute(tags[TAG_D], "auto_sync_read_ms", 200), PLCTAG_ERR_NOT_ALLOWED, "D auto_sync_read_ms");
    expect_rc(plc_tag_set_int_attribute(tags[TAG_D], ATTR_NAME, 9), PLCTAG_ERR_OUT_OF_BOUNDS, "D set 9");
    expect_rc(plc_tag_set_int_attribute(tags[TAG_D], ATTR_NAME, 0), PLCTAG_STATUS_OK, "D set 0");

    wait_until(8000);
    printf("8 s: A alive, C set to 10.\n");
    expect_alive(tags[TAG_A], "A");
    expect_rc(plc_tag_set_int_attribute(tags[TAG_C], ATTR_NAME, 10), PLCTAG_STATUS_OK, "C set 10");

    wait_until(10000);
    read_tag(tags[TAG_B], "B");

    wait_until(12000);
    printf("12 s: A gone, DESTROYED seen.\n");
    expect_gone(tags[TAG_A], "A");
    expect_rc(compat_atomic_load_int32(&destroyed_events[TAG_A]), 1, "A DESTROYED events");

    wait_until(15000);
    printf("15 s: read B, C alive.\n");
    read_tag(tags[TAG_B], "B");
    expect_alive(tags[TAG_C], "C");

    wait_until(20000);
    printf("20 s: read B, C gone.\n");
    read_tag(tags[TAG_B], "B");
    expect_gone(tags[TAG_C], "C");

    wait_until(25000);
    read_tag(tags[TAG_B], "B");

    wait_until(30000);
    read_tag(tags[TAG_B], "B");

    wait_until(35000);
    printf("35 s: B alive.\n");
    expect_alive(tags[TAG_B], "B");

    wait_until(42000);
    printf("42 s: B gone, D alive.\n");
    expect_gone(tags[TAG_B], "B");
    expect_alive(tags[TAG_D], "D");

    for(size_t i = 0; i < NUM_TAGS; i++) { plc_tag_destroy(tags[i]); }

    return 0;
}


static int32_t run_idle_connection(void) {
    int32_t tag = 0;
    int32_t conn_tag = 0;
    int32_t status = 0;
    bool idled = false;
    bool gone = false;

    start_ms = compat_time_ms();

    /* the create read brings the connection up before the @connection tag joins it. */
    tag = make_tag(ATTR_NAME "=10", TAG_A);
    if(tag < 0) {
        fail(plc_tag_decode_error(tag), "A");
        return 1;
    }

    conn_tag = plc_tag_create(conn_tag_str, DATA_TIMEOUT);
    if(conn_tag < 0) {
        fail(plc_tag_decode_error(conn_tag), "@connection");
        plc_tag_destroy(tag);
        return 1;
    }

    expect_rc(plc_tag_set_int_attribute(tag, "connection_inactivity_timeout_ms", IDLE_TIMEOUT_MS), PLCTAG_STATUS_OK,
              "A connection_inactivity_timeout_ms");

    printf("Polling the connection until 14 s.\n");

    while(compat_time_ms() - start_ms < 14000) {
        int64_t elapsed = compat_time_ms() - start_ms;

        status = plc_tag_get_int_attribute(conn_tag, "connection_status", -1);

        if(status == PLCTAG_CONN_STATUS_IDLE_WAIT || status == PLCTAG_CONN_STATUS_DOWN) {
            if(!idled) { printf("Connection idle at %.1f s.\n", (double)elapsed / 1000.0); }
            idled = true;
        } else if(idled) {
            printf("    connection_status %d\n", (int)status);
            fail("connection came back after idling", "@connection");
            break;
        }

        if(!gone && plc_tag_status(tag) == PLCTAG_ERR_NOT_FOUND) {
            printf("Tag gone at %.1f s.\n", (double)elapsed / 1000.0);
            gone = true;
            if(elapsed < 9500) { fail("destroyed early", "A"); }
        }

        compat_sleep_ms(POLL_MS, NULL);
    }

    if(!idled) { fail("connection never idled out", "@connection"); }
    if(!gone) { fail("not destroyed", "A"); }
    expect_rc(compat_atomic_load_int32(&destroyed_events[TAG_A]), 1, "A DESTROYED events");

    plc_tag_destroy(tag);
    plc_tag_destroy(conn_tag);

    return 0;
}


int main(int argc, char **argv) {
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    for(int i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) { base_tag = &argv[i][6]; }
        if(strncmp(argv[i], "--conn-tag=", 11) == 0) { conn_tag_str = &argv[i][11]; }
    }

    if(base_tag == NULL) {
        printf("Usage: test_auto_destroy --tag=BASE [--conn-tag=CONN]\n");
        return 1;
    }

    for(size_t i = 0; i <= NUM_TAGS; i++) { compat_atomic_store_int32(&destroyed_events[i], 0); }

    if(conn_tag_str) {
        run_idle_connection();
    } else {
        run_timeline();
    }

    if(failures) {
        printf("FAILED with %d failing checks.\n", (int)failures);
        return 1;
    }

    printf("PASSED\n");

    return 0;
}
