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
 * Connection settings: the connection takes the latest value it is given, from any tag on it, at any time.
 *
 * The settings are connection_inactivity_timeout_ms, connection_retry_max_delay_ms and, on Modbus,
 * max_requests_in_flight.  A tag string sets them for the tag that creates the connection and for one that joins it;
 * plc_tag_set_int_attribute() sets them through any data tag or @connection tag on the connection.  Out of range is
 * clamped with a warning: create still succeeds, a set returns PLCTAG_ERR_OUT_OF_BOUNDS.
 *
 * Modes:
 *   --mode=functional (default)  join, set and @connection checks; with --max-requests, max_requests_in_flight too,
 *                                and reads in flight when it is lowered all finish
 *   --mode=idle-join             a joining tag's short idle timeout disconnects the connection
 *   --mode=retry-runtime         a retry cap set at runtime caps the reconnect waits (--tag points at a dead port)
 *   --mode=raise-limit           Modbus: max_requests_in_flight 1, then raised to 4.  Run against modbus_server
 *                                --delay-ms=10 --max-in-flight=4; its log must reach 4 in flight and never go over.
 *   --mode=lower-limit           Modbus: a connection created at 8 is lowered to 2.  Run against modbus_server
 *                                --delay-ms=10 --max-in-flight=2; its log must reach 2 and never go over.
 *
 * The client cannot see how many requests are on the wire, so the server counts them.
 *
 * Every check uses its own connection_group_id, so each starts from a fresh connection.
 */

#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define DATA_TIMEOUT_MS (5000)
#define BATCH_SIZE (16)
#define BATCH_TIMEOUT_MS (10000)
#define GET_FAILED (-1)
#define IDLE_JOIN_TIMEOUT_MS (1000)
#define IDLE_JOIN_WAIT_MS (4000)
#define RETRY_RUNTIME_CAP_MS (300)
#define RETRY_RUNTIME_WATCH_MS (4000)
#define RETRY_RUNTIME_MIN_RETRIES (5)

typedef struct {
    const char *name;
    int32_t min_value;
    int32_t max_value;
    int32_t default_value;
    int32_t in_range; /* a value other than the default */
} setting_t;

static const setting_t settings[] = {
    {"connection_inactivity_timeout_ms", 100, 30000, 30000, 5000},
    {"connection_retry_max_delay_ms", 100, 300000, 10000, 2000},
    {"max_requests_in_flight", 1, 16, 1, 4},
};

static const char *tag_string = NULL;
static const char *conn_tag_string = NULL;
static const char *mode = "functional";
static int32_t setting_count = 2; /* 3 with --max-requests */

/* filled by the logger callback, which runs on library threads. */
static compat_mutex_t log_mutex;
static int32_t warn_count[3];
static int32_t retry_count = 0;
static int32_t retry_max_wait_ms = 0;
static bool watch_retries = false;


static void usage(void);
static void parse_args(int argc, char **argv);
static void logger(int32_t tag_id, int debug_level, const char *message);
static void reset_log_state(void);
static void fail(const char *what);
static int32_t create_tag(const char *base, int32_t group_id, const char *extra);
static void expect_value(int32_t tag, const setting_t *s, int32_t want, const char *what);
static void expect_set(int32_t tag, const setting_t *s, int32_t value, int32_t want_rc, int32_t want_value, int32_t other);
static void build_settings(char *buf, size_t size, int32_t which); /* 0 in range, 1 low, 2 high */
static void check_join_applies(void);
static void check_join_without_values(void);
static void check_join_out_of_range(void);
static void check_setters(void);
static void check_conn_tag(void);
static void run_batch(int32_t *tags);
static void start_reads(int32_t *tags);
static void wait_reads(int32_t *tags);
static void check_max_requests_drain(void);
static void check_limit_batches(int32_t create_limit, int32_t first_limit, int32_t second_limit);
static void check_idle_join(void);
static void check_retry_runtime(void);


static void usage(void) {
    // NOLINTNEXTLINE
    fprintf(stderr, "Usage: test_conn_settings --tag=<data tag> [--conn-tag=<@connection tag>] [--max-requests]\n"
                    "                          [--mode=functional|idle-join|retry-runtime|raise-limit|lower-limit]\n");
    exit(1);
}


