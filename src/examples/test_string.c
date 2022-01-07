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
 * Read an array of 48 STRINGs.  Note that the actual data size of a string is 88 bytes, not 82+4.
 *
 * STRING types are a DINT (4 bytes) followed by 82 bytes of characters.  Then two bytes of padding.
 */

#define REQUIRED_VERSION 2,4,10

static const char *tag_string = "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_size=88&elem_count=1&name=TestSSTRING";

#define DATA_TIMEOUT 5000


int main()
{
    int32_t tag = 0;
    int rc;
    int str_num = 1;
    int offset = 0;
    int str_cap = 0;
    char *str = NULL;

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
        fprintf(stderr,"Error %s creating tag!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    /* get the data */
    rc = plc_tag_read(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error %s trying to read tag!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    /* print out the data */
    str_cap = plc_tag_get_string_length(tag, offset) + 1; /* +1 for the zero termination. */
    str = malloc((size_t)(unsigned int)str_cap);
    if(!str) {
        fprintf(stderr, "Unable to allocate memory for the string!\n");
        plc_tag_destroy(tag);
        return PLCTAG_ERR_NO_MEM;
    }

    rc = plc_tag_get_string(tag, offset, str, str_cap);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error %s getting string value!\n", plc_tag_decode_error(rc));
        free(str);
        plc_tag_destroy(tag);
        return rc;
    }

    fprintf(stderr, "tag string data = '%s'\n", str);

    free(str);

    /* now try to overwrite memory */
    str_cap = plc_tag_get_string_capacity(tag, offset) * 2 + 1;
    str = malloc((size_t)(unsigned int)str_cap);
    if(!str) {
        fprintf(stderr, "Unable to allocate memory for the string write test!\n");
        plc_tag_destroy(tag);
        return PLCTAG_ERR_NO_MEM;
    }

    /* clear out the string memory */
    memset(str, 0, str_cap);

    /* fill it in with garbage */
    for(int i=0; i < (str_cap - 1); i++) {
        str[i] = (char)(0x30 + (i % 10)); /* 01234567890123456789... */
    }

    rc = plc_tag_set_string(tag, offset, str);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error %s setting string!\n", plc_tag_decode_error(rc));
        free(str);
        plc_tag_destroy(tag);
        return rc;
    }

    /* try to write it. */
    rc = plc_tag_write(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error %s writing string!\n", plc_tag_decode_error(rc));
        free(str);
        plc_tag_destroy(tag);
        return rc;
    }

    /* we are done */
    free(str);
    plc_tag_destroy(tag);

    return 0;
}


