/***************************************************************************
 *   Copyright (C) 2015 by OmanTek                                         *
 *   Author Kyle Hayes  kylehayes@omantek.com                              *
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
#include "../lib/libplctag.h"
#include "utils.h"

/*
 * This tests the use of simultaneous connected (DH+ to a PLC) and unconnected messages (to an LGX).  The first
 * sets up a connection object and uses different packet formats.  The second uses UCMM and uses the usual
 * unconnected packet format.
 */


#define TAG_PATH1 "protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=LGX&elem_size=4&elem_count=10&name=TestDINTArray&debug=1"
#define TAG_PATH2 "protocol=ab_eip&gateway=10.206.1.39&path=1,2,A:27:1&cpu=plc5&elem_count=4&elem_size=4&name=F8:0&debug=1"
#define DATA_TIMEOUT 5000


int main()
{
    plc_tag tag1 = PLC_TAG_NULL;
    plc_tag tag2 = PLC_TAG_NULL;
    int rc1,rc2;
    int i;

    /* create the tag */
    tag1 = plc_tag_create(TAG_PATH1);
    tag2 = plc_tag_create(TAG_PATH2);

    /* everything OK? */
    if(!tag1) {
        fprintf(stderr,"ERROR: Could not create tag1!\n");

        return 0;
    }

    if(!tag2) {
        fprintf(stderr,"ERROR: Could not create tag2!\n");

        return 0;
    }

    /* wait for tags to finish setting up */
    sleep_ms(1000);

    if(plc_tag_status(tag1) != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag internal state. %s\n", plc_tag_decode_error(plc_tag_status(tag1)));
        return 0;
    }

    if(plc_tag_status(tag2) != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag internal state. %s\n", plc_tag_decode_error(plc_tag_status(tag2)));
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
    sleep_ms(1000);

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

    sleep_ms(1000);

    plc_tag_destroy(tag1);

    sleep_ms(1000);

    return 0;
}
