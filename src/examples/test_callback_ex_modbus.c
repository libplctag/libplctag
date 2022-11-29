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

#include "../lib/libplctag.h"
#include "utils.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

#define REQUIRED_VERSION 2, 5, 0

#define TAG_PATH "protocol=modbus-tcp&gateway=127.0.0.1:5020&path=0&elem_count=10&name=hr1"
#define DATA_TIMEOUT 5000

typedef int16_t TAG_ELEMENT;


void tag_callback(int32_t tag_id, int event, int status, void *userdata)
{
    TAG_ELEMENT *data = (TAG_ELEMENT *)userdata;

    /* handle the events. */
    switch(event) {
        case PLCTAG_EVENT_ABORTED:
            printf("Tag operation was aborted with status %s!\n", plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_CREATED:
            printf("Tag created with status %s.\n", plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_DESTROYED:
            printf("Tag was destroyed with status %s.\n", plc_tag_decode_error(status));
            if(data) {
                free((void *)data);
            }
            break;

        case PLCTAG_EVENT_READ_COMPLETED:
            if(status == PLCTAG_STATUS_OK && data) {
                int elem_count = plc_tag_get_int_attribute(tag_id, "elem_count", -1);
                int elem_size = plc_tag_get_int_attribute(tag_id, "elem_size", 0);

                for(int i = 0; i < elem_count; i++) {
                    data[i] = plc_tag_get_int16(tag_id, (i * elem_size));
                }
            }

            printf("Tag read operation completed with status %s.\n", plc_tag_decode_error(status));

            break;

        case PLCTAG_EVENT_READ_STARTED:
            printf("Tag read operation started with status %s.\n", plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_WRITE_COMPLETED:
            printf("Tag write operation completed with status %s!\n", plc_tag_decode_error(status));
            break;

        case PLCTAG_EVENT_WRITE_STARTED:
            if(status == PLCTAG_STATUS_OK && data) {
                int elem_count = plc_tag_get_int_attribute(tag_id, "elem_count", -1);
                int elem_size = plc_tag_get_int_attribute(tag_id, "elem_size", 0);

                for(int i = 0; i < elem_count; i++) {
                    plc_tag_set_int16(tag_id, (i * elem_size), data[i]);
                }
            }

            printf("Tag write operation started with status %s.\n", plc_tag_decode_error(status));

            break;

        default:
            printf("Unexpected event %d!\n", event);
            break;
    }
}



void wait_for_ok(int32_t tag) 
{
    int rc = PLCTAG_STATUS_OK;

    fprintf(stderr, "wait_for_ok() starting.\n");

    rc = plc_tag_status(tag);
    while(rc == PLCTAG_STATUS_PENDING) {
        util_sleep_ms(10);
        rc = plc_tag_status(tag);
    }

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "wait_for_ok(): Error %s returned on tag operation.!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        exit(1);
    }

    fprintf(stderr, "wait_for_ok() done.\n");
}


int main(int argc, const char **argv)
{
    int32_t tag = 0;
    int rc;
    int i;
    int elem_count = 10;
    int elem_size = 2;
    int64_t start = 0;
    int64_t end = 0;
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);
    TAG_ELEMENT *tag_element_array = NULL;

    (void)argc;
    (void)argv;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available, found %d.%d.%d!\n", REQUIRED_VERSION, version_major, version_minor, version_patch);
        return 1;
    }

    printf("Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    tag_element_array = (TAG_ELEMENT *)calloc((size_t)elem_size, (size_t)elem_count);
    if(!tag_element_array) {
        printf("Unable to allocate memory for tag array!\n");
        return 1;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /* create the tag */
    printf("Creating test tag.\n");
    tag = plc_tag_create_ex(TAG_PATH, tag_callback, tag_element_array, 0);
    if(tag < 0) {
        printf("ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return 1;
    }

    printf("Waiting for tag creation to complete.\n");
    wait_for_ok(tag);

    /* get the data */
    printf("Reading tag data.\n");
    rc = plc_tag_read(tag, 0);
    if(rc != PLCTAG_STATUS_PENDING) {
        printf("ERROR: Unable to read the data! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    printf("Waiting for tag read to complete.\n");
    wait_for_ok(tag);

    /* print out the data */
    for(i = 0; i < elem_count; i++) {
        printf("data[%d]=%d\n", i, tag_element_array[i]);
    }

    /* now test a write */
    for(i = 0; i < elem_count; i++) {
        tag_element_array[i]++;
    }

    printf("Writing tag data.\n");
    rc = plc_tag_write(tag, 0);
    if(rc != PLCTAG_STATUS_PENDING) {
        printf("ERROR: Unable to read the data! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    printf("Waiting for tag write to complete.\n");
    wait_for_ok(tag);

    /* get the data again*/
    printf("Reading tag data.\n");
    rc = plc_tag_read(tag, 0);
    if(rc != PLCTAG_STATUS_PENDING) {
        printf("ERROR: Unable to read the data! Got error code %d: %s\n", rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    printf("Waiting for tag read to complete.\n");
    wait_for_ok(tag);

    /* print out the data */
    for(i = 0; i < elem_count; i++) {
        printf("data[%d]=%d\n", i, tag_element_array[i]);
    }

    /*
     * we do not need to free the array tag_element_array because the callback will do
     * it when the tag is destroyed.
     */
    plc_tag_destroy(tag);

    printf("SUCCESS!\n");

    return 0;
}
