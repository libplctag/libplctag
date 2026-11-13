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
 * test_modbus_split_write
 *
 * Writes that the library must split over more than one Modbus request reach every register
 * (issue #686).  A write request carries at most 123 holding registers or 1968 coils.
 *
 * One read tag covers all --total registers from address 0.  For each size N from --first to
 * --last, a write tag of N registers from address 0 is created and written, then the read tag
 * reads everything back.  Exactly the first N registers must hold the new value and every
 * other register must be unchanged.
 *
 *   hr   each write of N registers writes the value N, so the values increase from one size
 *        to the next and the number of registers holding N is the number written.
 *   co   each size writes all ones, checks, then writes all zeros and checks again, so a
 *        dropped coil shows whatever its state was before.
 *
 * Defaults: hr --total=140 --first=120 --last=130, co --total=2000 --first=1963 --last=1973.
 *
 * --auto: the write tag uses auto_sync_write_ms and the read tag auto_sync_read_ms, so both
 * the auto write and the auto read are split.  The test waits for the auto write to complete
 * and for two auto reads after it, and checks that every split operation raised exactly one
 * STARTED and one COMPLETED event.
 *
 * Usage: test_modbus_split_write --type=<hr|co> --base="<tag string without name or elem_count>"
 *                                [--total=N] [--first=N] [--last=N] [--auto]
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 6, 0
#define DATA_TIMEOUT (10000)
#define ATTR_LEN (512)
#define MAX_TOTAL (2000)
#define MAX_MISMATCHES_SHOWN (10)
#define AUTO_READ_MS (50)
#define AUTO_WRITE_MS (20)
#define AUTO_WAIT_POLL_MS (5)

typedef enum { REG_HOLDING, REG_COIL } reg_kind_t;

static int32_t run_test(const char *base, reg_kind_t kind, int32_t total, int32_t first, int32_t last);
static int32_t write_and_check(int32_t write_id, int32_t read_id, reg_kind_t kind, int32_t total, int32_t count,
                               int32_t value);
static int32_t create_tag(const char *base, reg_kind_t kind, int32_t count, bool is_read_tag);
static int32_t read_all(int32_t read_id, reg_kind_t kind, int32_t total, int32_t *values);
static int32_t check_values(reg_kind_t kind, int32_t total, int32_t count, int32_t value);
static void tag_callback(int32_t tag_id, int event, int status, void *userdata);
static int32_t wait_for_count(compat_atomic_int32_t *counter, int32_t target, const char *what);
static int32_t check_balance(const char *what, compat_atomic_int32_t *started, compat_atomic_int32_t *completed,
                             int32_t max_in_flight);

static int32_t expected[MAX_TOTAL];
static int32_t actual[MAX_TOTAL];

static bool auto_mode = false;
static int32_t read_tag_role = 0; /* its address marks the read tag's callbacks. */
static compat_atomic_int32_t read_started;
static compat_atomic_int32_t read_completed;
static compat_atomic_int32_t write_started;
static compat_atomic_int32_t write_completed;


int main(int argc, char **argv) {
    const char *type = NULL;
    const char *base = NULL;
    reg_kind_t kind = REG_HOLDING;
    int32_t total = -1;
    int32_t first = -1;
    int32_t last = -1;
    int32_t failures = 0;

    for(int32_t i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--type=", 7) == 0) {
            type = argv[i] + 7;
        } else if(strncmp(argv[i], "--base=", 7) == 0) {
            base = argv[i] + 7;
        } else if(strncmp(argv[i], "--total=", 8) == 0) {
            total = (int32_t)atoi(argv[i] + 8);
        } else if(strncmp(argv[i], "--first=", 8) == 0) {
            first = (int32_t)atoi(argv[i] + 8);
        } else if(strncmp(argv[i], "--last=", 7) == 0) {
            last = (int32_t)atoi(argv[i] + 7);
        } else if(strcmp(argv[i], "--auto") == 0) {
            auto_mode = true;
        }
    }

    if(type && strcmp(type, "hr") == 0) {
        kind = REG_HOLDING;
        if(total < 0) { total = 140; }
        if(first < 0) { first = 120; }
        if(last < 0) { last = 130; }
    } else if(type && strcmp(type, "co") == 0) {
        kind = REG_COIL;
        if(total < 0) { total = 2000; }
        if(first < 0) { first = 1963; }
        if(last < 0) { last = 1973; }
    } else {
        type = NULL;
    }

    if(!type || !base || total < 1 || total > MAX_TOTAL || first < 1 || first > last || last > total) {
        printf("Usage: test_modbus_split_write --type=<hr|co> --base=\"<tag string>\" "
               "[--total=1..%d] [--first=N] [--last=N] [--auto]  (1 <= first <= last <= total)\n",
               MAX_TOTAL);
        return 1;
    }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    failures = run_test(base, kind, total, first, last);

    if(failures) {
        printf("FAILURE: %" PRId32 " write size(s) did not reach every register.\n", failures);
        return 1;
    }

    printf("SUCCESS: every %swrite from %" PRId32 " to %" PRId32 " %s reached every register.\n",
           (auto_mode ? "auto " : ""), first, last, (kind == REG_HOLDING ? "holding registers" : "coils"));
    return 0;
}


