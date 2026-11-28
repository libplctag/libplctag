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
 * Tag ID assignment after the ID counter wraps (#681).  A wrapped counter lands on IDs that
 * long-lived tags still hold.  Creates must step past any run of them, however long, as long
 * as a free ID exists.
 *
 * Reaching the wrap through the API takes 2^28 creates, so lib.c is compiled in and the test
 * moves next_tag_id back by hand.  The linker then never pulls plctag_static's own copy.
 */

#include "mini_mock.h"

#include "../../libplctag/lib/lib.c"

/* more than the old fixed search of 50 IDs. */
#define LIVE_TAGS (60)

#define SYSTEM_TAG "protocol=system&name=version"


static void test_create_after_wrap_onto_live_tags(void **state) {
    int32_t ids[LIVE_TAGS];
    int32_t first_id = 0;
    int32_t id = 0;

    (void)state;

    for(size_t i = 0; i < LIVE_TAGS; i++) {
        ids[i] = plc_tag_create(SYSTEM_TAG, 0);
        assert_true(ids[i] > 0);
    }

    first_id = ids[0];

    /* as if the counter had wrapped: the next IDs it tries are all held. */
    next_tag_id = first_id - 1;

    id = plc_tag_create(SYSTEM_TAG, 0);
    printf("  create after the wrap returned %" PRId32 " (%s).\n", id, id < 0 ? plc_tag_decode_error(id) : "an ID");
    assert_true(id > 0);

    for(size_t i = 0; i < LIVE_TAGS; i++) { assert_true(id != ids[i]); }

    plc_tag_destroy(id);
    for(size_t i = 0; i < LIVE_TAGS; i++) { plc_tag_destroy(ids[i]); }
}


/*
 * IDs run up to INT32_MAX.  The last one is handed out like any other, so it must be
 * destroyable too, and the next create wraps to 1 without overflowing.
 */
static void test_destroy_highest_id(void **state) {
    int32_t id = 0;

    (void)state;

    next_tag_id = TAG_ID_MASK - 1;

    id = plc_tag_create(SYSTEM_TAG, 0);
    printf("  create at the top of the ID range returned %" PRId32 ".\n", id);
    assert_int_equal(id, INT32_MAX);

    assert_int_equal(plc_tag_destroy(id), PLCTAG_STATUS_OK);
    assert_int_equal(plc_tag_status(id), PLCTAG_ERR_NOT_FOUND);

    /* the earlier test never used IDs below 10, so 1 is free. */
    id = plc_tag_create(SYSTEM_TAG, 0);
    printf("  create after the top of the ID range returned %" PRId32 ".\n", id);
    assert_int_equal(id, 1);

    plc_tag_destroy(id);
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_create_after_wrap_onto_live_tags),
        cmocka_unit_test(test_destroy_highest_id),
    };

    int rc = cmocka_run_group_tests(tests, NULL, NULL);

    plc_tag_shutdown();

    return rc;
}
