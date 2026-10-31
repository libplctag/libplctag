#pragma once

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
 * A protocol handler's list of the tags that need its attention, sorted by when they next
 * need the wire.
 *
 * Each tag carries its own op_time and in_active_vector (TAG_BASE_STRUCT).  The list belongs to
 * the handler, and the caller holds whatever mutex guards it.  Equal op_times keep insertion
 * order, so tags due together go out first come, first served.
 */

#include <stdint.h>

#include <libplctag/lib/tag.h>
#include <utils/vector.h>


extern void tag_schedule_insert(vector_p list, plc_tag_p tag, int64_t op_time);
extern void tag_schedule_move(vector_p list, plc_tag_p tag, int64_t op_time);
extern void tag_schedule_remove(vector_p list, plc_tag_p tag);

/* The first of next, next + period_ms, next + 2 * period_ms, ... that is later than now. */
extern int64_t tag_schedule_next_period(int64_t next, int32_t period_ms, int64_t now);
