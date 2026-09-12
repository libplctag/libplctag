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
 * String helpers.
 *
 * This replaces the string section that used to live in the platform shims
 * (src/platform/posix/platform.[ch] and src/platform/windows/platform.[ch]).
 * The API is unchanged; only the home of the declarations moved.
 *
 * Every entry point tolerates a NULL pointer, treating it as an empty string.
 * The comparisons order NULL and "" ahead of any non-empty string and report
 * them equal to each other, so callers do not have to pre-check.
 *
 * str_dup(), str_split() and str_concat() return memory the caller must free
 * with mem_free().
 */

#include <stdint.h>

#include <utils/macros.h>


/* comparisons: -1, 0 or 1, ordering NULL and "" below any non-empty string. */
extern int str_cmp(const char *first, const char *second);
extern int str_cmp_i(const char *first, const char *second);
extern int str_cmp_i_n(const char *first, const char *second, int count);

/* case-insensitive strstr(); NULL when either side is empty or there is no match. */
extern char *str_str_cmp_i(const char *haystack, const char *needle);

/*
 * Copies only when the whole source fits.  Returns PLCTAG_ERR_TOO_LARGE and
 * writes nothing rather than truncating, so the destination is never left
 * holding a partial string.
 */
extern int str_copy(char *dst, int dst_size, const char *src);

extern int str_length(const char *str);
extern char *str_dup(const char *str);

/* return 0 on success, -1 if the text does not convert or does not fit. */
extern int str_to_int(const char *str, int *val);
extern int str_to_float(const char *str, float *val);

/*
 * Splits into a single allocation: an array of pointers terminated by a NULL
 * entry, followed by a private copy of the string with the separators zeroed.
 * One mem_free() of the returned pointer releases all of it.
 */
extern char **str_split(const char *str, const char *sep);

/* concatenates its arguments into one new string. */
#define str_concat(s1, ...) str_concat_impl(COUNT_NARG(__VA_ARGS__) + 1, s1, __VA_ARGS__)
extern char *str_concat_impl(int num_args, ...);
