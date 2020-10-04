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

#define REQUIRED_VERSION 2,1,19

// #define TAG_PATH1 "protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=LGX&elem_size=4&elem_count=10&name=TestDINTArray&debug=1"
// #define TAG_PATH2 "protocol=ab_eip&gateway=10.206.1.39&path=1,2,A:27:1&cpu=plc5&elem_count=4&elem_size=4&name=F8:0&debug=1"

#define TAG_PATH2 "protocol=ab_eip&gateway=10.206.1.38&path=0&plc=plc5&elem_size=4&elem_count=1&name=F8:0"
#define TAG_PATH1 "protocol=ab_eip&gateway=10.206.1.38&plc=plc5&elem_size=2&elem_count=1&name=N7:0"


#define DATA_TIMEOUT 1000


int main()
{
    int32_t tag1 = 0;
    int32_t tag2 = 0;
    int rc1,rc2;
    int i;
    int elem_size = 0;
    int elem_count = 0;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    /* set debug level if we need it. */
    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /* create the tag, async */
    tag1 = plc_tag_create(TAG_PATH1, 0);
    tag2 = plc_tag_create(TAG_PATH2, 0);

    /* everything OK? */
    if(plc_tag_status(tag1) != PLCTAG_STATUS_OK && plc_tag_status(tag1) != PLCTAG_STATUS_PENDING) {
        fprintf(stderr,"ERROR, %s: Could not create tag 1!\n", plc_tag_decode_error(plc_tag_status(tag1)));
        plc_tag_destroy(tag1);
        plc_tag_destroy(tag2);
        return plc_tag_status(tag1);
    }

    if(plc_tag_status(tag2) != PLCTAG_STATUS_OK && plc_tag_status(tag2) != PLCTAG_STATUS_PENDING) {
        fprintf(stderr,"ERROR, %s: Could not create tag 2!\n", plc_tag_decode_error(plc_tag_status(tag2)));
        plc_tag_destroy(tag1);
        plc_tag_destroy(tag2);
        return plc_tag_status(tag2);
    }

    /* brute force wait for tags to finish setting up */
    util_sleep_ms(DATA_TIMEOUT);

    rc1 = plc_tag_status(tag1);
    rc2 = plc_tag_status(tag2);

    if(rc1 != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag 1 internal state. %s\n", plc_tag_decode_error(rc1));
        plc_tag_destroy(tag1);
        plc_tag_destroy(tag2);
        return rc1;
    }

    if(rc2 != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag 2 internal state. %s\n", plc_tag_decode_error(rc2));
        plc_tag_destroy(tag1);
        plc_tag_destroy(tag2);
        return rc2;
    }

    /* get the data */
    rc2 = plc_tag_read(tag2, 0);
    rc1 = plc_tag_read(tag1, 0);

    if(rc1 != PLCTAG_STATUS_OK && rc1 != PLCTAG_STATUS_PENDING) {
        fprintf(stderr,"ERROR: Unable to start reading the tag 1 data! Got error code %d: %s\n",rc1, plc_tag_decode_error(rc1));
        plc_tag_destroy(tag1);
        plc_tag_destroy(tag2);
        return rc1;
    }

    if(rc2 != PLCTAG_STATUS_OK && rc2 != PLCTAG_STATUS_PENDING) {
        fprintf(stderr,"ERROR: Unable to start reading the tag 2 data! Got error code %d: %s\n",rc2, plc_tag_decode_error(rc2));
        plc_tag_destroy(tag1);
        plc_tag_destroy(tag2);
        return rc2;
    }

    /* let the reads complete */
    util_sleep_ms(DATA_TIMEOUT);

    rc1 = plc_tag_status(tag1);
    rc2 = plc_tag_status(tag2);

    if(rc1 != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the tag 1 data! Got error code %d: %s\n",rc1, plc_tag_decode_error(rc1));
        plc_tag_destroy(tag1);
        plc_tag_destroy(tag2);
        return rc1;
    }

    if(rc2 != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the tag 2 data! Got error code %d: %s\n",rc2, plc_tag_decode_error(rc2));
        plc_tag_destroy(tag1);
        plc_tag_destroy(tag2);
        return rc2;
    }


    /* print out the data for tag 1 */
    elem_count = plc_tag_get_int_attribute(tag1, "elem_count", 0);
    if(elem_count == 0) {
        fprintf(stderr, "Tag element count is zero!\n");
        plc_tag_destroy(tag1);
        plc_tag_destroy(tag2);
        return PLCTAG_ERR_NO_DATA;
    }

    elem_size = plc_tag_get_size(tag1)/elem_count;

    for(i=0; i < elem_count; i++) {
        switch(elem_size) {
            case 1:
                fprintf(stderr,"tag 1 data[%d]=%d\n",i,plc_tag_get_int8(tag1,(i*1)));
                break;

            case 2:
                fprintf(stderr,"tag 1 data[%d]=%d\n",i,plc_tag_get_int16(tag1,(i*2)));
                break;

            case 4:
                fprintf(stderr,"tag 1 data[%d]=%f\n",i,plc_tag_get_float32(tag1,(i*4)));
                break;

            default:
                fprintf(stderr, "Unsupported size %d!", elem_size);
                plc_tag_destroy(tag1);
                plc_tag_destroy(tag2);
                return PLCTAG_ERR_NO_DATA;
                break;
        }
    }

    /* print out the data for tag 2 */
    elem_count = plc_tag_get_int_attribute(tag2, "elem_count", 0);
    if(elem_count == 0) {
        fprintf(stderr, "Tag element count is zero!\n");
        plc_tag_destroy(tag1);
        plc_tag_destroy(tag2);
        return PLCTAG_ERR_NO_DATA;
    }

    elem_size = plc_tag_get_size(tag2)/elem_count;

    for(i=0; i < elem_count; i++) {
        switch(elem_size) {
            case 1:
                fprintf(stderr,"tag 2 data[%d]=%d\n",i,plc_tag_get_int8(tag2,(i*1)));
                break;

            case 2:
                fprintf(stderr,"tag 2 data[%d]=%d\n",i,plc_tag_get_int16(tag2,(i*2)));
                break;

            case 4:
                fprintf(stderr,"tag 2 data[%d]=%f\n",i,plc_tag_get_float32(tag2,(i*4)));
                break;

            default:
                fprintf(stderr, "Unsupported size %d!", elem_size);
                plc_tag_destroy(tag1);
                plc_tag_destroy(tag2);
                return PLCTAG_ERR_NO_DATA;
                break;
        }
    }

    /* we are done, clean up tag 2 first as order does not matter */
    plc_tag_destroy(tag2);
    plc_tag_destroy(tag1);

    return 0;
}
