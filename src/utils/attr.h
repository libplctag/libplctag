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

extern attr attr_create(void);
extern attr attr_create_from_str(const char *attr_str);
extern int attr_set_str(attr attrs, const char *name, const char *val);
extern int attr_set_int(attr attrs, const char *name, int val);
extern const char *attr_get_str(attr attrs, const char *name, const char *def);
extern int attr_get_int(attr attrs, const char *name, int def);
extern void attr_destroy(attr attrs);

/* A deep copy, for a holder that has to outlive the set it was given.  NULL on failure. */
extern attr attr_copy(attr src);

/* the largest attribute name, terminator included, that a lookup canonicalizes on the stack. */
#define ATTR_NAME_MAX_SIZE (64)

/*
 * The canonical form of an attribute name: ASCII 'A'-'Z' lowercased and '_' removed, every
 * other byte unchanged.  Names are stored and looked up in this form, so "elem_count",
 * "ElemCount" and "ELEM_COUNT" are one name and comparisons are exact.
 *
 * Byte-wise on purpose: no locale and no UTF-8 decoding, so no non-ASCII byte sequence (an
 * overlong UTF-8 form, a Unicode case fold) can turn into an ASCII name.  The result is never
 * longer than the input, so dst may be name.
 *
 * Returns the length, PLCTAG_ERR_NULL_PTR, or PLCTAG_ERR_TOO_SMALL if dst_size cannot hold
 * the result and its terminator.
 */
extern int32_t attr_canonical_name(char *dst, int32_t dst_size, const char *name);

/*
 * A copy of attr_str with each name trimmed of spaces as attr_create_from_str() trims it and
 * made canonical, and each value unchanged, in the original order.  The find_or_create_tag
 * key.  NULL on failure; the caller frees it with mem_free().
 */
extern char *attr_canonical_str(const char *attr_str);
