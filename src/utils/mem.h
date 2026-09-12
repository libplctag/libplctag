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
 * Memory allocation and block operations.
 *
 * This replaces the memory section that used to live in the platform shims
 * (src/platform/posix/platform.[ch] and src/platform/windows/platform.[ch]).
 * The API is unchanged; only the home of the declarations moved.
 *
 * Nothing here is platform-specific -- the two shim copies were the same code
 * -- so this module has no #ifdef in it.  It is a thin layer over the C
 * library that adds argument checking and logging:
 *
 *   - a NULL pointer or a non-positive size is reported and ignored rather
 *     than passed down to a function that would treat it as undefined
 *     behaviour;
 *   - mem_alloc() zeroes what it returns;
 *   - mem_free() accepts NULL.
 *
 * Sizes are counts of bytes and must be positive.  The block operations
 * return no status: a rejected call logs a warning and does nothing.
 */

#include <stdint.h>



/* returns zeroed memory, or NULL on failure or a non-positive size. */
extern void *mem_alloc(int32_t size);

/* returns NULL on failure or a non-positive size; orig is untouched then. */
extern void *mem_realloc(void *orig, int32_t size);

/* does nothing when passed NULL. */
extern void mem_free(const void *mem);

extern void mem_set(void *dest, int32_t c, int32_t size);
extern void mem_copy(void *dest, void *src, int32_t size);
extern void mem_move(void *dest, void *src, int32_t size);

/*
 * Compares two blocks.  A NULL pointer or a non-positive size counts as an
 * empty block, and two empty blocks are equal.  Blocks of different lengths
 * are ordered by length without looking at their contents.
 */
extern int32_t mem_cmp(void *src1, int32_t src1_size, void *src2, int32_t src2_size);
