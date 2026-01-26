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


#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
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

/**
 * @brief Get the current log level.
 *
 * @return log_level_t
 */
log_level_t log_get_level(void);

/**
 * @brief Set the current log level.
 *
 * @param level New log level
 * @return log_level_t Previous log level
 */
log_level_t log_set_level(log_level_t level);

/**
 * @brief Log a message.
 *
 * @param func name of the function in which the log is generated
 * @param line_num line number in the source file
 * @param lvl log level
 * @param templ format string for the log message
 * @param ... additional arguments for the format string
 */
void log_impl(const char *func, int line_num, log_level_t lvl, const char *templ, ...);

/* helper macros */

#define log_error(...)                                                                                       \
    do {                                                                                                     \
        if((LOG_LEVEL_ERROR) <= log_get_level()) log_impl(__func__, __LINE__, LOG_LEVEL_ERROR, __VA_ARGS__); \
    } while(0)
#define log_warn(...)                                                                                      \
    do {                                                                                                   \
        if((LOG_LEVEL_WARN) <= log_get_level()) log_impl(__func__, __LINE__, LOG_LEVEL_WARN, __VA_ARGS__); \
    } while(0)
#define log_info(...)                                                                                      \
    do {                                                                                                   \
        if((LOG_LEVEL_INFO) <= log_get_level()) log_impl(__func__, __LINE__, LOG_LEVEL_INFO, __VA_ARGS__); \
    } while(0)
#define log_detail(...)                                                                                        \
    do {                                                                                                       \
        if((LOG_LEVEL_DETAIL) <= log_get_level()) log_impl(__func__, __LINE__, LOG_LEVEL_DETAIL, __VA_ARGS__); \
    } while(0)
#define log_spew(...)                                                                                      \
    do {                                                                                                   \
        if((LOG_LEVEL_SPEW) <= log_get_level()) log_impl(__func__, __LINE__, LOG_LEVEL_SPEW, __VA_ARGS__); \
    } while(0)


#ifdef __cplusplus
}
#endif
