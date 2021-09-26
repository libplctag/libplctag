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
 * This example reads a small set of tags repeatedly as fast as possible.  It does not destroy the tags on errors, but simply calls
 * plc_tag_abort() and retries.
 *
 * Use ^C to terminate.
 */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef WIN32
    #include <Windows.h>
#else
    #include <signal.h>
#endif
#include "../lib/libplctag.h"
#include "utils.h"


#define REQUIRED_VERSION 2,4,0

#define DATA_TIMEOUT (5000)



void usage(void)
{
    printf("Usage:\n "
        "async_stress <num tags> <path>\n"
        "  <num_tags> - The number of tags to use in the test.\n"
        "  <path> - The tag path to use.\n"
        "\n"
        "Example: async_stress 14 'protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=LGX&elem_size=4&elem_count=1&name=test_tag'\n");

    exit(PLCTAG_ERR_BAD_PARAM);
}




#ifdef _WIN32
volatile int done = 0;

/* straight from MS' web site :-) */
BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType)
    {
        // Handle the CTRL-C signal.
    case CTRL_C_EVENT:
        done = 1;
        return TRUE;

        // CTRL-CLOSE: confirm that the user wants to exit.
    case CTRL_CLOSE_EVENT:
        done = 1;
        return TRUE;

        // Pass other signals to the next handler.
    case CTRL_BREAK_EVENT:
        done = 1;
        return FALSE;

    case CTRL_LOGOFF_EVENT:
        done = 1;
        return FALSE;

    case CTRL_SHUTDOWN_EVENT:
        done = 1;
        return FALSE;

    default:
        return FALSE;
    }
}


void setup_break_handler(void)
{
    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE))
    {
        printf("\nERROR: Could not set control handler!\n");
        usage();
    }
}

#else
volatile sig_atomic_t done = 0;

void SIGINT_handler(int not_used)
{
    (void)not_used;

    done = 1;
}

void setup_break_handler(void)
{
    struct sigaction act;

    /* set up signal handler. */
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIGINT_handler;
    sigaction(SIGINT, &act, NULL);
}

#endif




// int check_tag_status(int32_t tag, int64_t timeout)
// {
//     int rc = PLCTAG_STATUS_OK;

//     rc = plc_tag_status(tag);

//     if(rc == PLCTAG_STATUS_OK) {
//         return rc;
//     } else if(rc != PLCTAG_STATUS_PENDING) {
//         return rc;
//     } else if(timeout <= util_time_ms()) {
//         return PLCTAG_ERR_TIMEOUT;
//     }

//     return PLCTAG_STATUS_PENDING;
// }


static int read_tags(int32_t *tags, int32_t *statuses, int num_tags, int timeout_ms);
static int wait_for_tags(int32_t *tags, int32_t *statuses, int num_tags, int timeout_ms);



