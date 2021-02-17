/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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

#include <util/platform.h>

#ifdef PLATFORM_IS_WINDOWS
    #define snprintf_platform sprintf_s
#else
    #define snprintf_platform snprintf
#endif

extern int str_cmp(const char *first, const char *second);
extern int str_cmp_i(const char *first, const char *second);
extern int str_cmp_i_n(const char *first, const char *second, int num_chars);
extern int str_copy(char *dst, int dst_size, const char *src);
extern int str_length(const char *str);
extern char *str_dup(const char *str);
extern int str_to_int(const char *str, int *val);
extern int str_to_float(const char *str, float *val);
extern char **str_split(const char *str, const char *sep);
#define str_concat_2(num_args, str1, ...) str_concat_impl(num_args, str1, __VA_ARGS__)
#define str_concat(str1, ...) str_concat_2(COUNT_NARG(__VA_ARGS__)+1, str1, __VA_ARGS__)
//#define str_concat(s1, ...) str_concat_impl(COUNT_NARG(__VA_ARGS__)+1, s1, __VA_ARGS__)
extern char *str_concat_impl(int num_args, ...);

