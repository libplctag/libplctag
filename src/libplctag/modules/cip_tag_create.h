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

/*
 * Which family and dialect a CIP tag belongs to, and what to call to set one up.
 *
 * This file is the composition root for CIP tag creation: it is the one place that knows
 * both the Rockwell and the OMRON modules, so neither has to know the other.  modules/cip
 * stays below both and names neither.
 *
 * The table is a dispatch table, not a data table.  Tag creation is not one algorithm with
 * per-family constants -- PCCC logical-address encoding, Logix symbolic encoding and
 * OMRON's are different code.  So each row carries functions, and the only data in a row is
 * its key.  Where the families really are parameters of one algorithm, as in Forward Open
 * negotiation, the existing cip_conn_profile_t table already does the job and this one does
 * not duplicate it.
 */

#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/plc_type.h>
#include <libplctag/modules/cip/tag.h>
#include <utils/attr.h>

/*
 * How the tag reaches the PLC, as opposed to which PLC it is.
 *
 * Part of the key rather than a branch inside a leaf.  cip_encode_path() decides DH+ purely
 * from the path string, before any connection exists, so (PLC/5, direct) and (PLC/5, DH+)
 * can be two rows instead of one row with an if.
 */
typedef enum {
    CIP_DIALECT_DIRECT = 0,
    CIP_DIALECT_DHP,
} cip_dialect_t;


typedef struct cip_plc_entry_t cip_plc_entry_t;

struct cip_plc_entry_t {
    const char *name;
    cip_plc_type_t plc_type;
    cip_dialect_t dialect;

    /*
     * Everything this family does to a freshly allocated tag that the shared skeleton
     * cannot: its vtable, its byte order, its messaging policy.  NULL while a family is
     * still on the old path.
     *
     * It is handed its own row so that several rows can share a leaf and the leaf can still
     * tell them apart -- which is how the three PCCC families need only two leaves between
     * their six rows.
     */
    int (*setup_tag)(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs);
};


/* NULL if no row matches, which is a configuration error and not an internal one. */
extern const cip_plc_entry_t *cip_plc_entry_find(cip_plc_type_t plc_type, cip_dialect_t dialect);

/*
 * Which dialect this tag's attributes describe, decided before any connection exists.
 *
 * Only the path can make a tag DH+, and only for a family that has a DH+ row, so the table
 * is the list of what can bridge and there is no second one to keep in step.  An
 * unparseable path reads as direct: connection creation runs the same parser and reports it
 * properly, and guessing a dialect for a path that will be rejected anyway buys nothing.
 */
extern cip_dialect_t cip_dialect_from_attributes(cip_plc_type_t plc_type, attr attribs);


/*
 * Which CIP family the plc (or legacy cpu) attribute names, for every family the library
 * speaks.
 *
 * One parser, here, because the answer decides which module handles the tag and so cannot
 * belong to either of them.  There were two: the Rockwell module's, which accepted the OMRON
 * spellings so it could hand those tags on, and the OMRON module's, which accepted only
 * those.  Each module's check_cpu() still decides what it will accept.
 */
extern cip_plc_type_t cip_plc_type_from_attributes(attr attribs);

/*
 * The constructor the library dispatches protocol=ab-eip to.
 *
 * It reads the plc attribute, or a cloned tag's protocol, and calls the module that serves
 * it.  Registered in lib/init.c's tag_type_map.
 */
extern plc_tag_p cip_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                void *userdata, plc_tag_p src_tag);
