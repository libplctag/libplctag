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
 * The CIP tag name encoder writes into a fixed MAX_TAG_NAME buffer inside the tag struct.
 * The driver loop only checks the encoded index between segments, but a single array
 * reference such as "[a,b,c]" emits up to eighteen bytes, so an over-long name used to run
 * past the end of the buffer and into the fields that follow it -- among them
 * encoded_type_info_size, which is later used as a copy length.
 *
 * These tests drive the encoder through the public create call.  Tag creation parses and
 * encodes the name before any network I/O happens, so nothing here needs a PLC: a name that
 * encodes cleanly gets as far as PLCTAG_STATUS_PENDING and is then destroyed.  Run under
 * ASan/UBSan these fail loudly if the bounds checks regress.
 */

#include "mini_mock.h"

#include <libplctag/lib/libplctag.h>
#include <stdio.h>
#include <string.h>


#define ATTRIB_PREFIX "protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=1&name="


/* Create a tag with the given name and return the resulting status. */
static int create_status(const char *name) {
    char attribs[8192];
    int32_t tag_id = 0;
    int rc = 0;

    // NOLINTNEXTLINE
    snprintf(attribs, sizeof(attribs), "%s%s", ATTRIB_PREFIX, name);

    tag_id = plc_tag_create(attribs, 0);

    rc = (tag_id < 0) ? (int)tag_id : plc_tag_status(tag_id);

    if(tag_id > 0) { plc_tag_destroy(tag_id); }

    return rc;
}


/* Many four-byte numeric segments: six encoded bytes each, three per bracket group. */
static void test_long_numeric_segments_rejected(void **state) {
    (void)state;

    char name[4096];
    int len = 0;

    name[len++] = 'A';

    for(int i = 0; i < 200; i++) {
        // NOLINTNEXTLINE
        len += snprintf(&name[len], sizeof(name) - (size_t)len, "[70000]");
    }

    name[len] = '\0';

    assert_int_equal(create_status(name), PLCTAG_ERR_BAD_PARAM);
}


/* Many symbolic segments: three encoded bytes each before the bounded copy loop. */
static void test_long_symbolic_segments_rejected(void **state) {
    (void)state;

    char name[4096];
    int len = 0;

    name[len++] = 'A';

    for(int i = 0; i < 300; i++) {
        // NOLINTNEXTLINE
        len += snprintf(&name[len], sizeof(name) - (size_t)len, ".A");
    }

    name[len] = '\0';

    assert_int_equal(create_status(name), PLCTAG_ERR_BAD_PARAM);
}


/* strtol() hands back a long; anything that will not fit in the encoding must be refused. */
static void test_oversized_array_index_rejected(void **state) {
    (void)state;

    assert_int_equal(create_status("A[4294967296]"), PLCTAG_ERR_BAD_PARAM);
}


/*
 * Arrays are allowed far more than 65535 elements, so a large but representable index must
 * still encode.  It cannot connect here, so pending is the success case.
 */
static void test_large_valid_index_accepted(void **state) {
    (void)state;

    assert_int_equal(create_status("TestBigArray[70000]"), PLCTAG_STATUS_PENDING);
}


/* An ordinary name must not be caught by any of the new checks. */
static void test_normal_name_accepted(void **state) {
    (void)state;

    assert_int_equal(create_status("TestBigArray[0]"), PLCTAG_STATUS_PENDING);
}


/* Zero elements is not a valid array and would be a divisor later on. */
static void test_zero_elem_count_rejected(void **state) {
    (void)state;

    int32_t tag_id = plc_tag_create("protocol=ab-eip&gateway=127.0.0.1&path=1,0&plc=ControlLogix&elem_count=0&name=A", 0);
    int rc = (tag_id < 0) ? (int)tag_id : plc_tag_status(tag_id);

    if(tag_id > 0) { plc_tag_destroy(tag_id); }

    assert_int_equal(rc, PLCTAG_ERR_BAD_PARAM);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_long_numeric_segments_rejected), cmocka_unit_test(test_long_symbolic_segments_rejected),
        cmocka_unit_test(test_oversized_array_index_rejected),  cmocka_unit_test(test_large_valid_index_accepted),
        cmocka_unit_test(test_normal_name_accepted),            cmocka_unit_test(test_zero_elem_count_rejected),
    };

    int rc = cmocka_run_group_tests(tests, NULL, NULL);

    plc_tag_shutdown();

    return rc;
}
