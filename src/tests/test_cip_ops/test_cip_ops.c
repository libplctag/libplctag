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
 * test_cip_ops
 *
 * Pins down how a CIP operation behaves while it is queued, packed or on the wire, so the
 * move of operation state onto the tag (Phase O) can be checked against it.  Each mode is one
 * scenario:
 *
 *   abort-packed           read --count tags at once, abort one; the others complete with
 *                          good data and the aborted one reads fine afterwards.
 *   destroy-in-flight      read --count tags at once and destroy half of them at varying
 *                          moments; the other half complete with good data.  Run under ASan.
 *   many-tags              create and read --count tags at once, more than fit in one
 *                          Multiple Service Packet; all complete with good data.
 *   callback-holds-mutex   a tag's reply arrives while its own callback is running, and so
 *                          holding the tag's API mutex; the read completes once it returns.
 *
 * Every tag is an element of TestBigArray, a DINT array the simulator must provide with at
 * least --count elements.  The test writes element i as i * 3 + 1 first, so each read checks
 * its own value.
 *
 * Usage: test_cip_ops --mode=<mode> --base="<tag string without name or elem_count>" [--count=N]
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 6, 0
#define DATA_TIMEOUT (5000)
#define MANY_TIMEOUT (30000)
#define CALLBACK_SLEEP_MS (500)
#define DESTROY_ROUNDS (20)
#define MAX_TAGS (2000)
#define ATTR_LEN (512)

static int32_t run_abort_packed(const char *base, int32_t count);
static int32_t run_destroy_in_flight(const char *base, int32_t count);
static int32_t run_many_tags(const char *base, int32_t count);
static int32_t run_callback_holds_mutex(const char *base);
static int32_t write_values(const char *base, int32_t count);
static int32_t create_elem(const char *base, int32_t index, int32_t timeout);
static int32_t wait_not_pending(const int32_t *ids, int32_t count, int32_t timeout_ms);
static int32_t check_value(int32_t id, int32_t index);
static void callback(int32_t tag_id, int event, int status, void *userdata);

static int32_t ids[MAX_TAGS];

static compat_atomic_int32_t callback_armed;
static compat_atomic_int32_t callback_slept;
static compat_atomic_int32_t reads_completed;


static int32_t value_of(int32_t index) { return index * 3 + 1; }


int main(int argc, char **argv) {
    const char *mode = NULL;
    const char *base = NULL;
    int32_t count = 20;
    int32_t failures = 0;

    for(int32_t i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--mode=", 7) == 0) {
            mode = argv[i] + 7;
        } else if(strncmp(argv[i], "--base=", 7) == 0) {
            base = argv[i] + 7;
        } else if(strncmp(argv[i], "--count=", 8) == 0) {
            count = (int32_t)atoi(argv[i] + 8);
        }
    }

    if(!mode || !base || count < 2 || count > MAX_TAGS) {
        printf("Usage: test_cip_ops --mode=<abort-packed|destroy-in-flight|many-tags|callback-holds-mutex> "
               "--base=\"<tag string>\" [--count=2..%d]\n",
               MAX_TAGS);
        return 1;
    }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    if(write_values(base, count) != PLCTAG_STATUS_OK) { return 1; }

    if(strcmp(mode, "abort-packed") == 0) {
        failures = run_abort_packed(base, count);
    } else if(strcmp(mode, "destroy-in-flight") == 0) {
        failures = run_destroy_in_flight(base, count);
    } else if(strcmp(mode, "many-tags") == 0) {
        failures = run_many_tags(base, count);
    } else if(strcmp(mode, "callback-holds-mutex") == 0) {
        failures = run_callback_holds_mutex(base);
    } else {
        printf("Unknown mode \"%s\"!\n", mode);
        return 1;
    }

    plc_tag_shutdown();

    printf("%s: %s with %" PRId32 " failure(s).\n", mode, failures ? "FAILED" : "PASSED", failures);

    return failures ? 1 : 0;
}


