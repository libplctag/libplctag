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
 * Read a tag listing twice on one handle and check both reads describe the same listing.
 *
 * Review defect R1: a new @tags read resets the offset but not elem_count, so the second
 * read reports about twice the symbols.  The size and the bytes must match too.
 *
 * Usage: test_tag_listing_reread --tag=<attribs with name=@tags>
 */

#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define REQUIRED_VERSION 2, 5, 5
#define DATA_TIMEOUT_MS (20000)


static bool read_listing(int32_t tag, int32_t *size, int32_t *elem_count, uint8_t **data);


int main(int argc, char **argv) {
    const char *tag_attribs = NULL;
    int32_t tag = 0;
    int32_t size[2] = {0};
    int32_t elem_count[2] = {0};
    uint8_t *data[2] = {NULL, NULL};
    bool ok = true;

    for(int32_t i = 1; i < argc; i++) {
        if(strncmp(argv[i], "--tag=", 6) == 0) { tag_attribs = &argv[i][6]; }
    }

    if(!tag_attribs) {
        fprintf(stderr, "Usage: %s --tag=<attribs with name=@tags>\n", argv[0]);
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

    for(int32_t pass = 0; pass < 2 && ok; pass++) {
        ok = read_listing(tag, &size[pass], &elem_count[pass], &data[pass]);
        if(ok) {
            fprintf(stderr, "Read %" PRId32 ": size %" PRId32 " bytes, elem_count %" PRId32 ".\n", pass + 1, size[pass],
                    elem_count[pass]);
        }
    }

    if(ok && size[0] != size[1]) {
        fprintf(stderr, "FAIL: size changed between reads.\n");
        ok = false;
    }

    if(ok && elem_count[0] != elem_count[1]) {
        fprintf(stderr, "FAIL: elem_count changed between reads.\n");
        ok = false;
    }

    if(ok && size[0] > 0 && memcmp(data[0], data[1], (size_t)size[0]) != 0) {
        fprintf(stderr, "FAIL: listing bytes changed between reads.\n");
        ok = false;
    }

    free(data[0]);
    free(data[1]);
    plc_tag_destroy(tag);

    fprintf(stderr, "%s\n", ok ? "PASS" : "FAIL");

    return ok ? 0 : 1;
}


/* read the whole listing and copy it out; the caller frees *data. */
static bool read_listing(int32_t tag, int32_t *size, int32_t *elem_count, uint8_t **data) {
    int32_t rc = plc_tag_read(tag, DATA_TIMEOUT_MS);

    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "ERROR: read failed: %s\n", plc_tag_decode_error(rc));
        return false;
    }

    *size = plc_tag_get_size(tag);
    *elem_count = plc_tag_get_int_attribute(tag, "elem_count", -1);

    if(*size <= 0) {
        fprintf(stderr, "ERROR: listing is empty (size %" PRId32 ").\n", *size);
        return false;
    }

    *data = (uint8_t *)malloc((size_t)*size);
    if(!*data) {
        fprintf(stderr, "ERROR: out of memory.\n");
        return false;
    }

    rc = plc_tag_get_raw_bytes(tag, 0, *data, *size);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "ERROR: copying the listing failed: %s\n", plc_tag_decode_error(rc));
        return false;
    }

    return true;
}
