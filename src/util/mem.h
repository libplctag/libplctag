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

#define mem_alloc(size) mem_alloc_impl(__func__, __LINE__, size)
extern void *mem_alloc_impl(const char *func, int line_num, int size);

#define mem_realloc(orig, size) mem_realloc_impl(__func__, __LINE__, orig, size)
extern void *mem_realloc_impl(const char *func, int line_num, void *orig, int size);

#define mem_free(mem) mem_free_impl(__func__, __LINE__, mem)
extern void mem_free_impl(const char *func, int line_num, const void *mem);

extern void mem_set(void *dest, int c, int size);
extern void mem_copy(void *dest, void *src, int size);
extern void mem_move(void *dest, void *src, int size);
extern int mem_cmp(void *src1, int src1_size, void *src2, int src2_size);

typedef void (*rc_cleanup_func)(void *);

#define rc_alloc(size, cleaner) rc_alloc_impl(__func__, __LINE__, size, cleaner)
extern void *rc_alloc_impl(const char *func, int line_num, int size, rc_cleanup_func cleaner);

#define rc_inc(ref) rc_inc_impl(__func__, __LINE__, ref)
extern void *rc_inc_impl(const char *func, int line_num, void *ref);

#define rc_dec(ref) rc_dec_impl(__func__, __LINE__, ref)
extern void *rc_dec_impl(const char *func, int line_num, void *ref);

