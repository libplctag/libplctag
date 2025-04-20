/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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


#include "compat_utils.h"
#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define REQUIRED_VERSION 2, 4, 0

#define DATA_TIMEOUT (5000)
#define TAG_CREATE_TIMEOUT (5000)
#define RETRY_TIMEOUT (10000)

#define DEFAULT_TAG_PATH "protocol=modbus-tcp&gateway=10.206.1.59:5020&path=0&elem_count=2&name=hr10"


void usage(void) {
    printf(
        "Usage:\n "
        "async_stress <num tags> <path>\n"
        "  <num_tags> - The number of tags to use in the test.\n"
        "  <path> - The tag path to use.\n"
        "\n"
        "Example: async_stress 14 'protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=LGX&elem_size=4&elem_count=1&name=test_tag'\n");

    exit(PLCTAG_ERR_BAD_PARAM);
}

static volatile int done = 0;

static void interrupt_handler(void) { done = 1; }

static int read_tags(int32_t *tags, int32_t *statuses, int num_tags, int timeout_ms);
static int wait_for_tags(int32_t *tags, int32_t *statuses, int num_tags, int timeout_ms);


int main(int argc, char **argv) {
    int32_t *tags = NULL;
    int *statuses = NULL;
    int num_tags = 0;
    int rc = PLCTAG_STATUS_OK;
    int i = 0;
    int64_t start = 0;
    int64_t end = 0;
    int64_t total_ms = 0;
    int64_t min_ms = INT64_MAX;
    int64_t max_ms = 0;
    int64_t iteration = 1;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /* check the command line arguments */
    if(argc != 3) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Must have number of tags and tag path!\n");
        usage();
    }

    num_tags = atoi(argv[1]);

    if(num_tags <= 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Number of tags must be greater than zero!\n");
        usage();
    }

    tags = calloc((size_t)(unsigned int)num_tags, sizeof(*tags));
    if(!tags) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error allocating tags array!\n");
        exit(PLCTAG_ERR_NO_MEM);
    }

    statuses = calloc((size_t)(unsigned int)num_tags, sizeof(*statuses));
    if(!statuses) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error allocating status array!\n");
        free(tags);
        exit(PLCTAG_ERR_NO_MEM);
    }

    /* set up handler for ^C etc. */
    compat_set_interrupt_handler(interrupt_handler);

    // NOLINTNEXTLINE
    fprintf(stderr, "Hit ^C to terminate the test.\n");

    start = compat_time_ms();

    /* create the tags */
    for(i = 0; i < num_tags && !done; i++) {
        tags[i] = plc_tag_create(argv[2], 0);
        statuses[i] = plc_tag_status(tags[i]);

        if(tags[i] < 0) {
            // NOLINTNEXTLINE
            fprintf(stderr, "Error %s: could not create tag %d\n", plc_tag_decode_error(tags[i]), i);
            done = 1;
        }
    }

    if(!done) {
        rc = wait_for_tags(tags, statuses, num_tags, TAG_CREATE_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            for(int i = 0; i < num_tags; i++) {
                if(statuses[i] != PLCTAG_STATUS_OK) {
                    // NOLINTNEXTLINE
                    fprintf(stderr, "Creation of tag %d failed with status %s!\n", i, plc_tag_decode_error(statuses[i]));
                }

                plc_tag_destroy(tags[i]);
            }

            done = 1;
        }
    }

    end = compat_time_ms();

    // NOLINTNEXTLINE
    fprintf(stderr, "Creation of %d tags took %dms.\n", num_tags, (int)(end - start));

    /* read in a loop until ^C pressed */
    while(!done) {
        start = compat_time_ms();

        rc = read_tags(tags, statuses, num_tags, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            int need_sleep = 0;

            for(int i = 0; i < num_tags; i++) {
                if(statuses[i] != PLCTAG_STATUS_OK) {
                    if(statuses[i] != PLCTAG_ERR_TIMEOUT) {
                        // NOLINTNEXTLINE
                        fprintf(stderr, "Tag %d read failed with status %s!\n", i, plc_tag_decode_error(statuses[i]));
                        done = 1;
                    } else {
                        // NOLINTNEXTLINE
                        fprintf(stderr, "Tag %d read failed with a timeout, will retry.\n", i);
                        plc_tag_abort(tags[i]);
                        need_sleep = 1;
                    }
                }
            }

            if(need_sleep) { compat_sleep_ms(10, NULL); /* give the background thread time to process the abort. */ }
        }

        end = compat_time_ms();

        /* count up the total ms */
        total_ms += (end - start);

        /* calculate the min and max time */
        if(max_ms < (end - start)) { max_ms = end - start; }

        if(min_ms > (end - start)) { min_ms = end - start; }

        // NOLINTNEXTLINE
        fprintf(stderr, "Read of %d tags took %dms.\n", num_tags, (int)(end - start));

        /* test */
        // thrd_sleep_ms(5);

        iteration++;
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Program terminated!\n");

    /* we are done */
    for(i = 0; i < num_tags; i++) { plc_tag_destroy(tags[i]); }

    free(tags);
    free(statuses);

    // NOLINTNEXTLINE
    fprintf(stderr,
            "--- Ran %" PRId64 " iterations with a total io time of %" PRId64 "ms and min/avg/max of %" PRId64 "ms/%" PRId64
            "ms/%" PRId64 "ms.\n",
            iteration, total_ms, min_ms, total_ms / iteration, max_ms);

    return 0;
}


