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


#define REQUIRED_VERSION 2,4,7

#define TAG_ATTRIBS "protocol=ab_eip&gateway=10.206.1.40&path=1,4&cpu=LGX&elem_type=DINT&elem_count=1%d&name=TestBigArray[1]&auto_sync_read_ms=200&auto_sync_write_ms=20"
#define NUM_TAGS (200)

#define DATA_TIMEOUT (5000)
#define RUN_PERIOD (10000)
#define READ_SLEEP_MS (100)
#define WRITE_SLEEP_MS (300)

#define CREATE_TIMEOUT_MS ((1000 + ((NUM_TAGS/200) * 50)))

#define READ_PERIOD_MS (200)

struct tag_entry {
    int32_t tag_id;
    int status;
    int create_complete;
    int read_count;
    int write_count;
    int write_trigger_count;
};

static volatile struct tag_entry tags[NUM_TAGS] = {0};

// static volatile int read_count = 0;
// static volatile int write_count = 0;


// void *reader_function(void *tag_arg)
// {
//     int32_t tag = (int32_t)(intptr_t)tag_arg;
//     int64_t start_time = util_time_ms();
//     int64_t run_until = start_time + RUN_PERIOD;
//     int iteration = 1;

//     while(run_until > util_time_ms()) {
//         int32_t val = plc_tag_get_int32(tag, 0);

//         fprintf(stderr, "READER: Iteration %d, got value: %d at time %" PRId64 "\n", iteration++, val, util_time_ms()-start_time);

//         util_sleep_ms(READ_SLEEP_MS);
//     }

//     return NULL;
// }


void *writer_function(void *unused)
{
    int64_t start_time = util_time_ms();
    int64_t run_until = start_time + RUN_PERIOD;
    int iteration = 1;

    (void)unused;

    util_sleep_ms(WRITE_SLEEP_MS);

    while(run_until > util_time_ms()) {
        int index = rand() % NUM_TAGS;
        int tag_id = tags[index].tag_id;
        int32_t val = plc_tag_get_int32(tag_id, 0);
        int32_t new_val = ((val+1) > 499) ? 0 : (val+1);

        /* write the value */
        plc_tag_set_int32(tag_id, 0, new_val);

        /* mark the attempt. */
        tags[index].write_trigger_count++;

        fprintf(stderr, "%06" PRId64 " WRITER: Iteration %d, wrote value: %d to tag %" PRId32 " at time %" PRId64 "\n", util_time_ms() - start_time,  iteration++, new_val, tag_id, util_time_ms()-start_time);

        util_sleep_ms(WRITE_SLEEP_MS);
    }

    fprintf(stderr, "WRITER: terminating.\n");

    return NULL;
}


