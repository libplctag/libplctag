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


#pragma once


#include <stdbool.h>
#include <stdint.h>
#include <utils/atomic_utils.h>


/*
 * Statistics about one connection (a CIP session or a Modbus PLC), read as attributes on an
 * @connection tag.
 *
 * One writer: the connection's IO thread.  It publishes each value with an atomic store, so a
 * reader never blocks it and it never blocks a reader.  The exception is tag_count, which API
 * threads change as tags are created and destroyed, with an atomic add.
 *
 * "Since connect" values are a total minus a baseline stored at each connect, so no counter is
 * ever zeroed.
 */
struct conn_stats_t {
    atomic_int64_t created_ms;
    atomic_int64_t connected_at_ms;  /* 0 while not connected */
    atomic_int64_t next_retry_at_ms; /* 0 unless waiting to reconnect after an error */
    atomic_int32_t retry_delay_ms;
    atomic_int32_t connect_count;
    atomic_int32_t connect_failure_count;
    atomic_int32_t idle_disconnect_count;
    atomic_int32_t tag_count;

    atomic_int64_t requests_sent;
    atomic_int64_t responses_received;
    atomic_int64_t bytes_sent;
    atomic_int64_t bytes_received;
    atomic_int64_t tag_ops_completed;
    atomic_int64_t request_timeouts;
    atomic_int64_t error_responses;

    atomic_int64_t requests_sent_at_connect;
    atomic_int64_t responses_received_at_connect;
    atomic_int64_t bytes_sent_at_connect;
    atomic_int64_t bytes_received_at_connect;
    atomic_int64_t tag_ops_completed_at_connect;

    atomic_int32_t response_us_last;
    atomic_int32_t response_us_avg; /* since connect, integer EWMA: avg += (sample - avg) / 8 */
    atomic_int32_t response_us_min; /* since connect; 0 until the first response */
    atomic_int32_t response_us_max; /* since connect */
    atomic_int32_t requests_in_flight;

    /* writer only: the connection reached UP since the last connect attempt started. */
    bool up_since_attempt;
};

typedef struct conn_stats_t conn_stats_t;


/* writer side: the connection's IO thread, except the tag count. */
extern void conn_stats_init(conn_stats_t *stats);
extern void conn_stats_status_changed(conn_stats_t *stats, int32_t old_status, int32_t new_status);
extern void conn_stats_retry_wait(conn_stats_t *stats, int64_t delay_ms);
extern void conn_stats_bytes_sent(conn_stats_t *stats, int32_t bytes);
extern void conn_stats_bytes_received(conn_stats_t *stats, int32_t bytes);
extern void conn_stats_request_sent(conn_stats_t *stats);
extern void conn_stats_response_received(conn_stats_t *stats);
extern void conn_stats_response_time(conn_stats_t *stats, int64_t response_us);
extern void conn_stats_tag_ops(conn_stats_t *stats, int32_t completed, int32_t errors);
extern void conn_stats_request_timeout(conn_stats_t *stats);
extern void conn_stats_requests_in_flight(conn_stats_t *stats, int32_t count);
extern void conn_stats_tag_added(conn_stats_t *stats);
extern void conn_stats_tag_removed(conn_stats_t *stats);

/* reader side: a total less its baseline, never negative. */
extern int64_t conn_stats_since_connect(atomic_int64_t *total, atomic_int64_t *baseline);
