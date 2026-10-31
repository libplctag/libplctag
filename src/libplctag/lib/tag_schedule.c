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
#include <utils/debug.h>
#include <utils/vector.h>


static int32_t insert_index(vector_p list, int64_t op_time);


/*
 * Insert a tag that is not on the list.  A tag already on it is moved instead, so concurrent
 * API and auto-sync paths cannot put it on twice.
 */
void tag_schedule_insert(vector_p list, plc_tag_p tag, int64_t op_time) {
    if(tag->in_active_vector) {
        tag_schedule_move(list, tag, op_time);
        return;
    }

    tag->op_time = op_time;
    tag->in_active_vector = true;

    if(vector_insert(list, insert_index(list, op_time), tag) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Unable to insert the tag into the active list!");
        tag->in_active_vector = false;
    }
}


/* Reposition a tag already on the list, with one memmove rather than a remove and an insert. */
void tag_schedule_move(vector_p list, plc_tag_p tag, int64_t op_time) {
    int32_t current_index = vector_find_index(list, tag);

    if(current_index < 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "The tag is not on the active list!");
        tag->in_active_vector = false;
        return;
    }

    /*
     * insert_index() is the insert-before position, which is right in both directions: moving
     * later, the tag lands just before it and the tags between shift down; moving earlier, it
     * lands on it and they shift up.
     */
    vector_swap_element(list, current_index, insert_index(list, op_time));

    tag->op_time = op_time;
}


void tag_schedule_remove(vector_p list, plc_tag_p tag) {
    int32_t index = 0;

    if(!tag->in_active_vector) { return; }

    index = vector_find_index(list, tag);
    if(index >= 0) {
        vector_remove(list, index);
    } else {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "The tag was marked active but was not on the active list!");
    }

    tag->in_active_vector = false;
}


int64_t tag_schedule_next_period(int64_t next, int32_t period_ms, int64_t now) {
    while(next <= now) { next += period_ms; }

    return next;
}


/* The first index whose op_time is later than op_time, so equal times stay in insertion order. */
static int32_t insert_index(vector_p list, int64_t op_time) {
    int32_t low = 0;
    int32_t high = vector_length(list);

    while(low < high) {
        int32_t mid = low + (high - low) / 2;
        plc_tag_p mid_tag = vector_get(list, mid);

        if(mid_tag && mid_tag->op_time <= op_time) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    return low;
}
