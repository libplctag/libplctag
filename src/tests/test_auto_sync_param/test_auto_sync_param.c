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
 * Same as test_auto_sync, but the tag attribute string is supplied on the
 * command line with --tag=... instead of being a compile-time constant, so
 * the same test binary can exercise auto_sync_read_ms/auto_sync_write_ms
 * against any protocol (ab_eip, enip-tcp, modbus-tcp, ...).
 *
 * Usage: test_auto_sync_param --tag="protocol=...&...&auto_sync_read_ms=...&auto_sync_write_ms=..."
 */

#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define REQUIRED_VERSION 2, 4, 7
#define DATA_TIMEOUT (5000)
#define RUN_PERIOD (30000)
#define READ_SLEEP_MS (100)
#define WRITE_SLEEP_MS (800)

static volatile int read_start_count = 0;
static volatile int read_complete_count = 0;
static volatile int write_start_count = 0;
static volatile int write_complete_count = 0;


void *reader_function(void *tag_arg) {
    int32_t tag = (int32_t)(intptr_t)tag_arg;
    int64_t start_time = compat_time_ms();
    int64_t run_until = start_time + RUN_PERIOD;
    int iteration = 1;

    while(run_until > compat_time_ms()) {
        int32_t val = plc_tag_get_int32(tag, 0);

        // NOLINTNEXTLINE
        fprintf(stderr, "READER: Iteration %d, got value: %d at time %" PRId64 "\n", iteration++, val,
                compat_time_ms() - start_time);

        compat_sleep_ms(READ_SLEEP_MS, NULL);
    }

    return 0;
}


void *writer_function(void *tag_arg) {
    int32_t tag = (int32_t)(intptr_t)tag_arg;
    int64_t start_time = compat_time_ms();
    int64_t run_until = start_time + RUN_PERIOD;
    int iteration = 1;

    while(run_until > compat_time_ms()) {
        int32_t val = plc_tag_get_int32(tag, 0);
        int32_t new_val = ((val + 1) > 499) ? 0 : (val + 1);

        /* write the value */
        plc_tag_set_int32(tag, 0, new_val);

        // NOLINTNEXTLINE
        fprintf(stderr, "WRITER: Iteration %d, wrote value: %d at time %" PRId64 "\n", iteration++, new_val,
                compat_time_ms() - start_time);

        compat_sleep_ms(WRITE_SLEEP_MS, NULL);
    }

    return 0;
}


void tag_callback(int32_t tag_id, int event, int status, void *user_data) {
    (void)user_data; /* unused */
    /* handle the events. */
    switch(event) {
        case PLCTAG_EVENT_ABORTED:
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag %d automatic operation was aborted!\n", tag_id);
            break;

        case PLCTAG_EVENT_CREATED:
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag was creation finished.\n");
            break;

        case PLCTAG_EVENT_DESTROYED:
            // NOLINTNEXTLINE
            fprintf(stderr, "Tag was destroyed.\n");
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


static void usage(void) {
    // NOLINTNEXTLINE
    fprintf(stderr, "Usage: test_auto_sync_param --tag=<tag attribute string>\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "    The tag attribute string must include auto_sync_read_ms and/or\n");
    // NOLINTNEXTLINE
    fprintf(stderr, "    auto_sync_write_ms and reference a DINT (32-bit) element.\n");
    exit(1);
}


int main(int argc, char **argv) {
    int rc = PLCTAG_STATUS_OK;
    int32_t tag = 0;
    compat_thread_t read_thread, write_thread;
    const char *tag_attribs = NULL;
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);

    for(int i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) {
            tag_attribs = &argv[i][6];
        } else if(strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
        } else {
            // NOLINTNEXTLINE
            fprintf(stderr, "Unknown argument \"%s\"!\n", argv[i]);
            usage();
        }
    }

    if(!tag_attribs || strlen(tag_attribs) == 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: --tag=<tag attribute string> is required!\n");
        usage();
    }

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

    tag = plc_tag_create_ex(tag_attribs, tag_callback, NULL, DATA_TIMEOUT);
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error, %s, creating tag!\n", plc_tag_decode_error(tag));
        return 1;
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Tag status %s.\n", plc_tag_decode_error(plc_tag_status(tag)));

    // NOLINTNEXTLINE
    fprintf(stderr, "Ready to start threads.\n");

    /* create the threads. */
    compat_thread_create(&read_thread, reader_function, (void *)(intptr_t)tag);
    compat_thread_create(&write_thread, writer_function, (void *)(intptr_t)tag);

    // NOLINTNEXTLINE
    fprintf(stderr, "Waiting for threads to quit.\n");

    compat_thread_join(read_thread, NULL);
    compat_thread_join(write_thread, NULL);

    // NOLINTNEXTLINE
    fprintf(stderr, "Done.\n");

    plc_tag_destroy(tag);

    /* check the results. */
    // NOLINTNEXTLINE
    fprintf(stderr, "Total reads triggered %d, finished %d.\n", read_start_count, read_complete_count);
    // NOLINTNEXTLINE
    fprintf(stderr, "Total writes triggered %d, finished %d.\n", write_start_count, write_complete_count);

    rc = 0;

    /* allow 10% margin - at least 90% of triggered operations should complete */
    if(read_start_count > 0) {
        int read_success_actual = (read_complete_count * 100) / read_start_count;

        if(read_success_actual < 90) {
            // NOLINTNEXTLINE
            fprintf(stderr, "FAILURE: Number of completed reads, %d%%, not close to the number triggered!\n", read_success_actual);
            rc = 1;
        }
    }

    if(write_start_count > 0) {
        int write_success_actual = (write_complete_count * 100) / write_start_count;

        if(write_success_actual < 90) {
            // NOLINTNEXTLINE
            fprintf(stderr, "FAILURE: Number of completed writes, %d%%, not close to the number triggered!\n", write_success_actual);
            rc = 1;
        }
    }

    if(rc == 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "SUCCESS: Test completed successfully.\n");
    }

    return rc;
}
