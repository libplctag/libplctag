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
 * test_modbus_ops
 *
 * How Modbus operations behave with several requests in flight, under aborts, across dropped
 * connections and with auto-sync.  Each mode is one scenario:
 *
 *   pipelined     write and read --count holding registers at once, three rounds; every
 *                 value checks.  Run it with max_requests_in_flight above 1, against a server
 *                 that reorders replies, and against one that drops the connection.
 *   abort-storm   start a read on every tag and abort them at varying moments, many rounds;
 *                 then every tag reads its value, and every READ_STARTED had its completion.
 *   write-read    write then read back one register many times, each synchronously.  Against
 *                 a server that drops the connection, every operation still succeeds.
 *   tid-wrap      read --count tags at once until more than 65536 requests have gone out,
 *                 so the transaction ID wraps; every value checks.
 *   auto-dirty    a tag with auto-sync read and write gets new data while its reads run;
 *                 each value reaches the server, and every STARTED had its completion.
 *
 * Tag i is holding register hr<i>, with value_of(i, round).
 *
 * Usage: test_modbus_ops --mode=<mode> --base="<tag string without name or elem_count>" [--count=N]
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 6, 0
#define DATA_TIMEOUT (10000)
#define MAX_TAGS (100)
#define ATTR_LEN (512)
#define ABORT_ROUNDS (200)
#define WRITE_READ_ROUNDS (40)
#define TID_WRAP_REQUESTS (66000)
#define AUTO_DIRTY_ROUNDS (40)
#define AUTO_REGISTER (90)

typedef struct {
    compat_atomic_int32_t read_started;
    compat_atomic_int32_t read_completed;
    compat_atomic_int32_t write_started;
    compat_atomic_int32_t write_completed;
    compat_atomic_int32_t aborted;
} event_counts_t;

static int32_t run_pipelined(const char *base, int32_t count);
static int32_t run_abort_storm(const char *base, int32_t count);
static int32_t run_write_read(const char *base);
static int32_t run_tid_wrap(const char *base, int32_t count);
static int32_t run_auto_dirty(const char *base);
static int32_t create_reg(const char *base, int32_t reg, const char *extra, event_counts_t *counts);
static int32_t wait_not_pending(const int32_t *tag_ids, int32_t count, int32_t timeout_ms);
static int32_t check_reg(int32_t id, int32_t reg, int16_t expected);
static int32_t check_balanced(const char *what, event_counts_t *counts);
static void callback(int32_t tag_id, int event, int status, void *userdata);

static int32_t ids[MAX_TAGS];
static event_counts_t counts[MAX_TAGS];


static int16_t value_of(int32_t reg, int32_t round) { return (int16_t)((reg * 7 + round * 131 + 3) & 0x7FFF); }


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

    if(!mode || !base || count < 1 || count > MAX_TAGS) {
        printf("Usage: test_modbus_ops --mode=<pipelined|abort-storm|write-read|tid-wrap|auto-dirty> "
               "--base=\"<tag string>\" [--count=1..%d]\n",
               MAX_TAGS);
        return 1;
    }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    if(strcmp(mode, "pipelined") == 0) {
        failures = run_pipelined(base, count);
    } else if(strcmp(mode, "abort-storm") == 0) {
        failures = run_abort_storm(base, count);
    } else if(strcmp(mode, "write-read") == 0) {
        failures = run_write_read(base);
    } else if(strcmp(mode, "tid-wrap") == 0) {
        failures = run_tid_wrap(base, count);
    } else if(strcmp(mode, "auto-dirty") == 0) {
        failures = run_auto_dirty(base);
    } else {
        printf("Unknown mode %s.\n", mode);
        return 1;
    }

    printf("%s: %s with %" PRId32 " failure(s).\n", mode, (failures ? "FAILED" : "PASSED"), failures);

    plc_tag_shutdown();

    return failures ? 1 : 0;
}


static int32_t run_pipelined(const char *base, int32_t count) {
    int32_t failures = 0;

    for(int32_t i = 0; i < count; i++) {
        ids[i] = create_reg(base, i, "", NULL);
        if(ids[i] < 0) { return 1; }
    }

    for(int32_t round = 1; round <= 3 && !failures; round++) {
        /* every tag's write goes out at once, then every tag's read. */
        for(int32_t i = 0; i < count; i++) {
            plc_tag_set_int16(ids[i], 0, value_of(i, round));
            plc_tag_write(ids[i], 0);
        }

        if(wait_not_pending(ids, count, DATA_TIMEOUT) != PLCTAG_STATUS_OK) { failures++; }

        for(int32_t i = 0; i < count; i++) {
            plc_tag_set_int16(ids[i], 0, 0);
            plc_tag_read(ids[i], 0);
        }

        if(wait_not_pending(ids, count, DATA_TIMEOUT) != PLCTAG_STATUS_OK) { failures++; }

        for(int32_t i = 0; i < count; i++) { failures += check_reg(ids[i], i, value_of(i, round)); }
    }

    for(int32_t i = 0; i < count; i++) { plc_tag_destroy(ids[i]); }

    return failures;
}


