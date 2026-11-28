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
 * Connection statistics.  See conn_stats.h for the threading rules: the IO thread is the only
 * writer of everything but tag_count, so it adds with a load and a store, not a locked add.
 */

#include <libplctag/lib/conn_stats.h>
#include <libplctag/lib/libplctag.h>
#include <utils/atomic_utils.h>
#include <utils/time_utils.h>


#define RESPONSE_TIME_EWMA_SHIFT (3) /* weight 1/8 */


static void add64(atomic_int64_t *counter, int64_t amount);


extern void conn_stats_init(conn_stats_t *stats) { atomic_init_int64(&stats->created_ms, time_ms()); }


/* every status change of the connection, from the IO thread. */
extern void conn_stats_status_changed(conn_stats_t *stats, int32_t old_status, int32_t new_status) {
    if(new_status == PLCTAG_CONN_STATUS_CONNECTING && old_status != PLCTAG_CONN_STATUS_CONNECTING) {
        stats->up_since_attempt = false;
    }

    if(new_status == PLCTAG_CONN_STATUS_UP) {
        /* baselines first: a reader takes the baseline, then the total. */
        atomic_set_int64(&stats->requests_sent_at_connect, atomic_get_int64(&stats->requests_sent));
        atomic_set_int64(&stats->responses_received_at_connect, atomic_get_int64(&stats->responses_received));
        atomic_set_int64(&stats->bytes_sent_at_connect, atomic_get_int64(&stats->bytes_sent));
        atomic_set_int64(&stats->bytes_received_at_connect, atomic_get_int64(&stats->bytes_received));
        atomic_set_int64(&stats->tag_ops_completed_at_connect, atomic_get_int64(&stats->tag_ops_completed));

        /* the response times describe this connection, so the average starts over with min and max. */
        atomic_set_int32(&stats->response_us_avg, 0);
        atomic_set_int32(&stats->response_us_min, 0);
        atomic_set_int32(&stats->response_us_max, 0);

        atomic_set_int32(&stats->connect_count, atomic_get_int32(&stats->connect_count) + 1);
        atomic_set_int64(&stats->connected_at_ms, time_ms());
        stats->up_since_attempt = true;
    } else if(old_status == PLCTAG_CONN_STATUS_UP) {
        atomic_set_int64(&stats->connected_at_ms, 0);
    }

    /* a wait after an attempt that never came up is a failed connect, not a drop. */
    if(new_status == PLCTAG_CONN_STATUS_ERR_WAIT && !stats->up_since_attempt) {
        atomic_set_int32(&stats->connect_failure_count, atomic_get_int32(&stats->connect_failure_count) + 1);
    }

    if(old_status == PLCTAG_CONN_STATUS_ERR_WAIT && new_status != PLCTAG_CONN_STATUS_ERR_WAIT) {
        atomic_set_int64(&stats->next_retry_at_ms, 0);
    }

    if(new_status == PLCTAG_CONN_STATUS_IDLE_WAIT) {
        atomic_set_int32(&stats->idle_disconnect_count, atomic_get_int32(&stats->idle_disconnect_count) + 1);
    }
}


/* the backoff wait before the next connect attempt. */
extern void conn_stats_retry_wait(conn_stats_t *stats, int64_t delay_ms) {
    atomic_set_int32(&stats->retry_delay_ms, (int32_t)delay_ms);
    atomic_set_int64(&stats->next_retry_at_ms, time_ms() + delay_ms);
}


extern void conn_stats_bytes_sent(conn_stats_t *stats, int32_t bytes) {
    if(bytes > 0) { add64(&stats->bytes_sent, bytes); }
}


extern void conn_stats_bytes_received(conn_stats_t *stats, int32_t bytes) {
    if(bytes > 0) { add64(&stats->bytes_received, bytes); }
}


extern void conn_stats_request_sent(conn_stats_t *stats) { add64(&stats->requests_sent, 1); }


extern void conn_stats_response_received(conn_stats_t *stats) { add64(&stats->responses_received, 1); }


extern void conn_stats_response_time(conn_stats_t *stats, int64_t response_us) {
    int32_t sample = (int32_t)((response_us < 0) ? 0 : ((response_us > INT32_MAX) ? INT32_MAX : response_us));
    int32_t avg = atomic_get_int32(&stats->response_us_avg);
    int32_t min = atomic_get_int32(&stats->response_us_min);

    atomic_set_int32(&stats->response_us_last, sample);

    /* the first sample is the average; after that each moves it 1/8 of the way. */
    avg = (avg == 0) ? sample : (int32_t)(avg + ((int64_t)sample - avg) / (1 << RESPONSE_TIME_EWMA_SHIFT));
    atomic_set_int32(&stats->response_us_avg, avg);

    if(min == 0 || sample < min) { atomic_set_int32(&stats->response_us_min, sample); }
    if(sample > atomic_get_int32(&stats->response_us_max)) { atomic_set_int32(&stats->response_us_max, sample); }
}


extern void conn_stats_tag_ops(conn_stats_t *stats, int32_t completed, int32_t errors) {
    if(completed > 0) { add64(&stats->tag_ops_completed, completed); }
    if(errors > 0) { add64(&stats->error_responses, errors); }
}


extern void conn_stats_request_timeout(conn_stats_t *stats) { add64(&stats->request_timeouts, 1); }


extern void conn_stats_requests_in_flight(conn_stats_t *stats, int32_t count) {
    atomic_set_int32(&stats->requests_in_flight, count);
}


/* API threads: a locked add, as several can create or destroy tags at once. */
extern void conn_stats_tag_added(conn_stats_t *stats) { atomic_add_int32(&stats->tag_count, 1); }


extern void conn_stats_tag_removed(conn_stats_t *stats) { atomic_add_int32(&stats->tag_count, -1); }


extern int64_t conn_stats_since_connect(atomic_int64_t *total, atomic_int64_t *baseline) {
    int64_t base = atomic_get_int64(baseline);
    int64_t value = atomic_get_int64(total) - base;

    return (value < 0) ? 0 : value;
}


/* one writer, so a load and a store; no locked read-modify-write. */
static void add64(atomic_int64_t *counter, int64_t amount) {
    atomic_set_int64(counter, atomic_get_int64(counter) + amount);
}
