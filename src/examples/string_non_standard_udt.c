/***************************************************************************
 *   Copyright (C) 2022 by Kyle Hayes                                      *
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
#include <string.h>
#include "../lib/libplctag.h"
#include "utils.h"

/*

This example shows how to access non-standard strings for which you are using a user-defined UDT.   This UDT
must have roughly the same definition as a string, with different lengths.

In this example, we want to read an array of user-defined string-like UDTs.  The array is defined as:

CB_Rpt[19,12] of UDT type CB_Rp.

The UDT CB_Rp is defined as:

UDT CB_Rp (ID 920, 20 bytes, struct handle f639):
    Field 0: LEN, offset 0, type (00c4) DINT: Signed 32-bit integer value.
    Field 1: DATA, offset 4, array [16] of type (20c2) SINT: Signed 8-bit integer value.

We have two parts of a count word of four bytes followed by an array of SINT to hold the character data that is 16 long.

All UDTs in a Control/CompactLogix must be a multiple of 4 bytes in length.   A normal STRING type needs some padding at the
end to maintain this requirement.  In this case, the CB_Rp type is 20 bytes long which is already a multiple of 4 bytes. No
padding is necessary.

We construct the tag string with the special string attributes:

str_count_word_bytes=4 -- the count word is a DINT
str_is_byte_swapped=0 -- the string character bytes are in normal order
str_is_counted=1 -- the string type has a count word
str_is_fixed_length=1 -- the string is a fixed size
str_is_zero_terminated=0 -- the string is not nul-terminated as C strings are
str_max_capacity=16 -- the string can have 16 bytes of characters
str_pad_bytes=0 -- the string has no bytes of padding characters
str_total_length=20 -- the string occupies 20 bytes in the tag data buffer

We add these to the tag string that was constructed by the tag listing program:

"protocol=ab-eip&gateway=10.206.1.39&path=1,0&plc=ControlLogix&elem_size=20&elem_count=228&name=CB_Rpt&str_count_word_bytes=4&str_is_byte_swapped=0&str_is_counted=1&str_is_fixed_length=1&str_is_zero_terminated=0&str_max_capacity=16&str_pad_bytes=0&str_total_length=20"

*/


#define REQUIRED_VERSION 2,2,0

static const char *tag_string = "protocol=ab-eip&gateway=10.206.1.39&path=1,0&plc=ControlLogix&elem_size=20&elem_count=228&name=CB_Rpt&str_count_word_bytes=4&str_is_byte_swapped=0&str_is_counted=1&str_is_fixed_length=1&str_is_zero_terminated=0&str_max_capacity=16&str_pad_bytes=0&str_total_length=20";

#define DATA_TIMEOUT 5000


int main()
{
    int32_t tag = 0;
    int rc;
    int str_num = 1;
    int offset = 0;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        return 1;
    }

    fprintf(stderr, "Using library version %d.%d.%d.\n",
                                            plc_tag_get_int_attribute(0, "version_major", -1),
                                            plc_tag_get_int_attribute(0, "version_minor", -1),
                                            plc_tag_get_int_attribute(0, "version_patch", -1));

    /* turn off debugging output. */
    plc_tag_set_debug_level(PLCTAG_DEBUG_NONE);

    tag = plc_tag_create(tag_string, DATA_TIMEOUT);

    /* everything OK? */
    if((rc = plc_tag_status(tag)) != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error creating tag! Error %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    /* get the data */
    rc = plc_tag_read(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR: Unable to read the data for tag! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    /* print out the data */
    offset = 0;
    str_num = 1;
    /*
     * we could use the element count here because these strings are fixed size, but in the case where
     * they are not, this is the correct way to loop across strings.
     */
    while(offset < plc_tag_get_size(tag)) {
        char *str = NULL;
        int str_size = plc_tag_get_string_length(tag, offset);
        int str_buf_size = str_size + 1; /* +1 for the nul termination character. */

        str = (char *)malloc((size_t)(unsigned int)str_buf_size);
        if(!str) {
            fprintf(stderr, "Unable to allocate memory for the string %d of the tag!\n", str_num);
            plc_tag_destroy(tag);
            return PLCTAG_ERR_NO_MEM;
        }

        /* read the string into the buffer */
        rc = plc_tag_get_string(tag, offset, str, str_buf_size);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr, "Unable to get string %d of tag, got error %s!\n", str_num, plc_tag_decode_error(rc));
            free(str);
            plc_tag_destroy(tag);
            return rc;
        }

        fprintf(stderr, "tag string %d (%u chars) = '%s'\n", str_num, (unsigned int)strlen(str), str);

        free(str);

        str_num++;

        /* this gets the total length of the string in the buffer, including count word, nul termination, padding etc. */
        offset += plc_tag_get_string_total_length(tag, offset);
    }

    /* we are done */
    plc_tag_destroy(tag);

    return 0;
}