static int32_t run_abort_storm(const char *base, int32_t count) {
    int32_t failures = 0;

    for(int32_t i = 0; i < count; i++) {
        ids[i] = create_reg(base, i, "", &counts[i]);
        if(ids[i] < 0) { return 1; }

        plc_tag_set_int16(ids[i], 0, value_of(i, 0));
        if(plc_tag_write(ids[i], DATA_TIMEOUT) != PLCTAG_STATUS_OK) {
            printf("FAIL: unable to write register %" PRId32 ".\n", i);
            return 1;
        }
    }

    for(int32_t round = 0; round < ABORT_ROUNDS; round++) {
        for(int32_t i = 0; i < count; i++) { plc_tag_read(ids[i], 0); }

        /* abort while queued, while on the wire, or once some have answered. */
        if(round % 3 == 1) { compat_sleep_ms(1, NULL); }
        if(round % 3 == 2) { (void)plc_tag_status(ids[count - 1]); }

        for(int32_t i = (round & 1); i < count; i += 2) { plc_tag_abort(ids[i]); }

        if(wait_not_pending(ids, count, DATA_TIMEOUT) != PLCTAG_STATUS_OK) {
            failures++;
            break;
        }
    }

    for(int32_t i = 0; i < count; i++) {
        int32_t rc = plc_tag_read(ids[i], DATA_TIMEOUT);

        if(rc != PLCTAG_STATUS_OK) {
            printf("FAIL: register %" PRId32 " read after the storm returned %s.\n", i, plc_tag_decode_error(rc));
            failures++;
            continue;
        }

        failures += check_reg(ids[i], i, value_of(i, 0));
    }

    /* the last completions are dispatched by the handler thread; give it a moment. */
    compat_sleep_ms(200, NULL);

    for(int32_t i = 0; i < count; i++) {
        failures += check_balanced("abort-storm", &counts[i]);
        plc_tag_destroy(ids[i]);
    }

    printf("Aborted %" PRId32 " operations on register 0.\n", compat_atomic_load_int32(&counts[0].aborted));

    return failures;
}


static int32_t run_write_read(const char *base) {
    int32_t failures = 0;
    int32_t reg = 5;
    int32_t id = create_reg(base, reg, "", NULL);

    if(id < 0) { return 1; }

    for(int32_t round = 0; round < WRITE_READ_ROUNDS; round++) {
        int32_t rc = PLCTAG_STATUS_OK;

        plc_tag_set_int16(id, 0, value_of(reg, round));

        rc = plc_tag_write(id, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            printf("FAIL: write %" PRId32 " returned %s.\n", round, plc_tag_decode_error(rc));
            failures++;
            continue;
        }

        plc_tag_set_int16(id, 0, 0);

        rc = plc_tag_read(id, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            printf("FAIL: read %" PRId32 " returned %s.\n", round, plc_tag_decode_error(rc));
            failures++;
            continue;
        }

        failures += check_reg(id, reg, value_of(reg, round));
    }

    plc_tag_destroy(id);

    return failures;
}


static int32_t run_tid_wrap(const char *base, int32_t count) {
    int32_t failures = 0;
    int32_t requests = 0;
    int64_t start = compat_time_ms();

    for(int32_t i = 0; i < count; i++) {
        ids[i] = create_reg(base, i, "", NULL);
        if(ids[i] < 0) { return 1; }

        plc_tag_set_int16(ids[i], 0, value_of(i, 2));
        if(plc_tag_write(ids[i], DATA_TIMEOUT) != PLCTAG_STATUS_OK) {
            printf("FAIL: unable to write register %" PRId32 ".\n", i);
            return 1;
        }
    }

    while(requests < TID_WRAP_REQUESTS && !failures) {
        for(int32_t i = 0; i < count; i++) { plc_tag_read(ids[i], 0); }

        if(wait_not_pending(ids, count, DATA_TIMEOUT) != PLCTAG_STATUS_OK) { failures++; }

        for(int32_t i = 0; i < count && !failures; i++) { failures += check_reg(ids[i], i, value_of(i, 2)); }

        requests += count;
    }

    printf("Read %" PRId32 " times in %" PRId64 "ms.\n", requests, compat_time_ms() - start);

    for(int32_t i = 0; i < count; i++) { plc_tag_destroy(ids[i]); }

    return failures;
}


