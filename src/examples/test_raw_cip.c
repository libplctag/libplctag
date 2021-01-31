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


#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "../lib/libplctag.h"
#include "utils.h"

#define REQUIRED_VERSION 2,2,1

#define TAG_PATH "protocol=ab-eip2&gateway=10.206.1.40&path=1,4&plc=ControlLogix&name=@raw"
#define DATA_TIMEOUT 5000


int main()
{
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;
    int size = 0;
    int version_major = plc_tag_get_int_attribute(0, "version_major", 0);
    int version_minor = plc_tag_get_int_attribute(0, "version_minor", 0);
    int version_patch = plc_tag_get_int_attribute(0, "version_patch", 0);
    uint8_t raw_payload[] = { 0x55,
                              0x03,
                              0x20,
                              0x6b,
                              0x25,
                              0x00,
                              0x00,
                              0x00,
                              0x04,
                              0x00,
                              0x02,
                              0x00,
                              0x07,
                              0x00,
                              0x08,
                              0x00,
                              0x01,
                              0x00 };

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available, found %d.%d.%d!\n", REQUIRED_VERSION, version_major, version_minor, version_patch);
        return 1;
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    printf("Starting with library version %d.%d.%d.\n", version_major, version_minor, version_patch);

    /* create the tag */
    tag = plc_tag_create(TAG_PATH, DATA_TIMEOUT);
    if(tag < 0) {
        printf("ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return 1;
    }

    /* force the tag size. */
    rc = plc_tag_set_int_attribute(tag, "payload_size", (int)(unsigned int)sizeof(raw_payload));
    if(rc != PLCTAG_STATUS_OK) {
        printf( "Unable to set the payload size on the tag %s!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    /* set up the raw data */
    for(int i=0; i < (int)(unsigned int)sizeof(raw_payload) && rc == PLCTAG_STATUS_OK; i++) {
        rc = plc_tag_set_uint8(tag, i, raw_payload[i]);
    }

    if(rc != PLCTAG_STATUS_OK) {
        printf( "Unable to set the payload data in the tag %s!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    /* get the data */
    rc = plc_tag_read(tag, DATA_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        printf("ERROR: Unable to read the data! Got error code %d: %s\n",rc, plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return 1;
    }

    /* get the size of the returned data. */
    size = plc_tag_get_int_attribute(tag, "payload_size", -1);
    if(size <= 0) {
        printf("ERROR: Unable to get the data size!\n");
        plc_tag_destroy(tag);
        return 1;
    }

    /* print out the data */
    for(int i=0; i < size; i++) {
        uint8_t data = plc_tag_get_uint8(tag, i);
        printf("data[%d]=%u (%x)\n",i, (unsigned int)data, (unsigned int)data);
    }

    plc_tag_destroy(tag);

    printf("SUCCESS!\n");

    return 0;
}


