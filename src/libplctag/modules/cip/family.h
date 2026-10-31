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

#include <libplctag/modules/cip/plc_type.h>
#include <libplctag/modules/cip/tag.h>
#include <stdbool.h>
#include <stdint.h>
#include <utils/attr.h>

/*
 * A CIP PLC family: what the connection needs before any tag exists, and the function that
 * sets a tag up for it.  Each family module defines its own (ab_common.c, omron_common.c);
 * nothing looks them up in a table.  The plc attribute or the Identity object picks one, the
 * connection keeps it, and a tag is set up by the family its connection holds.
 */
typedef struct cip_family_t cip_family_t;

struct cip_family_t {
    const char *name;
    cip_plc_type_t plc_type;

    /* the same PLC reached across a DH+ bridge, NULL if it cannot be; and whether this is that one. */
    const cip_family_t *dhp;
    bool via_dhp;

    /*
     * Whether requests travel over a CIP connection.  -1 lets the tag string decide and
     * defaults to connected; any other answer is fixed.
     */
    int8_t use_connected_msg;

    /*
     * Everything about a new tag that is the family's own: protocol, vtable, byte order, ops,
     * debug module, packing, the name and how the size is found.  Runs after the element count
     * is known, because a symbolic name encodes it.
     */
    int (*setup_tag)(cip_tag_p tag, attr attribs);

    /* connection sizing and negotiation. */
    int max_payload_capacity;  /* sizes the connection's receive buffer */
    int fo_conn_size;          /* payload to ask for in a plain Forward Open */
    int fo_ex_conn_size;       /* payload to ask for in an extended Forward Open, 0 if unsupported */
    bool only_use_old_forward_open;
    uint16_t min_payload_size; /* refuse to negotiate below this */
};

/* the largest receive buffer any family asks for, for a connection that does not know its family yet. */
#define CIP_FAMILY_MAX_PAYLOAD_CAPACITY (4000)
