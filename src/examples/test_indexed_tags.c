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


#include <stdio.h>
#include <stdlib.h>
#include "../lib/libplctag.h"
#include "utils.h"

#define REQUIRED_VERSION 2,6,4

/* test against a DINT array. */
#define TAG_PATH_2_DIM "protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=1&name=Test_2_DimArray_2x3[%d][%d]"
#define TAG_PATH_2_DIM_ALL "protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=6&name=Test_2_DimArray_2x3"
#define TAG_PATH_3_DIM "protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=1&name=Test_3_DimArray_2x3x4[%d][%d][%d]"
#define TAG_PATH_3_DIM_ALL "protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=24&name=Test_3_DimArray_2x3x4"
#define DATA_TIMEOUT 5000


#define Z_DIM (2)
#define Y_DIM (3)
#define X_DIM (4)


int32_t test_2_dim_tag_write_all(void)
{
    int32_t tag = 0;
    int32_t rc = 0;

    /* write the values */
    tag = plc_tag_create(TAG_PATH_2_DIM_ALL, DATA_TIMEOUT);

    if(tag < 0) {
        printf("Failed to create write tag with error %s\n", plc_tag_decode_error(tag));
        return tag;
    }

    for(int i = 0; i < (Z_DIM * Y_DIM); i++) {
        int z = i / Y_DIM;
        int y = i % Y_DIM;
        plc_tag_set_int32(tag, i, 1000 + (10 * z) + y);

        printf("Setting element %d to %d\n", i, 1000 + (10 * z) + y);
    }

    rc = plc_tag_write(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        printf("Failed to write tag with error %s\n", plc_tag_decode_error(rc));
    }

    plc_tag_destroy(tag);

    return rc;
}



int32_t test_2_dim_tag_read_individual(void)
{
    int32_t tag = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    for(int i = 0; i < (Z_DIM * Y_DIM) && rc == PLCTAG_STATUS_OK; i++) {
        char tag_path[strlen(TAG_PATH_2_DIM) + 16]; // 16 for fudge factor
        int z = i / Y_DIM;
        int y = i % Y_DIM;
        int32_t correct_val = 1000 + (10 * z) + y;
        int32_t actual_val = -1;

        snprintf(tag_path, sizeof(tag_path), TAG_PATH_2_DIM, z, y);

        tag = plc_tag_create(tag_path, DATA_TIMEOUT);
        if(tag < 0) {
            printf("Failed to create individual read tag with error %s\n", plc_tag_decode_error(tag));
            return tag;
        }

        actual_val = plc_tag_get_int32(tag, 0);

        if(correct_val == actual_val) {
            printf("Element %d is %d\n", i, actual_val);
        } else {
            printf("Element %d is %d, expected %d\n", i, actual_val, correct_val);
            rc = PLCTAG_ERR_BAD_DATA;
        }

        plc_tag_destroy(tag);
    }


    return rc;
}



int32_t test_2_dim_tag_write_individual(void)
{
    int32_t tag = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    for(int i = 0; i < (Z_DIM * Y_DIM) && rc == PLCTAG_STATUS_OK; i++) {
        char tag_path[strlen(TAG_PATH_2_DIM) + 16]; // 16 for fudge factor
        int z = i / Y_DIM;
        int y = i % Y_DIM;
        int32_t correct_val = 1000 + (10 * z) + y;

        snprintf(tag_path, sizeof(tag_path), TAG_PATH_2_DIM, z, y);

        tag = plc_tag_create(tag_path, DATA_TIMEOUT);
        if(tag < 0) {
            printf("Failed to create individual read tag with error %s\n", plc_tag_decode_error(tag));
            return tag;
        }

        plc_tag_set_int32(tag, 0, correct_val);

        printf("Setting element [%d][%d] to %d\n", z, y, correct_val);

        rc = plc_tag_write(tag, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) {
            printf("Failed to write tag with error %s\n", plc_tag_decode_error(rc));
        }

        plc_tag_destroy(tag);
    }

    return rc;
}




int32_t test_2_dim_tag_read_all(void)
{
    int32_t tag = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    /* write the values */
    tag = plc_tag_create(TAG_PATH_2_DIM_ALL, DATA_TIMEOUT);

    if(tag < 0) {
        printf("Failed to create read all tag with error %s\n", plc_tag_decode_error(tag));
        return tag;
    }

    rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        printf("Failed to read tag with error %s\n", plc_tag_decode_error(rc));
    } else {
        for(int i = 0; i < (Z_DIM * Y_DIM) && rc == PLCTAG_STATUS_OK; i++) {
                int z = i / Y_DIM;
                int y = i % Y_DIM;
                int32_t correct_val = 1000 + (10 * z) + y;
                int32_t actual_val = plc_tag_get_int32(tag, i*4);

                if( correct_val == actual_val) {
                    printf("Element %d is %d\n", i, actual_val);
                } else {
                    printf("Element %d is %d, expected %d\n", i, actual_val, correct_val);
                    rc = PLCTAG_ERR_BAD_DATA;
                }
        }
    }

    plc_tag_destroy(tag);

    return rc;
}




int main()
{
    int32_t tag = 0;
    int32_t rc;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /* test writing all elements */
    rc = test_2_dim_tag_write_all();
    if(rc != PLCTAG_STATUS_OK) {
        printf("Failed to write all elements with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }

    /* test reading one at a time */
    rc = test_2_dim_tag_read_individual();
    if(rc != PLCTAG_STATUS_OK) {
        printf("Failed to read individual elements with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }


    /* clean up array */
    tag = plc_tag_create(TAG_PATH_2_DIM_ALL, DATA_TIMEOUT);
    if(tag < 0) {
        printf("Failed to create clean up tag with error %s\n", plc_tag_decode_error(tag));
        exit(1);
    }

    for(int i = 0; i < (Z_DIM * Y_DIM); i++) {
        plc_tag_set_int32(tag, i * 4, 0);
    }

    rc = plc_tag_write(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        printf("Failed to write tag with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }


    /* test writing individual elements */
    rc = test_2_dim_tag_write_individual();
    if(rc != PLCTAG_STATUS_OK) {
        printf("Failed to write individual elements with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }

    /* test read individual elements */
    rc = test_2_dim_tag_read_all();
    if(rc != PLCTAG_STATUS_OK) {
        printf("Failed to read all elements with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }

    return 0;
}
