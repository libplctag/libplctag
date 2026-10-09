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
#include <libplctag/modules/cip_tag_create.h>
#include <libplctag/modules/cip/tag.h>
#include <utils/attr.h>






/* Runtime attributes shared by every AB tag type. */
extern const attr_def_t ab_attribs[];

extern int check_cpu(cip_tag_p tag, attr attribs);
extern int check_tag_name(cip_tag_p tag, const char *name);

/*
 * The per-family leaves, reached through the dispatch table in modules/cip_tag_create.c.
 *
 * Six of the table's rows share two of these: PLC/5 direct and bridged both reach
 * ab_setup_plc5_tag, and SLC and MicroLogix in both dialects reach ab_setup_slc_tag, because
 * nothing but the byte order and the dialect differs between them.
 */
extern int ab_setup_plc5_tag(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs);
extern int ab_setup_slc_tag(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs);
extern int ab_setup_lgx_pccc_tag(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs);
extern int ab_setup_logix_tag(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs);
extern int ab_setup_micro800_tag(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs);
extern int ab_setup_generic_tag(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs);

/* special tag setup, see raw_tag.c, listing_tag.c, udt_tag.c and identity_tag.c */
extern int setup_raw_tag(cip_tag_p tag);
extern int setup_tag_listing_tag(cip_tag_p tag, const char *name);
extern int setup_udt_tag(cip_tag_p tag, const char *name);
extern int setup_identity_tag(cip_tag_p tag);


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
extern int session_startup(void);
extern void session_teardown(void);
extern int session_find_or_create(cip_conn_p *session, attr attribs, int *is_new_session);