int main(int argc, char **argv)
{
    int32_t *tags = NULL;
    int *statuses = NULL;
    int num_tags = 0;
    int rc = PLCTAG_STATUS_OK;
    int i = 0;
    int64_t start = 0;
    int64_t end = 0;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);

    /* check the command line arguments */
    if(argc != 3) {
        fprintf(stderr, "Must have number of tags and tag path!\n");
        usage();
    }

    num_tags = atoi(argv[1]);

    if(num_tags <= 0) {
        fprintf(stderr, "Number of tags must be greater than zero!\n");
        usage();
    }

    tags = calloc(sizeof(*tags), (size_t)(unsigned int)num_tags);
    if(!tags) {
        fprintf(stderr, "Error allocating tags array!\n");
        exit(PLCTAG_ERR_NO_MEM);
    }

    statuses = calloc(sizeof(*statuses), (size_t)(unsigned int)num_tags);
    if(!statuses) {
        fprintf(stderr, "Error allocating status array!\n");
        free(tags);
        exit(PLCTAG_ERR_NO_MEM);
    }

    /* set up handler for ^C etc. */
    setup_break_handler();

    fprintf(stderr, "Hit ^C to terminate the test.\n");

    start = util_time_ms();

    /* create the tags */
    for(i=0; i< num_tags && !done; i++) {
        tags[i]  = plc_tag_create(argv[2], 0);
        statuses[i] = plc_tag_status(tags[i]);

        if(tags[i] < 0) {
            fprintf(stderr,"Error %s: could not create tag %d\n", plc_tag_decode_error(tags[i]), i);
            done = 1;
        }
    }

    if(!done) {
        rc = wait_for_tags(tags, statuses, num_tags, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            for(int i=0; i<num_tags; i++) {
                if(statuses[i] != PLCTAG_STATUS_OK) {
                    fprintf(stderr, "Creation of tag %d failed with status %s!\n", i, plc_tag_decode_error(statuses[i]));
                }

                plc_tag_destroy(tags[i]);
            }

            done = 1;
        }
    }

    end = util_time_ms();

    fprintf(stderr, "Creation of %d tags took %dms.\n", num_tags, (int)(end - start));

    /* read in a loop until ^C pressed */
    while(!done) {
        start = util_time_ms();

        rc = read_tags(tags, statuses, num_tags, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            int need_sleep = 0;

            for(int i=0; i<num_tags; i++) {
                if(statuses[i] != PLCTAG_ERR_TIMEOUT) {
                    fprintf(stderr, "Tag %d read failed with status %s!\n", i, plc_tag_decode_error(statuses[i]));
                    done = 1;
                } else {
                    fprintf(stderr, "Tag %d read failed with a timeout, will retry.\n", i);
                    plc_tag_abort(tags[i]);
                    need_sleep = 1;
                }
            }

            if(need_sleep) {
                need_sleep = 0;
                util_sleep_ms(10); /* give the background thread time to process the abort. */
            }
        }

        end = util_time_ms();

        fprintf(stderr, "Read of %d tags took %dms.\n", num_tags, (int)(end - start));

        /* test */
        //util_sleep_ms(25);
    }

    fprintf(stderr, "Program terminated!\n");

    /* we are done */
    for(i=0; i < num_tags; i++) {
        plc_tag_destroy(tags[i]);
    }

    free(tags);
    free(statuses);

    return 0;
}


int read_tags(int32_t *tags, int *statuses, int num_tags, int timeout_ms)
{
    if(timeout_ms <= 0) {
        fprintf(stderr, "Timeout to read_tags() must be greater than zero!\n");

        return PLCTAG_ERR_BAD_PARAM;
    }

    /* start the read. */
    for(int i=0; i<num_tags; i++) {
        statuses[i] = plc_tag_read(tags[i], 0);

        /* if any failed, we need to abort the request. */
        if(statuses[i] != PLCTAG_STATUS_OK && statuses[i] != PLCTAG_STATUS_PENDING) {
            fprintf(stderr, "1 Calling plc_tag_abort() on tag %d!\n", i);
            plc_tag_abort(tags[i]);
        }
    }

    return wait_for_tags(tags, statuses, num_tags, timeout_ms);
}


int wait_for_tags(int32_t *tags, int *statuses, int num_tags, int timeout_ms)
{
    int64_t end_timeout = (int64_t)timeout_ms + util_time_ms();
    int rc = PLCTAG_STATUS_OK;
    int tags_pending = 0;

    /* the statuses must be primed before calling this function! */
    do {
        /* check the pending tags. */
        tags_pending = 0;
        for(int i=0; i<num_tags; i++) {
            if(statuses[i] == PLCTAG_STATUS_PENDING) {
                statuses[i] = plc_tag_status(tags[i]);

                /* still pending? */
                if(statuses[i] == PLCTAG_STATUS_PENDING) {
                    tags_pending++;
                } else if(statuses[i] != PLCTAG_STATUS_OK) {
                    /* not good, some sort of error! */

                    fprintf(stderr, "Tag %d failed with status %s!\n", i, plc_tag_decode_error(statuses[i]));

                    fprintf(stderr, "2 Calling plc_tag_abort() on tag %d!\n", i);
                    plc_tag_abort(tags[i]);
                }
            }
        }

        /* anything left to do? */
        if(tags_pending > 0) {
            /* yes, there is, delay a bit. */
            util_sleep_ms(1);
        }
    } while(tags_pending > 0 && end_timeout > util_time_ms() && !done);

    rc = PLCTAG_STATUS_OK;

    /* did any tags time out? */
    if(end_timeout <= util_time_ms()) {
        for(int i=0; i<num_tags; i++) {
            if(statuses[i] == PLCTAG_STATUS_PENDING) {
                /* we timed out, so abort and mark the status. */
                fprintf(stderr, "3 Calling plc_tag_abort() on tag %d!\n", i);
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
