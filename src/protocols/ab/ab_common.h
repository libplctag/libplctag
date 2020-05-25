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

#include <ab/defs.h>
#include <lib/libplctag.h>
#include <lib/tag.h>
#include <util/vector.h>

typedef struct ab_tag_t *ab_tag_p;
#define AB_TAG_NULL ((ab_tag_p)NULL)

typedef struct ab_session_t *ab_session_p;
#define AB_SESSION_NULL ((ab_session_p)NULL)

typedef struct ab_request_t *ab_request_p;
#define AB_REQUEST_NULL ((ab_request_p)NULL)


extern int ab_tag_abort(ab_tag_p tag);
extern int ab_tag_status(ab_tag_p tag);


extern int ab_get_int_attrib(plc_tag_p tag, const char *attrib_name, int default_value);
extern int ab_set_int_attrib(plc_tag_p tag, const char *attrib_name, int new_value);

extern int ab_get_bit(plc_tag_p tag, int offset_bit);
extern int ab_set_bit(plc_tag_p tag, int offset_bit, int val);

extern uint64_t ab_get_uint64(plc_tag_p tag, int offset);
extern int ab_set_uint64(plc_tag_p tag, int offset, uint64_t val);

extern int64_t ab_get_int64(plc_tag_p tag, int offset);
extern int ab_set_int64(plc_tag_p tag, int offset, int64_t val);


extern uint32_t ab_get_uint32(plc_tag_p tag, int offset);
extern int ab_set_uint32(plc_tag_p tag, int offset, uint32_t val);

extern int32_t ab_get_int32(plc_tag_p tag, int offset);
extern int ab_set_int32(plc_tag_p tag, int offset, int32_t val);


extern uint16_t ab_get_uint16(plc_tag_p tag, int offset);
extern int ab_set_uint16(plc_tag_p tag, int offset, uint16_t val);

extern int16_t ab_get_int16(plc_tag_p tag, int offset);
extern int ab_set_int16(plc_tag_p tag, int offset, int16_t val);


extern uint8_t ab_get_uint8(plc_tag_p tag, int offset);
extern int ab_set_uint8(plc_tag_p tag, int offset, uint8_t val);

extern int8_t ab_get_int8(plc_tag_p tag, int offset);
extern int ab_set_int8(plc_tag_p tag, int offset, int8_t val);


extern double ab_get_float64(plc_tag_p tag, int offset);
extern int ab_set_float64(plc_tag_p tag, int offset, double val);

extern float ab_get_float32(plc_tag_p tag, int offset);
extern int ab_set_float32(plc_tag_p tag, int offset, float val);


//int ab_tag_destroy(ab_tag_p p_tag);
extern plc_type_t get_plc_type(attr attribs);
extern int check_cpu(ab_tag_p tag, attr attribs);
extern int check_tag_name(ab_tag_p tag, const char *name);
extern int check_mutex(int debug);
extern vector_p find_read_group_tags(ab_tag_p tag);

THREAD_FUNC(request_handler_func);


#define rc_is_error(rc) (rc < PLCTAG_STATUS_OK)

