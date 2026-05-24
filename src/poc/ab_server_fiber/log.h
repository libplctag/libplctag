#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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
 * Copied from modbus_server/log.h.  buf_t dependency removed:
 *   - #include "buf.h" removed
 *   - log_bytes_impl() declaration removed
 *   - pdlog_bytes() macro removed
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LOG_LEVEL_NONE = 0,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_WARN,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DETAIL,
    LOG_LEVEL_SPEW,

    LOG_LEVEL_END
} log_level_t;

/* Log modules - generated from log_modules.def */
typedef enum {
#define LOG_MODULE_ENTRY(name, bit) LOG_MODULE_##name = (1ULL << bit),
#include "log_modules.def"
#undef LOG_MODULE_ENTRY
} log_module_t;

typedef uint64_t log_module_mask_t;

void log_module_set_level(log_module_t module, log_level_t level);
log_level_t log_module_get_level(log_module_t module);
void log_set_all_modules(log_level_t level);
bool log_is_enabled(log_module_mask_t modules, log_level_t level);

void log_impl(const char *func, int line_num, log_level_t lvl,
              log_module_mask_t modules, const char *templ, ...);

#define pdlog(modules, level, ...) \
    do { if (log_is_enabled(modules, level)) \
        log_impl(__func__, __LINE__, level, modules, __VA_ARGS__); } while (0)

#ifdef __cplusplus
}
#endif
