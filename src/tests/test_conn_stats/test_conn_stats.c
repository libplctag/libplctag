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
 * The connection statistic attributes on @connection tags.  Every attribute is checked by at
 * least one mode:
 *
 *   basic      data tags refuse the statistics; attribute sizes; connect_count,
 *              connected_time_ms and connection_age_ms grow; tag_count follows creates and
 *              destroys; N reads move every traffic counter and its "since connect" value
 *              together; response times are ordered; nothing failed, timed out or waits.
 *   idle       an inactivity disconnect counts in idle_disconnect_count and stops
 *              connected_time_ms; the read after it reconnects, so connect_count grows and the
 *              "since connect" values restart below their totals.
 *   dead       --conn-tag names a port nobody listens on: connect_failure_count grows,
 *              retry_delay_ms is set and retry_wait_left_ms counts down inside it.
 *   in-flight  the server delays its replies: requests_in_flight is 1 or more during a read
 *              and 0 after it.
 *   errors     --error-attrs makes a tag the device answers with an error status:
 *              error_responses grows.
 *   timeout    the server leaves the first request unanswered (ab_server --no_reply=1):
 *              request_timeouts grows and the request goes through after a reconnect.
 *
 * --forward-open (connected AB only) adds the Forward Open attributes: in basic,
 * forward_open_size, connection_id_ot and connection_id_to are set; in idle, they are 0 while
 * disconnected and the reconnect brings a new T->O connection ID.
 *
 * Usage:
 *   test_conn_stats --mode=<mode> --tag=<data tag string> [--forward-open]
 *                   [--conn-tag=<@connection tag string>] [--error-attrs=<attributes>]
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REQUIRED_VERSION 2, 7, 3

#define DATA_TIMEOUT (5000)
#define NUM_READS (10)
#define IDLE_TIMEOUT_MS (500)
#define POLL_MS (10)
#define MISSING (-12345)

static int32_t failures = 0;
static bool forward_open = false;


static void check(bool ok, const char *what) {
    if(ok) {
        printf("\tOK: %s\n", what);
    } else {
        printf("\tFAIL: %s\n", what);
        failures++;
    }
}


static int64_t get64(int32_t tag, const char *name) { return plc_tag_get_int64_attribute(tag, name, MISSING); }


static int32_t get32(int32_t tag, const char *name) { return plc_tag_get_int_attribute(tag, name, MISSING); }


static void show(int32_t conn) {
    printf("\tconnect %d, failures %d, idle %d, tags %d, in flight %d, requests %" PRId64 "/%" PRId64 ", replies %" PRId64
           "/%" PRId64 ", ops %" PRId64 "/%" PRId64 ", errors %" PRId64 ", timeouts %" PRId64 ", rtt %d/%d/%d/%d us\n",
           get32(conn, "connect_count"), get32(conn, "connect_failure_count"), get32(conn, "idle_disconnect_count"),
           get32(conn, "tag_count"), get32(conn, "requests_in_flight"), get64(conn, "requests_sent_since_connect"),
           get64(conn, "requests_sent"), get64(conn, "responses_received_since_connect"), get64(conn, "responses_received"),
           get64(conn, "tag_ops_completed_since_connect"), get64(conn, "tag_ops_completed"), get64(conn, "error_responses"),
           get64(conn, "request_timeouts"), get32(conn, "response_time_min_us"), get32(conn, "response_time_avg_us"),
           get32(conn, "response_time_max_us"), get32(conn, "response_time_us"));
}


/* poll an integer attribute until it equals want or the time runs out. */
static bool wait_for32(int32_t tag, const char *name, int32_t want, int32_t timeout_ms) {
    int64_t end = compat_time_ms() + timeout_ms;

    while(get32(tag, name) != want) {
        if(compat_time_ms() > end) { return false; }
        compat_sleep_ms(POLL_MS, NULL);
    }

    return true;
}


static int32_t open_conn_from(int32_t data_tag) {
    int32_t conn = plc_tag_create_from_tag(data_tag, "name=@connection", NULL, NULL, DATA_TIMEOUT);

    if(conn < 0) { printf("\tFAIL: @connection create returned %s.\n", plc_tag_decode_error(conn)); }

    return conn;
}


