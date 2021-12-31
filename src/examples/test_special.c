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


#include <stdio.h>
#include <stdlib.h>
#include "../lib/libplctag.h"
#include "utils.h"

#define REQUIRED_VERSION 2,1,0

#define TAG_CREATE_TIMEOUT (100)

int test_version(void)
{
    int32_t tag = 0;
    int rc = PLCTAG_STATUS_OK;
    char ver[16] = {0,};

    fprintf(stderr,"Testing version tag.\n");

    tag = plc_tag_create("make=system&family=library&name=version&debug=4", TAG_CREATE_TIMEOUT);
    if(tag < 0) {
        fprintf(stderr,"ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return tag;
    }

    plc_tag_read(tag, 0);

    rc = plc_tag_get_string(tag, 0, ver, (int)(unsigned int)sizeof(ver));
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "ERROR: %s: Could not get version string!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    fprintf(stderr,"Library version %s\n", ver);

    plc_tag_destroy(tag);

    return PLCTAG_STATUS_OK;
}



int test_debug(void)
{
    int rc = PLCTAG_STATUS_OK;
    int32_t tag = 0;
    uint32_t old_debug, new_debug;

    fprintf(stderr,"Testing debug tag.\n");

    tag = plc_tag_create("make=system&family=library&name=debug&debug=4", TAG_CREATE_TIMEOUT);
    if(tag < 0) {
        fprintf(stderr,"ERROR %s: Could not create tag!\n", plc_tag_decode_error(tag));
        return tag;
    }

    rc = plc_tag_read(tag, 0);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error %s trying to read debug level!\n", plc_tag_decode_error(rc));
        return rc;
    }

    old_debug = plc_tag_get_uint32(tag,0);

    fprintf(stderr,"Current debug level is %d\n",old_debug);

    new_debug = (old_debug == 3 ? 4 : 3);

    fprintf(stderr,"Changing debug level to %d\n", new_debug);

    plc_tag_set_uint32(tag, 0, new_debug);

    rc = plc_tag_write(tag, 0);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error %s trying to write debug level!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    rc = plc_tag_read(tag, 0);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error %s trying to read debug level!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    new_debug = plc_tag_get_uint32(tag,0);

    if(old_debug == 3) {
        if(new_debug == 4) {
            fprintf(stderr, "New debug level is correctly 4.\n.");
        } else {
            fprintf(stderr,"New debug level is %d but should be 4.\n", new_debug);
            plc_tag_destroy(tag);
            return PLCTAG_ERR_BAD_REPLY;
        }
    } else {
        if(new_debug == 3) {
            fprintf(stderr, "New debug level is correctly 3.\n.");
        } else {
            fprintf(stderr,"New debug level is %d but should be 3.\n", new_debug);
            plc_tag_destroy(tag);
            return PLCTAG_ERR_BAD_DATA;
        }
    }

    fprintf(stderr,"Changing debug level back to %d\n", old_debug);

    plc_tag_set_uint32(tag, 0, old_debug);

    rc = plc_tag_write(tag, 0);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error %s trying to write debug level!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    rc = plc_tag_read(tag, 0);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error %s trying to read debug level!\n", plc_tag_decode_error(rc));
        plc_tag_destroy(tag);
        return rc;
    }

    new_debug = plc_tag_get_uint32(tag,0);

    if(old_debug == new_debug) {
        fprintf(stderr, "Correctly set debug level back to old value %d.\n", old_debug);
    } else {
        fprintf(stderr, "Unable to set debug level back to old value %d, was %d!\n", old_debug, new_debug);
        plc_tag_destroy(tag);
        return PLCTAG_ERR_BAD_DATA;
    }

    plc_tag_destroy(tag);

    return rc;
}





int main()
{
    int rc = PLCTAG_STATUS_OK;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    rc = test_version();
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error %s trying to test \"version\" special tag!\n", plc_tag_decode_error(rc));
        return rc;
    }

    rc = test_debug();
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Error %s trying to test \"debug\" special tag!\n", plc_tag_decode_error(rc));
        return rc;
    }

    return 0;
}


