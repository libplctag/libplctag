/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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


#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <stdlib.h>

#define REQUIRED_VERSION 2, 6, 0

/* test against a DINT array. */
#define DATA_TIMEOUT 5000

int test_tag_buffer_errors(const char *tag_name, int32_t tag) {
    int rc = PLCTAG_STATUS_OK;
    uint8_t type_buffer[32];
    int type_size = 0;

    printf("Testing tag %s.\n", tag_name);

    /* get the type size */
    printf("\tTest getting attribute \"raw_tag_type_bytes.length\": ");
    type_size = plc_tag_get_int_attribute(tag, "raw_tag_type_bytes.length", -1);
    if((type_size != 2) && (type_size != 4)) {
        printf("ERROR: expected type byte array reported length to be 2 or 4 bytes, but got %d!\n", type_size);
        return PLCTAG_ERR_BAD_REPLY;
    } else {
        printf("PASSED\n");
    }

    /* test null pointer */
    printf("\tTest with NULL buffer pointer: ");
    rc = plc_tag_get_byte_array_attribute(tag, "raw_tag_type_bytes", NULL, sizeof(type_buffer));
    if(rc != PLCTAG_ERR_BAD_PARAM) {
        printf(
            "ERROR: getting type info with a NULL buffer pointer does not return PLCTAG_ERR_BAD_PARAM but instead returns %s!\n",
            plc_tag_decode_error(rc));
        return PLCTAG_ERR_BAD_REPLY;
    } else {
        printf("PASSED\n");
    }

    /* test zero length buffer */
    printf("\tTest with zero buffer length: ");
    rc = plc_tag_get_byte_array_attribute(tag, "raw_tag_type_bytes", &(type_buffer[0]), 0);
    if(rc != PLCTAG_ERR_BAD_PARAM) {
        printf(
            "ERROR: getting type info with a zero length buffer does not return PLCTAG_ERR_BAD_PARAM but instead returns %s!\n",
            plc_tag_decode_error(rc));
        return PLCTAG_ERR_BAD_REPLY;
    } else {
        printf("PASSED\n");
    }

    /* test exact buffer length */
    printf("\tTest with exact buffer length: ");
    type_size = plc_tag_get_byte_array_attribute(tag, "raw_tag_type_bytes", &(type_buffer[0]), type_size);
    if(type_size < 0) {
        printf("ERROR: got error %s (%d) trying to get the attribute byte array!\n", plc_tag_decode_error(type_size), type_size);
        return PLCTAG_ERR_BAD_REPLY;
    }

    if((type_size != 2) && (type_size != 4)) {
        printf("ERROR: expected type byte array copied length to be 2 or 4 bytes, but got %d!\n", type_size);
        return PLCTAG_ERR_BAD_REPLY;
    } else {
        printf("PASSED\n");
    }

    /* check the type size that comes back when the data is copied */
    printf("\tTest type array size after copy: ");
    type_size =
        plc_tag_get_byte_array_attribute(tag, "raw_tag_type_bytes", &(type_buffer[0]), (int)(unsigned int)sizeof(type_buffer));
    if((type_size != 2) && (type_size != 4)) {
        printf("ERROR: expected type byte array copied length to be 2 or 4 bytes, but got %d!\n", type_size);
        return PLCTAG_ERR_BAD_REPLY;
    } else {
        printf("PASSED\n");
    }

    printf("\tRetrieved tag %s native type bytes: ", tag_name);
    for(int i = 0; i < type_size; i++) { printf(" %02x", (int)(unsigned int)type_buffer[i]); }
    printf("\n");


    return PLCTAG_STATUS_OK;
}


int main(void) {
    int32_t tag1 = 0;
    int32_t tag2 = 0;
    int rc = PLCTAG_STATUS_OK;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        // NOLINTNEXTLINE
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_NONE);

    do {
        /* create the tag */
        tag1 = plc_tag_create("protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_count=1&name=TestBigSINTArray",
                              DATA_TIMEOUT);

        /* everything OK? */
        if(tag1 < 0) {
            // NOLINTNEXTLINE
            fprintf(stderr, "ERROR %s: Could not create tag TestBigSINTArray!\n", plc_tag_decode_error(tag1));
            break;
        }

        /* get the data */
        rc = plc_tag_read(tag1, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            // NOLINTNEXTLINE
            fprintf(stderr, "ERROR: Unable to read the data for TestBigSINTArray! Got error code %d: %s\n", rc,
                    plc_tag_decode_error(rc));
            break;
        }

        tag2 = plc_tag_create(
            "protocol=ab-eip&gateway=10.206.1.40&path=1,4&plc=ControlLogix&elem_count=1&name=TestManyBOOLFields", DATA_TIMEOUT);
        if(tag2 < 0) {
            // NOLINTNEXTLINE
            fprintf(stderr, "ERROR %s: Could not create tag TestManyBOOLFields!\n", plc_tag_decode_error(tag2));
            break;
        }

        /* get the data */
        rc = plc_tag_read(tag2, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            // NOLINTNEXTLINE
            fprintf(stderr, "ERROR: Unable to read the data for TestManyBOOLFields! Got error code %d: %s\n", rc,
                    plc_tag_decode_error(rc));
            break;
        }

        rc = test_tag_buffer_errors("TestBigSINTArray", tag1);
        if(rc != PLCTAG_STATUS_OK) {
            // NOLINTNEXTLINE
            fprintf(stderr, "ERROR: testing for tag buffer errors with a large SINT array failed %s!\n",
                    plc_tag_decode_error(rc));
            break;
        }

        rc = test_tag_buffer_errors("TestManyBOOLFields", tag2);
        if(rc != PLCTAG_STATUS_OK) {
            // NOLINTNEXTLINE
            fprintf(stderr, "ERROR: testing for tag buffer errors with a large BOOL array failed %s!\n",
                    plc_tag_decode_error(rc));
            break;
        }
    } while(0);

    if(tag1 > 0) { plc_tag_destroy(tag1); }
    if(tag2 > 0) { plc_tag_destroy(tag2); }

    return rc;
}
