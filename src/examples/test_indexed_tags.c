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

#define REQUIRED_VERSION 2, 6, 4

/* test against a DINT array. */
#define TAG_PATH_MAX (256)
#define TAG_PATH_1_DIM "protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=1&name=Test_Array_1[%d]"
#define TAG_PATH_1_DIM_ALL "protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=1000&name=Test_Array_1"
#define TAG_PATH_2_DIM "protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=1&name=Test_Array_2x3[%d][%d]"
#define TAG_PATH_2_DIM_ALL "protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=6&name=Test_Array_2x3"
#define TAG_PATH_3_DIM "protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=1&name=Test_Array_2x3x4[%d][%d][%d]"
#define TAG_PATH_3_DIM_ALL "protocol=ab-eip&gateway=127.0.0.1&path=1,0&cpu=LGX&elem_count=24&name=Test_Array_2x3x4"
#define DATA_TIMEOUT 5000


#define Test_Array_1_DIM_ELEMENT_COUNT (1000)

#define Z_DIM (2)
#define Y_DIM (3)
#define X_DIM (4)


int32_t test_1_dim_tag_write_all(void) {
    int32_t tag = 0;
    int32_t rc = 0;

    printf("      1D Test: writing all elements at once.\n");

    /* write the values */
    tag = plc_tag_create(TAG_PATH_1_DIM_ALL, DATA_TIMEOUT);

    if(tag < 0) {
        printf("      !!!! Failed to create write tag with error %s\n", plc_tag_decode_error(tag));
        return tag;
    }

    for(int i = 0; i < Test_Array_1_DIM_ELEMENT_COUNT; i++) {
        int val = (Test_Array_1_DIM_ELEMENT_COUNT - 1) - i;
        plc_tag_set_int32(tag, i * 4, val);
        printf("        Setting element %d to %d\n", i, val);
    }

    rc = plc_tag_write(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) { printf("      !!!! Failed to write tag with error %s\n", plc_tag_decode_error(rc)); }

    plc_tag_destroy(tag);

    return rc;
}


int32_t test_1_dim_tag_read_individual(void) {
    int32_t tag = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    printf("      1D Test: reading individual elements.\n");

    for(int i = 0; i < Test_Array_1_DIM_ELEMENT_COUNT && rc == PLCTAG_STATUS_OK; i++) {
        char tag_path[TAG_PATH_MAX];
        int32_t correct_val = (Test_Array_1_DIM_ELEMENT_COUNT - 1) - i;
        int32_t actual_val = -1;

        // NOLINTNEXTLINE
        snprintf(tag_path, sizeof(tag_path), TAG_PATH_1_DIM, i);

        tag = plc_tag_create(tag_path, DATA_TIMEOUT);
        if(tag < 0) {
            printf("      !!!! Failed to create individual read tag with error %s\n", plc_tag_decode_error(tag));
            return tag;
        }

        actual_val = plc_tag_get_int32(tag, 0);

        if(correct_val == actual_val) {
            printf("        Checking individual element %d is %d\n", i, actual_val);
        } else {
            printf("        !! Element %d is %d, expected %d\n", i, actual_val, correct_val);
            rc = PLCTAG_ERR_BAD_DATA;
        }

        plc_tag_destroy(tag);
    }

    return rc;
}


int32_t test_1_dim_tag_write_individual(void) {
    int32_t tag = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    printf("      1D Test: writing individual elements.\n");

    for(int i = 0; i < Test_Array_1_DIM_ELEMENT_COUNT && rc == PLCTAG_STATUS_OK; i++) {
        char tag_path[TAG_PATH_MAX];
        int32_t correct_val = (Test_Array_1_DIM_ELEMENT_COUNT - 1) - i;

        // NOLINTNEXTLINE
        snprintf(tag_path, sizeof(tag_path), TAG_PATH_1_DIM, i);

        tag = plc_tag_create(tag_path, DATA_TIMEOUT);
        if(tag < 0) {
            printf("      !!!! Failed to create individual read tag with error %s\n", plc_tag_decode_error(tag));
            return tag;
        }

        plc_tag_set_int32(tag, 0, correct_val);

        printf("        Setting individual element %d to %d\n", i, correct_val);

        rc = plc_tag_write(tag, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) { printf("        !! Failed to write tag with error %s\n", plc_tag_decode_error(rc)); }

        plc_tag_destroy(tag);
    }

    return rc;
}

