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


#define REQUIRED_VERSION 2,1,0

//#define TAG_ATTRIBS "protocol=ab_eip&gateway=10.206.1.40&path=1,4&cpu=LGX&use_connected_msg=0&elem_size=4&elem_count=1&name=TestBigArray[0]&debug=4"
//#define TAG_ATTRIBS "protocol=ab_eip&gateway=10.206.1.38&cpu=PLC5&elem_size=4&elem_count=1&name=F8:0&debug=4"
#define NUM_TAGS  (10)
//#define NUM_ELEMS (1)
#define DATA_TIMEOUT (1000)



void usage(void)
{
    printf("Usage:\n "
        "async_stress <path>\n"
        "  <path> - The path to the device containing the named data.\n"
        "\n"
        "Example: async_stress 'protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=LGX&elem_size=4&elem_count=1&name=test_tag'\n");

    exit(1);
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




int check_tag_status(int32_t tag, int64_t timeout)
{
    int rc = PLCTAG_STATUS_OK;

    rc = plc_tag_status(tag);

    if(rc == PLCTAG_STATUS_OK) {
        return rc;
    } else if(rc != PLCTAG_STATUS_PENDING) {
        return rc;
    } else if(timeout <= util_time_ms()) {
        return PLCTAG_ERR_TIMEOUT;
    }

    return PLCTAG_STATUS_PENDING;
}





int main(int argc, char **argv)
{
    int32_t tag[NUM_TAGS] = {0,};
    int rc = PLCTAG_STATUS_OK;
    int i = 0;
    int64_t timeout = DATA_TIMEOUT + util_time_ms();
    int64_t start = 0;
    int64_t end = 0;
    int tags_done = 0;
    int creation_failed = 0;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    /* check the command line arguments */
    if(argc < 2 || !argv[1] || strlen(argv[1]) == 0) {
        printf("Tag path must not be zero length or missing!\n");
        usage();
    }

    /* set up handler for ^C etc. */
    setup_break_handler();

    fprintf(stderr, "Hit ^C to terminate the test.\n");

    /* create the tags */
    for(i=0; i< NUM_TAGS; i++) {
        tag[i]  = plc_tag_create(argv[1], 0);

        if(tag[i] < 0) {
            fprintf(stderr,"Error %s: could not create tag %d\n",plc_tag_decode_error(tag[i]), i);
        }
    }

    /* wait for all the tags to complete creation. */
    do {
        /* delay a bit. */
        util_sleep_ms(1);

        /* start the count */
        tags_done = 0;

        for(i=0; i < NUM_TAGS; i++) {
            rc = check_tag_status(tag[i], timeout);

            switch(rc) {
                case PLCTAG_STATUS_PENDING: break;
                case PLCTAG_STATUS_OK: tags_done++; break;
                case PLCTAG_ERR_TIMEOUT: creation_failed = 1; fprintf(stderr, "Timeout waiting for tag %d to finish creation!\n", i); break;
                default: creation_failed = 1; fprintf(stderr, "Tag %d returned error on creation %s!\n", i, plc_tag_decode_error(rc)); break;
            }
        }
    } while(tags_done < NUM_TAGS && !done && !creation_failed);

    if(creation_failed || done) {
        fprintf(stderr, "Error waiting for tags to be ready!\n");

        for(i=0; i < NUM_TAGS; i++) {
            plc_tag_destroy(tag[i]);
        }

        return 1;
    }


    /* read in a loop until ^C pressed */
    while(!done) {
        start = util_time_ms();
        timeout = DATA_TIMEOUT + start;

        /* start the read. */
        for(i=0; i < NUM_TAGS; i++) {
            rc = plc_tag_read(tag[i], 0);

            if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
                fprintf(stderr,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));

                return 0;
            }
        }


        /*
         * wait for the tags to finish the read, abort the operation in flight on any tag that
         * has an error or on timeout.
         */
        do {
            /* delay a bit. */
            util_sleep_ms(1);

            /* start the count */
            tags_done = 0;

            for(i=0; i < NUM_TAGS; i++) {
                rc = check_tag_status(tag[i], timeout);
                switch(rc) {
                    case PLCTAG_STATUS_PENDING: break;
                    case PLCTAG_STATUS_OK: tags_done++; break;
                    case PLCTAG_ERR_TIMEOUT: tags_done++; plc_tag_abort(tag[i]); fprintf(stderr, "Error: Timeout waiting for tag %d to finish reading!\n", i); break;
                    default: tags_done++; plc_tag_abort(tag[i]); fprintf(stderr, "Error: Tag %d returned error on read %s!\n", i, plc_tag_decode_error(rc)); break;
                }
            }
        } while(tags_done < NUM_TAGS && !done);

        end = util_time_ms();

        fprintf(stderr, "Read %d tags in %dms\n", NUM_TAGS, (int)(end - start));
    }

    fprintf(stderr, "Program terminated!\n");

    /* we are done */
    for(i=0; i < NUM_TAGS; i++) {
        plc_tag_destroy(tag[i]);
    }

    return 0;
}