/* the traffic totals and their "since connect" values. */
typedef struct {
    int64_t total;
    int64_t since;
} pair_t;

static const char *traffic[] = {"requests_sent", "responses_received", "bytes_sent", "bytes_received", "tag_ops_completed"};
#define NUM_TRAFFIC (sizeof(traffic) / sizeof(traffic[0]))


static void snapshot(int32_t conn, pair_t *out) {
    char name[64];

    for(size_t i = 0; i < NUM_TRAFFIC; i++) {
        compat_snprintf(name, sizeof(name), "%s_since_connect", traffic[i]);
        out[i].since = get64(conn, name);
        out[i].total = get64(conn, traffic[i]);
    }
}


static int32_t run_basic(int32_t data_tag, const char *tag_string) {
    int32_t conn = open_conn_from(data_tag);
    int32_t extra = 0;
    pair_t before[NUM_TRAFFIC];
    pair_t after[NUM_TRAFFIC];
    int64_t connected = 0;
    int64_t age = 0;
    char what[128];

    if(conn < 0) { return 1; }

    printf("A data tag has no statistics.\n");
    check(get64(data_tag, "requests_sent") == MISSING, "requests_sent on a data tag returns the default");
    check(get32(data_tag, "connect_count") == MISSING, "connect_count on a data tag returns the default");

    printf("Attribute sizes.\n");
    check(plc_tag_get_attribute_size(conn, "requests_sent") == 8, "a 64-bit statistic is 8 bytes");
    check(plc_tag_get_attribute_size(conn, "connect_count") == 4, "a 32-bit statistic is 4 bytes");
    check(get32(conn, "requests_sent") == MISSING, "the 32-bit getter refuses a 64-bit statistic");

    printf("Connected.\n");
    compat_sleep_ms(50, NULL);
    check(get32(conn, "connect_count") == 1, "connect_count is 1");
    connected = get64(conn, "connected_time_ms");
    age = get64(conn, "connection_age_ms");
    check(connected > 0, "connected_time_ms > 0");
    check(age >= connected, "connection_age_ms >= connected_time_ms");

    compat_sleep_ms(200, NULL);
    check(get64(conn, "connected_time_ms") >= connected + 150, "connected_time_ms grows with time");
    check(get64(conn, "connection_age_ms") >= age + 150, "connection_age_ms grows with time");

    printf("tag_count follows the tags on the connection.\n");
    check(get32(conn, "tag_count") == 2, "tag_count is 2: the data tag and the @connection tag");
    extra = plc_tag_create(tag_string, DATA_TIMEOUT);
    check(extra > 0 && get32(conn, "tag_count") == 3, "tag_count is 3 with another data tag");
    if(extra > 0) { plc_tag_destroy(extra); }
    /* the library frees a destroyed tag on its own thread, so the count drops a moment later. */
    check(wait_for32(conn, "tag_count", 2, 1000), "tag_count is 2 again after its destroy");

    printf("%d reads move the traffic counters.\n", NUM_READS);
    snapshot(conn, before);
    for(int32_t i = 0; i < NUM_READS; i++) {
        int32_t rc = plc_tag_read(data_tag, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            printf("\tFAIL: read %d returned %s.\n", (int)i, plc_tag_decode_error(rc));
            failures++;
        }
    }
    snapshot(conn, after);

    for(size_t i = 0; i < NUM_TRAFFIC; i++) {
        int64_t grew = after[i].total - before[i].total;
        bool is_bytes = (strncmp(traffic[i], "bytes", 5) == 0);

        compat_snprintf(what, sizeof(what), "%s grew by %" PRId64 ", at least %d", traffic[i], grew, is_bytes ? 1 : NUM_READS);
        check(grew >= (is_bytes ? 1 : NUM_READS), what);

        compat_snprintf(what, sizeof(what), "%s_since_connect grew by the same, and is <= the total", traffic[i]);
        check(after[i].since - before[i].since == grew && after[i].since <= after[i].total && after[i].since > 0, what);
    }

    printf("Response times.\n");
    {
        int32_t last = get32(conn, "response_time_us");
        int32_t avg = get32(conn, "response_time_avg_us");
        int32_t min = get32(conn, "response_time_min_us");
        int32_t max = get32(conn, "response_time_max_us");

        compat_snprintf(what, sizeof(what), "0 < min %d <= avg %d <= max %d, and min <= last %d <= max", (int)min, (int)avg,
                        (int)max, (int)last);
        check(min > 0 && min <= avg && avg <= max && min <= last && last <= max, what);
    }

    if(forward_open) {
        int32_t size = get32(conn, "forward_open_size");

        printf("The Forward Open.\n");
        compat_snprintf(what, sizeof(what), "forward_open_size %d > 0", (int)size);
        check(size > 0, what);
        check(get64(conn, "connection_id_ot") > 0, "connection_id_ot is set");
        check(get64(conn, "connection_id_to") > 0, "connection_id_to is set");
        check(plc_tag_get_attribute_size(conn, "connection_id_ot") == 8, "a connection ID is 8 bytes");
    }

    printf("Nothing failed, timed out or waits.\n");
    /* Modbus publishes the count once per handler pass, just after the read completes. */
    check(wait_for32(conn, "requests_in_flight", 0, 1000), "requests_in_flight is 0 when idle");
    check(get64(conn, "request_timeouts") == 0, "request_timeouts is 0");
    check(get64(conn, "error_responses") == 0, "error_responses is 0");
    check(get32(conn, "connect_failure_count") == 0, "connect_failure_count is 0");
    check(get32(conn, "idle_disconnect_count") == 0, "idle_disconnect_count is 0");
    check(get32(conn, "retry_delay_ms") == 0, "retry_delay_ms is 0");
    check(get32(conn, "retry_wait_left_ms") == 0, "retry_wait_left_ms is 0");

    show(conn);
    plc_tag_destroy(conn);

    return 0;
}


