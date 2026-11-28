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
 * Unit tests for the protocol handlers' sorted active-tag list (lib/tag_schedule.c).
 *
 * The list decides which tag goes on the wire next, so the order is the behavior under test:
 * ascending op_time, and insertion order among equal times.  No mocking; the tags are bare
 * plc_tag_t structs that never reach a protocol.
 */

#include "mini_mock.h"

#include <stdint.h>

#include <libplctag/lib/tag.h>
#include <libplctag/lib/tag_schedule.h>

#define TAG_COUNT (5)


static struct plc_tag_t tags[TAG_COUNT];


static tag_list_t the_list;


static tag_list_t *fresh_list(void) {
    for(size_t i = 0; i < TAG_COUNT; i++) {
        tags[i] = (struct plc_tag_t){0};
        tags[i].tag_id = (int32_t)i;
    }

    the_list = (tag_list_t){0};

    return &the_list;
}


/* checks the treap: parent links, search order and heap order.  Returns the subtree size. */
static int32_t check_subtree(plc_tag_p tag, plc_tag_p parent) {
    if(!tag) { return 0; }

    assert_true(tag->sched_parent == parent);
    assert_true(tag->in_active_list);
    if(tag->sched_left) {
        assert_true(tag->sched_left->op_time <= tag->op_time);
        assert_true(tag->sched_left->sched_priority <= tag->sched_priority);
    }
    if(tag->sched_right) {
        assert_true(tag->sched_right->op_time >= tag->op_time);
        assert_true(tag->sched_right->sched_priority <= tag->sched_priority);
    }

    return 1 + check_subtree(tag->sched_left, tag) + check_subtree(tag->sched_right, tag);
}


static int32_t length_of(tag_list_t *list) {
    int32_t length = check_subtree(list->root, NULL);
    int32_t walked = 0;

    for(plc_tag_p tag = tag_schedule_first(list); tag; tag = tag_schedule_next(tag)) { walked++; }

    assert_int_equal(walked, length);

    return length;
}


/* the tag IDs in list order, as one number with a digit per tag: 0, 2, 1 -> 21 (leading 0 dropped). */
static int64_t order_of(tag_list_t *list) {
    int64_t order = 0;

    (void)length_of(list);

    for(plc_tag_p tag = tag_schedule_first(list); tag; tag = tag_schedule_next(tag)) { order = order * 10 + tag->tag_id; }

    return order;
}


static void test_insert_sorts_by_time_and_keeps_ties_in_order(void **state) {
    tag_list_t *list = fresh_list();

    (void)state;

    tag_schedule_insert(list, &tags[1], 200);
    tag_schedule_insert(list, &tags[2], 100);
    tag_schedule_insert(list, &tags[3], 200);
    tag_schedule_insert(list, &tags[4], 300);
    tag_schedule_insert(list, &tags[0], 100);

    assert_int_equal(order_of(list), 20134);
    assert_true(tags[0].in_active_list);
    assert_int_equal(tags[3].op_time, 200);
}


static void test_insert_of_a_listed_tag_moves_it(void **state) {
    tag_list_t *list = fresh_list();

    (void)state;

    tag_schedule_insert(list, &tags[1], 100);
    tag_schedule_insert(list, &tags[2], 200);
    tag_schedule_insert(list, &tags[1], 300);

    assert_int_equal(length_of(list), 2);
    assert_int_equal(order_of(list), 21);
}


static void test_move_both_directions(void **state) {
    tag_list_t *list = fresh_list();

    (void)state;

    for(size_t i = 0; i < TAG_COUNT; i++) { tag_schedule_insert(list, &tags[i], (int64_t)(i * 100)); }

    /* later: to the end, then between 2 and 3 */
    tag_schedule_move(list, &tags[0], 1000);
    assert_int_equal(order_of(list), 12340);
    tag_schedule_move(list, &tags[0], 250);
    assert_int_equal(order_of(list), 12034);

    /* earlier: to the front, then after its equal */
    tag_schedule_move(list, &tags[4], 0);
    assert_int_equal(order_of(list), 41203);
    tag_schedule_move(list, &tags[3], 100);
    assert_int_equal(order_of(list), 41320);

    /* to its own time: it stays put */
    tag_schedule_move(list, &tags[2], 200);
    assert_int_equal(order_of(list), 41320);
}


static void test_remove(void **state) {
    tag_list_t *list = fresh_list();

    (void)state;

    tag_schedule_insert(list, &tags[1], 100);
    tag_schedule_insert(list, &tags[2], 200);

    tag_schedule_remove(list, &tags[1]);
    assert_int_equal(order_of(list), 2);
    assert_true(!tags[1].in_active_list);

    /* removing a tag that is not on the list does nothing */
    tag_schedule_remove(list, &tags[1]);
    tag_schedule_remove(list, &tags[3]);
    assert_int_equal(length_of(list), 1);
}


static void test_next_period(void **state) {
    (void)state;

    /* already in the future: unchanged */
    assert_int_equal(tag_schedule_next_period(1500, 100, 1000), 1500);

    /* behind by several periods: skips to the first one after now, keeping the phase */
    assert_int_equal(tag_schedule_next_period(1000, 100, 1350), 1400);

    /* exactly now is not later than now */
    assert_int_equal(tag_schedule_next_period(1000, 100, 1000), 1100);
}


/*
 * Random inserts, moves and removes against a reference: the walk must give time order, and
 * insertion order among equal times.  Few distinct times, so ties are common.
 */
#define RANDOM_TAGS (64)
#define RANDOM_OPS (20000)

static void test_random_ops_match_reference(void **state) {
    static struct plc_tag_t many[RANDOM_TAGS];
    int64_t inserted_at[RANDOM_TAGS] = {0};
    tag_list_t list = {0};
    uint64_t rng = 12345;

    (void)state;

    for(int i = 0; i < RANDOM_TAGS; i++) {
        many[i] = (struct plc_tag_t){0};
        many[i].tag_id = i;
    }

    for(int64_t op = 1; op <= RANDOM_OPS; op++) {
        int i = 0;

        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        i = (int)((rng >> 33) % RANDOM_TAGS);

        if((rng >> 20) % 4 == 0) {
            tag_schedule_remove(&list, &many[i]);
        } else {
            tag_schedule_insert(&list, &many[i], (int64_t)((rng >> 40) % 16));
            inserted_at[i] = op;
        }

        if(op % 97 == 0 || op == RANDOM_OPS) {
            plc_tag_p prev = NULL;
            int32_t listed = 0;

            for(int k = 0; k < RANDOM_TAGS; k++) { listed += many[k].in_active_list ? 1 : 0; }
            assert_int_equal(length_of(&list), listed);

            for(plc_tag_p tag = tag_schedule_first(&list); tag; tag = tag_schedule_next(tag)) {
                if(prev) {
                    assert_true(prev->op_time <= tag->op_time);
                    if(prev->op_time == tag->op_time) { assert_true(inserted_at[prev->tag_id] < inserted_at[tag->tag_id]); }
                }
                prev = tag;
            }
        }
    }
}


int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_insert_sorts_by_time_and_keeps_ties_in_order),
        cmocka_unit_test(test_insert_of_a_listed_tag_moves_it),
        cmocka_unit_test(test_move_both_directions),
        cmocka_unit_test(test_remove),
        cmocka_unit_test(test_next_period),
        cmocka_unit_test(test_random_ops_match_reference),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
