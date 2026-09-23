/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
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

/*
 * Non-timing coverage of the attributes the library core owns: the values that can be read
 * at runtime, the one that can also be written at runtime, and the error returned when an
 * attribute is accessed in a direction it does not support.
 *
 * The timing-dependent core attributes, auto_sync_read_ms and auto_sync_write_ms, have
 * their own tests.
 */

#include "compat_utils.h"
#include <libplctag/lib/libplctag.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRED_VERSION 2, 6, 0

#define DATA_TIMEOUT (5000)

#define DEFAULT_TAG "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name=TestBigArray[0]"
#define DEFAULT_BIT_TAG "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&name=TestBigArray[0].7"
#define DEFAULT_GROUP (3)

static int32_t failures = 0;


static void check_get(int32_t tag, const char *attrib, int32_t expected) {
    int32_t actual = plc_tag_get_int_attribute(tag, attrib, INT_MIN);

    if(actual == expected) {
        printf("\tOK: get \"%s\" = %d.\n", attrib, (int)actual);
    } else {
        printf("\tFAIL: get \"%s\" = %d, expected %d.\n", attrib, (int)actual, (int)expected);
        failures++;
    }
}


static void check_set(int32_t tag, const char *attrib, int32_t value, int32_t expected_rc) {
    int32_t rc = plc_tag_set_int_attribute(tag, attrib, value);

    if(rc == expected_rc) {
        printf("\tOK: set \"%s\" = %d returned %s.\n", attrib, (int)value, plc_tag_decode_error(rc));
    } else {
        printf("\tFAIL: set \"%s\" = %d returned %s, expected %s.\n", attrib, (int)value, plc_tag_decode_error(rc),
               plc_tag_decode_error(expected_rc));
        failures++;
    }
}


static void check_attr_size(int32_t tag, const char *attrib, int32_t expected) {
    int32_t actual = plc_tag_get_attribute_size(tag, attrib);

    if(actual == expected) {
        printf("\tOK: size of \"%s\" = %d.\n", attrib, (int)actual);
    } else {
        printf("\tFAIL: size of \"%s\" = %d, expected %d.\n", attrib, (int)actual, (int)expected);
        failures++;
    }
}


static int32_t open_tag(const char *tag_string) {
    int32_t tag = plc_tag_create(tag_string, DATA_TIMEOUT);

    if(tag < 0) {
        printf("Unable to create tag \"%s\": %s!\n", tag_string, plc_tag_decode_error(tag));
        exit(1);
    }

    return tag;
}


int main(void) {
    char group_tag_string[512] = {0};
    int32_t tag = 0;
    int32_t bit_tag = 0;
    int32_t group_tag = 0;

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        printf("Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    tag = open_tag(DEFAULT_TAG);

    printf("Read-only core attributes.\n");

    /* the tag is a single DINT element. */
    check_get(tag, "size", 4);

    /* not a bit tag, so the bit number is zero. */
    check_get(tag, "bit_num", 0);

    /* no connection_group_id in the tag string, so it defaults to zero. */
    check_get(tag, "connection_group_id", 0);

    printf("Read-only core attributes reject a write.\n");

    check_set(tag, "size", 8, PLCTAG_ERR_UNSUPPORTED);
    check_set(tag, "bit_num", 3, PLCTAG_ERR_UNSUPPORTED);
    check_set(tag, "connection_group_id", 1, PLCTAG_ERR_UNSUPPORTED);

    printf("read_cache_ms round trips at runtime.\n");

    check_set(tag, "read_cache_ms", 250, PLCTAG_STATUS_OK);
    check_get(tag, "read_cache_ms", 250);
    check_set(tag, "read_cache_ms", 0, PLCTAG_STATUS_OK);
    check_get(tag, "read_cache_ms", 0);
    check_set(tag, "read_cache_ms", -1, PLCTAG_ERR_OUT_OF_BOUNDS);

    /* the rejected write must not have changed the value. */
    check_get(tag, "read_cache_ms", 0);

    printf("allow_field_resize round trips at runtime.\n");

    check_set(tag, "allow_field_resize", 1, PLCTAG_STATUS_OK);
    check_get(tag, "allow_field_resize", 1);
    check_set(tag, "allow_field_resize", 0, PLCTAG_STATUS_OK);
    check_get(tag, "allow_field_resize", 0);

    /* any positive value means enabled, and it reads back normalized to one. */
    check_set(tag, "allow_field_resize", 42, PLCTAG_STATUS_OK);
    check_get(tag, "allow_field_resize", 1);

    printf("Attribute sizes.\n");

    /* every core attribute holds an integer. */
    check_attr_size(tag, "size", (int32_t)sizeof(int32_t));
    check_attr_size(tag, "read_cache_ms", (int32_t)sizeof(int32_t));
    check_attr_size(tag, "allow_field_resize", (int32_t)sizeof(int32_t));

    /* an unknown name has no size, and a bad argument is rejected before the lookup. */
    check_attr_size(tag, "boodleflokker", PLCTAG_ERR_UNSUPPORTED);
    check_attr_size(tag, "", PLCTAG_ERR_BAD_PARAM);

    printf("An unknown attribute is reported, not silently defaulted.\n");

    check_get(tag, "boodleflokker", INT_MIN);
    check_set(tag, "boodleflokker", 1, PLCTAG_ERR_UNSUPPORTED);

    plc_tag_destroy(tag);

    printf("bit_num reflects the bit the tag was created with.\n");

    bit_tag = open_tag(DEFAULT_BIT_TAG);
    check_get(bit_tag, "bit_num", 7);
    plc_tag_destroy(bit_tag);

    printf("connection_group_id reflects the group the tag was created with.\n");

    compat_snprintf(group_tag_string, sizeof(group_tag_string), "%s&connection_group_id=%d", DEFAULT_TAG, DEFAULT_GROUP);

    group_tag = open_tag(group_tag_string);
    check_get(group_tag, "connection_group_id", DEFAULT_GROUP);
    check_set(group_tag, "connection_group_id", DEFAULT_GROUP + 1, PLCTAG_ERR_UNSUPPORTED);
    check_get(group_tag, "connection_group_id", DEFAULT_GROUP);
    plc_tag_destroy(group_tag);

    if(failures) {
        printf("FAILED with %d failing checks.\n", (int)failures);
        return 1;
    }

    printf("PASSED\n");

    return 0;
}
