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

#pragma once

#include <ab/defs.h>
#include <lib/libplctag.h>
#include <lib/tag.h>
#include <util/vector.h>

typedef struct ab_tag_t *ab_tag_p;
#define AB_TAG_NULL ((ab_tag_p)NULL)

//typedef struct ab_connection_t *ab_connection_p;
//#define AB_CONNECTION_NULL ((ab_connection_p)NULL)

typedef struct ab_session_t *ab_session_p;
#define AB_SESSION_NULL ((ab_session_p)NULL)

typedef struct ab_request_t *ab_request_p;
#define AB_REQUEST_NULL ((ab_request_p)NULL)


extern int ab_tag_abort(ab_tag_p tag);
extern int ab_tag_status(ab_tag_p tag);
//int ab_tag_destroy(ab_tag_p p_tag);
extern plc_type_t get_plc_type(attr attribs);
extern int check_cpu(ab_tag_p tag, attr attribs);
extern int check_tag_name(ab_tag_p tag, const char *name);
extern int check_mutex(int debug);
extern vector_p find_read_group_tags(ab_tag_p tag);

THREAD_FUNC(request_handler_func);


#define rc_is_error(rc) (rc < PLCTAG_STATUS_OK)

