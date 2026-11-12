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
 * Verify find_or_create_tag, the tests of ephemeral_docs/tag_reuse.md:
 *   1  the same string returns the same handle.
 *   2  the same attributes in another order give another handle.
 *   3  a tag created without find_or_create_tag=1 never matches and is never matched.
 *   4  after destroy the other holder gets PLCTAG_ERR_NOT_FOUND and a create makes a new tag.
 *   5  a create with a timeout gets the handle of a create still pending.
 *   6  (--missing-tag= only) a failed create: the second create gets the error, a later one
 *      makes a new tag.
 *   7  16 threads creating one string at once all get one handle.
 *   8  the attribute reads back, is not writable, and is refused with a callback, with
 *      create_from_tag and with a value other than 0 or 1.
 *   9  a reusable tag auto-destroyed by destroy_tag_after_idle_sec is not found again.
 *
 * Each case adds its own read_cache_ms value, so no two cases share a string.
 *
 * Usage:
 *   test_find_or_create_tag --tag=BASE [--missing-tag=MISSING]
 * BASE is a tag string without find_or_create_tag.  MISSING is a tag string whose create fails.
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
#define NUM_THREADS (16)
#define REUSE "find_or_create_tag=1"

static const char *base_tag = NULL;
static const char *missing_tag = NULL;
static int32_t failures = 0;

static char race_str[TAG_STR_SIZE];
static compat_atomic_int32_t race_go;
static int32_t race_ids[NUM_THREADS];


static void fail(int32_t case_no, const char *msg) {
    printf("FAIL case %d: %s\n", (int)case_no, msg);
    failures++;
}


static void expect_rc(int32_t case_no, int32_t rc, int32_t expected, const char *what) {
    if(rc != expected) {
        printf("FAIL case %d: %s: got %d (%s), expected %d (%s)\n", (int)case_no, what, (int)rc, plc_tag_decode_error(rc),
               (int)expected, plc_tag_decode_error(expected));
        failures++;
    }
}


/* BASE, this case's read_cache_ms, then extra. */
static void make_str(char *buf, const char *base, int32_t case_no, const char *extra) {
    compat_snprintf(buf, TAG_STR_SIZE, "%s&read_cache_ms=%d%s", base, (int)case_no, extra);
}


static int32_t create(const char *str, int32_t timeout) { return plc_tag_create(str, (int)timeout); }


static void dummy_callback(int32_t tag_id, int event, int status, void *userdata) {
    (void)tag_id;
    (void)event;
    (void)status;
    (void)userdata;
}


static void case_same_string(void) {
    char str[TAG_STR_SIZE];
    int32_t a = 0;
    int32_t b = 0;

    make_str(str, base_tag, 1, "&" REUSE);
    a = create(str, DATA_TIMEOUT);
    b = create(str, DATA_TIMEOUT);

    if(a <= 0 || b <= 0) {
        fail(1, "create failed");
    } else if(a != b) {
        fail(1, "same string gave two handles");
    }

    plc_tag_destroy(a);
    if(b != a) { plc_tag_destroy(b); }
}


static void case_order(void) {
    char str1[TAG_STR_SIZE];
    char str2[TAG_STR_SIZE];
    int32_t a = 0;
    int32_t b = 0;

    make_str(str1, base_tag, 2, "&" REUSE);
    compat_snprintf(str2, sizeof(str2), REUSE "&%s&read_cache_ms=2", base_tag);

    a = create(str1, DATA_TIMEOUT);
    b = create(str2, DATA_TIMEOUT);

    if(a <= 0 || b <= 0) {
        fail(2, "create failed");
    } else if(a == b) {
        fail(2, "reordered string gave the same handle");
    }

    plc_tag_destroy(a);
    plc_tag_destroy(b);
}


