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

#pragma once

#include <stdbool.h>
#include <stdint.h>


typedef struct attr_entry_t *attr_entry;
typedef struct attr_t *attr;

attr_entry find_entry(attr a, const char *name);
extern attr attr_create(void);
extern attr attr_create_from_str(const char *attr_str);
extern int attr_set_str(attr attrs, const char *name, const char *val);
extern int attr_set_int(attr attrs, const char *name, int val);
extern const char *attr_get_str(attr attrs, const char *name, const char *def);
extern int attr_get_int(attr attrs, const char *name, int def);
extern void attr_destroy(attr attrs);

/* A deep copy, for a holder that has to outlive the set it was given.  NULL on failure. */
extern attr attr_copy(attr src);

/*
 * Bring an attribute value inside its range.
 *
 * The value is passed by pointer and the result says whether it had to move, because every
 * caller that clamps a value the application supplied also has to say something about it --
 * log a warning, return PLCTAG_ERR_OUT_OF_BOUNDS, or both.  Returning the clamped value
 * instead would make each of those callers compare it against what they passed in.
 *
 * This serves values from a tag string and values from plc_tag_set_int_attribute() alike,
 * so it takes the value rather than the attribute set it may or may not have come from.
 *
 * A min_value above max_value is a caller error: the value ends up at max_value, so a bad
 * range shows up as a clamped value rather than letting anything through.
 */
extern bool attr_clamp_int32(int32_t *value, int32_t min_value, int32_t max_value);