static void parse_args(int argc, char **argv) {
    for(int i = 1; i < argc; i++) {
        if(!strncmp(argv[i], "--tag=", 6)) {
            tag_string = argv[i] + 6;
        } else if(!strncmp(argv[i], "--conn-tag=", 11)) {
            conn_tag_string = argv[i] + 11;
        } else if(!strcmp(argv[i], "--max-requests")) {
            setting_count = 3;
        } else if(!strncmp(argv[i], "--mode=", 7)) {
            mode = argv[i] + 7;
        } else {
            // NOLINTNEXTLINE
            fprintf(stderr, "Unknown argument \"%s\"!\n", argv[i]);
            usage();
        }
    }

    if(!tag_string) { usage(); }
    if(!conn_tag_string && strcmp(mode, "idle-join") && strcmp(mode, "retry-runtime")) { usage(); }
}


/* counts the clamp warnings and the reconnect waits. */
static void logger(int32_t tag_id, int debug_level, const char *message) {
    const char *p = NULL;
    long long wait_ms = 0;

    (void)tag_id;
    (void)debug_level;

    compat_mutex_lock(&log_mutex);

    if(strstr(message, "must be between")) {
        for(int32_t i = 0; i < 3; i++) {
            if(strstr(message, settings[i].name)) { warn_count[i]++; }
        }
    }

    p = strstr(message, ", waiting ");
    if(watch_retries && p && strstr(message, "before reconnecting") && sscanf(p, ", waiting %lldms", &wait_ms) == 1) {
        /* the first wait may have been computed before the set; only later ones must obey it. */
        if(retry_count > 0 && wait_ms > retry_max_wait_ms) { retry_max_wait_ms = (int32_t)wait_ms; }
        retry_count++;
    }

    compat_mutex_unlock(&log_mutex);
}


static void reset_log_state(void) {
    compat_mutex_lock(&log_mutex);
    memset(warn_count, 0, sizeof(warn_count));
    retry_count = 0;
    retry_max_wait_ms = 0;
    compat_mutex_unlock(&log_mutex);
}


static void fail(const char *what) {
    // NOLINTNEXTLINE
    fprintf(stderr, "ERROR: %s\n", what);
    exit(1);
}


/* base plus its own connection group plus extra, created and ready. */
static int32_t create_tag(const char *base, int32_t group_id, const char *extra) {
    char buf[1024];
    int32_t tag = 0;

    // NOLINTNEXTLINE
    snprintf(buf, sizeof(buf), "%s&connection_group_id=%d%s", base, (int)group_id, extra);

    tag = plc_tag_create(buf, DATA_TIMEOUT_MS);
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: could not create \"%s\", %s!\n", buf, plc_tag_decode_error(tag));
        exit(1);
    }

    return tag;
}


static void expect_value(int32_t tag, const setting_t *s, int32_t want, const char *what) {
    int32_t got = plc_tag_get_int_attribute(tag, s->name, GET_FAILED);

    if(got != want) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: %s: %s is %d, expected %d.\n", what, s->name, (int)got, (int)want);
        exit(1);
    }
}


/* set through tag, check the status, and check the value through tag and through other. */
static void expect_set(int32_t tag, const setting_t *s, int32_t value, int32_t want_rc, int32_t want_value, int32_t other) {
    int32_t rc = plc_tag_set_int_attribute(tag, s->name, value);

    if(rc != want_rc) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: setting %s to %d returned %s, expected %s.\n", s->name, (int)value, plc_tag_decode_error(rc),
                plc_tag_decode_error(want_rc));
        exit(1);
    }

    expect_value(tag, s, want_value, "after a set, the tag that set it");
    expect_value(other, s, want_value, "after a set, another tag on the connection");
}


static void build_settings(char *buf, size_t size, int32_t which) {
    size_t used = 0;

    buf[0] = '\0';

    for(int32_t i = 0; i < setting_count; i++) {
        const setting_t *s = &settings[i];
        int32_t value = (which == 0) ? s->in_range : (which == 1) ? s->min_value - 1 : s->max_value + 1;

        // NOLINTNEXTLINE
        used += (size_t)snprintf(buf + used, size - used, "&%s=%d", s->name, (int)value);
    }
}


/* 1. a joining tag's values are applied to the connection. */
static void check_join_applies(void) {
    char extra[256];
    int32_t a = create_tag(tag_string, 201, "");
    int32_t b = 0;

    build_settings(extra, sizeof(extra), 0);
    b = create_tag(tag_string, 201, extra);

    for(int32_t i = 0; i < setting_count; i++) {
        expect_value(a, &settings[i], settings[i].in_range, "join applies, the creating tag");
        expect_value(b, &settings[i], settings[i].in_range, "join applies, the joining tag");
    }

    plc_tag_destroy(b);
    plc_tag_destroy(a);

    // NOLINTNEXTLINE
    fprintf(stderr, "PASS: a joining tag's values are applied to the connection.\n");
}


