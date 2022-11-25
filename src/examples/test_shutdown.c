/***************************************************************************
 *   Copyright (C) 2021 by Kyle Hayes                                      *
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


#define REQUIRED_VERSION 2,5,5
#define TAG_ATTRIBS_TMPL "protocol=ab_eip&gateway=10.206.1.40&path=1,4&cpu=LGX&elem_type=DINT&elem_count=1&name=TestBigArray[%d]&auto_sync_read_ms=200&auto_sync_write_ms=20"
#define DATA_TIMEOUT (5000)
#define RUN_PERIOD (10000)
#define READ_SLEEP_MS (100)
#define WRITE_SLEEP_MS (300)

#define READ_PERIOD_MS (200)

static volatile int read_start_count = 0;
static volatile int read_complete_count = 0;
static volatile int write_start_count = 0;
static volatile int write_complete_count = 0;


void *reader_function(void *tag_arg)
{
    int32_t tag_id = (int32_t)(intptr_t)tag_arg;
    int64_t start_time = util_time_ms();
    int64_t run_until = start_time + RUN_PERIOD;
    int iteration = 1;

    while(run_until > util_time_ms()) {
        int status = plc_tag_status(tag_id);
        int32_t val = plc_tag_get_int32(tag_id, 0);

        if(status < 0) {
            fprintf(stderr, "Tag %" PRId32 " has error status %s, terminating!\n", tag_id, plc_tag_decode_error(status));
            break;
        }

        fprintf(stderr, "READER: Tag %" PRId32 " iteration %d, got value: %d at time %" PRId64 "\n", tag_id, iteration++, val, util_time_ms()-start_time);

        util_sleep_ms(READ_SLEEP_MS);
    }

    return NULL;
}


void *writer_function(void *tag_arg)
{
    int32_t tag_id = (int32_t)(intptr_t)tag_arg;
    int64_t start_time = util_time_ms();
    int64_t run_until = start_time + RUN_PERIOD;
    int iteration = 1;

    util_sleep_ms(WRITE_SLEEP_MS);

    while(run_until > util_time_ms()) {
        int32_t val = plc_tag_get_int32(tag_id, 0);
        int32_t new_val = ((val+1) > 499) ? 0 : (val+1);
        int status = plc_tag_status(tag_id);

        if(status < 0) {
            fprintf(stderr, "Tag %" PRId32 " has error status %s, terminating!\n", tag_id, plc_tag_decode_error(status));
            break;
        }

        /* write the value */
        plc_tag_set_int32(tag_id, 0, new_val);

        fprintf(stderr, "WRITER: Tag %" PRId32 " iteration %d, wrote value: %d at time %" PRId64 "\n", tag_id, iteration++, new_val, util_time_ms()-start_time);

        util_sleep_ms(WRITE_SLEEP_MS);
    }

    return NULL;
}


void tag_callback(int32_t tag_id, int event, int status, void *not_used)
{
    (void)not_used;

    /* handle the events. */
    switch(event) {
        case PLCTAG_EVENT_CREATED:
            fprintf(stderr,"Tag %" PRId32 " created.\n", tag_id);
            break;
        
        case PLCTAG_EVENT_ABORTED:
            fprintf(stderr, "Tag %" PRId32 " automatic operation was aborted!\n", tag_id);
            break;

        case PLCTAG_EVENT_DESTROYED:
            fprintf(stderr, "Tag %" PRId32 " was destroyed.\n", tag_id);
            break;

        case PLCTAG_EVENT_READ_COMPLETED:
            read_complete_count++;
            fprintf(stderr, "Tag %" PRId32 " automatic read operation completed with status %s.\n", tag_id, plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_READ_STARTED:
            read_start_count++;
            fprintf(stderr, "Tag %" PRId32 " automatic read operation started with status %s.\n", tag_id, plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_WRITE_COMPLETED:
            write_complete_count++;
            fprintf(stderr, "Tag %" PRId32 " automatic write operation completed with status %s.\n", tag_id, plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_WRITE_STARTED:
            write_start_count++;
            fprintf(stderr, "Tag %" PRId32 " automatic write operation started with status %s.\n", tag_id, plc_tag_decode_error(status));

            break;

        default:
            fprintf(stderr, "Tag %" PRId32 " unexpected event %d!\n", tag_id, event);
            break;

    }
}



#define NUM_TAGS (10)


int main(int argc, char **argv)
{
    int rc = PLCTAG_STATUS_OK;
    char tag_attr_str[sizeof(TAG_ATTRIBS_TMPL)+10] = {0};
    int32_t tags[NUM_TAGS] = {0};
    pthread_t read_threads[NUM_TAGS];
    pthread_t write_threads[NUM_TAGS];
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

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /* create all the tags. */
    for(int i=0; i<NUM_TAGS; i++) {
        int32_t tag_id = PLCTAG_ERR_CREATE;

        snprintf(tag_attr_str, sizeof(tag_attr_str), TAG_ATTRIBS_TMPL, (int32_t)i);
        tag_id = plc_tag_create_ex(tag_attr_str, tag_callback, NULL, DATA_TIMEOUT);

        if(tag_id <= 0) {
            fprintf(stderr, "Error %s trying to create tag %d!\n", plc_tag_decode_error(tag_id), i);
            plc_tag_shutdown();
            return 1;
        }

        /* create read and write thread for this tag. */
        /* FIXME - check error returns! */
        pthread_create(&read_threads[i], NULL, reader_function, (void *)(intptr_t)tag_id);
        pthread_create(&write_threads[i], NULL, writer_function, (void *)(intptr_t)tag_id);

        tags[i] = tag_id;
    }

    /* let everything run for a while */
    util_sleep_ms(RUN_PERIOD/2);

    /* forcible shut down the entire library. */
    fprintf(stderr, "Forcing library shutdown.\n");
    plc_tag_shutdown();

    fprintf(stderr, "Waiting for threads to quit.\n");

    for(int i=0; i<NUM_TAGS; i++) {
        pthread_join(read_threads[i], NULL);
        pthread_join(write_threads[i], NULL);
    }

    fprintf(stderr, "Done.\n");

    return rc;
}

