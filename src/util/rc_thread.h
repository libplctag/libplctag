/***************************************************************************
 *   Copyright (C) 2017 by Kyle Hayes                                      *
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
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifndef __UTIL_RC_THREAD_H__
#define __UTIL_RC_THREAD_H__ 1

#include <util/macros.h>

typedef struct rc_thread_t *rc_thread_p;
typedef void (*rc_thread_func)(int arg_count, void **args);

#define rc_thread_create(func, ...) rc_thread_create_impl(func, COUNT_NARG(__VA_ARGS__), __VA_ARGS__)
extern rc_thread_p rc_thread_create_impl(rc_thread_func func, int arg_count, ...);
extern int rc_thread_abort(rc_thread_p thread);
extern int rc_thread_check_abort(void);


/* needed for set up of the rc_thread service */
//~ extern int rc_thread_service_init(void);
//~ extern void rc_thread_service_teardown(void);


#endif
