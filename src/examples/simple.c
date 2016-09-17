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
#include <unistd.h>
#include "../lib/libplctag.h"


#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.39&path=1,0&cpu=LGX&elem_size=4&elem_count=10&name=TestDINTArray[0]&debug=4"
#define ELEM_COUNT 10
#define ELEM_SIZE 4
#define DATA_TIMEOUT 5000


int main(int argc, char **argv)
{
    plc_tag tag = PLC_TAG_NULL;
    int rc;
    int i;

    /* create the tag */
    tag = plc_tag_create(TAG_PATH);

    /* everything OK? */
    if(!tag) {
        fprintf(stderr,"ERROR: Could not create tag!\n");

        return 0;
    }

    /* let the connect succeed we hope */
    while(plc_tag_status(tag) == PLCTAG_STATUS_PENDING) {
        sleep(1);
    }

    if(plc_tag_status(tag) != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag internal state.\n");
        return 0;
    }

    /* get the data */
    rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d\n",rc);

        return 0;
    }

    /* print out the data */
    for(i=0; i < ELEM_COUNT; i++) {
        fprintf(stderr,"data[%d]=%d\n",i,plc_tag_get_int32(tag,(i*ELEM_SIZE)));
    }

    /* now test a write */
    for(i=0; i < ELEM_COUNT; i++) {
        int32_t val = plc_tag_get_int32(tag,(i*ELEM_SIZE));

        val = val+1;

        fprintf(stderr,"Setting element %d to %d\n",i,val);

        plc_tag_set_int32(tag,(i*ELEM_SIZE),val);
    }

    rc = plc_tag_write(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d\n",rc);

        return 0;
    }


    /* get the data again*/
    rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d\n",rc);

        return 0;
    }

    /* print out the data */
    for(i=0; i < ELEM_COUNT; i++) {
        fprintf(stderr,"data[%d]=%d\n",i,plc_tag_get_int32(tag,(i*ELEM_SIZE)));
    }

    /* we are done */
    plc_tag_destroy(tag);

    return 0;
}


