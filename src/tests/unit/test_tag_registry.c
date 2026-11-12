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


/* The tag registry: id lookup, the reuse map and id assignment.  Tags here are bare: only what the registry touches. */

#include "mini_mock.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/lib/tag_registry.h>
#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/rc.h>

/*
 * Compiled in, not linked from plctag_static, so a test can set next_tag_id and use a small
 * table.  The linker then never pulls plctag_static's own copy.
 */
#define PLCTAG_MAX_TAGS (1024)
#include "../../libplctag/lib/tag_registry.c"


static plc_tag_p new_tag(void) {
    plc_tag_p tag = rc_alloc((int)sizeof(struct plc_tag_t), NULL);

    assert_non_null(tag);
    memset(tag, 0, sizeof(*tag));

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

    /* the same entry under another id, a free entry, and invalid ids */
    assert_null(lookup_tag(id + PLCTAG_MAX_TAGS));
    assert_null(lookup_tag(id + 1));
    assert_null(lookup_tag(0));
    assert_null(lookup_tag(-id));

    drop_tag(tag);
    assert_null(lookup_tag(id));
}


/* Ids count up to TAG_ID_MASK, skip 0 and wrap back to 1. */
static void test_ids_skip_zero_and_wrap(void **state) {
    int32_t start[] = {TAG_ID_MASK - 2, TAG_ID_MASK - 1, TAG_ID_MASK, 1};
    int32_t expected[] = {TAG_ID_MASK - 1, TAG_ID_MASK, 1, 2};
    plc_tag_p existing = NULL;

    (void)state;

    for(size_t i = 0; i < 4; i++) {
        plc_tag_p tag = new_tag();
        plc_tag_p found = NULL;

        atomic_set_int32(&next_tag_id, start[i]);
        assert_int_equal(add_tag_lookup(tag, NULL, &existing), expected[i]);

        found = lookup_tag(expected[i]);
        assert_true(found == tag);
        rc_dec(found);

        drop_tag(tag);
    }
}


/* A second add with the same string gets the first tag; a removed or forgotten tag is not shared. */
static void test_find_or_create_shares_then_forgets(void **state) {
    plc_tag_p a = new_tag();
    plc_tag_p b = new_tag();
    plc_tag_p c = new_tag();
    plc_tag_p existing = NULL;
    plc_tag_p found = NULL;
    int32_t a_id = 0;
    int32_t c_id = 0;

    (void)state;

    a_id = add_tag_lookup(a, "name=s", &existing);
    assert_true(a_id > 0);
    assert_null(existing);

    /* b loses the race: nothing is published for it */
    assert_int_equal(add_tag_lookup(b, "name=s", &existing), a_id);
    assert_true(existing == a);
    rc_dec(existing);
    assert_int_equal(b->tag_id, 0);

    found = tag_registry_find_reusable("name=s");
    assert_true(found == a);
    rc_dec(found);
    assert_null(tag_registry_find_reusable("name=t"));

    drop_tag(a);
    assert_null(tag_registry_find_reusable("name=s"));

    c_id = add_tag_lookup(c, "name=s", &existing);
    assert_true(c_id > 0 && c_id != a_id);
    assert_null(existing);
    found = tag_registry_find_reusable("name=s");
    assert_true(found == c);
    rc_dec(found);

    /* forgotten: no longer shared, still registered */
    tag_registry_forget_reusable(c);
    assert_null(tag_registry_find_reusable("name=s"));
    found = lookup_tag(c_id);
    assert_true(found == c);
    rc_dec(found);

    drop_tag(c);
    rc_dec(b);
}


