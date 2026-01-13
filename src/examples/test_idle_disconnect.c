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


#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 1, 10
#define DATA_TIMEOUT 5000


static int delay_seconds = 0;
static char *tag_path = NULL;


static void parse_args(int argc, char **argv) {
    if(argc < 3) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Usage: test_idle_disconnect --delay=N --tag=TAG_STRING\n");
        // NOLINTNEXTLINE
        fprintf(stderr, "  --delay=N: number of seconds to delay between reads\n");
        // NOLINTNEXTLINE
        fprintf(stderr, "  --tag=TAG_STRING: tag path string\n");
        exit(1);
    }

    for(int i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--delay=", 8) == 0) {
            delay_seconds = atoi(&argv[i][8]);
        } else if(strncmp(argv[i], "--tag=", 6) == 0) {
            tag_path = &argv[i][6];
        }
    }

    if(delay_seconds <= 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error: delay must be a positive number of seconds\n");
        exit(1);
    }

    if(tag_path == NULL || strlen(tag_path) == 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error: tag path must be specified\n");
        exit(1);
    }
}


static int32_t create_tag(void) {
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;

    /* create the tag */
    tag = plc_tag_create(tag_path, DATA_TIMEOUT);

    /* everything OK? */
    if(tag < 0) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        exit(-tag);
    }

    if((rc = plc_tag_status(tag)) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Error setting up tag internal state. Error %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        exit(-rc);
    }

    return tag;
}


static const char *status_to_string(int status) {
    switch(status) {
        case PLCTAG_CONN_STATUS_UP:
            return "UP";
        case PLCTAG_CONN_STATUS_DOWN:
            return "DOWN";
        case PLCTAG_CONN_STATUS_CONNECTING:
            return "CONNECTING";
        case PLCTAG_CONN_STATUS_DISCONNECTING:
            return "DISCONNECTING";
        case PLCTAG_CONN_STATUS_WAIT:
            return "WAIT";
        default:
            return "UNKNOWN";
    }
}


static void read_tag(int32_t tag) {
    int rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Unable to read the data! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        exit(-rc);
    }

    // NOLINTNEXTLINE
    fprintf(stderr, "Read successful\n");
}


int main(int argc, char **argv) {
    int32_t tag = 0;

    /* check library API version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    parse_args(argc, argv);

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /* create the tag */
    tag = create_tag();

    /* perform initial read */
    read_tag(tag);

    /* check connection status before delay */
    int status_before_delay = plc_tag_get_int_attribute(tag, "connection_status", PLCTAG_CONN_STATUS_DOWN);
    // NOLINTNEXTLINE
    fprintf(stderr, "Connection status before delay: %s\n", status_to_string(status_before_delay));

    /* wait for idle timeout */
    // NOLINTNEXTLINE
    fprintf(stderr, "Waiting %d seconds for idle disconnect...\n", delay_seconds);
    int remaining_ms = delay_seconds * 1000;
    while(remaining_ms > 0) {
        int sleep_ms = (remaining_ms > 500) ? 500 : remaining_ms;
        compat_sleep_ms((uint32_t)sleep_ms, NULL);
        remaining_ms -= sleep_ms;
    }

    /* check connection status after delay */
    int status_after_delay = plc_tag_get_int_attribute(tag, "connection_status", PLCTAG_CONN_STATUS_DOWN);
    // NOLINTNEXTLINE
    fprintf(stderr, "Connection status after delay: %s\n", status_to_string(status_after_delay));

    /* perform second read after idle period */
    // NOLINTNEXTLINE
    fprintf(stderr, "Performing read after idle period...\n");
    read_tag(tag);

    /* check connection status after second read */
    int status_after_read = plc_tag_get_int_attribute(tag, "connection_status", PLCTAG_CONN_STATUS_DOWN);
    // NOLINTNEXTLINE
    fprintf(stderr, "Connection status after second read: %s\n", status_to_string(status_after_read));

    /* validate connection status transition */
    if((status_after_delay == PLCTAG_CONN_STATUS_WAIT || status_after_delay == PLCTAG_CONN_STATUS_DOWN) &&
       status_after_read != PLCTAG_CONN_STATUS_UP) {
        // NOLINTNEXTLINE
        fprintf(stderr, "ERROR: Connection should be UP after read, but is %s\n", status_to_string(status_after_read));
        plc_tag_destroy(tag);
        exit(1);
    }

    /* clean up */
    plc_tag_destroy(tag);

    return 0;
}