/* 2. a joining tag without values leaves the connection's values alone. */
static void check_join_without_values(void) {
    char extra[256];
    int32_t a = 0;
    int32_t b = 0;

    build_settings(extra, sizeof(extra), 0);
    a = create_tag(tag_string, 202, extra);
    b = create_tag(tag_string, 202, "");

    for(int32_t i = 0; i < setting_count; i++) {
        expect_value(a, &settings[i], settings[i].in_range, "join without values, the creating tag");
        expect_value(b, &settings[i], settings[i].in_range, "join without values, the joining tag");
    }

    plc_tag_destroy(b);
    plc_tag_destroy(a);

    // NOLINTNEXTLINE
    fprintf(stderr, "PASS: a joining tag without values leaves the connection's values alone.\n");
}


/* 3. out-of-range values on a join are clamped, with one warning each, and create succeeds. */
static void check_join_out_of_range(void) {
    char extra[256];
    int32_t a = create_tag(tag_string, 203, "");

    for(int32_t which = 1; which <= 2; which++) {
        int32_t b = 0;

        build_settings(extra, sizeof(extra), which);
        reset_log_state();
        b = create_tag(tag_string, 203, extra);

        for(int32_t i = 0; i < setting_count; i++) {
            int32_t want = (which == 1) ? settings[i].min_value : settings[i].max_value;

            expect_value(a, &settings[i], want, "join out of range, the creating tag");

            compat_mutex_lock(&log_mutex);
            int32_t warns = warn_count[i];
            compat_mutex_unlock(&log_mutex);

            if(warns != 1) {
                // NOLINTNEXTLINE
                fprintf(stderr, "ERROR: joining with %s out of range logged %d clamp warnings, expected 1.\n", settings[i].name,
                        (int)warns);
                exit(1);
            }
        }

        plc_tag_destroy(b);
    }

    plc_tag_destroy(a);

    // NOLINTNEXTLINE
    fprintf(stderr, "PASS: out-of-range values on a join are clamped with one warning each.\n");
}


/* 4. any data tag sets the values at runtime, clamped, and every tag on the connection sees them. */
static void check_setters(void) {
    int32_t a = create_tag(tag_string, 204, "");
    int32_t b = create_tag(tag_string, 204, "");

    for(int32_t i = 0; i < setting_count; i++) {
        const setting_t *s = &settings[i];

        expect_set(a, s, s->in_range, PLCTAG_STATUS_OK, s->in_range, b);
        expect_set(a, s, s->min_value - 1, PLCTAG_ERR_OUT_OF_BOUNDS, s->min_value, b);
        expect_set(a, s, s->max_value + 1, PLCTAG_ERR_OUT_OF_BOUNDS, s->max_value, b);
        expect_set(b, s, s->min_value, PLCTAG_STATUS_OK, s->min_value, a);
        expect_set(b, s, s->max_value, PLCTAG_STATUS_OK, s->max_value, a);
    }

    plc_tag_destroy(b);
    plc_tag_destroy(a);

    // NOLINTNEXTLINE
    fprintf(stderr, "PASS: the values are set at runtime through any tag, clamped at the bounds.\n");
}


/* 5. an @connection tag reads and sets the values, and its tag string applies them like a data tag's. */
static void check_conn_tag(void) {
    char extra[256];
    int32_t a = create_tag(tag_string, 205, "");
    int32_t c = create_tag(conn_tag_string, 205, "");
    int32_t d = 0;

    for(int32_t i = 0; i < setting_count; i++) {
        const setting_t *s = &settings[i];

        expect_value(c, s, s->default_value, "@connection tag, the default");
        expect_set(c, s, s->in_range, PLCTAG_STATUS_OK, s->in_range, a);
        expect_set(a, s, s->default_value, PLCTAG_STATUS_OK, s->default_value, c);
        expect_set(c, s, s->max_value + 1, PLCTAG_ERR_OUT_OF_BOUNDS, s->max_value, a);
    }

    build_settings(extra, sizeof(extra), 0);
    d = create_tag(conn_tag_string, 205, extra);

    for(int32_t i = 0; i < setting_count; i++) {
        expect_value(a, &settings[i], settings[i].in_range, "a joining @connection tag's values, the data tag");
    }

    plc_tag_destroy(d);
    plc_tag_destroy(c);
    plc_tag_destroy(a);

    // NOLINTNEXTLINE
    fprintf(stderr, "PASS: an @connection tag reads, sets and applies the values.\n");
}


