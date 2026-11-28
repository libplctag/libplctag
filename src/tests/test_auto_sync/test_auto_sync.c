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


#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define REQUIRED_VERSION 2, 6, 13
#define DEFAULT_TAG_ATTRIBS \
    "protocol=ab_eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_type=DINT&elem_count=1&name=TestBigArray[4]&auto_sync_read_ms=600&auto_sync_write_ms=20"
#define DATA_TIMEOUT (5000)
#define DEFAULT_RUN_PERIOD_MS (30000)
#define DEFAULT_READ_SLEEP_MS (100)
#define DEFAULT_WRITE_SLEEP_MS (800)

static volatile int read_start_count = 0;
static volatile int read_complete_count = 0;
static volatile int write_start_count = 0;
static volatile int write_complete_count = 0;
static volatile int abort_count = 0;

/* Set from CLI args in main() before the threads start; read-only afterward. */
static int64_t g_run_period_ms = DEFAULT_RUN_PERIOD_MS;
static uint32_t g_read_sleep_ms = DEFAULT_READ_SLEEP_MS;
static uint32_t g_write_sleep_ms = DEFAULT_WRITE_SLEEP_MS;


/* AB tags in this test use a 4-byte DINT; Modbus tags use a 2-byte register.
 * Read the width off the tag itself instead of hardcoding it per protocol. */
static int32_t get_val(int32_t tag) {
    int elem_size = plc_tag_get_int_attribute(tag, "elem_size", 4);
    return (elem_size <= 2) ? (int32_t)plc_tag_get_int16(tag, 0) : plc_tag_get_int32(tag, 0);
}


static void set_val(int32_t tag, int32_t val) {
    int elem_size = plc_tag_get_int_attribute(tag, "elem_size", 4);
    if(elem_size <= 2) {
        plc_tag_set_int16(tag, 0, (int16_t)val);
    } else {
        plc_tag_set_int32(tag, 0, val);
    }
}


void *reader_function(void *tag_arg) {
    int32_t tag = (int32_t)(intptr_t)tag_arg;
    int64_t start_time = compat_time_ms();
    int64_t run_until = start_time + g_run_period_ms;
    int iteration = 1;

    while(run_until > compat_time_ms()) {
        int32_t val = get_val(tag);

        // NOLINTNEXTLINE
        fprintf(stderr, "READER: Iteration %d, got value: %d at time %" PRId64 "\n", iteration++, val,
                compat_time_ms() - start_time);

        compat_sleep_ms(g_read_sleep_ms, NULL);
    }

    return 0;
}


void *writer_function(void *tag_arg) {
    int32_t tag = (int32_t)(intptr_t)tag_arg;
    int64_t start_time = compat_time_ms();
    int64_t run_until = start_time + g_run_period_ms;
    int iteration = 1;

    while(run_until > compat_time_ms()) {
        int32_t val = get_val(tag);
        int32_t new_val = ((val + 1) > 499) ? 0 : (val + 1);

        /* write the value */
        set_val(tag, new_val);

        // NOLINTNEXTLINE
        fprintf(stderr, "WRITER: Iteration %d, wrote value: %d at time %" PRId64 "\n", iteration++, new_val,
                compat_time_ms() - start_time);

        compat_sleep_ms(g_write_sleep_ms, NULL);
    }

    return 0;
}


void tag_callback(int32_t tag_id, int event, int status, void *user_data) {
    (void)user_data; /* unused */
    /* handle the events. */
    switch(event) {
        case PLCTAG_EVENT_ABORTED:
            abort_count++;
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %d automatic operation was aborted!\n", tag_id);
            break;

        case PLCTAG_EVENT_CREATED:
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %d creation finished.\n", tag_id);
            break;

        case PLCTAG_EVENT_DESTROYED:
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %d was destroyed.\n", tag_id);
            break;

        case PLCTAG_EVENT_READ_COMPLETED:
            read_complete_count++;
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %d automatic read operation completed with status %s.\n", tag_id, plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_READ_STARTED:
            read_start_count++;
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %d automatic read operation started with status %s.\n", tag_id, plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_WRITE_COMPLETED:
            write_complete_count++;
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %d automatic write operation completed with status %s.\n", tag_id, plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_WRITE_STARTED:
            write_start_count++;
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %d automatic write operation started with status %s.\n", tag_id, plc_tag_decode_error(status));

            break;

        default:
            // NOLINTNEXTLINE
            fprintf(stderr, "Unexpected event %d on tag %d!\n", event, tag_id);
            break;
    }
}


