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
 * Read a STRING from a PCCC-based PLC.  Note that the actual data size of a string is 84 bytes.
 *
 * STRING types are an INT (2 bytes) followed by 82 bytes of characters.  The character data is
 * byteswapped! Character 0 is at offset 1, character 1 is at offset 0, character 2 is at offset 3,
 * character 4 is at offset 2...
 */

#define REQUIRED_VERSION 2,1,16

#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.38&plc=plc5&elem_size=84&elem_count=2&name=ST18:0"
#define DATA_TIMEOUT 5000

int main()
{
    int32_t tag = 0;
    int elem_size = 0;
    int elem_count = 0;
    int tag_size = 0;
    int rc;
    int i;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
    }

    /* turn off debugging */
    plc_tag_set_debug_level(PLCTAG_DEBUG_NONE);

    /* open the tag handle to the PLC tag */
    tag = plc_tag_create(TAG_PATH, DATA_TIMEOUT);

    /* everything OK? */
    if((rc = plc_tag_status(tag)) != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag internal state. Error %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 0;
    }

    /* read the data from the PLC */
    rc = plc_tag_read(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 0;
    }

    /* 
     * determine how much data we got and how big each element is.
     * 
     * This allows us to change the tag string without changing the code.
     * It is extra boilerplate if you already know this and it will not change.
     */

    tag_size = plc_tag_get_size(tag);
    elem_count = plc_tag_get_int_attribute(tag, "elem_count", 1);
    elem_size = tag_size / elem_count;

    /* print out the data */
    for(i=0; i < elem_count; i++) {
        /* get the string character count, as a INT */
        int str_size = plc_tag_get_int16(tag,(i*elem_size));
        char str[elem_size];
        int j = 0;

        for(j=0; j<str_size; j++) {
            int char_index = (i*elem_size) /* each string has a full buffer. */
                           + 2             /* skip the string length INT */
                           + (j ^ 0x01);   /* byteswap the index. */
            str[j] = (char)plc_tag_get_uint8(tag,char_index);
        }
        str[j] = (char)0; /* null terminate since we are using a stack-based buffer! */

        printf("string %d (%d chars) = '%s'\n",i, str_size, str);
    }

    /* we are done */
    plc_tag_destroy(tag);

    return 0;
}