static void case_opt_in(void) {
    char plain_str[TAG_STR_SIZE];
    char off_str[TAG_STR_SIZE];
    int32_t plain1 = 0;
    int32_t plain2 = 0;
    int32_t off1 = 0;
    int32_t off2 = 0;

    make_str(plain_str, base_tag, 3, "");
    make_str(off_str, base_tag, 3, "&find_or_create_tag=0");

    plain1 = create(plain_str, DATA_TIMEOUT);
    plain2 = create(plain_str, DATA_TIMEOUT);
    off1 = create(off_str, DATA_TIMEOUT);
    off2 = create(off_str, DATA_TIMEOUT);

    if(plain1 <= 0 || plain2 <= 0 || off1 <= 0 || off2 <= 0) {
        fail(3, "create failed");
    } else {
        if(plain1 == plain2) { fail(3, "string without find_or_create_tag gave the same handle"); }
        if(off1 == off2) { fail(3, "string with find_or_create_tag=0 gave the same handle"); }
    }

    plc_tag_destroy(plain1);
    plc_tag_destroy(plain2);
    plc_tag_destroy(off1);
    plc_tag_destroy(off2);
}


static void case_destroy(void) {
    char str[TAG_STR_SIZE];
    int32_t a = 0;
    int32_t b = 0;
    int32_t c = 0;

    make_str(str, base_tag, 4, "&" REUSE);
    a = create(str, DATA_TIMEOUT);
    b = create(str, DATA_TIMEOUT);

    if(a <= 0 || b != a) {
        fail(4, "setup: expected one shared handle");
        plc_tag_destroy(a);
        if(b != a) { plc_tag_destroy(b); }
        return;
    }

    expect_rc(4, plc_tag_destroy(a), PLCTAG_STATUS_OK, "first destroy");
    expect_rc(4, plc_tag_status(b), PLCTAG_ERR_NOT_FOUND, "other holder after destroy");
    expect_rc(4, plc_tag_destroy(b), PLCTAG_ERR_NOT_FOUND, "second destroy");

    c = create(str, DATA_TIMEOUT);
    if(c <= 0) {
        fail(4, "create after destroy failed");
    } else if(c == a) {
        fail(4, "create after destroy returned the destroyed handle");
    }

    plc_tag_destroy(c);
}


static void case_pending(void) {
    char str[TAG_STR_SIZE];
    int32_t a = 0;
    int32_t b = 0;

    make_str(str, base_tag, 5, "&" REUSE);
    a = create(str, 0);
    b = create(str, DATA_TIMEOUT);

    if(a <= 0 || b <= 0) {
        fail(5, "create failed");
    } else if(a != b) {
        fail(5, "create with a timeout did not get the pending handle");
    } else {
        expect_rc(5, plc_tag_status(b), PLCTAG_STATUS_OK, "status after the waiting create");
    }

    plc_tag_destroy(a);
    if(b != a) { plc_tag_destroy(b); }
}


static void case_failed_create(void) {
    char str[TAG_STR_SIZE];
    int32_t a = 0;
    int32_t b = 0;
    int32_t c = 0;

    make_str(str, missing_tag, 6, "&" REUSE);

    a = create(str, 0);
    if(a <= 0) {
        printf("Case 6: the first create failed at once with %s.\n", plc_tag_decode_error(a));
    }

    b = create(str, DATA_TIMEOUT);
    if(b > 0) {
        fail(6, "create of a missing tag returned a handle");
        plc_tag_destroy(b);
    }

    c = create(str, 0);
    if(c > 0 && c == a) { fail(6, "a later create returned the failed tag"); }

    if(a > 0) { plc_tag_destroy(a); }
    if(c > 0) { plc_tag_destroy(c); }
}


static void *race_thread(void *arg) {
    intptr_t index = (intptr_t)arg;

    while(!compat_atomic_load_int32(&race_go)) { compat_thread_yield(); }

    race_ids[index] = create(race_str, DATA_TIMEOUT);

    return NULL;
}


static void case_race(void) {
    compat_thread_t threads[NUM_THREADS];
    int32_t first_id = 0;

    make_str(race_str, base_tag, 7, "&" REUSE);
    compat_atomic_store_int32(&race_go, 0);

    for(intptr_t i = 0; i < NUM_THREADS; i++) { compat_thread_create(&threads[i], race_thread, (void *)i); }

    compat_atomic_store_int32(&race_go, 1);

    for(size_t i = 0; i < NUM_THREADS; i++) { compat_thread_join(threads[i], NULL); }

    for(size_t i = 0; i < NUM_THREADS; i++) {
        if(race_ids[i] <= 0) {
            expect_rc(7, race_ids[i], PLCTAG_STATUS_OK, "a racing create");
        } else if(first_id == 0) {
            first_id = race_ids[i];
        } else if(race_ids[i] != first_id) {
            fail(7, "racing creates got different handles");
        }
    }

    /* every thread got the same handle, so one destroy ends it; the rest fail harmlessly. */
    for(size_t i = 0; i < NUM_THREADS; i++) {
        if(race_ids[i] > 0) { plc_tag_destroy(race_ids[i]); }
    }
}