static int32_t run_idle(int32_t data_tag) {
    int32_t conn = open_conn_from(data_tag);
    pair_t after[NUM_TRAFFIC];
    int64_t first_id_to = get64(conn, "connection_id_to");
    char what[128];

    if(conn < 0) { return 1; }

    check(plc_tag_set_int_attribute(conn, "connection_inactivity_timeout_ms", IDLE_TIMEOUT_MS) == PLCTAG_STATUS_OK,
          "set a short inactivity timeout");

    printf("An inactivity disconnect.\n");
    check(wait_for32(conn, "connection_status", PLCTAG_CONN_STATUS_IDLE_WAIT, IDLE_TIMEOUT_MS * 10),
          "the connection goes to IDLE_WAIT");
    check(get32(conn, "idle_disconnect_count") == 1, "idle_disconnect_count is 1");
    check(get64(conn, "connected_time_ms") == 0, "connected_time_ms is 0 while disconnected");
    check(get32(conn, "connect_count") == 1, "connect_count is still 1");

    if(forward_open) {
        check(get32(conn, "forward_open_size") == 0, "forward_open_size is 0 while disconnected");
        check(get64(conn, "connection_id_ot") == 0 && get64(conn, "connection_id_to") == 0,
              "the connection IDs are 0 while disconnected");
    }

    printf("A read reconnects.\n");
    check(plc_tag_read(data_tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "the read succeeds");
    check(get32(conn, "connect_count") == 2, "connect_count is 2");
    check(get64(conn, "connected_time_ms") >= 0 && get32(conn, "connection_status") == PLCTAG_CONN_STATUS_UP,
          "connected again");

    if(forward_open) {
        int64_t id_to = get64(conn, "connection_id_to");

        check(get32(conn, "forward_open_size") > 0, "forward_open_size is set again");
        compat_snprintf(what, sizeof(what), "the new connection_id_to %" PRId64 " differs from the first, %" PRId64, id_to,
                        first_id_to);
        check(first_id_to > 0 && id_to > 0 && id_to != first_id_to, what);
    }

    snapshot(conn, after);
    for(size_t i = 0; i < NUM_TRAFFIC; i++) {
        compat_snprintf(what, sizeof(what), "%s_since_connect %" PRId64 " restarted below the total %" PRId64, traffic[i],
                        after[i].since, after[i].total);
        check(after[i].since >= 0 && after[i].since < after[i].total, what);
    }

    show(conn);
    plc_tag_destroy(conn);

    return 0;
}


static int32_t run_dead(const char *conn_string) {
    int32_t conn = 0;
    int32_t delay = 0;
    bool counting_down = false;
    char what[128];

    if(!conn_string) {
        printf("FAIL: --mode=dead needs --conn-tag.\n");
        return 1;
    }

    conn = plc_tag_create(conn_string, DATA_TIMEOUT);
    if(conn < 0) {
        printf("FAIL: @connection create returned %s.\n", plc_tag_decode_error(conn));
        return 1;
    }

    printf("Connecting to a closed port fails.\n");
    check(wait_for32(conn, "connection_status", PLCTAG_CONN_STATUS_ERR_WAIT, 10000), "the connection goes to ERR_WAIT");
    check(get32(conn, "connect_failure_count") >= 1, "connect_failure_count >= 1");
    check(get32(conn, "connect_count") == 0, "connect_count is 0");
    check(get64(conn, "connected_time_ms") == 0, "connected_time_ms is 0");

    delay = get32(conn, "retry_delay_ms");
    compat_snprintf(what, sizeof(what), "retry_delay_ms %d > 0", (int)delay);
    check(delay > 0, what);

    /* sample a few times: the wait left is inside the delay, and counts down while it waits. */
    for(int32_t i = 0; i < 5 && get32(conn, "connection_status") == PLCTAG_CONN_STATUS_ERR_WAIT; i++) {
        int32_t left = get32(conn, "retry_wait_left_ms");

        if(left < 0 || left > get32(conn, "retry_delay_ms")) {
            printf("\tFAIL: retry_wait_left_ms %d is outside the delay %d.\n", (int)left, (int)get32(conn, "retry_delay_ms"));
            failures++;
        }

        if(left > 0) { counting_down = true; }

        compat_sleep_ms(POLL_MS, NULL);
    }
    check(counting_down, "retry_wait_left_ms was above 0 during the wait");

    show(conn);
    plc_tag_destroy(conn);

    return 0;
}


static int32_t run_in_flight(int32_t data_tag) {
    int32_t conn = open_conn_from(data_tag);
    int32_t most = 0;
    int32_t rc = 0;
    int64_t end = 0;

    if(conn < 0) { return 1; }

    printf("A read against a slow server.\n");
    rc = plc_tag_read(data_tag, 0);
    check(rc == PLCTAG_STATUS_PENDING, "the read is pending");

    end = compat_time_ms() + DATA_TIMEOUT;
    while(plc_tag_status(data_tag) == PLCTAG_STATUS_PENDING && compat_time_ms() < end) {
        int32_t now = get32(conn, "requests_in_flight");
        if(now > most) { most = now; }
        compat_sleep_ms(1, NULL);
    }

    check(plc_tag_status(data_tag) == PLCTAG_STATUS_OK, "the read completed");
    check(most >= 1, "requests_in_flight was 1 or more during the read");
    check(wait_for32(conn, "requests_in_flight", 0, 1000), "requests_in_flight is 0 after it");

    show(conn);
    plc_tag_destroy(conn);

    return 0;
}


static int32_t run_errors(int32_t data_tag, const char *error_attrs) {
    int32_t conn = open_conn_from(data_tag);
    int64_t before = 0;
    int32_t bad = 0;
    char what[128];

    if(conn < 0) { return 1; }

    if(!error_attrs) {
        printf("FAIL: --mode=errors needs --error-attrs.\n");
        return 1;
    }

    printf("A tag the device answers with an error.\n");
    before = get64(conn, "error_responses");

    /* the create may read the tag and fail, or succeed and leave the failure to the read. */
    bad = plc_tag_create_from_tag(data_tag, error_attrs, NULL, NULL, DATA_TIMEOUT);
    if(bad > 0) {
        int32_t rc = plc_tag_read(bad, DATA_TIMEOUT);

        compat_snprintf(what, sizeof(what), "the read fails, %s", plc_tag_decode_error(rc));
        check(rc != PLCTAG_STATUS_OK, what);
        plc_tag_destroy(bad);
    } else {
        printf("\tthe create failed with %s.\n", plc_tag_decode_error(bad));
    }

    compat_snprintf(what, sizeof(what), "error_responses grew from %" PRId64 " to %" PRId64, before,
                    get64(conn, "error_responses"));
    check(get64(conn, "error_responses") > before, what);

    show(conn);
    plc_tag_destroy(conn);

    return 0;
}


static int32_t run_timeout(int32_t data_tag) {
    int32_t conn = open_conn_from(data_tag);

    if(conn < 0) { return 1; }

    /* the data tag's create sent the request the server left unanswered. */
    printf("A request with no reply.\n");
    check(get64(conn, "request_timeouts") >= 1, "request_timeouts >= 1");
    check(get32(conn, "connect_count") >= 2, "connect_count >= 2: the timeout dropped the connection");
    check(plc_tag_read(data_tag, DATA_TIMEOUT) == PLCTAG_STATUS_OK, "a read now succeeds");

    show(conn);
    plc_tag_destroy(conn);

    return 0;
}


int main(int argc, char **argv) {
    const char *mode = NULL;
    const char *tag_string = NULL;
    const char *conn_string = NULL;
    const char *error_attrs = NULL;
    int32_t data_tag = 0;
    int32_t rc = 0;

    for(int i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--mode=", 7) == 0) {
            mode = argv[i] + 7;
        } else if(strncmp(argv[i], "--tag=", 6) == 0) {
            tag_string = argv[i] + 6;
        } else if(strncmp(argv[i], "--conn-tag=", 11) == 0) {
            conn_string = argv[i] + 11;
        } else if(strncmp(argv[i], "--error-attrs=", 14) == 0) {
            error_attrs = argv[i] + 14;
        } else if(strcmp(argv[i], "--forward-open") == 0) {
            forward_open = true;
        }
    }

    if(!mode || (!tag_string && strcmp(mode, "dead") != 0)) {
        printf("Usage: test_conn_stats --mode=<basic|idle|dead|in-flight|errors|timeout> --tag=<data tag string>\n"
               "                       [--forward-open] [--conn-tag=<@connection tag string>] [--error-attrs=<attributes>]\n");
        return 1;
    }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    if(strcmp(mode, "dead") == 0) {
        rc = run_dead(conn_string);
    } else {
        /* timeout mode: the server's first unanswered request costs a 2 s wait and a reconnect. */
        data_tag = plc_tag_create(tag_string, (strcmp(mode, "timeout") == 0) ? DATA_TIMEOUT * 2 : DATA_TIMEOUT);
        if(data_tag < 0) {
            printf("FAIL: data tag create returned %s.\n", plc_tag_decode_error(data_tag));
            return 1;
        }

        if(strcmp(mode, "basic") == 0) {
            rc = run_basic(data_tag, tag_string);
        } else if(strcmp(mode, "idle") == 0) {
            rc = run_idle(data_tag);
        } else if(strcmp(mode, "in-flight") == 0) {
            rc = run_in_flight(data_tag);
        } else if(strcmp(mode, "errors") == 0) {
            rc = run_errors(data_tag, error_attrs);
        } else if(strcmp(mode, "timeout") == 0) {
            rc = run_timeout(data_tag);
        } else {
            printf("FAIL: unknown mode \"%s\".\n", mode);
            rc = 1;
        }

        plc_tag_destroy(data_tag);
    }

    plc_tag_shutdown();

    failures += rc;
    printf("%s: %s with %d failure(s).\n", mode, failures ? "FAILED" : "PASSED", (int)failures);

    return failures ? 1 : 0;
}
