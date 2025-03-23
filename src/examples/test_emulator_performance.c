/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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
 * This example reads a small set of tags repeatedly as fast as possible.  It does not destroy the tags on errors, but simply
 * calls plc_tag_abort() and retries.
 *
 * Use ^C to terminate.
 */


#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(WIN32) || defined(_WIN32)
#    include <Windows.h>
#else
#    include <signal.h>
#endif
#include "../lib/libplctag.h"
#include "utils.h"


#define REQUIRED_VERSION 2, 4, 0

#define NUM_TAGS (100000)

#define TAG_OP_TIMEOUT_MS (200)
#define MAX_TEST_TIME_MS (10000)
#define MAX_CONNECTION_GROUPS (100)

#define DEFAULT_TAG_PATH \
    "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray&connection_group_id=%d"

static volatile int failed = 0;

struct {
    int32_t tag_handle;
    int32_t iteration_count;
} tag_info[NUM_TAGS] = {0};

static void tag_callback(int32_t tag_id, int event, int status, void *index_arg);

int main(void) {
    int64_t test_interval_start = 0;
    int64_t test_interval_end = 0;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);

    /* increase the connection group count each time */
    for(int connection_group_count = 1; connection_group_count < MAX_CONNECTION_GROUPS && !failed; connection_group_count += 10) {
        /* calculate our test interval */
        test_interval_start = util_time_ms();
        test_interval_end = test_interval_start + MAX_TEST_TIME_MS;


        /* create the tags */
        for(int tag_index = 0; tag_index < NUM_TAGS && !failed; tag_index++) {
            char buf[250] = {0};
            int32_t tag_id_or_status = PLCTAG_STATUS_OK;

            snprintf(buf, sizeof(buf), DEFAULT_TAG_PATH, connection_group_count);

            tag_id_or_status = plc_tag_create_ex(buf, tag_callback, (void *)(intptr_t)tag_index, 0);

            if(tag_id_or_status < 0) {
                fprintf(stderr, "ERROR: Error %s creating tag %d with connection group ID %d!\n",
                        plc_tag_decode_error(tag_id_or_status), tag_index, connection_group_count);
                failed = 1;
            } else {
                tag_info[tag_index].tag_handle = tag_id_or_status;
                tag_info[tag_index].iteration_count = 0;
            }
        }

        /* wait for the iteration cycle time */
        while(util_time_ms() < test_interval_end && !failed) { thrd_sleep_ms(100, NULL); }

        if(!failed) {
            /* calculate statistics */
            int64_t total_iterations = 0;
            int64_t total_ms = util_time_ms() - test_interval_start;

            for(int tag_index = 0; tag_index < NUM_TAGS; tag_index++) { total_iterations += tag_info[tag_index].iteration_count; }

            fprintf(stderr,
                    "Test %d connection groups for %" PRId64 "ms: read %" PRId64 " iterations with an average of %" PRId64
                    "ms per iteration per tag.\n",
                    connection_group_count, total_ms, total_iterations, total_ms / NUM_TAGS);
        } else {
            fprintf(stderr, "FAILED on connection group count %d!\n", connection_group_count);
        }
    }

    /* clean up tags */
    for(int tag_index = 0; tag_index < NUM_TAGS; tag_index++) {
        if(tag_info[tag_index].tag_handle != 0) { plc_tag_destroy(tag_info[tag_index].tag_handle); }
    }

    return 0;
}


void tag_callback(int32_t tag_id, int event, int status, void *index_arg) {
    int32_t index = (int32_t)(intptr_t)index_arg;
    int rc = PLCTAG_STATUS_OK;

    (void)status;

    switch(event) {
        case PLCTAG_EVENT_ABORTED:
            fprintf(stderr, "Tag %" PRId32 " aborted!\n", tag_id);
            failed = 1;
            break;

        case PLCTAG_EVENT_CREATED:
            /* start reading the tag now. */
            rc = plc_tag_read(tag_id, 0);
            if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
                fprintf(stderr, "ERROR: Error %s trying to start tag read!\n", plc_tag_decode_error(rc));
                failed = 1;
            }
            break;

        case PLCTAG_EVENT_DESTROYED: fprintf(stderr, "Tag %d being destroyed.\n", tag_id); break;

        case PLCTAG_EVENT_READ_COMPLETED:
            tag_info[index].iteration_count += 1;
            rc = plc_tag_read(tag_id, 0);
            if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
                fprintf(stderr, "ERROR: Error %s trying to start tag read!\n", plc_tag_decode_error(rc));
                failed = 1;
            }
            break;

        case PLCTAG_EVENT_READ_STARTED: break;

        case PLCTAG_EVENT_WRITE_COMPLETED:
            fprintf(stderr, "ERROR: Tag %d had write completed event raised!\n", tag_id);
            failed = 1;
            break;

        case PLCTAG_EVENT_WRITE_STARTED:
            fprintf(stderr, "ERROR: Tag %d had write started event raised!\n", tag_id);
            failed = 1;
            break;

        default:
            fprintf(stderr, "ERROR! Unknown event %d!\n", event);
            failed = 1;
            break;
    }
}