void tag_callback(int32_t tag_id, int event, int status)
{
    int low = 0;
    int high = NUM_TAGS-1;
    int mid = (high + low)/2;
    int iteration = 0;

    /* find the tag entry */
    /* FIXME - WARNING - This only works if tag IDs are sequentially allocated!!!! */
    while(abs(high - low) > 0) {
        fprintf(stderr, "tag_callback() iteration %d, high %d, mid %d, low %d.\n", ++iteration, high, mid, low);

        if(tag_id < tags[mid].tag_id) {
            high = (mid > low ? mid-1 : low);
            fprintf(stderr, "Search in lower half [%d, %d], looking for id %" PRId32 " is less than id %" PRId32 " at index %d.\n", low, high, tag_id, tags[mid].tag_id, mid);
        } else if(tag_id > tags[mid].tag_id) {
            low = (mid < high ? mid+1 : high);
            fprintf(stderr, "Search in upper half [%d, %d], looking for id %" PRId32 " is more than id %" PRId32 " at index %d.\n", low, high, tag_id, tags[mid].tag_id, mid);
        } else {
            break;
        }

        mid = (high + low)/2;
    }

    if(tag_id == tags[mid].tag_id) {
        fprintf(stderr, "found id %" PRId32 " at index %d.\n", tag_id, mid);
    } else {
        fprintf(stderr, "!!! Lookup problem, tag %" PRId32 " not found!\n", tag_id);
        return;
    }

    switch(event) {
        case PLCTAG_EVENT_ABORTED:
            fprintf(stderr, "Tag %d automatic operation was aborted!\n", tag_id);
            break;

        case PLCTAG_EVENT_DESTROYED:
            fprintf(stderr, "Tag[%d] %" PRId32 " was destroyed.\n", mid, tag_id);
            break;

        case PLCTAG_EVENT_READ_COMPLETED:
            fprintf(stderr, "Tag %d automatic read operation completed with status %s.\n", tag_id, plc_tag_decode_error(status));
            tags[mid].status = status;

            if(status == PLCTAG_STATUS_OK && tags[mid].create_complete == 0) {
                tags[mid].create_complete = 1;
            }

            break;

        case PLCTAG_EVENT_READ_STARTED:
            tags[mid].read_count++;
            tags[mid].status = status;
            fprintf(stderr, "Tag %d automatic read operation started with status %s.\n", tag_id, plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_WRITE_COMPLETED:
            fprintf(stderr, "Tag %d automatic write operation completed with status %s.\n", tag_id, plc_tag_decode_error(status));
            tags[mid].status = status;
            break;

        case PLCTAG_EVENT_WRITE_STARTED:
            tags[mid].write_count++;
            tags[mid].status = status;
            fprintf(stderr, "Tag %d automatic write operation started with status %s.\n", tag_id, plc_tag_decode_error(status));

            break;

        default:
            fprintf(stderr, "Unexpected event %d on tag %d!\n", event, tag_id);
            break;

    }
}


int compare_tag_entries(const void *tag_entry_1, const void *tag_entry_2)
{
    struct tag_entry *t1 = (struct tag_entry *)tag_entry_1;
    struct tag_entry *t2 = (struct tag_entry *)tag_entry_2;

    if(t1->tag_id < t2->tag_id) {
        return -1;
    } else if(t1->tag_id > t2->tag_id) {
        return 1;
    } else {
        return 0;
    }
}



int main(int argc, char **argv)
{
    int rc = PLCTAG_STATUS_OK;
    int64_t start_time = util_time_ms();
    int64_t timeout_time = 0;
    // pthread_t read_thread;
    pthread_t write_thread;
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

    for(int i=0; i < NUM_TAGS; i++) {
        int32_t tag = plc_tag_create(TAG_ATTRIBS, 0);
        if(tag < 0) {
            fprintf(stderr, "Error, %s, creating tag!\n", plc_tag_decode_error(tag));
            rc = 1;
            goto done;
        }

        tags[i].tag_id = tag;
        tags[i].status = plc_tag_status(tag);

        fprintf(stderr, "%06" PRId64 " Tag[%d] %" PRId32 " status %s.\n", util_time_ms() - start_time, i, tag, plc_tag_decode_error(plc_tag_status(tag)));
    }

    /* sort the tag IDs just in case */
    qsort((void *)tags, (size_t)NUM_TAGS, sizeof(struct tag_entry), compare_tag_entries);

    /*
     * add the callbacks
     *
     * Do this after all the tags have been created and
     * the tag IDs are sorted. Otherwise the lookup loop
     * in the callback function will fail by hitting out of order
     * tag IDs!
     */
    for(int i=0; i < NUM_TAGS; i++) {
        /* register the callback */
        rc = plc_tag_register_callback(tags[i].tag_id, tag_callback);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr, "Unable to register callback for tag[%d] %" PRId32 ", caught error %s!\n", i, tags[i].tag_id, plc_tag_decode_error(rc));
            rc = 1;
            goto done;
        }
    }


    /* wait for tag creation to complete. */

    fprintf(stderr, "%06" PRId64 " Waiting up to %dms for tag creation to complete.\n", util_time_ms() - start_time, CREATE_TIMEOUT_MS);

    timeout_time = util_time_ms() + CREATE_TIMEOUT_MS;

    while(timeout_time > util_time_ms()) {
        int waiting_tag_count = 0;

        for(int i=0; i < NUM_TAGS; i++) {
            /* read once so it cannot change in the middle of the if statement */
            int status = tags[i].status;

            if(status != PLCTAG_STATUS_OK && status != PLCTAG_STATUS_PENDING) {
                fprintf(stderr, "Error %s creating tag[%d] %" PRId32 "!\n", plc_tag_decode_error(tags[i].status), i, tags[i].tag_id);
                rc = 1;
                goto done;
            }

            if(tags[i].create_complete == 0) {
                waiting_tag_count++;
            }
        }

        if(waiting_tag_count == 0) {
            break;
        } else {
            util_sleep_ms(100);
        }
    }

    if(timeout_time < util_time_ms()) {
        fprintf(stderr, "!!! Timeout waiting %dms for tag creation!\n", CREATE_TIMEOUT_MS);
        rc = 1;
        goto done;
    }

    fprintf(stderr, "%06" PRId64 " Ready to start threads.\n", util_time_ms() - start_time);

    /* create the threads. */
    // pthread_create(&read_thread, NULL, reader_function, (void *)(intptr_t)tag);
    pthread_create(&write_thread, NULL, writer_function, (void *)NULL);

    fprintf(stderr, "%06" PRId64 " Waiting for threads to quit.\n", util_time_ms() - start_time);

    // pthread_join(read_thread, NULL);
    pthread_join(write_thread, NULL);

    fprintf(stderr, "%06" PRId64 " Done.\n", util_time_ms() - start_time);

done:
    for(int i=0; i < NUM_TAGS; i++) {
        plc_tag_destroy(tags[i].tag_id);
    }

    /* check the results. */
    if(rc == 0) {
        for(int i=0; i < NUM_TAGS; i++) {
            /* allow for about 10% */
            int read_epsilon = (RUN_PERIOD/READ_PERIOD_MS) / 10;

            if(abs((RUN_PERIOD/READ_PERIOD_MS) - tags[i].read_count) > read_epsilon) {
                fprintf(stderr, "Number of reads for tag[%d] %" PRId32 ", %d, is NOT close to the expected number, %d!\n", i, tags[i].tag_id, tags[i].read_count, (RUN_PERIOD/READ_PERIOD_MS));
                rc = 1;
            } else {
                fprintf(stderr, "Number of reads for tag[%d] %" PRId32 ", %d, is close to the expected number, %d.\n", i, tags[i].tag_id, tags[i].read_count, (RUN_PERIOD/READ_PERIOD_MS));
            }

            if(abs(tags[i].write_trigger_count - tags[i].write_count) > 2) {
                fprintf(stderr, "Number of writes for tag[%d] %" PRId32 ", %d, is NOT close to the expected number, %d!\n", i, tags[i].tag_id, tags[i].write_count, tags[i].write_trigger_count);
                rc = 1;
            } else {
                fprintf(stderr, "Number of writes for tag[%d] %" PRId32 ", %d, is close to the expected number, %d.\n", i, tags[i].tag_id, tags[i].write_count, tags[i].write_trigger_count);
            }
        }
    }

    return rc;
}

