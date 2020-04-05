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

/*
 * Read an array of 48 STRINGs.  Note that the actual data size of a string is 88 bytes, not 82+4.
 *
 * STRING types are a DINT (4 bytes) followed by 82 bytes of characters.  Then two bytes of padding.
 */

#define REQUIRED_VERSION 2,1,0

#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.27&path=1,0&cpu=LGX&elem_size=88&elem_count=48&debug=1&name=Loc_Txt"
#define ELEM_COUNT 48
#define ELEM_SIZE 88
#define DATA_TIMEOUT 5000


int main()
{
    int32_t tag = 0;
    int rc;
    int i;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
    }

    /* create the tag */
    tag = plc_tag_create(TAG_PATH, DATA_TIMEOUT);

    /* everything OK? */
    if(tag < 0) {
        fprintf(stderr,"ERROR: Could not create tag!\n");
        return 0;
    }

    if((rc = plc_tag_status(tag)) != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag internal state. Error %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 0;
    }

    /* get the data */
    rc = plc_tag_read(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 0;
    }

    /* print out the data */
    for(i=0; i < ELEM_COUNT; i++) {
        int str_size = plc_tag_get_int32(tag,(i*ELEM_SIZE));
        char str[ELEM_SIZE] = {0};
        int j;

        for(j=0; j<str_size; j++) {
            str[j] = (char)plc_tag_get_uint8(tag,(i*ELEM_SIZE)+j+4);
        }
        str[j] = (char)0;

        printf("string %d (%d chars) = '%s'\n",i, str_size, str);
    }

    /* we are done */
    plc_tag_destroy(tag);

    return 0;
}


