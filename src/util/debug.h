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

/*
 * debug.h
 *
 *  Created on: August 1, 2016
 *      Author: Kyle Hayes
 */

#pragma once

#define DEBUG_NONE      (0)
#define DEBUG_ERROR     (1)
#define DEBUG_WARN      (2)
#define DEBUG_INFO      (3)
#define DEBUG_DETAIL    (4)
#define DEBUG_SPEW      (5)
#define DEBUG_END       (6)

extern int set_debug_level(int debug_level);
extern int get_debug_level(void);
extern void debug_set_tag_id(int tag_id);

extern void pdebug_impl(const char *func, int line_num, int debug_level, const char *templ, ...);

#if defined(_WIN32) && defined(_MSC_VER)
    /* MinGW on Windows does not need this. */
    #define __func__ __FUNCTION__
#endif


#define pdebug(dbg,...)                                                \
   do { if((dbg) != DEBUG_NONE && (dbg) <= get_debug_level()) pdebug_impl(__func__, __LINE__, dbg, __VA_ARGS__); } while(0)

extern void pdebug_dump_bytes_impl(const char *func, int line_num, int debug_level, uint8_t *data,int count);
#define pdebug_dump_bytes(dbg, d,c)  do { if((dbg) != DEBUG_NONE && (dbg) <= get_debug_level()) pdebug_dump_bytes_impl(__func__, __LINE__,dbg,d,c); } while(0)

extern int debug_register_logger(void (*log_callback_func)(int32_t tag_id, int debug_level, const char *message));
extern int debug_unregister_logger(void);