static int32_t run_abort_packed(const char *base, int32_t count) {
    int32_t failures = 0;
    int32_t victim = count / 3;
    int32_t rc = PLCTAG_STATUS_OK;

    for(int32_t i = 0; i < count; i++) {
        ids[i] = create_elem(base, i, DATA_TIMEOUT);
        if(ids[i] < 0) { return 1; }
    }

    /* start them all before any can finish, so they queue and pack together. */
    for(int32_t i = 0; i < count; i++) {
        rc = plc_tag_read(ids[i], 0);
        if(rc != PLCTAG_STATUS_PENDING && rc != PLCTAG_STATUS_OK) {
            printf("FAIL: starting the read of element %" PRId32 " returned %s.\n", i, plc_tag_decode_error(rc));
            failures++;
        }
    }

    rc = plc_tag_abort(ids[victim]);
    printf("Aborted element %" PRId32 ": %s.\n", victim, plc_tag_decode_error(rc));

    if(wait_not_pending(ids, count, DATA_TIMEOUT) != PLCTAG_STATUS_OK) { failures++; }

    for(int32_t i = 0; i < count; i++) {
        if(i == victim) { continue; }
        failures += check_value(ids[i], i);
    }

    /* the aborted tag must be usable again. */
    rc = plc_tag_read(ids[victim], DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        printf("FAIL: reading the aborted element again returned %s.\n", plc_tag_decode_error(rc));
        failures++;
    } else {
        failures += check_value(ids[victim], victim);
    }

    for(int32_t i = 0; i < count; i++) { plc_tag_destroy(ids[i]); }

    return failures;
}


static int32_t run_destroy_in_flight(const char *base, int32_t count) {
    int32_t failures = 0;
    int32_t survivors[MAX_TAGS];
    int32_t survivor_count = 0;

    for(int32_t round = 0; round < DESTROY_ROUNDS; round++) {
        survivor_count = 0;

        for(int32_t i = 0; i < count; i++) {
            ids[i] = create_elem(base, i, DATA_TIMEOUT);
            if(ids[i] < 0) { return failures + 1; }
        }

        for(int32_t i = 0; i < count; i++) { plc_tag_read(ids[i], 0); }

        /* vary the moment, so the destroy lands while queued, on the wire, or as the reply lands. */
        compat_sleep_ms((uint32_t)((round % 5) * 20), NULL);

        for(int32_t i = 0; i < count; i += 2) { plc_tag_destroy(ids[i]); }

        for(int32_t i = 1; i < count; i += 2) { survivors[survivor_count++] = ids[i]; }

        if(wait_not_pending(survivors, survivor_count, DATA_TIMEOUT) != PLCTAG_STATUS_OK) { failures++; }

        for(int32_t i = 1; i < count; i += 2) {
            failures += check_value(ids[i], i);
            plc_tag_destroy(ids[i]);
        }
    }

    printf("%d rounds of destroy while reading done.\n", DESTROY_ROUNDS);

    return failures;
}


static int32_t run_many_tags(const char *base, int32_t count) {
    int32_t failures = 0;
    int32_t rc = PLCTAG_STATUS_OK;
    int64_t start = 0;

    start = compat_time_ms();

    for(int32_t i = 0; i < count; i++) {
        ids[i] = create_elem(base, i, 0);
        if(ids[i] < 0) { return 1; }
    }

    if(wait_not_pending(ids, count, MANY_TIMEOUT) != PLCTAG_STATUS_OK) { return 1; }

    for(int32_t i = 0; i < count; i++) {
        rc = plc_tag_status(ids[i]);
        if(rc != PLCTAG_STATUS_OK) {
            printf("FAIL: creating element %" PRId32 " ended with %s.\n", i, plc_tag_decode_error(rc));
            return 1;
        }
    }

    printf("Created %" PRId32 " tags in %" PRId64 "ms.\n", count, compat_time_ms() - start);

    start = compat_time_ms();

    for(int32_t i = 0; i < count; i++) { plc_tag_read(ids[i], 0); }

    if(wait_not_pending(ids, count, MANY_TIMEOUT) != PLCTAG_STATUS_OK) { failures++; }

    printf("Read %" PRId32 " tags at once in %" PRId64 "ms.\n", count, compat_time_ms() - start);

    for(int32_t i = 0; i < count; i++) {
        failures += check_value(ids[i], i);
        plc_tag_destroy(ids[i]);
    }

    return failures;
}


