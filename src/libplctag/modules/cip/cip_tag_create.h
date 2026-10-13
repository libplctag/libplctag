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
 * both the Rockwell and the OMRON modules, so neither has to know the other.  It lives in
 * modules/cip because what it dispatches is CIP and nothing else, but it is the one file
 * here that sits *above* the families rather than below them -- the rest of modules/cip
 * names no family and must keep it that way.
 *
 * A row is data -- what the connection needs before any tag exists, and what a new tag
 * starts with -- plus one function, for the name, because PCCC logical-address encoding,
 * Logix symbolic encoding and OMRON's are different code.  The connection holds its row, so
 * a tag asks the connection rather than the table.
 */

#include <libplctag/lib/tag.h>
#include <stdbool.h>
#include <libplctag/modules/cip/plc_type.h>
#include <libplctag/modules/cip/standard_tag.h>
#include <libplctag/modules/cip/tag.h>
#include <utils/debug.h>
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
     * Whether a request to this family travels over a CIP connection.
     *
     * Data on the row rather than a line in the leaf, because the EtherNet/IP connection is
     * created knowing the answer and no tag exists at that point.  -1 means the family lets
     * the tag string decide and defaults to connected; every other answer is fixed and a tag
     * string cannot change it.
     */
    int8_t use_connected_msg;

    /*
     * What a new tag of this family starts with.  A special tag (@identity, @raw, @tags,
     * @udt) replaces the vtable, byte order and debug module while its name is parsed.
     * vtable is NULL for a family that has only special tags.
     */
    tag_protocol_t protocol;
    tag_vtable_p vtable;
    tag_byte_order_t *byte_order;
    const cip_standard_tag_ops_t *std_ops;
    debug_module_t debug_module;

    bool allow_packing;     /* the default */
    bool packing_settable;  /* whether the tag string's allow_packing may change it */
    bool size_from_plc;     /* the first read reports the size; otherwise elem_size states it */

    /*
     * The one piece of tag creation that is code: parse the name, special or data, and
     * encode it.  Rows share these -- PCCC file addressing, Logix symbolic, OMRON symbolic.
     * It runs after the element count is known, because a symbolic name encodes it.
     */
    int (*setup_name)(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs);

    /*
     * Connection sizing and negotiation.  Shared code reads these rather than plc_type, so
     * it never has to know which family it is talking to.
     */
    int max_payload_capacity;       /* sizes the connection's receive buffer */
    int fo_conn_size;               /* payload to ask for in a plain Forward Open */
    int fo_ex_conn_size;            /* payload to ask for in an extended Forward Open, 0 if unsupported */
    bool only_use_old_forward_open;
    uint16_t min_payload_size;      /* refuse to negotiate below this */
};


/*
 * Whether a tag of this family uses a CIP connection, from the row and, for the one family
 * that allows it, the tag string.
 *
 * Both the connection and the tag need this answer, and the connection needs it first, so it
 * is resolved here instead of inside the family's setup leaf.
 */
extern bool cip_entry_use_connected_msg(const cip_plc_entry_t *entry, attr attribs);


/* NULL if no row matches, which is a configuration error and not an internal one. */
extern const cip_plc_entry_t *cip_plc_entry_find(cip_plc_type_t plc_type, cip_dialect_t dialect);

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
 * It settles the family and the connection, then hands both to the module that serves the
 * family.  Registered in lib/init.c's tag_type_map.
 */
extern int cip_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                void *userdata, plc_tag_p src_tag, plc_tag_p *tag_out);
