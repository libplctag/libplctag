/***************************************************************************
 *   Copyright (C) 2021 by Kyle Hayes                                      *
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


#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include "../lib/libplctag.h"
#include "utils.h"

#define REQUIRED_VERSION 2,1,3

#define TAG_PATH "protocol=ab-eip&gateway=10.206.1.39&path=1,5&cpu=LGX&elem_count=1&name=TestDINTArray&read_cache_ms=100"
#define DATA_TIMEOUT 5000


static void expect_match(int32_t tag, const char *attrib, int match_val);





int main()
{
    int32_t tag;
    int rc = PLCTAG_STATUS_OK;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    /* turn on debugging. */
    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    /* create the tag */
    tag = plc_tag_create(TAG_PATH, DATA_TIMEOUT);

    /* everything OK? */
    if(tag < 0) {
        fprintf(stderr,"ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        exit(1);
    }

    if((rc = plc_tag_status(tag)) != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag internal state. Error %s\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        exit(1);
    }

    printf("Testing library attributes.\n");

    expect_match(0, "debug", PLCTAG_DEBUG_DETAIL);

    printf("\tLibrary version from attributes %d.%d.%d\n",
                    plc_tag_get_int_attribute(0, "version_major", 0),
                    plc_tag_get_int_attribute(0, "version_minor", 0),
                    plc_tag_get_int_attribute(0, "version_patch", 0)
                    );

    printf("Testing generic tag attributes.\n");
    expect_match(tag, "size", 4);
    expect_match(tag, "read_cache_ms", 100);

    printf("Testing protocol-specific attributes.\n");
    expect_match(tag, "elem_size", 4);
    expect_match(tag, "elem_count", 1);

    printf("Testing unknown attribute.\n");
    expect_match(tag, "boodleflokker", INT_MIN);

    /* we are done */
    plc_tag_destroy(tag);

    return 0;
}


void expect_match(int32_t tag, const char *attrib, int match_val)
{
    int val = plc_tag_get_int_attribute(tag, attrib, INT_MIN);

    if(val == match_val) {
        printf("\tOK: attribute \"%s\" = %d.\n", attrib, val);
    } else if(val == INT_MIN) {
        printf("\tError getting tag attribute \"%s\".\n", attrib);
        plc_tag_destroy(tag);
        exit(1);
    } else if(val != match_val) {
        printf("\tFAIL: attribute \"%s\" = %d, expected %d.\n", attrib, val, match_val);
        plc_tag_destroy(tag);
        exit(1);
    }
}



