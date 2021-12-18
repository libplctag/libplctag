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


#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/time.h>
#include "../lib/libplctag.h"
#include "utils.h"


#define REQUIRED_VERSION 2,1,12
#define TAG_ATTRIBS "protocol=ab_eip&gateway=10.206.1.40&path=1,4&cpu=LGX&elem_type=DINT&elem_count=1%d&name=TestBigArray[4]&auto_sync_read_ms=200&auto_sync_write_ms=20"
#define DATA_TIMEOUT (5000)
#define RUN_PERIOD (10000)
#define READ_SLEEP_MS (100)
#define WRITE_SLEEP_MS (300)

#define READ_PERIOD_MS (200)

static volatile int read_count = 0;
static volatile int write_count = 0;


void *reader_function(void *tag_arg)
{
    int32_t tag = (int32_t)(intptr_t)tag_arg;
    int64_t start_time = util_time_ms();
    int64_t run_until = start_time + RUN_PERIOD;
    int iteration = 1;

    while(run_until > util_time_ms()) {
        int32_t val = plc_tag_get_int32(tag, 0);

        fprintf(stderr, "READER: Iteration %d, got value: %d at time %" PRId64 "\n", iteration++, val, util_time_ms()-start_time);

        util_sleep_ms(READ_SLEEP_MS);
    }

    return NULL;
}


void *writer_function(void *tag_arg)
{
    int32_t tag = (int32_t)(intptr_t)tag_arg;
    int64_t start_time = util_time_ms();
    int64_t run_until = start_time + RUN_PERIOD;
    int iteration = 1;

    util_sleep_ms(WRITE_SLEEP_MS);

    while(run_until > util_time_ms()) {
        int32_t val = plc_tag_get_int32(tag, 0);
        int32_t new_val = ((val+1) > 499) ? 0 : (val+1);

        /* write the value */
        plc_tag_set_int32(tag, 0, new_val);

        fprintf(stderr, "WRITER: Iteration %d, wrote value: %d at time %" PRId64 "\n", iteration++, new_val, util_time_ms()-start_time);

        util_sleep_ms(WRITE_SLEEP_MS);
    }

    return NULL;
}


void tag_callback(int32_t tag_id, int event, int status)
{
    /* handle the events. */
    switch(event) {
        case PLCTAG_EVENT_ABORTED:
            fprintf(stderr, "Tag %d automatic operation was aborted!\n", tag_id);
            break;

        case PLCTAG_EVENT_DESTROYED:
            fprintf(stderr, "Tag was destroyed.\n");
            break;

        case PLCTAG_EVENT_READ_COMPLETED:
            fprintf(stderr, "Tag %d automatic read operation completed with status %s.\n", tag_id, plc_tag_decode_error(status));

            break;

        case PLCTAG_EVENT_READ_STARTED:
            read_count++;
            fprintf(stderr, "Tag %d automatic read operation started with status %s.\n", tag_id, plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_WRITE_COMPLETED:
            fprintf(stderr, "Tag %d automatic write operation completed with status %s.\n", tag_id, plc_tag_decode_error(status));

            break;

        case PLCTAG_EVENT_WRITE_STARTED:
            write_count++;
            fprintf(stderr, "Tag %d automatic write operation started with status %s.\n", tag_id, plc_tag_decode_error(status));

            break;

        default:
            fprintf(stderr, "Unexpected event %d on tag %d!\n", event, tag_id);
            break;

    }
}




int main(int argc, char **argv)
{
    int rc = PLCTAG_STATUS_OK;
    int32_t tag = 0;
    pthread_t read_thread, write_thread;
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);

    (void)argc;
    (void)argv;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        fprintf(stderr, "Available library version is %d.%d.%d.\n", version_major, version_minor, version_patch);
        exit(1);
    }

    fprintf(stderr, "Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);

    tag = plc_tag_create(TAG_ATTRIBS, DATA_TIMEOUT);
    if(tag < 0) {
        fprintf(stderr, "Error, %s, creating tag!\n", plc_tag_decode_error(tag));
        return 1;
    }

    fprintf(stderr, "Tag status %s.\n", plc_tag_decode_error(plc_tag_status(tag)));

    /* register the callback */
    rc = plc_tag_register_callback(tag, tag_callback);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Unable to register callback for tag %s!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    fprintf(stderr, "Ready to start threads.\n");

    /* create the threads. */
    pthread_create(&read_thread, NULL, reader_function, (void *)(intptr_t)tag);
    pthread_create(&write_thread, NULL, writer_function, (void *)(intptr_t)tag);

    fprintf(stderr, "Waiting for threads to quit.\n");

    pthread_join(read_thread, NULL);
    pthread_join(write_thread, NULL);

    fprintf(stderr, "Done.\n");

    plc_tag_destroy(tag);

    /* check the results. */
    fprintf(stderr, "Total reads triggered %d and total expected %d.\n", read_count, RUN_PERIOD/READ_PERIOD_MS);
    fprintf(stderr, "Total writes triggered %d and total expected %d.\n", write_count, RUN_PERIOD/WRITE_SLEEP_MS);

    rc = 0;

    if(abs((RUN_PERIOD/READ_PERIOD_MS) - read_count) > 3) {
        fprintf(stderr, "Number of reads, %d, not close to the expected number, %d!\n", read_count, (RUN_PERIOD/READ_PERIOD_MS));
        rc = 1;
    }

    if(abs((RUN_PERIOD/WRITE_SLEEP_MS) - write_count) > 3) {
        fprintf(stderr, "Number of writes, %d, not close to the expected number, %d!\n", write_count, (RUN_PERIOD/WRITE_SLEEP_MS));
        rc = 1;
    }

    return rc;
}

