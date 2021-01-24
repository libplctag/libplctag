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
#include <string.h>
#include "../lib/libplctag.h"
#include "utils.h"

/*
 * This example shows how to read and write an array of strings.
 */

#define REQUIRED_VERSION 2,2,0

#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.39&path=1,0&plc=ControlLogix&elem_count=6&name=Loc_Txt"
//#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.38&plc=plc5&elem_size=84&elem_count=2&name=ST18:0"

#define STRING_DATA_SIZE (82)
#define DATA_TIMEOUT 5000



int dump_strings(int32_t tag)
{
    int str_number = 0;
    int offset = 0;
    int tag_size = plc_tag_get_size(tag);

    /* loop over the whole thing. */
    offset = 0;
    while(offset < tag_size) {
        int rc = PLCTAG_STATUS_OK;

        /*
         * This is not a high-performance way to do this.   Allocation
         * is a relatively heavy operation.   Ideally you would get the string
         * size in advance.  However, this will work regardless of the size
         * of each string.
         */
        int str_cap = plc_tag_get_string_capacity(tag, offset) + 1;
        char *str_data = malloc((size_t)(unsigned int)str_cap);

        if(!str_data) {
            fprintf(stderr, "Unable to allocate buffer for string data!\n");
            return PLCTAG_ERR_NO_MEM;
        }

        str_number++;

        /* get the string length */
        rc = plc_tag_get_string(tag, offset, str_data, str_cap);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr, "Error getting string %d!  Got error status %s.\n", str_number, plc_tag_decode_error(rc));
        } else {
            printf("String [%d] = \"%s\"\n", str_number, str_data);
        }

        free(str_data);

        offset += plc_tag_get_string_total_length(tag, offset);
    }

    return 0;

}



void update_string(int32_t tag, int str_number, char *str)
{
    int rc = 0;
    int str_total_length = plc_tag_get_string_total_length(tag, 0); /* assume all are the same size */

    rc = plc_tag_set_string(tag, str_total_length * str_number, str);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error setting string %d, error %s!\n", str_number, plc_tag_decode_error(rc));
        return;
    }
}



int main()
{
    int rc = PLCTAG_STATUS_OK;
    char str[STRING_DATA_SIZE+1] = {0};
    int32_t tag = 0;
    int string_count = 0;

    /* check library API version */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    fprintf(stderr, "Using library version %d.%d.%d.\n",
                                            plc_tag_get_int_attribute(0, "version_major", -1),
                                            plc_tag_get_int_attribute(0, "version_minor", -1),
                                            plc_tag_get_int_attribute(0, "version_patch", -1));

    /* set up debugging output. */
    plc_tag_set_debug_level(PLCTAG_DEBUG_NONE);

    /* set up the RNG */
    srand((unsigned int)(uint64_t)util_time_ms());

    /* create the tag. */
    if((tag = plc_tag_create(TAG_PATH, DATA_TIMEOUT)) < 0) {
        fprintf(stderr,"ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return 0;
    }

    /* get the data */
    rc = plc_tag_read(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stdout,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        return 0;
    }

    /* dump the "before" state. */
    printf("Strings before update:\n");
    dump_strings(tag);

    /* how many strings do we have? */
    string_count = plc_tag_get_int_attribute(tag, "elem_count", 1);

    /* update the string. */
    for(int i=0; i < string_count; i++) {
        snprintf_platform(str, sizeof(str), "string value for element %d is %d.", i, (int)(rand() % 1000));
        update_string(tag, i, str);
    }

    /* write the data */
    rc = plc_tag_write(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stdout,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        return 0;
    }

    /* get the data again */
    rc = plc_tag_read(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stdout,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        return 0;
    }

    /* dump the "after" state */
    printf("\nStrings after update:\n");
    dump_strings(tag);

    plc_tag_destroy(tag);

    return 0;
}


