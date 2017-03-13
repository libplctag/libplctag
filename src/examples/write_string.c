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
#include <string.h>
#include "../lib/libplctag.h"
#include "utils.h"

/*
 * This example shows how to read and write an array of strings.
 */


#define TAG_PATH "protocol=ab_eip&gateway=10.206.1.27&path=1,0&cpu=LGX&elem_size=88&elem_count=6&debug=1&name=Loc_Txt"
#define ARRAY_1_DIM_SIZE (48)
#define ARRAY_2_DIM_SIZE (6)
#define STRING_DATA_SIZE (82)
#define ELEM_COUNT 1
#define ELEM_SIZE 88
#define DATA_TIMEOUT 1000



int dump_strings(plc_tag tag)
{
    char str_data[STRING_DATA_SIZE];
    int str_index;
    int str_len;
    int num_strings = plc_tag_get_size(tag) / ELEM_SIZE;
    int i;

    /* loop over the whole thing. */
    for(i=0; i< num_strings; i++) {
        /* get the string length */
        str_len = plc_tag_get_int32(tag,i * ELEM_SIZE);

        /* copy the data */
        for(str_index=0; str_index<str_len; str_index++) {
            str_data[str_index] = (char)plc_tag_get_uint8(tag,(i*ELEM_SIZE)+4+str_index);
        }

        /* pad with zeros */
        for(;str_index<STRING_DATA_SIZE; str_index++) {
            str_data[str_index] = 0;
        }

        printf("String [%d] = \"%s\"\n",i,str_data);
    }

    return 0;

}



void update_string(plc_tag tag, int i, char *str)
{
    int str_len;
    int base_offset = i * ELEM_SIZE;
    int str_index;

    /* now write the data */
    str_len = strlen(str);

    /* set the length */
    plc_tag_set_int32(tag, base_offset, str_len);

    /* copy the data */
    for(str_index=0; str_index < str_len && str_index < STRING_DATA_SIZE; str_index++) {
        plc_tag_set_uint8(tag,base_offset + 4 + str_index, str[str_index]);
    }

    /* pad with zeros */
    for(;str_index<STRING_DATA_SIZE; str_index++) {
        plc_tag_set_uint8(tag,base_offset + 4 + str_index, 0);
    }
}



int main()
{
    int i;
    char str[STRING_DATA_SIZE] = {0};
    plc_tag tag = plc_tag_create(TAG_PATH, DATA_TIMEOUT);
    int rc;

    if(tag < 0) {
        fprintf(stdout,"ERROR: Unable to create tag! error %s\n", plc_tag_decode_error(tag));
        return 0;
    }

    /* test pre-read by writing first */
    for(i=0; i<ARRAY_2_DIM_SIZE; i++) {
        snprintf_platform(str,sizeof(str), "string value for element %d", i);
        update_string(tag, i, str);
    }

    /* write the data */
    rc = plc_tag_write(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stdout,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        return 0;
    }

    /* get the data */
    rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stdout,"ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        return 0;
    }

    dump_strings(tag);

    plc_tag_destroy(tag);

    return 0;
}