static int32_t run_auto_dirty(const char *base) {
    int32_t failures = 0;
    int32_t auto_id = create_reg(base, AUTO_REGISTER, "&auto_sync_read_ms=10&auto_sync_write_ms=5", &counts[0]);
    int32_t plain_id = create_reg(base, AUTO_REGISTER, "", NULL);

    if(auto_id < 0 || plain_id < 0) { return 1; }

    for(int32_t round = 1; round <= AUTO_DIRTY_ROUNDS && !failures; round++) {
        int16_t value = value_of(AUTO_REGISTER, round);
        int64_t end = compat_time_ms() + DATA_TIMEOUT;
        int16_t seen = 0;

        /* new data while reads run every 10ms: the write has to win. */
        plc_tag_set_int16(auto_id, 0, value);

        do {
            compat_sleep_ms(5, NULL);
            if(plc_tag_read(plain_id, DATA_TIMEOUT) == PLCTAG_STATUS_OK) { seen = plc_tag_get_int16(plain_id, 0); }
        } while(seen != value && compat_time_ms() < end);

        if(seen != value) {
            printf("FAIL: round %" PRId32 " wrote %d but the server holds %d.\n", round, value, seen);
            failures++;
        }

        /* let a few auto-sync reads run, so the next write lands on one. */
        compat_sleep_ms(35, NULL);
    }

    /* stop the reads and let the last operation finish before counting. */
    plc_tag_set_int_attribute(auto_id, "auto_sync_read_ms", 0);
    compat_sleep_ms(300, NULL);

    failures += check_balanced("auto-dirty", &counts[0]);

    printf("Auto-sync tag: %" PRId32 " reads, %" PRId32 " writes, %" PRId32 " aborts.\n",
           compat_atomic_load_int32(&counts[0].read_completed), compat_atomic_load_int32(&counts[0].write_completed),
           compat_atomic_load_int32(&counts[0].aborted));

    plc_tag_destroy(auto_id);
    plc_tag_destroy(plain_id);

    return failures;
}


static int32_t create_reg(const char *base, int32_t reg, const char *extra, event_counts_t *event_counts) {
    char attrs[ATTR_LEN];
    int32_t id = 0;

    compat_snprintf(attrs, sizeof(attrs), "%s&elem_count=1&name=hr%" PRId32 "%s", base, reg, extra);

    id = plc_tag_create_ex(attrs, (event_counts ? callback : NULL), event_counts, DATA_TIMEOUT);
    if(id < 0) { printf("FAIL: unable to create register %" PRId32 ", %s.\n", reg, plc_tag_decode_error(id)); }

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

            compat_sleep_ms(1, NULL);
        }
    }

    return PLCTAG_STATUS_OK;
}


static int32_t check_reg(int32_t id, int32_t reg, int16_t expected) {
    int32_t rc = plc_tag_status(id);
    int16_t value = 0;

    if(rc != PLCTAG_STATUS_OK) {
        printf("FAIL: register %" PRId32 " ended with %s.\n", reg, plc_tag_decode_error(rc));
        return 1;
    }

    value = plc_tag_get_int16(id, 0);
    if(value != expected) {
        printf("FAIL: register %" PRId32 " read %d, expected %d.\n", reg, value, expected);
        return 1;
    }

    return 0;
}


static int32_t check_balanced(const char *what, event_counts_t *event_counts) {
    int32_t rs = compat_atomic_load_int32(&event_counts->read_started);
    int32_t rc = compat_atomic_load_int32(&event_counts->read_completed);
    int32_t ws = compat_atomic_load_int32(&event_counts->write_started);
    int32_t wc = compat_atomic_load_int32(&event_counts->write_completed);

    if(rs != rc || ws != wc) {
        printf("FAIL: %s: %" PRId32 " reads started but %" PRId32 " completed, %" PRId32 " writes started but %" PRId32
               " completed.\n",
               what, rs, rc, ws, wc);
        return 1;
    }

    return 0;
}


static void callback(int32_t tag_id, int event, int status, void *userdata) {
    event_counts_t *event_counts = (event_counts_t *)userdata;

    (void)tag_id;
    (void)status;

    if(!event_counts) { return; }

    switch(event) {
        case PLCTAG_EVENT_READ_STARTED: compat_atomic_inc_int32(&event_counts->read_started); break;
        case PLCTAG_EVENT_READ_COMPLETED: compat_atomic_inc_int32(&event_counts->read_completed); break;
        case PLCTAG_EVENT_WRITE_STARTED: compat_atomic_inc_int32(&event_counts->write_started); break;
        case PLCTAG_EVENT_WRITE_COMPLETED: compat_atomic_inc_int32(&event_counts->write_completed); break;
        case PLCTAG_EVENT_ABORTED: compat_atomic_inc_int32(&event_counts->aborted); break;
        default: break;
    }
}
