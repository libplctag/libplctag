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
#include <stdlib.h>
#include "../lib/libplctag.h"
#include "utils.h"

#define REQUIRED_VERSION 2,1,4

/* test against a DINT array. */
#define TAG_PATH "protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=10&name=TestBigArray"
#define DATA_TIMEOUT 5000

int main()
{
    int32_t tag = 0;
    int rc;
    int i;
    int elem_size = 0;
    int elem_count = 0;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    /* create the tag */
    tag = plc_tag_create(TAG_PATH, DATA_TIMEOUT);

    /* everything OK? */
    if(tag < 0) {
        fprintf(stderr,"ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return 1;
    }

    /* get the data */
    rc = plc_tag_read(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    /* get the tag size and element size. Do this _AFTER_ reading the tag otherwise we may not know how big the tag is! */
    elem_size = plc_tag_get_int_attribute(tag, "elem_size", 0);
    elem_count = plc_tag_get_int_attribute(tag, "elem_count", 0);

    fprintf(stderr,"Tag has %d elements each of %d bytes.\n", elem_count, elem_size);

    /* print out the data */
    for(i=0; i < elem_count; i++) {
        fprintf(stderr,"data[%d]=%d\n",i,plc_tag_get_int32(tag,(i*elem_size)));
    }

    /* now test a write */
    for(i=0; i < elem_count; i++) {
        int32_t val = plc_tag_get_int32(tag,(i*elem_size));

        val = val+1;

        fprintf(stderr,"Setting element %d to %d\n",i,val);

        plc_tag_set_int32(tag,(i*elem_size),val);
    }

    rc = plc_tag_write(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to write the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    /* get the data again*/
    rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    /* print out the data */
    for(i=0; i < elem_count; i++) {
        fprintf(stderr,"data[%d]=%d\n",i,plc_tag_get_int32(tag,(i*elem_size)));
    }

    /* we are done */
    plc_tag_destroy(tag);

    return 0;
}


