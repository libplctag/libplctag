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

#pragma once

#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/ab/defs.h>
#include <utils/vector.h>

typedef struct ab_tag_t *ab_tag_p;
#define AB_TAG_NULL ((ab_tag_p)NULL)

typedef cip_conn_t ab_session_t;
typedef cip_conn_p ab_session_p;
#define AB_SESSION_NULL ((ab_session_p)NULL)

typedef cip_request_t ab_request_t;
typedef cip_request_p ab_request_p;
#define AB_REQUEST_NULL ((ab_request_p)NULL)

extern int ab_tag_abort_request_only(ab_tag_p tag);
extern int ab_tag_abort_request(ab_tag_p tag);
extern int ab_tag_abort(ab_tag_p tag);
extern int ab_tag_status(ab_tag_p tag);


/* Runtime attributes shared by every AB tag type. */
extern const attr_def_t ab_attribs[];

extern ab_plc_type_t get_plc_type(attr attribs);
extern int check_cpu(ab_tag_p tag, attr attribs);
extern int check_tag_name(ab_tag_p tag, const char *name);

/* special tag setup, see raw_tag.c, listing_tag.c, udt_tag.c and identity_tag.c */
extern int setup_raw_tag(ab_tag_p tag);
extern int setup_tag_listing_tag(ab_tag_p tag, const char *name);
extern int setup_udt_tag(ab_tag_p tag, const char *name);
extern int setup_identity_tag(ab_tag_p tag);


/* helpers for checking request status. */
extern int check_request_status(ab_tag_p tag);

#define rc_is_error(rc) (rc < PLCTAG_STATUS_OK)