static void start_reads(int32_t *tags) {
    for(int32_t i = 0; i < BATCH_SIZE; i++) {
        int32_t rc = plc_tag_read(tags[i], 0);

        if(rc != PLCTAG_STATUS_PENDING && rc != PLCTAG_STATUS_OK) {
            // NOLINTNEXTLINE
            fprintf(stderr, "ERROR: starting read %d returned %s!\n", (int)i, plc_tag_decode_error(rc));
            exit(1);
        }
    }
}


/* start a read on every tag and wait for all of them to finish OK. */
static void run_batch(int32_t *tags) {
    start_reads(tags);
    wait_reads(tags);
}


static void wait_reads(int32_t *tags) {
    int64_t end_ms = compat_time_ms() + BATCH_TIMEOUT_MS;
    bool pending = true;

    while(pending && compat_time_ms() < end_ms) {
        pending = false;

        for(int32_t i = 0; i < BATCH_SIZE; i++) {
            int32_t rc = plc_tag_status(tags[i]);

            if(rc == PLCTAG_STATUS_PENDING) {
                pending = true;
            } else if(rc != PLCTAG_STATUS_OK) {
                // NOLINTNEXTLINE
                fprintf(stderr, "ERROR: read %d finished with %s!\n", (int)i, plc_tag_decode_error(rc));
                exit(1);
            }
        }

        if(pending) { compat_sleep_ms(1, NULL); }
    }

    if(pending) { fail("a batch of reads did not finish in time."); }
}


/*
 * 6. Modbus: max_requests_in_flight lowered while requests are in flight.  Those requests are not cut off: they all
 * finish, and the connection goes on under the new limit.
 */
static void check_max_requests_drain(void) {
    int32_t tags[BATCH_SIZE];
    int32_t c = create_tag(conn_tag_string, 206, "&max_requests_in_flight=8");

    for(int32_t i = 0; i < BATCH_SIZE; i++) { tags[i] = create_tag(tag_string, 206, ""); }

    start_reads(tags);
    if(plc_tag_set_int_attribute(c, "max_requests_in_flight", 2) != PLCTAG_STATUS_OK) { fail("could not lower the limit to 2."); }
    wait_reads(tags);
    run_batch(tags); /* and the connection goes on under the new limit */

    expect_value(tags[0], &settings[2], 2, "after lowering with reads in flight");

    for(int32_t i = 0; i < BATCH_SIZE; i++) { plc_tag_destroy(tags[i]); }
    plc_tag_destroy(c);

    // NOLINTNEXTLINE
    fprintf(stderr, "PASS: reads in flight when max_requests_in_flight is lowered all finish.\n");
}


/*
 * Modbus, against a server that counts requests in flight: a connection created at create_limit runs a batch at
 * first_limit, then one at second_limit, each set through an @connection tag.  A limit of 0 skips that batch.  The
 * handler applies a new limit between passes, so each batch starts a moment after its set.
 */
static void check_limit_batches(int32_t create_limit, int32_t first_limit, int32_t second_limit) {
    int32_t tags[BATCH_SIZE];
    int32_t limits[2] = {first_limit, second_limit};
    char extra[64];
    int32_t c = 0;

    // NOLINTNEXTLINE
    snprintf(extra, sizeof(extra), "&max_requests_in_flight=%d", (int)create_limit);
    c = create_tag(conn_tag_string, 209, extra);

    for(int32_t i = 0; i < BATCH_SIZE; i++) { tags[i] = create_tag(tag_string, 209, ""); }

    for(int32_t i = 0; i < 2; i++) {
        if(limits[i] == 0) { continue; }

        if(plc_tag_set_int_attribute(c, "max_requests_in_flight", limits[i]) != PLCTAG_STATUS_OK) {
            fail("could not set max_requests_in_flight.");
        }

        compat_sleep_ms(100, NULL);

        run_batch(tags);

        // NOLINTNEXTLINE
        fprintf(stderr, "Ran %d reads with max_requests_in_flight %d.\n", BATCH_SIZE, (int)limits[i]);
    }

    for(int32_t i = 0; i < BATCH_SIZE; i++) { plc_tag_destroy(tags[i]); }
    plc_tag_destroy(c);

    // NOLINTNEXTLINE
    fprintf(stderr, "PASS: the batches finished; the server log says how many requests were in flight.\n");
}


