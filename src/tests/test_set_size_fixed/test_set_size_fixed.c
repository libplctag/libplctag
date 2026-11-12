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
 * plc_tag_set_size() on a tag whose data buffer is part of the tag allocation (Modbus, system)
 * must fail and leave the size and the data alone.  It used to realloc a pointer into the
 * middle of the tag allocation.
 *
 * Usage: test_set_size_fixed --tag=<attribs of a Modbus or system tag>
 */

#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>


#define REQUIRED_VERSION 2, 5, 5
#define DATA_TIMEOUT_MS (5000)


static bool set_size_fails(int32_t tag, int32_t old_size, int32_t new_size);


int main(int argc, char **argv) {
    const char *tag_attribs = NULL;
    int32_t tag = 0;
    int32_t size = 0;
    bool ok = true;

    for(int32_t i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) { tag_attribs = &argv[i][6]; }
    }

    if(!tag_attribs) {
        fprintf(stderr, "Usage: %s --tag=<attribs of a Modbus or system tag>\n", argv[0]);
        return 1;
    }

    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!\n", REQUIRED_VERSION);
        return 1;
    }

    tag = plc_tag_create(tag_attribs, DATA_TIMEOUT_MS);
    if(tag < 0) {
        fprintf(stderr, "ERROR: create failed: %s\n", plc_tag_decode_error(tag));
        return 1;
    }

    size = plc_tag_get_size(tag);
    fprintf(stderr, "Tag size is %" PRId32 " bytes.\n", size);

    if(size <= 0) {
        fprintf(stderr, "FAIL: tag has no data.\n");
        ok = false;
    }

    ok = ok && set_size_fails(tag, size, size + 4096);
    ok = ok && set_size_fails(tag, size, size - 1);

    plc_tag_destroy(tag);
    plc_tag_shutdown();

    fprintf(stderr, "%s\n", ok ? "PASS" : "FAIL");

    return ok ? 0 : 1;
}


static bool set_size_fails(int32_t tag, int32_t old_size, int32_t new_size) {
    int32_t rc = plc_tag_set_size(tag, new_size);

    fprintf(stderr, "plc_tag_set_size(%" PRId32 ") returned %s.\n", new_size, plc_tag_decode_error(rc));

    if(rc >= 0) {
        fprintf(stderr, "FAIL: setting the size to %" PRId32 " succeeded.\n", new_size);
        return false;
    }

    if(plc_tag_get_size(tag) != old_size) {
        fprintf(stderr, "FAIL: size changed to %" PRId32 ".\n", plc_tag_get_size(tag));
        return false;
    }

    return true;
}
