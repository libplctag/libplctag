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

#define REQUIRED_VERSION 2,1,0

#define TAG_PATH "protocol=ab-eip&gateway=10.206.1.39&path=1,5&cpu=LGX&elem_size=4&elem_count=1&name=TestBigArray&debug=4"
//#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.38&cpu=PLC5&elem_size=2&elem_count=1&name=N7:0&debug=4"
#define ELEM_COUNT 1
#define ELEM_SIZE 4
//#define ELEM_SIZE 2
#define DATA_TIMEOUT 5000


int create_tag()
{
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;

    /* create the tag */
    tag = plc_tag_create(TAG_PATH, DATA_TIMEOUT);

    /* everything OK? */
    if(tag < 0) {
        fprintf(stderr,"ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        exit( -tag);
    }

    if((rc = plc_tag_status(tag)) != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag internal state. Error %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        exit( -rc);
    }

    return tag;
}


void update_tag(int32_t tag)
{
    int rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        exit( -rc);
    }

    /* print out the data */
    for(int i=0; i < ELEM_COUNT; i++) {
        fprintf(stderr,"data[%d]=%d\n",i,plc_tag_get_int32(tag,(i*ELEM_SIZE)));
//        fprintf(stderr,"data[%d]=%d\n",i,plc_tag_get_int16(tag,(i*ELEM_SIZE)));
    }

    /* now test a write */
    for(int i=0; i < ELEM_COUNT; i++) {
        int32_t val = plc_tag_get_int32(tag,(i*ELEM_SIZE));
//        int16_t val = plc_tag_get_int16(tag, (i*ELEM_SIZE));

        val = val+1;

        fprintf(stderr,"Setting element %d to %d\n",i,val);

        plc_tag_set_int32(tag,(i*ELEM_SIZE),val);
//        plc_tag_set_int16(tag,(i*ELEM_SIZE),val);
    }

    rc = plc_tag_write(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        exit( -rc);
    }
}





int main(int argc, char **argv)
{
    int32_t tag = 0;
    int wait_time_sec = 0;

    /* check library API version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }


    if(argc == 2) {
        wait_time_sec = atoi(argv[1]);
    } else {
        fprintf(stderr,"Usage: test_reconnect <number of seconds to pause>\n");
        return 1;
    }

    /* create the tag */
    tag = create_tag();

    /* update the data */
    update_tag(tag);

    fprintf(stderr, "Waiting for %dms.\n",(wait_time_sec*1000));
    util_sleep_ms(wait_time_sec * 1000);

    /* update the data again */
    update_tag(tag);

    /* we are done */
    plc_tag_destroy(tag);

    return 0;
}