int32_t test_1_dim_tag_read_all(void) {
    int32_t tag = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    printf("      1D Test: reading all elements at once.\n");

    /* write the values */
    tag = plc_tag_create(TAG_PATH_1_DIM_ALL, DATA_TIMEOUT);

    if(tag < 0) {
        printf("      !!!! Failed to create read all tag with error %s\n", plc_tag_decode_error(tag));
        return tag;
    }

    rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        printf("      !!!! Failed to read tag with error %s\n", plc_tag_decode_error(rc));
    } else {
        for(int i = 0; i < Test_Array_1_DIM_ELEMENT_COUNT && rc == PLCTAG_STATUS_OK; i++) {
            int32_t correct_val = (Test_Array_1_DIM_ELEMENT_COUNT - 1) - i;
            int32_t actual_val = plc_tag_get_int32(tag, i * 4);

            if(correct_val == actual_val) {
                printf("        Checking element %d is %d\n", i, actual_val);
            } else {
                printf("        !! Element %d is %d, expected %d\n", i, actual_val, correct_val);
                rc = PLCTAG_ERR_BAD_DATA;
            }
        }
    }

    plc_tag_destroy(tag);

    return rc;
}


void cleanup_test_1_dim(void) {
    printf("      1D Test: cleaning up test array.\n");

    int32_t tag = plc_tag_create(TAG_PATH_1_DIM_ALL, DATA_TIMEOUT);
    if(tag < 0) {
        printf("        !!!! Failed to create clean up tag with error %s\n", plc_tag_decode_error(tag));
        exit(1);
    }

    for(int i = 0; i < Test_Array_1_DIM_ELEMENT_COUNT; i++) { plc_tag_set_int32(tag, i * 4, 0); }

    int32_t rc = plc_tag_write(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        printf("         !!!! Failed to write cleanup tag with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }

    plc_tag_destroy(tag);
}


int32_t test_2_dim_tag_write_all(void) {
    int32_t tag = 0;
    int32_t rc = 0;

    printf("      2D Test: writing all elements at once.\n");

    /* write the values */
    tag = plc_tag_create(TAG_PATH_2_DIM_ALL, DATA_TIMEOUT);
    if(tag < 0) {
        printf("      !!!! Failed to create write tag with error %s\n", plc_tag_decode_error(tag));
        return tag;
    }

    for(int i = 0; i < (Z_DIM * Y_DIM); i++) {
        int z = i / Y_DIM;
        int y = i % Y_DIM;

        plc_tag_set_int32(tag, i * 4, 1000 + (10 * z) + y);

        printf("        Setting element %d [%d][%d] to %d\n", i, z, y, 1000 + (10 * z) + y);
    }

    rc = plc_tag_write(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) { printf("        !!!! Failed to write tag with error %s\n", plc_tag_decode_error(rc)); }

    plc_tag_destroy(tag);

    return rc;
}


int32_t test_2_dim_tag_read_individual(void) {
    int32_t tag = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    printf("      2D Test: reading individual elements.\n");

    for(int i = 0; i < (Z_DIM * Y_DIM) && rc == PLCTAG_STATUS_OK; i++) {
        char tag_path[TAG_PATH_MAX];
        int z = i / Y_DIM;
        int y = i % Y_DIM;
        int32_t correct_val = 1000 + (10 * z) + y;
        int32_t actual_val = -1;

        // NOLINTNEXTLINE
        snprintf(tag_path, sizeof(tag_path), TAG_PATH_2_DIM, z, y);

        tag = plc_tag_create(tag_path, DATA_TIMEOUT);
        if(tag < 0) {
            printf("      !!!! Failed to create individual read tag with error %s\n", plc_tag_decode_error(tag));
            return tag;
        }

        actual_val = plc_tag_get_int32(tag, 0);

        if(correct_val == actual_val) {
            printf("        Checking individual element %d [%d][%d] is %d\n", i, z, y, actual_val);
        } else {
            printf("        !! Eslement %d [%d][%d] is %d, expected %d\n", i, z, y, actual_val, correct_val);
            rc = PLCTAG_ERR_BAD_DATA;
        }

        plc_tag_destroy(tag);
    }


    return rc;
}


int32_t test_2_dim_tag_write_individual(void) {
    int32_t tag = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    printf("      2D Test: writing individual elements.\n");

    for(int i = 0; i < (Z_DIM * Y_DIM) && rc == PLCTAG_STATUS_OK; i++) {
        char tag_path[TAG_PATH_MAX];
        int z = i / Y_DIM;
        int y = i % Y_DIM;
        int32_t correct_val = 1000 + (10 * z) + y;

        // NOLINTNEXTLINE
        snprintf(tag_path, sizeof(tag_path), TAG_PATH_2_DIM, z, y);

        tag = plc_tag_create(tag_path, DATA_TIMEOUT);
        if(tag < 0) {
            printf("        !!!! Failed to create individual read tag with error %s\n", plc_tag_decode_error(tag));
            return tag;
        }

        plc_tag_set_int32(tag, 0, correct_val);

        printf("        Setting individual element %d [%d][%d] to %d\n", i, z, y, correct_val);

        rc = plc_tag_write(tag, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) { printf("        !! Failed to write tag with error %s\n", plc_tag_decode_error(rc)); }

        plc_tag_destroy(tag);
    }

    return rc;
}


int32_t test_2_dim_tag_read_all(void) {
    int32_t tag = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    printf("      2D Test: reading all elements at once.\n");

    /* write the values */
    tag = plc_tag_create(TAG_PATH_2_DIM_ALL, DATA_TIMEOUT);
    if(tag < 0) {
        printf("      !!!! Failed to create read all tag with error %s\n", plc_tag_decode_error(tag));
        return tag;
    }

    rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        printf("      !!!! Failed to read tag with error %s\n", plc_tag_decode_error(rc));
    } else {
        for(int i = 0; i < (Z_DIM * Y_DIM) && rc == PLCTAG_STATUS_OK; i++) {
            int z = i / Y_DIM;
            int y = i % Y_DIM;
            int32_t correct_val = 1000 + (10 * z) + y;
            int32_t actual_val = plc_tag_get_int32(tag, i * 4);

            if(correct_val == actual_val) {
                printf("        Checking element %d [%d][%d] is %d\n", i, z, y, actual_val);
            } else {
                printf("        !! Element %d [%d][%d] is %d, expected %d\n", i, z, y, actual_val, correct_val);
                rc = PLCTAG_ERR_BAD_DATA;
            }
        }
    }

    plc_tag_destroy(tag);

    return rc;
}


void cleanup_test_2_dim(void) {
    printf("      2D Test: cleaning up test array.\n");

    int32_t tag = plc_tag_create(TAG_PATH_2_DIM_ALL, DATA_TIMEOUT);
    if(tag < 0) {
        printf("        !!!! Failed to create clean up tag with error %s\n", plc_tag_decode_error(tag));
        exit(1);
    }

    for(int i = 0; i < (Z_DIM * Y_DIM); i++) { plc_tag_set_int32(tag, i * 4, 0); }

    int32_t rc = plc_tag_write(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        printf("         !!!! Failed to write cleanup tag with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }

    plc_tag_destroy(tag);
}


int32_t test_3_dim_tag_write_all(void) {
    int32_t tag = 0;
    int32_t rc = 0;

    printf("      3D Test: writing all elements at once.\n");

    /* write the values */
    tag = plc_tag_create(TAG_PATH_3_DIM_ALL, DATA_TIMEOUT);

    if(tag < 0) {
        printf("        !!!! Failed to create write tag with error %s\n", plc_tag_decode_error(tag));
        return tag;
    }

    for(int i = 0; i < (Z_DIM * Y_DIM * X_DIM); i++) {
        int z = i / (Y_DIM * X_DIM);
        int y = (i / X_DIM) % Y_DIM;
        int x = i % X_DIM;
        int val = 10000 + (100 * z) + (10 * y) + x;

        plc_tag_set_int32(tag, i * 4, val);

        printf("        Setting element %d [%d][%d][%x] to %d\n", i, z, y, x, val);
    }

    rc = plc_tag_write(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) { printf("        !!!! Failed to write tag with error %s\n", plc_tag_decode_error(rc)); }

    plc_tag_destroy(tag);

    return rc;
}


int32_t test_3_dim_tag_read_individual(void) {
    int32_t tag = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    printf("      3D Test: reading individual elements.\n");

    for(int i = 0; i < (Z_DIM * Y_DIM * X_DIM) && rc == PLCTAG_STATUS_OK; i++) {
        char tag_path[TAG_PATH_MAX];
        int z = i / (Y_DIM * X_DIM);
        int y = (i / X_DIM) % Y_DIM;
        int x = i % X_DIM;
        int correct_val = 10000 + (100 * z) + (10 * y) + x;
        int32_t actual_val = -1;

        // NOLINTNEXTLINE
        snprintf(tag_path, sizeof(tag_path), TAG_PATH_3_DIM, z, y, x);

        tag = plc_tag_create(tag_path, DATA_TIMEOUT);
        if(tag < 0) {
            printf("        !!!! Failed to create individual read tag with error %s\n", plc_tag_decode_error(tag));
            return tag;
        }

        actual_val = plc_tag_get_int32(tag, 0);

        if(correct_val == actual_val) {
            printf("        Checking individual element %d [%d][%d][%d] is %d\n", i, z, y, x, actual_val);
        } else {
            printf("        !! Element %d [%d][%d][%d] is %d, expected %d\n", i, z, y, x, actual_val, correct_val);
            rc = PLCTAG_ERR_BAD_DATA;
        }

        plc_tag_destroy(tag);
    }


    return rc;
}


int32_t test_3_dim_tag_write_individual(void) {
    int32_t tag = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    printf("      3D Test: writing individual elements.\n");

    for(int i = 0; i < (Z_DIM * Y_DIM * X_DIM) && rc == PLCTAG_STATUS_OK; i++) {
        char tag_path[TAG_PATH_MAX];
        int z = i / (Y_DIM * X_DIM);
        int y = (i / X_DIM) % Y_DIM;
        int x = i % X_DIM;
        int correct_val = 10000 + (100 * z) + (10 * y) + x;

        // NOLINTNEXTLINE
        snprintf(tag_path, sizeof(tag_path), TAG_PATH_3_DIM, z, y, x);

        tag = plc_tag_create(tag_path, DATA_TIMEOUT);
        if(tag < 0) {
            printf("        !!!! Failed to create individual read tag with error %s\n", plc_tag_decode_error(tag));
            return tag;
        }

        plc_tag_set_int32(tag, 0, correct_val);

        printf("        Setting individual element %d [%d][%d][%d] to %d\n", i, z, y, x, correct_val);

        rc = plc_tag_write(tag, DATA_TIMEOUT);
        if(rc != PLCTAG_STATUS_OK) { printf("        !!!! Failed to write tag with error %s\n", plc_tag_decode_error(rc)); }

        plc_tag_destroy(tag);
    }

    return rc;
}


int32_t test_3_dim_tag_read_all(void) {
    int32_t tag = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    printf("      3D Test: reading all elements at once.\n");

    printf("        Using tag path %s\n", TAG_PATH_3_DIM_ALL);

    /* write the values */
    tag = plc_tag_create(TAG_PATH_3_DIM_ALL, DATA_TIMEOUT);

    if(tag < 0) {
        printf("        !!!! Failed to create read all tag with error %s\n", plc_tag_decode_error(tag));
        return tag;
    }

    rc = plc_tag_read(tag, DATA_TIMEOUT);

    if(rc != PLCTAG_STATUS_OK) {
        printf("        !!!! Failed to read tag with error %s\n", plc_tag_decode_error(rc));
    } else {
        for(int i = 0; i < (Z_DIM * Y_DIM * X_DIM) && rc == PLCTAG_STATUS_OK; i++) {
            int z = i / (Y_DIM * X_DIM);
            int y = (i / X_DIM) % Y_DIM;
            int x = i % X_DIM;
            int correct_val = 10000 + (100 * z) + (10 * y) + x;
            int32_t actual_val = plc_tag_get_int32(tag, i * 4);

            if(correct_val == actual_val) {
                printf("        Checking element %d [%d][%d][%d] is %d\n", i, z, y, x, actual_val);
            } else {
                printf("        !! Element %d [%d][%d][%d] is %d, expected %d\n", i, z, y, x, actual_val, correct_val);
                rc = PLCTAG_ERR_BAD_DATA;
            }
        }
    }

    plc_tag_destroy(tag);

    return rc;
}


void cleanup_test_3_dim(void) {
    printf("      3D Test: cleaning up test array.\n");

    int32_t tag = plc_tag_create(TAG_PATH_3_DIM_ALL, DATA_TIMEOUT);
    if(tag < 0) {
        printf("        !!!! Failed to create clean up tag with error %s\n", plc_tag_decode_error(tag));
        exit(1);
    }

    for(int i = 0; i < (Z_DIM * Y_DIM * X_DIM); i++) { plc_tag_set_int32(tag, i * 4, 0); }

    int32_t rc = plc_tag_write(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        printf("        !!!! Failed to write cleanup tag with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }

    plc_tag_destroy(tag);
}


int main(void) {
    int32_t rc;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_WARN);


    printf("**** Testing 1-dimensional array tags ****\n");

    /* Make sure array is clean first */
    cleanup_test_1_dim();

    /* test writing all elements */
    rc = test_1_dim_tag_write_all();
    if(rc != PLCTAG_STATUS_OK) {
        printf("Failed to write all elements with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }

    /* test reading one at a time */
    rc = test_1_dim_tag_read_individual();
    if(rc != PLCTAG_STATUS_OK) {
        printf("Failed to read individual elements with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }

    /* make sure array is clean for next tests */
    cleanup_test_1_dim();

    /* test writing individual elements */
    rc = test_1_dim_tag_write_individual();
    if(rc != PLCTAG_STATUS_OK) {
        printf("Failed to write individual elements with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }

    /* test read individual elements */
    rc = test_1_dim_tag_read_all();
    if(rc != PLCTAG_STATUS_OK) {
        printf("Failed to read all elements with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }


    printf("\n**** Testing 2-dimensional array tags ****\n");

    /* Make sure array is clean first */
    cleanup_test_2_dim();

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

    /* make sure array is clean for next tests */
    cleanup_test_2_dim();

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

    printf("\n**** Testing 3-dimensional array tags ****\n");

    /* clean up array first */
    cleanup_test_3_dim();

    /* test writing all elements */
    rc = test_3_dim_tag_write_all();
    if(rc != PLCTAG_STATUS_OK) {
        printf("Failed to write all elements with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }

    /* test reading one at a time */
    rc = test_3_dim_tag_read_individual();
    if(rc != PLCTAG_STATUS_OK) {
        printf("Failed to read individual elements with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }

    /* clean up array */
    cleanup_test_3_dim();

    /* test writing individual elements */
    rc = test_3_dim_tag_write_individual();
    if(rc != PLCTAG_STATUS_OK) {
        printf("Failed to write individual elements with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }

    /* test read individual elements */
    rc = test_3_dim_tag_read_all();
    if(rc != PLCTAG_STATUS_OK) {
        printf("Failed to read all elements with error %s\n", plc_tag_decode_error(rc));
        exit(1);
    }

    printf("**** All tests passed. ****\n");

    return 0;
}
