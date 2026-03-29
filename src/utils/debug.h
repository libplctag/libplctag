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
 * debug.h
 *
 *  Created on: August 1, 2016
 *      Author: Kyle Hayes
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <libplctag/lib/libplctag.h>

/* Generated debug constants - parsed from libplctag.h at build time */
#include "debug_generated.h"

extern int set_debug_level(int debug_level);
extern int get_debug_level(void);
// extern void debug_set_tag_id(int32_t tag_id);

/* Module configuration API */
extern void debug_module_set_level(debug_module_t module, int level);
extern int debug_module_get_level(debug_module_t module);
extern void debug_set_all_modules(int level);

extern bool debug_is_enabled(debug_module_t module, int level);

extern void pdebug_impl(const char *func, int line_num, int debug_level, debug_module_t module, int32_t tag_id, const char *templ,
                        ...);

#if defined(_WIN32) && defined(_MSC_VER)
/* MinGW on Windows does not need this. */
#    define __func__ __FUNCTION__
#endif

/* set the compile-time max debug level if not already defined*/
#ifndef PLCTAG_COMPILE_DEBUG_LEVEL
#    define PLCTAG_COMPILE_DEBUG_LEVEL PLCTAG_DEBUG_DETAIL
#endif

#define pdebug(module, dbg, tag_id, ...)                                                                                       \
    do {                                                                                                                       \
        if((dbg) <= PLCTAG_COMPILE_DEBUG_LEVEL) {                                                                              \
            if(debug_is_enabled((module), (dbg))) { pdebug_impl(__func__, __LINE__, (dbg), (module), (tag_id), __VA_ARGS__); } \
        }                                                                                                                      \
    } while(0)

extern void pdebug_dump_bytes_impl(const char *func, int line_num, int debug_level, debug_module_t module, int32_t tag_id,
                                   uint8_t *data, int count);
#define pdebug_dump_bytes(module, dbg, tag_id, d, c)                                             \
    do {                                                                                         \
        if((dbg) <= PLCTAG_COMPILE_DEBUG_LEVEL) {                                                \
            if(debug_is_enabled((module), (dbg))) {                                              \
                pdebug_dump_bytes_impl(__func__, __LINE__, (dbg), (module), (tag_id), (d), (c)); \
            }                                                                                    \
        }                                                                                        \
    } while(0)

extern int debug_register_logger(void (*log_callback_func)(int32_t tag_id, int debug_level, const char *message));
extern int debug_unregister_logger(void);
extern void debug_flush(void);