static void case_attribute(void) {
    char str[TAG_STR_SIZE];
    int32_t a = 0;
    int32_t plain = 0;
    int32_t rc = 0;

    make_str(str, base_tag, 8, "&" REUSE);
    a = create(str, DATA_TIMEOUT);
    if(a <= 0) {
        fail(8, "create failed");
    } else {
        expect_rc(8, plc_tag_get_int_attribute(a, "find_or_create_tag", -1), 1, "attribute read");
        expect_rc(8, plc_tag_set_int_attribute(a, "find_or_create_tag", 0), PLCTAG_ERR_UNSUPPORTED, "attribute write");

        rc = plc_tag_create_from_tag(a, "name=TestDINTArray&" REUSE, NULL, NULL, DATA_TIMEOUT);
        if(rc > 0) { plc_tag_destroy(rc); }
        expect_rc(8, rc, PLCTAG_ERR_NOT_ALLOWED, "create_from_tag with " REUSE);
    }

    make_str(str, base_tag, 8, "");
    plain = create(str, DATA_TIMEOUT);
    if(plain <= 0) {
        fail(8, "plain create failed");
    } else {
        expect_rc(8, plc_tag_get_int_attribute(plain, "find_or_create_tag", -1), 0, "attribute read without it");
    }

    make_str(str, base_tag, 8, "&" REUSE);
    rc = plc_tag_create_ex(str, dummy_callback, NULL, DATA_TIMEOUT);
    if(rc > 0) { plc_tag_destroy(rc); }
    expect_rc(8, rc, PLCTAG_ERR_NOT_ALLOWED, "create with a callback");

    make_str(str, base_tag, 8, "&find_or_create_tag=2");
    rc = create(str, DATA_TIMEOUT);
    if(rc > 0) { plc_tag_destroy(rc); }
    expect_rc(8, rc, PLCTAG_ERR_BAD_PARAM, "find_or_create_tag=2");

    plc_tag_destroy(a);
    plc_tag_destroy(plain);
}


static void case_auto_destroy(void) {
    char str[TAG_STR_SIZE];
    int32_t a = 0;
    int32_t b = 0;

    make_str(str, base_tag, 9, "&destroy_tag_after_idle_sec=10&" REUSE);
    a = create(str, DATA_TIMEOUT);
    if(a <= 0) {
        fail(9, "create failed");
        return;
    }

    compat_sleep_ms(12000, NULL);

    expect_rc(9, plc_tag_status(a), PLCTAG_ERR_NOT_FOUND, "status after the idle time");

    b = create(str, DATA_TIMEOUT);
    if(b <= 0) {
        fail(9, "create after auto-destroy failed");
    } else if(b == a) {
        fail(9, "create after auto-destroy returned the destroyed handle");
    }

    plc_tag_destroy(b);
}


int main(int argc, char **argv) {
    for(int i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) {
            base_tag = argv[i] + 6;
        } else if(strncmp(argv[i], "--missing-tag=", 14) == 0) {
            missing_tag = argv[i] + 14;
        } else {
            printf("Unknown argument %s.\n", argv[i]);
            return 1;
        }
    }

    if(!base_tag) {
        printf("Usage: test_find_or_create_tag --tag=BASE [--missing-tag=MISSING]\n");
        return 1;
    }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    case_same_string();
    case_order();
    case_opt_in();
    case_destroy();
    case_pending();
    if(missing_tag) { case_failed_create(); }
    case_race();
    case_attribute();
    case_auto_destroy();

    plc_tag_shutdown();

    if(failures) {
        printf("FAILED: %d failures.\n", (int)failures);
        return 1;
    }

    printf("SUCCESS\n");
    return 0;
}
