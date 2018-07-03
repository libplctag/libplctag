/***************************************************************************
 *   Copyright (C) 2015 by OmanTek                                         *
 *   Author Kyle Hayes  kylehayes@omantek.com                              *
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

/**************************************************************************
 * CHANGE LOG                                                             *
 *                                                                        *
 * 2015-09-12  KRH - Created file.                                        *
 *                                                                        *
 **************************************************************************/

#ifndef __PLCTAG_AB_AB_COMMON_H__
#define __PLCTAG_AB_AB_COMMON_H__ 1

#include <lib/libplctag.h>
#include <lib/libplctag_tag.h>
#include <util/vector.h>

typedef struct ab_tag_t *ab_tag_p;
#define AB_TAG_NULL ((ab_tag_p)NULL)

typedef struct ab_connection_t *ab_connection_p;
#define AB_CONNECTION_NULL ((ab_connection_p)NULL)

typedef struct ab_session_t *ab_session_p;
#define AB_SESSION_NULL ((ab_session_p)NULL)

typedef struct ab_request_t *ab_request_p;
#define AB_REQUEST_NULL ((ab_request_p)NULL)


extern volatile ab_session_p sessions;
extern volatile mutex_p global_session_mut;
extern volatile thread_p io_handler_thread;


int ab_tag_abort(ab_tag_p tag);
//int ab_tag_destroy(ab_tag_p p_tag);
int check_cpu(ab_tag_p tag, attr attribs);
int check_tag_name(ab_tag_p tag, const char *name);
int check_mutex(int debug);
extern vector_p find_read_group_tags(ab_tag_p tag);

#ifdef _WIN32
DWORD __stdcall request_handler_func(LPVOID not_used);
#else
void *request_handler_func(void *not_used);
#endif
#ifdef _WIN32
DWORD __stdcall request_handler_func(LPVOID not_used);
#else
void *request_handler_func(void *not_used);
#endif

#endif
