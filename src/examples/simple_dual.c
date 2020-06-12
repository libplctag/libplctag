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
#include <stdlib.h>
#include "../lib/libplctag.h"
#include "utils.h"

/*
 * This tests the use of simultaneous connected (DH+ to a PLC) and unconnected messages (to an LGX).  The first
 * sets up a connection object and uses different packet formats.  The second uses UCMM and uses the usual
 * unconnected packet format.
 */

#define REQUIRED_VERSION 2,1,0

#define TAG_PATH1 "protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=LGX&elem_size=4&elem_count=10&name=TestDINTArray&debug=1"
#define TAG_PATH2 "protocol=ab_eip&gateway=10.206.1.39&path=1,2,A:27:1&cpu=plc5&elem_count=4&elem_size=4&name=F8:0&debug=1"
#define DATA_TIMEOUT 5000


int main()
{
    int32_t tag1 = 0;
    int32_t tag2 = 0;
    int rc1,rc2;
    int i;
    int done = 0;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    /* create the tag, async */
    tag1 = plc_tag_create(TAG_PATH1, 0);
    tag2 = plc_tag_create(TAG_PATH2, 0);

    /* everything OK? */
    if(tag1 < 0) {
        fprintf(stderr,"ERROR: Could not create tag1!\n");
        return 0;
    }

    if(tag2 < 0) {
        fprintf(stderr,"ERROR: Could not create tag2!\n");
        return 0;
    }

    /* wait for tags to finish setting up */
    util_sleep_ms(1000);

    do {
        done = 1;
        rc1 = plc_tag_status(tag1);
        rc2 = plc_tag_status(tag2);

        if(rc1 == PLCTAG_STATUS_PENDING) {
            done = 0;
        }

        if(rc2 == PLCTAG_STATUS_PENDING) {
            done = 0;
        }

        if(!done) {
            util_sleep_ms(5);
        }
    } while(!done);

    if(rc1 != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag internal state. %s\n", plc_tag_decode_error(rc1));
        return 0;
    }

    if(rc2 != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag internal state. %s\n", plc_tag_decode_error(rc2));
        return 0;
    }

    /* get the data */
    rc2 = plc_tag_read(tag2, 0);
    rc1 = plc_tag_read(tag1, 0);

    if(rc1 != PLCTAG_STATUS_OK && rc1 != PLCTAG_STATUS_PENDING) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d: %s\n",rc1, plc_tag_decode_error(rc1));
        return 0;
    }

    if(rc2 != PLCTAG_STATUS_OK && rc2 != PLCTAG_STATUS_PENDING) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d: %s\n",rc2, plc_tag_decode_error(rc2));
        return 0;
    }

    /* let the reads complete */
    util_sleep_ms(1000);

    rc1 = plc_tag_status(tag1);
    rc2 = plc_tag_status(tag2);

    if(rc1 != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the tag 1 data! Got error code %d: %s\n",rc1, plc_tag_decode_error(rc1));

        return 0;
    }

    if(rc2 != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the tag 2 data! Got error code %d: %s\n",rc2, plc_tag_decode_error(rc2));

        return 0;
    }

    /* print out the data */
    for(i=0; i < 10; i++) {
        fprintf(stderr,"tag 1 data[%d]=%d\n",i,plc_tag_get_int32(tag1,(i*4)));
    }

    /* print out the data */
    for(i=0; i < 4; i++) {
        fprintf(stderr,"tag 2 data[%d]=%f\n",i,plc_tag_get_float32(tag2,(i*4)));
    }

    /* we are done, clean up tag 2 first */
    plc_tag_destroy(tag2);
    plc_tag_destroy(tag1);

    return 0;
}
