/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
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

#include <stdint.h>
#include "slice.h"

extern int util_sleep_ms(int ms);
extern int64_t util_time_ms(void);

/* debug helpers */
void debug_on(void);
void debug_off(void);
#define error(...) error_impl(__func__, __LINE__, __VA_ARGS__)
extern void error_impl(const char *func, int line, const char *templ, ...);
#define info(...) info_impl(__func__, __LINE__, __VA_ARGS__)
extern void info_impl(const char *func, int line, const char *templ, ...);
extern void slice_dump(slice_s s);
