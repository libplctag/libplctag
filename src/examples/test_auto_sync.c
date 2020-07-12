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


#include <stdio.h>
#include <stdlib.h>
#include "../lib/libplctag.h"
#include "utils.h"


#define REQUIRED_VERSION 2,1,11
#define TAG_ATTRIBS "protocol=ab_eip&gateway=10.206.1.40&path=1,4&cpu=LGX&elem_type=DINT&elem_count=%d&name=TestBigArray[4]&auto_sync_read_ms=200"
#define DATA_TIMEOUT (5000)
#define NUM_ITERATIONS (100)
#define ITERATION_SLEEP_MS (100)


int main(int argc, char **argv)
{
    int32_t tag = 0;
    int iteration = NUM_ITERATIONS;

    (void)argc;
    (void)argv;

    /* check the library version. */
    if(plc_tag_check_lib_version(REQUIRED_VERSION) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Required compatible library version %d.%d.%d not available!", REQUIRED_VERSION);
        exit(1);
    }

    plc_tag_set_debug_level(PLCTAG_DEBUG_DETAIL);

    tag = plc_tag_create(TAG_ATTRIBS, DATA_TIMEOUT);
    if(tag < 0) {
        printf("Error, %s, creating tag!\n", plc_tag_decode_error(tag));
        return tag;
    }

    /* test read auto sync. */
    while(iteration--) {
        int32_t val = plc_tag_get_int32(tag, 0);

        printf("Iteration %d, got value: %d\n", NUM_ITERATIONS - iteration, val);

        util_sleep_ms(ITERATION_SLEEP_MS);
    }

    plc_tag_destroy(tag);

    return 0;
}