static void parse_args(int argc, char **argv, const char **tag_attribs, const char **write_tag_attribs) {
    for(int i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) {
            *tag_attribs = &argv[i][6];
        } else if(strncmp(argv[i], "--write-tag=", 12) == 0) {
            *write_tag_attribs = &argv[i][12];
        } else if(strncmp(argv[i], "--run-ms=", 9) == 0) {
            g_run_period_ms = atoi(&argv[i][9]);
        } else if(strncmp(argv[i], "--read-sleep-ms=", 16) == 0) {
            g_read_sleep_ms = (uint32_t)atoi(&argv[i][16]);
        } else if(strncmp(argv[i], "--write-sleep-ms=", 17) == 0) {
            g_write_sleep_ms = (uint32_t)atoi(&argv[i][17]);
        }
    }
}


int main(int argc, char **argv) {
    int rc = PLCTAG_STATUS_OK;
    int32_t read_tag = 0;
    int32_t write_tag = 0;
    compat_thread_t read_thread, write_thread;
    const char *tag_attribs = DEFAULT_TAG_ATTRIBS;
    /* If set, read_tag/write_tag are two independent tags (needed for Modbus,
     * where auto_sync_read_ms and auto_sync_write_ms can't share one tag);
     * otherwise the same tag is used for both, as the AB test always has. */
    const char *write_tag_attribs = NULL;
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);

    parse_args(argc, argv, &tag_attribs, &write_tag_attribs);

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        // NOLINTNEXTLINE
        fprintf(stderr, "Available library version is %d.%d.%d.\n", version_major, version_minor, version_patch);
        exit(1);
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);

    read_tag = plc_tag_create_ex(tag_attribs, tag_callback, NULL, DATA_TIMEOUT);
    if(read_tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error, %s, creating tag!\n", plc_tag_decode_error(read_tag));
        return 1;
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Tag status %s.\n", plc_tag_decode_error(plc_tag_status(read_tag)));

    if(write_tag_attribs) {
        write_tag = plc_tag_create_ex(write_tag_attribs, tag_callback, NULL, DATA_TIMEOUT);
        if(write_tag < 0) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Error, %s, creating write tag!\n", plc_tag_decode_error(write_tag));
            plc_tag_destroy(read_tag);
            return 1;
        }

        // NOLINTNEXTLINE
        fprintf(stderr, "Write tag status %s.\n", plc_tag_decode_error(plc_tag_status(write_tag)));
    } else {
        write_tag = read_tag;
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Ready to start threads.\n");

    /* create the threads. */
    compat_thread_create(&read_thread, reader_function, (void *)(intptr_t)read_tag);
    compat_thread_create(&write_thread, writer_function, (void *)(intptr_t)write_tag);

    // NOLINTNEXTLINE
    fprintf(stderr, "Waiting for threads to quit.\n");

    compat_thread_join(read_thread, NULL);
    compat_thread_join(write_thread, NULL);

    // NOLINTNEXTLINE
    fprintf(stderr, "Done.\n");

    plc_tag_destroy(read_tag);
    if(write_tag_attribs) { plc_tag_destroy(write_tag); }

    /* check the results. */
    // NOLINTNEXTLINE
    fprintf(stderr, "Total reads triggered %d, finished %d.\n", read_start_count, read_complete_count);
    // NOLINTNEXTLINE
    fprintf(stderr, "Total writes triggered %d, finished %d.\n", write_start_count, write_complete_count);

    rc = 0;

    /*
     * A write started while an auto-read is in flight aborts that read.  That is the designed
     * priority, not a lost operation, so the aborted reads count as accounted for.  How many
     * collisions happen is pure timing, so counting them as failures makes the threshold a
     * function of how loaded the machine is.
     */
    // NOLINTNEXTLINE
    fprintf(stderr, "Total operations aborted %d.\n", abort_count);

    /* allow 10% margin - at least 90% of triggered operations should complete */
    int read_accounted = read_complete_count + abort_count;

    /* the abort from plc_tag_destroy() has no started read behind it. */
    if(read_accounted > read_start_count) { read_accounted = read_start_count; }

    int read_success_actual = (read_accounted * 100) / read_start_count;
    int write_success_actual = (write_complete_count * 100) / write_start_count;

    if(read_success_actual < 90) {
        // NOLINTNEXTLINE
        fprintf(stderr, "FAILURE: Number of reads, %d%%, not close to the expected number, 90%%!\n", read_success_actual);
        rc = 1;
    }

    if(write_success_actual < 90) {
        // NOLINTNEXTLINE
        fprintf(stderr, "FAILURE: Number of writes, %d%%, not close to the expected number, 90%%!\n", write_success_actual);
        rc = 1;
    }

    if(rc == 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "SUCCESS: Test completed successfully.\n");
    }

    return rc;
}