static int32_t run_test(const char *base, reg_kind_t kind, int32_t total, int32_t first, int32_t last) {
    int32_t failures = 0;
    int32_t read_id = create_tag(base, kind, total, true);

    if(read_id < 0) { return 1; }

    /* the starting state is whatever the server holds; every check is against this model. */
    if(read_all(read_id, kind, total, expected) != PLCTAG_STATUS_OK) {
        plc_tag_destroy(read_id);
        return 1;
    }

    for(int32_t count = first; count <= last; count++) {
        int32_t write_id = create_tag(base, kind, count, false);
        int32_t size_failed = 0;

        if(write_id < 0) {
            failures++;
            continue;
        }

        if(kind == REG_HOLDING) {
            size_failed = write_and_check(write_id, read_id, kind, total, count, count);
        } else {
            size_failed = write_and_check(write_id, read_id, kind, total, count, 1);
            if(!size_failed) { size_failed = write_and_check(write_id, read_id, kind, total, count, 0); }
        }

        failures += size_failed;
        plc_tag_destroy(write_id);
    }

    plc_tag_destroy(read_id);

    /* a destroy mid-read still completes the read, so every STARTED has its COMPLETED. */
    if(auto_mode) {
        failures += check_balance("auto read", &read_started, &read_completed, 0);
        failures += check_balance("auto write", &write_started, &write_completed, 0);
    }

    return failures;
}


/* returns 1 when the write or the read back failed, 0 when every register is as expected. */
static int32_t write_and_check(int32_t write_id, int32_t read_id, reg_kind_t kind, int32_t total, int32_t count,
                               int32_t value) {
    int32_t rc = PLCTAG_STATUS_OK;
    int32_t write_target = compat_atomic_load_int32(&write_completed) + 1;

    for(int32_t i = 0; i < count; i++) {
        if(kind == REG_HOLDING) {
            rc = plc_tag_set_uint16(write_id, i * 2, (uint16_t)value);
        } else {
            rc = plc_tag_set_bit(write_id, i, (int)value);
        }

        if(rc != PLCTAG_STATUS_OK) {
            printf("ERROR: setting element %" PRId32 " of the %" PRId32 "-register write tag: %s\n", i, count,
                   plc_tag_decode_error(rc));
            return 1;
        }
    }

    if(auto_mode) {
        /* the sets above marked the tag dirty; the auto write goes out AUTO_WRITE_MS later. */
        int64_t deadline = compat_time_ms() + DATA_TIMEOUT;

        if(wait_for_count(&write_completed, write_target, "auto write") != PLCTAG_STATUS_OK) { return 1; }

        /* sets that land after the write went out schedule one follow-up write; wait it out. */
        while((rc = plc_tag_status(write_id)) == PLCTAG_STATUS_PENDING && compat_time_ms() < deadline) {
            compat_sleep_ms(AUTO_WAIT_POLL_MS, NULL);
        }

        if(check_balance("auto write", &write_started, &write_completed, 0)) { return 1; }
        if(rc != PLCTAG_STATUS_OK) {
            printf("ERROR: auto write of %" PRId32 " registers: %s\n", count, plc_tag_decode_error(rc));
            return 1;
        }
    } else {
        rc = plc_tag_write(write_id, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            printf("ERROR: writing %" PRId32 " registers: %s\n", count, plc_tag_decode_error(rc));
            return 1;
        }
    }

    for(int32_t i = 0; i < count; i++) { expected[i] = value; }

    if(read_all(read_id, kind, total, actual) != PLCTAG_STATUS_OK) { return 1; }

    return check_values(kind, total, count, value);
}


static int32_t create_tag(const char *base, reg_kind_t kind, int32_t count, bool is_read_tag) {
    char attrs[ATTR_LEN];
    int32_t id = 0;

    compat_snprintf(attrs, sizeof(attrs), "%s&elem_count=%" PRId32 "&name=%s0", base, count,
                    (kind == REG_HOLDING ? "hr" : "co"));

    if(auto_mode) {
        size_t len = strlen(attrs);
        if(is_read_tag) {
            compat_snprintf(attrs + len, sizeof(attrs) - len, "&auto_sync_read_ms=%d", AUTO_READ_MS);
        } else {
            compat_snprintf(attrs + len, sizeof(attrs) - len, "&auto_sync_write_ms=%d", AUTO_WRITE_MS);
        }
        id = plc_tag_create_ex(attrs, tag_callback, (is_read_tag ? &read_tag_role : NULL), DATA_TIMEOUT);
    } else {
        id = plc_tag_create(attrs, DATA_TIMEOUT);
    }
    if(id < 0) {
        printf("ERROR: creating tag \"%s\": %s\n", attrs, plc_tag_decode_error(id));
        return -1;
    }

    return id;
}