int read_tags(int32_t *tags, int *statuses, int num_tags, int timeout_ms) {
    if(timeout_ms <= 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Timeout to read_tags() must be greater than zero!\n");

        return PLCTAG_ERR_BAD_PARAM;
    }

    /* start the read. */
    for(int i = 0; i < num_tags; i++) {
        statuses[i] = plc_tag_read(tags[i], 0);

        /* if any failed, we need to abort the request. */
        if(statuses[i] != PLCTAG_STATUS_OK && statuses[i] != PLCTAG_STATUS_PENDING) {
            // NOLINTNEXTLINE
            fprintf(stderr, "1 Calling plc_tag_abort() on tag %d!\n", i);
            plc_tag_abort(tags[i]);
        }
    }

    return wait_for_tags(tags, statuses, num_tags, timeout_ms);
}


int wait_for_tags(int32_t *tags, int *statuses, int num_tags, int timeout_ms) {
    int64_t end_timeout = (int64_t)timeout_ms + compat_time_ms();
    int rc = PLCTAG_STATUS_OK;
    int tags_pending = 0;

    /* the statuses must be primed before calling this function! */
    do {
        /* check the pending tags. */
        tags_pending = 0;
        for(int i = 0; i < num_tags; i++) {
            if(statuses[i] == PLCTAG_STATUS_PENDING) {
                statuses[i] = plc_tag_status(tags[i]);

                /* still pending? */
                if(statuses[i] == PLCTAG_STATUS_PENDING) {
                    tags_pending++;
                } else if(statuses[i] != PLCTAG_STATUS_OK) {
                    /* not good, some sort of error! */

                    // NOLINTNEXTLINE
                    fprintf(stderr, "Tag %d failed with status %s!\n", i, plc_tag_decode_error(statuses[i]));

                    // NOLINTNEXTLINE
                    fprintf(stderr, "2 Calling plc_tag_abort() on tag %d!\n", i);
                    plc_tag_abort(tags[i]);
                }
            }
        }

        /* anything left to do? */
        if(tags_pending > 0) {
            /* yes, there is, delay a bit. */
            compat_sleep_ms(10, NULL);
        }
    } while(tags_pending > 0 && end_timeout > compat_time_ms() && !done);

    rc = PLCTAG_STATUS_OK;

    /* did any tags time out? */
    if(end_timeout <= compat_time_ms()) {
        for(int i = 0; i < num_tags; i++) {
            if(statuses[i] == PLCTAG_STATUS_PENDING) {
                /* we timed out, so abort and mark the status. */
                // NOLINTNEXTLINE
                fprintf(stderr, "Timed out, calling plc_tag_abort() on tag %d!\n", i);
                plc_tag_abort(tags[i]);

                statuses[i] = PLCTAG_ERR_TIMEOUT;

                rc = PLCTAG_ERR_PARTIAL;
            } else if(statuses[i] != PLCTAG_STATUS_OK) {
                rc = PLCTAG_ERR_PARTIAL;
            }
        }
    }

    return rc;
}