static int32_t run_callback_holds_mutex(const char *base) {
    char attrs[ATTR_LEN];
    int32_t failures = 0;
    int32_t index = 5;
    int32_t id = 0;
    int32_t rc = PLCTAG_STATUS_OK;
    int64_t start = 0;

    compat_snprintf(attrs, sizeof(attrs), "%s&elem_count=1&name=TestBigArray[%" PRId32 "]", base, index);

    id = plc_tag_create_ex(attrs, callback, NULL, DATA_TIMEOUT);
    if(id < 0) {
        printf("FAIL: unable to create the tag, %s.\n", plc_tag_decode_error(id));
        return 1;
    }

    compat_atomic_store_int32(&reads_completed, 0);

    rc = plc_tag_read(id, 0);
    if(rc != PLCTAG_STATUS_PENDING) {
        printf("FAIL: the first read returned %s, not PENDING.\n", plc_tag_decode_error(rc));
        failures++;
    }

    /*
     * A second read while the first is out raises READ_STARTED and runs the callback inside
     * the tag's API mutex before it finds the tag busy.  The callback sleeps there, so the
     * first read's reply arrives while the mutex is held.
     */
    start = compat_time_ms();
    compat_atomic_store_int32(&callback_armed, 1);

    rc = plc_tag_read(id, 0);
    if(rc != PLCTAG_ERR_BUSY) {
        printf("FAIL: the second read returned %s, not BUSY.\n", plc_tag_decode_error(rc));
        failures++;
    }

    if(!compat_atomic_load_int32(&callback_slept)) {
        printf("FAIL: the callback never held the mutex, so nothing was tested.\n");
        plc_tag_destroy(id);
        return failures + 1;
    }

    if(wait_not_pending(&id, 1, DATA_TIMEOUT) != PLCTAG_STATUS_OK) { failures++; }

    failures += check_value(id, index);

    /* the completion event is raised by a library thread; give it the same budget. */
    while(compat_atomic_load_int32(&reads_completed) < 1 && compat_time_ms() - start < DATA_TIMEOUT) {
        compat_sleep_ms(10, NULL);
    }

    if(compat_atomic_load_int32(&reads_completed) != 1) {
        printf("FAIL: READ_COMPLETED fired %" PRId32 " times, expected 1.\n", compat_atomic_load_int32(&reads_completed));
        failures++;
    }

    printf("The read completed %" PRId64 "ms after the callback took the mutex.\n", compat_time_ms() - start);

    plc_tag_destroy(id);

    return failures;
}


static void callback(int32_t tag_id, int event, int status, void *userdata) {
    (void)tag_id;
    (void)status;
    (void)userdata;

    if(event == PLCTAG_EVENT_READ_STARTED && compat_atomic_load_int32(&callback_armed)) {
        compat_atomic_store_int32(&callback_armed, 0);
        compat_sleep_ms(CALLBACK_SLEEP_MS, NULL);
        compat_atomic_store_int32(&callback_slept, 1);
    }

    if(event == PLCTAG_EVENT_READ_COMPLETED) { compat_atomic_add_int32(&reads_completed, 1); }
}


static int32_t write_values(const char *base, int32_t count) {
    char attrs[ATTR_LEN];
    int32_t id = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    compat_snprintf(attrs, sizeof(attrs), "%s&elem_count=%" PRId32 "&name=TestBigArray", base, count);

    id = plc_tag_create(attrs, DATA_TIMEOUT);
    if(id < 0) {
        printf("FAIL: unable to create the array tag, %s.\n", plc_tag_decode_error(id));
        return id;
    }

    for(int32_t i = 0; i < count; i++) { plc_tag_set_int32(id, i * 4, value_of(i)); }

    rc = plc_tag_write(id, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) { printf("FAIL: unable to write the array, %s.\n", plc_tag_decode_error(rc)); }

    plc_tag_destroy(id);

    return rc;
}


static int32_t create_elem(const char *base, int32_t index, int32_t timeout) {
    char attrs[ATTR_LEN];
    int32_t id = 0;

    compat_snprintf(attrs, sizeof(attrs), "%s&elem_count=1&name=TestBigArray[%" PRId32 "]", base, index);

    id = plc_tag_create(attrs, timeout);
    if(id < 0) { printf("FAIL: unable to create element %" PRId32 ", %s.\n", index, plc_tag_decode_error(id)); }

    return id;
}


static int32_t wait_not_pending(const int32_t *tag_ids, int32_t count, int32_t timeout_ms) {
    int64_t end = compat_time_ms() + timeout_ms;

    for(int32_t i = 0; i < count; i++) {
        while(plc_tag_status(tag_ids[i]) == PLCTAG_STATUS_PENDING) {
            if(compat_time_ms() > end) {
                printf("FAIL: tag %" PRId32 " still PENDING after %" PRId32 "ms.\n", tag_ids[i], timeout_ms);
                return PLCTAG_ERR_TIMEOUT;
            }

            compat_sleep_ms(5, NULL);
        }
    }

    return PLCTAG_STATUS_OK;
}


static int32_t check_value(int32_t id, int32_t index) {
    int32_t rc = plc_tag_status(id);
    int32_t value = 0;

    if(rc != PLCTAG_STATUS_OK) {
        printf("FAIL: element %" PRId32 " ended with %s.\n", index, plc_tag_decode_error(rc));
        return 1;
    }

    value = plc_tag_get_int32(id, 0);
    if(value != value_of(index)) {
        printf("FAIL: element %" PRId32 " read %" PRId32 ", expected %" PRId32 ".\n", index, value, value_of(index));
        return 1;
    }

    return 0;
}