/* Every entry in use: the next create is refused until one is removed. */
static void test_full_table(void **state) {
    static plc_tag_p tags[PLCTAG_MAX_TAGS];
    plc_tag_p extra = new_tag();
    plc_tag_p existing = NULL;
    int32_t id = 0;

    (void)state;

    for(size_t i = 0; i < PLCTAG_MAX_TAGS; i++) {
        tags[i] = new_tag();
        assert_true(add_tag_lookup(tags[i], NULL, &existing) > 0);
    }

    assert_int_equal(add_tag_lookup(extra, NULL, &existing), PLCTAG_ERR_NO_RESOURCES);
    assert_int_equal(extra->tag_id, 0);

    for(size_t i = 0; i < PLCTAG_MAX_TAGS; i++) {
        plc_tag_p found = lookup_tag(tags[i]->tag_id);

        assert_true(found == tags[i]);
        rc_dec(found);
    }

    /* the only free entry is the one just emptied */
    id = tags[7]->tag_id;
    drop_tag(tags[7]);
    assert_int_equal(TAG_SLOT(add_tag_lookup(extra, NULL, &existing)), TAG_SLOT(id));

    tags[7] = extra;
    for(size_t i = 0; i < PLCTAG_MAX_TAGS; i++) { drop_tag(tags[i]); }
}


/* While closed, a create is refused and nothing is published. */
static void test_closed_registry_refuses(void **state) {
    plc_tag_p tag = new_tag();
    plc_tag_p existing = NULL;
    int32_t next = 0;

    (void)state;

    assert_true(tag_registry_close());
    assert_true(!tag_registry_close());

    next = atomic_get_int32(&next_tag_id);
    assert_int_equal(add_tag_lookup(tag, NULL, &existing), PLCTAG_ERR_NOT_ALLOWED);
    assert_int_equal(add_tag_lookup(tag, "name=s", &existing), PLCTAG_ERR_NOT_ALLOWED);
    assert_int_equal(tag->tag_id, 0);
    assert_null(lookup_tag(next + 1));

    tag_registry_publish();
    rc_dec(tag);
}


#define READERS (8)
#define WRITER_ROUNDS (20000)

static atomic_bool writer_done = ATOMIC_BOOL_STATIC_INIT;
static atomic_int32_t bad_lookups = ATOMIC_INT_STATIC_INIT;
static atomic_int32_t good_lookups = ATOMIC_INT_STATIC_INIT;
static int32_t first_id = 0;

static THREAD_FUNC(reader_thread) {
    (void)arg;

    while(!atomic_get_bool(&writer_done)) {
        for(int32_t id = first_id; id < first_id + 64; id++) {
            plc_tag_p found = lookup_tag(id);

            if(!found) { continue; }
            if(found->tag_id != id) { atomic_add_int32(&bad_lookups, 1); }
            atomic_add_int32(&good_lookups, 1);
            rc_dec(found);
        }
    }

    THREAD_RETURN(0);
}


/* Readers racing creates and removes only ever get NULL or the tag they asked for. */
static void test_lookups_race_creates_and_removes(void **state) {
    thread_p readers[READERS] = {0};
    plc_tag_p existing = NULL;

    (void)state;

    /* the writer's ids cycle through first_id .. first_id + 63 */
    first_id = 100000;

    for(size_t i = 0; i < READERS; i++) { assert_int_equal(thread_create(&readers[i], reader_thread, 0, NULL), PLCTAG_STATUS_OK); }

    for(int32_t round = 0; round < WRITER_ROUNDS; round++) {
        plc_tag_p tag = new_tag();

        atomic_set_int32(&next_tag_id, first_id - 1 + (round % 64));
        assert_true(add_tag_lookup(tag, NULL, &existing) > 0);
        drop_tag(tag);
    }

    atomic_set_bool(&writer_done, true);
    for(size_t i = 0; i < READERS; i++) {
        thread_join(readers[i]);
        thread_destroy(&readers[i]);
    }

    assert_int_equal(atomic_get_int32(&bad_lookups), 0);
    printf("%" PRId32 " lookups found a tag.\n", atomic_get_int32(&good_lookups));
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_add_lookup_remove),
        cmocka_unit_test(test_ids_skip_zero_and_wrap),
        cmocka_unit_test(test_find_or_create_shares_then_forgets),
        cmocka_unit_test(test_full_table),
        cmocka_unit_test(test_closed_registry_refuses),
        cmocka_unit_test(test_lookups_race_creates_and_removes),
    };

    assert_int_equal(tag_registry_init(), PLCTAG_STATUS_OK);
    tag_registry_publish();

    return cmocka_run_group_tests(tests, NULL, NULL);
}
