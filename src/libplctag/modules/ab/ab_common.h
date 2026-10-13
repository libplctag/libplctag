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
#include <libplctag/modules/cip/standard_tag.h>
#include <libplctag/modules/cip/plc_type.h>
#include <libplctag/modules/cip/cip_tag_create.h>
#include <libplctag/modules/cip/tag.h>
#include <utils/attr.h>






/* Runtime attributes shared by every AB tag type. */
extern const attr_def_t ab_attribs[];

/*
 * The name parsers, one per kind of Rockwell addressing, reached through the rows in
 * modules/cip/cip_tag_create.c.  Each also handles the special tags its family has.
 */
extern int ab_setup_plc5_name(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs);
extern int ab_setup_slc_name(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs);
extern int ab_setup_logix_name(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs);
extern int ab_setup_generic_name(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs);

/* special tag setup, see listing_tag.c and udt_tag.c */
extern int setup_tag_listing_tag(cip_tag_p tag, const char *name);
extern int setup_udt_tag(cip_tag_p tag, const char *name);


/* per-family string and integer layouts, used when setting a tag up. */
extern tag_byte_order_t logix_tag_byte_order;
extern tag_byte_order_t logix_tag_listing_byte_order;

/* what Rockwell can do, and the vtable built around it, for the shared standard-tag engine. */
extern const cip_standard_tag_ops_t cip_standard_tag_ops_ab;
extern struct tag_vtable_t cip_standard_tag_vtable_ab;

/*
 * The AB module's own connection lifecycle.  Everything else about a connection --
 * sequence numbers, payload space, requests -- is declared by cip/conn.h.
 */

