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
 * The sorted active-tag list shared by the protocol handlers.  Lifted from the Modbus handler,
 * which was its first user.
 */

#include <stdint.h>

#include <libplctag/lib/tag_schedule.h>
#include <libplctag/lib/tag.h>


static uint64_t next_priority(tag_list_t *list);
static void rotate_up(tag_list_t *list, plc_tag_p tag);
static void unlink_tag(tag_list_t *list, plc_tag_p tag);


void tag_schedule_insert(tag_list_t *list, plc_tag_p tag, int64_t op_time) {
    plc_tag_p parent = NULL;
    plc_tag_p *link = &list->root;

    if(tag->in_active_list) { unlink_tag(list, tag); }

    tag->op_time = op_time;
    tag->in_active_list = true;
    tag->sched_left = NULL;
    tag->sched_right = NULL;
    tag->sched_priority = next_priority(list);

    /* an equal time goes right, after the tags already there. */
    while(*link) {
        parent = *link;
        link = (op_time < parent->op_time) ? &parent->sched_left : &parent->sched_right;
    }

    *link = tag;
    tag->sched_parent = parent;

    while(tag->sched_parent && tag->sched_parent->sched_priority < tag->sched_priority) { rotate_up(list, tag); }
}


void tag_schedule_move(tag_list_t *list, plc_tag_p tag, int64_t op_time) { tag_schedule_insert(list, tag, op_time); }


void tag_schedule_remove(tag_list_t *list, plc_tag_p tag) {
    if(!tag->in_active_list) { return; }

    unlink_tag(list, tag);
    tag->in_active_list = false;
}


plc_tag_p tag_schedule_first(tag_list_t *list) {
    plc_tag_p tag = list->root;

    while(tag && tag->sched_left) { tag = tag->sched_left; }

    return tag;
}


plc_tag_p tag_schedule_next(plc_tag_p tag) {
    if(tag->sched_right) {
        tag = tag->sched_right;
        while(tag->sched_left) { tag = tag->sched_left; }
        return tag;
    }

    while(tag->sched_parent && tag->sched_parent->sched_right == tag) { tag = tag->sched_parent; }

    return tag->sched_parent;
}


int64_t tag_schedule_next_period(int64_t next, int32_t period_ms, int64_t now) {
    while(next <= now) { next += period_ms; }

    return next;
}


/* splitmix64. */
static uint64_t next_priority(tag_list_t *list) {
    uint64_t z = (list->seed += 0x9e3779b97f4a7c15ULL);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;

    return z ^ (z >> 31);
}


/* put the tag where its parent was, the parent becoming its child; in-order is unchanged. */
static void rotate_up(tag_list_t *list, plc_tag_p tag) {
    plc_tag_p parent = tag->sched_parent;
    plc_tag_p grand = parent->sched_parent;

    if(parent->sched_left == tag) {
        parent->sched_left = tag->sched_right;
        if(tag->sched_right) { tag->sched_right->sched_parent = parent; }
        tag->sched_right = parent;
    } else {
        parent->sched_right = tag->sched_left;
        if(tag->sched_left) { tag->sched_left->sched_parent = parent; }
        tag->sched_left = parent;
    }

    parent->sched_parent = tag;
    tag->sched_parent = grand;

    if(!grand) {
        list->root = tag;
    } else if(grand->sched_left == parent) {
        grand->sched_left = tag;
    } else {
        grand->sched_right = tag;
    }
}


/* rotate the tag down past its higher-priority child until it has at most one, then splice it out. */
static void unlink_tag(tag_list_t *list, plc_tag_p tag) {
    plc_tag_p child = NULL;

    while(tag->sched_left && tag->sched_right) {
        rotate_up(list, (tag->sched_left->sched_priority > tag->sched_right->sched_priority) ? tag->sched_left : tag->sched_right);
    }

    child = tag->sched_left ? tag->sched_left : tag->sched_right;

    if(child) { child->sched_parent = tag->sched_parent; }

    if(!tag->sched_parent) {
        list->root = child;
    } else if(tag->sched_parent->sched_left == tag) {
        tag->sched_parent->sched_left = child;
    } else {
        tag->sched_parent->sched_right = child;
    }

    tag->sched_left = NULL;
    tag->sched_right = NULL;
    tag->sched_parent = NULL;
}
