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
#define ARRAY_1_DIM_SIZE (48)
#define ARRAY_2_DIM_SIZE (6)
#define STRING_DATA_SIZE (82)
#define ELEM_COUNT 1
#define ELEM_SIZE 88
#define DATA_TIMEOUT 5000



int32_t create_tag(const char *path)
{
    int rc = PLCTAG_STATUS_OK;
    int32_t tag = 0;

    tag = plc_tag_create(path, DATA_TIMEOUT);

    if((rc = plc_tag_status(tag)) != PLCTAG_STATUS_OK) {
        fprintf(stderr,"ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return rc;
    }

    return tag;
}



int dump_strings(int32_t tag)
{
    char str_data[STRING_DATA_SIZE];
    int str_index;
    int str_len;
    int str_number = 0;
    int offset = 0;
    int tag_size = plc_tag_get_size(tag);

    /* loop over the whole thing. */
    offset = 0;
    while(offset < tag_size) {
        str_number++;

        /* get the string length */
        str_len = plc_tag_get_string_length(tag, offset);
        if(str_len < 0) {
            fprintf(stderr, "Got error getting string length of string %d, error %s.\n", str_number, plc_tag_decode_error(str_len));
            break;
        }

        /* clamp */
        if(str_len >= STRING_DATA_SIZE) {
            str_len = STRING_DATA_SIZE - 1;
        }

        /* copy the data */
        for(str_index=0; str_index<str_len; str_index++) {
            str_data[str_index] = (char)plc_tag_get_string_char(tag, offset, str_index);
        }
        str_data[str_index] = (char)0;

        printf("String [%d] = \"%s\"\n", str_number, str_data);

        offset += plc_tag_get_string_total_length(tag, offset);
    }

    return 0;

}



void update_string(int32_t tag, int str_number, char *str)
{
    int rc = 0;
    int str_len = (int)strlen(str);
    int str_index;
    int str_total_length = plc_tag_get_string_total_length(tag, 0); /* assume all are the same size */
    int str_capacity = plc_tag_get_string_capacity(tag, 0);

    /* 
     * first zero out the whole string. 
     * 
     * In order to access a character in a string, it needs to be
     * within the string length.   So set the string length to
     * the largest possible amount.
     */

    /* set the string length to the maximum. */
    rc = plc_tag_set_string_length(tag, str_number * str_total_length, str_capacity);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error setting string length of string %d, error %s!\n", str_number, plc_tag_decode_error(rc));
        return;
    }

    for(str_index = 0;str_index < str_capacity; str_index++) {
        rc = plc_tag_set_string_char(tag, str_number * str_total_length, str_index, 0);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr, "Error zeroing character %d of string %d, error %s!\n", str_index, str_number, plc_tag_decode_error(rc));
            return;
        }
    }

    /* copy in the new string. */

    /* clamp before we do anything else. */
    if(str_len > str_capacity) {
        str_len = str_capacity;
    }

    /* set the length to the length of the string we are inserting. */
    rc = plc_tag_set_string_length(tag, str_number * str_total_length, str_len);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error setting string length of string %d, error %s!\n", str_number, plc_tag_decode_error(rc));
        return;
    }

    /* copy the data */
    for(str_index=0; str_index < str_len; str_index++) {
        rc = plc_tag_set_string_char(tag, str_number * str_total_length, str_index, (int)str[str_index]);
        if(rc != PLCTAG_STATUS_OK) {
            fprintf(stderr, "Error setting character %d of string %d, error %s!\n", str_index, str_number, plc_tag_decode_error(rc));
            return;
        }
    }
}



int main()
{
    int i;
    char str[STRING_DATA_SIZE] = {0};
    int32_t tag = 0; 
    int rc;

    /* check library API version */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    fprintf(stderr, "Using library version %d.%d.%d.\n", 
                                            plc_tag_get_int_attribute(0, "version_major", -1),
                                            plc_tag_get_int_attribute(0, "version_minor", -1),
                                            plc_tag_get_int_attribute(0, "version_patch", -1));


    /* set up the RNG */
    srand((unsigned int)(uint64_t)util_time_ms());

    /* create the tag. */
    if((tag = create_tag(TAG_PATH)) < 0) {
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

    /* test pre-read by writing first */
    for(i=0; i<ARRAY_2_DIM_SIZE; i++) {
        snprintf_platform(str,sizeof(str), "string value for element %d is %d.", i, (int)(rand() % 1000));
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


