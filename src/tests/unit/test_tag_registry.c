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

/* The tag registry's id -> tag slot table.  Tags here are bare: only what the registry touches. */

#include "mini_mock.h"

#include <stdint.h>
#include <string.h>

#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/lib/tag_registry.h>
#include <utils/rc.h>

/* plctag_static's TAG_SLOT_COUNT, from CMake. */
#define SLOTS (TAG_SLOT_COUNT)


static tag_registry_p registry = NULL;


static plc_tag_p new_tag(void) {
    plc_tag_p tag = rc_alloc((int)sizeof(struct plc_tag_t), NULL);

    assert_non_null(tag);
    memset(tag, 0, sizeof(*tag));
    tag->instance = registry;

    return tag;
}


/* Removes the tag and drops the registry's reference, which frees it. */
static void drop_tag(plc_tag_p tag) {
    assert_true(remove_tag_lookup(tag));
    rc_dec(tag);
}


static void test_add_lookup_remove(void **state) {
    plc_tag_p existing = NULL;
    plc_tag_p tag = new_tag();
    plc_tag_p found = NULL;
    int32_t id = add_tag_lookup(tag, NULL, &existing);

    (void)state;

    assert_true(id > 0);
    assert_int_equal(tag->tag_id, id);

    found = lookup_tag(id);
    assert_true(found == tag);
    rc_dec(found);

    /* same slot, different id */
    assert_null(lookup_tag(id + SLOTS));
    assert_null(lookup_tag(-id));

    drop_tag(tag);
    assert_null(lookup_tag(id));
}


static void test_full_table_refuses_then_reuses_a_freed_slot(void **state) {
    static plc_tag_p tags[SLOTS];
    plc_tag_p existing = NULL;
    plc_tag_p extra = new_tag();
    int32_t id = 0;

    (void)state;

    for(int i = 0; i < SLOTS; i++) {
        tags[i] = new_tag();
        assert_true(add_tag_lookup(tags[i], NULL, &existing) > 0);
    }

    assert_int_equal(add_tag_lookup(extra, NULL, &existing), PLCTAG_ERR_NO_RESOURCES);

    /* the new tag gets the freed slot, under a new id */
    id = tags[100]->tag_id;
    drop_tag(tags[100]);
    tags[100] = extra;
    assert_true(add_tag_lookup(extra, NULL, &existing) > 0);
    assert_int_equal(extra->tag_id % SLOTS, id % SLOTS);
    assert_true(extra->tag_id != id);
    assert_null(lookup_tag(id));

    for(int i = 0; i < SLOTS; i++) { drop_tag(tags[i]); }
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_add_lookup_remove),
        cmocka_unit_test(test_full_table_refuses_then_reuses_a_freed_slot),
    };

    assert_int_equal(tag_registry_init(), PLCTAG_STATUS_OK);
    tag_registry_publish();
    registry = tag_registry_acquire();

    return cmocka_run_group_tests(tests, NULL, NULL);
}