/* timing: a joining tag's short idle timeout disconnects the connection the creating tag opened. */
static void check_idle_join(void) {
    char extra[64];
    int32_t a = create_tag(tag_string, 207, "");
    int32_t b = 0;
    int32_t status = 0;

    if(plc_tag_read(a, DATA_TIMEOUT_MS) != PLCTAG_STATUS_OK) { fail("the first read failed."); }

    // NOLINTNEXTLINE
    snprintf(extra, sizeof(extra), "&connection_inactivity_timeout_ms=%d", IDLE_JOIN_TIMEOUT_MS);
    b = create_tag(tag_string, 207, extra);

    compat_sleep_ms(IDLE_JOIN_WAIT_MS, NULL);

    status = plc_tag_get_int_attribute(a, "connection_status", GET_FAILED);
    if(status != PLCTAG_CONN_STATUS_IDLE_WAIT) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: %d ms after a tag joined with a %d ms idle timeout, the connection status is %d, expected %d.\n",
                IDLE_JOIN_WAIT_MS, IDLE_JOIN_TIMEOUT_MS, (int)status, (int)PLCTAG_CONN_STATUS_IDLE_WAIT);
        exit(1);
    }

    plc_tag_destroy(b);
    plc_tag_destroy(a);

    // NOLINTNEXTLINE
    fprintf(stderr, "PASS: a joining tag's idle timeout disconnects the connection.\n");
}


/* timing: a retry cap set at runtime caps the reconnect waits.  The default cap would let them grow past it. */
static void check_retry_runtime(void) {
    int32_t tag = 0;
    int32_t rc = 0;
    int32_t count = 0;
    int32_t max_wait = 0;
    char buf[1024];

    // NOLINTNEXTLINE
    snprintf(buf, sizeof(buf), "%s&connection_group_id=208", tag_string);

    plc_tag_set_debug_level(PLCTAG_DEBUG_INFO);

    tag = plc_tag_create(buf, 0);
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: could not create the tag, %s!\n", plc_tag_decode_error(tag));
        exit(1);
    }

    reset_log_state();
    compat_mutex_lock(&log_mutex);
    watch_retries = true;
    compat_mutex_unlock(&log_mutex);

    rc = plc_tag_set_int_attribute(tag, "connection_retry_max_delay_ms", RETRY_RUNTIME_CAP_MS);
    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: setting the retry cap returned %s!\n", plc_tag_decode_error(rc));
        exit(1);
    }

    compat_sleep_ms(RETRY_RUNTIME_WATCH_MS, NULL);

    compat_mutex_lock(&log_mutex);
    watch_retries = false;
    count = retry_count;
    max_wait = retry_max_wait_ms;
    compat_mutex_unlock(&log_mutex);

    plc_tag_destroy(tag);
    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);

    // NOLINTNEXTLINE
    fprintf(stderr, "%d reconnect waits in %d ms, the longest after the first %d ms.\n", (int)count, RETRY_RUNTIME_WATCH_MS,
            (int)max_wait);

    if(count < RETRY_RUNTIME_MIN_RETRIES) { fail("too few reconnect attempts for a 300 ms cap."); }
    if(max_wait > RETRY_RUNTIME_CAP_MS) { fail("a reconnect wait exceeded the cap set at runtime."); }

    // NOLINTNEXTLINE
    fprintf(stderr, "PASS: a retry cap set at runtime caps the reconnect waits.\n");
}


int main(int argc, char **argv) {
    parse_args(argc, argv);

    compat_mutex_init(&log_mutex);
    plc_tag_register_logger(logger);
    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);

    if(!strcmp(mode, "functional")) {
        check_join_applies();
        check_join_without_values();
        check_join_out_of_range();
        check_setters();
        check_conn_tag();
        if(setting_count == 3) { check_max_requests_drain(); }
    } else if(!strcmp(mode, "idle-join")) {
        check_idle_join();
    } else if(!strcmp(mode, "retry-runtime")) {
        check_retry_runtime();
    } else if(!strcmp(mode, "raise-limit")) {
        check_limit_batches(1, 1, 4);
    } else if(!strcmp(mode, "lower-limit")) {
        check_limit_batches(8, 0, 2);
    } else {
        usage();
    }

    plc_tag_shutdown();

    // NOLINTNEXTLINE
    fprintf(stderr, "All tests PASSED!\n");

    return 0;
}
