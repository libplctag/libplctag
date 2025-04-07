/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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

#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/omron/defs.h>
#include <utils/vector.h>

typedef struct omron_tag_t *omron_tag_p;
#define OMRON_TAG_NULL ((omron_tag_p)NULL)

typedef struct omron_conn_t *omron_conn_p;
#define OMRON_CONN_NULL ((omron_conn_p)NULL)

typedef struct omron_request_t *omron_request_p;
#define OMRON_REQUEST_NULL ((omron_request_p)NULL)


extern int omron_tag_abort(omron_tag_p tag);
extern int omron_tag_status(omron_tag_p tag);

extern int omron_tag_abort_request(omron_tag_p tag);
extern int omron_tag_abort_request_only(omron_tag_p tag);

extern int omron_get_int_attrib(plc_tag_p tag, const char *attrib_name, int default_value);
extern int omron_set_int_attrib(plc_tag_p tag, const char *attrib_name, int new_value);

extern int omron_get_byte_array_attrib(plc_tag_p tag, const char *attrib_name, uint8_t *buffer, int buffer_length);

// THREAD_FUNC(request_handler_func);

/* helpers for checking request status. */
extern int omron_check_request_status(omron_tag_p tag);

#define rc_is_error(rc) (rc < PLCTAG_STATUS_OK)