static int32_t read_all(int32_t read_id, reg_kind_t kind, int32_t total, int32_t *values) {
    int32_t rc = PLCTAG_STATUS_OK;

    if(auto_mode) {
        /* the first read to complete may have started before the write landed; the second cannot. */
        int32_t target = compat_atomic_load_int32(&read_completed) + 2;
        rc = wait_for_count(&read_completed, target, "auto read");
        if(rc == PLCTAG_STATUS_OK && check_balance("auto read", &read_started, &read_completed, 1)) { rc = PLCTAG_ERR_BAD_STATUS; }
        if(rc == PLCTAG_STATUS_OK) { rc = plc_tag_lock(read_id); }
    } else {
        rc = plc_tag_read(read_id, DATA_TIMEOUT);
    }

    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: reading all %" PRId32 " registers back: %s\n", total, plc_tag_decode_error(rc));
        return rc;
    }

    for(int32_t i = 0; i < total; i++) {
        if(kind == REG_HOLDING) {
            values[i] = (int32_t)plc_tag_get_uint16(read_id, i * 2);
        } else {
            values[i] = (int32_t)plc_tag_get_bit(read_id, i);
        }
    }

    if(auto_mode) { plc_tag_unlock(read_id); }

    return PLCTAG_STATUS_OK;
}


static void tag_callback(int32_t tag_id, int event, int status, void *userdata) {
    (void)tag_id;
    (void)status;

    /* auto reads come only from the read tag, auto writes only from the write tags. */
    if(userdata == &read_tag_role) {
        if(event == PLCTAG_EVENT_READ_STARTED) { compat_atomic_inc_int32(&read_started); }
        if(event == PLCTAG_EVENT_READ_COMPLETED) { compat_atomic_inc_int32(&read_completed); }
    } else {
        if(event == PLCTAG_EVENT_WRITE_STARTED) { compat_atomic_inc_int32(&write_started); }
        if(event == PLCTAG_EVENT_WRITE_COMPLETED) { compat_atomic_inc_int32(&write_completed); }
    }
}


static int32_t wait_for_count(compat_atomic_int32_t *counter, int32_t target, const char *what) {
    int64_t deadline = compat_time_ms() + DATA_TIMEOUT;

    while(compat_atomic_load_int32(counter) < target) {
        if(compat_time_ms() > deadline) {
            printf("ERROR: timed out waiting for the %s to complete.\n", what);
            return PLCTAG_ERR_TIMEOUT;
        }
        compat_sleep_ms(AUTO_WAIT_POLL_MS, NULL);
    }

    return PLCTAG_STATUS_OK;
}


/* returns 1 when STARTED and COMPLETED events do not pair up, at most max_in_flight STARTED being open. */
static int32_t check_balance(const char *what, compat_atomic_int32_t *started, compat_atomic_int32_t *completed,
                             int32_t max_in_flight) {
    int32_t c = compat_atomic_load_int32(completed);
    int32_t s = compat_atomic_load_int32(started);
    int32_t open = s - c;

    if(open < 0 || open > max_in_flight) {
        printf("FAIL: %s raised %" PRId32 " STARTED and %" PRId32 " COMPLETED events.\n", what, s, c);
        return 1;
    }

    return 0;
}


/* compares the read back against the model and reports how many of the count registers got value. */
static int32_t check_values(reg_kind_t kind, int32_t total, int32_t count, int32_t value) {
    const char *unit = (kind == REG_HOLDING ? "registers" : "coils");
    int32_t written = 0;
    int32_t mismatches = 0;

    for(int32_t i = 0; i < count; i++) {
        if(actual[i] == value) { written++; }
    }

    for(int32_t i = 0; i < total; i++) {
        if(actual[i] == expected[i]) { continue; }

        if(mismatches < MAX_MISMATCHES_SHOWN) {
            printf("    %s %" PRId32 ": expected %" PRId32 ", got %" PRId32 "\n", unit, i, expected[i], actual[i]);
        }
        mismatches++;
    }

    if(mismatches) {
        printf("FAIL: wrote %" PRId32 " %s with value %" PRId32 ": %" PRId32 " of %" PRId32
               " hold it, %" PRId32 " of %" PRId32 " %s wrong.\n",
               count, unit, value, written, count, mismatches, total, unit);

        /* the next size starts from what the server really holds. */
        for(int32_t i = 0; i < total; i++) { expected[i] = actual[i]; }
        return 1;
    }

    printf("OK:   wrote %" PRId32 " %s with value %" PRId32 ": %" PRId32 " of %" PRId32 " hold it, the other %" PRId32
           " unchanged.\n",
           count, unit, value, written, count, total - count);
    return 0;
}
