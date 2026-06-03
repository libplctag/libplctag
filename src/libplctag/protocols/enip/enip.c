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
 * ENIP Protocol Entry Point
 *
 * STATUS: KEEP AS-IS for Phases 0-5.  Phase 6 additions needed.
 *
 * This file is the public entry point registered with the tag dispatch table.
 * enip_tag_create routes to either a @connection tag or a normal protocol tag.
 *
 * Phase 6: enip_init must set up the global connection list and its mutex
 *          (mirroring modbus.c mb_mutex + plcs linked list).
 *          enip_tag_create must call find_or_create_connection(attribs) and
 *          insert the new tag into conn->active_tags.
 *          enip_teardown must drain and destroy all connections.
 */
#include <libplctag/protocols/enip/enip.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <string.h>

/* Phase 6: ADD global connection-list mutex init here. */
int enip_init(void) { return PLCTAG_STATUS_OK; }

/* Phase 6: ADD drain all connections, set shutdown_requested, join threads. */
void enip_teardown(void) {}

/* Phase 1: Extract gateway attribute and validate it exists.
 * Phase 6: ADD find_or_create_connection(attribs) call here; set tag->conn
 *          and insert tag into conn->active_tags; signal conn->wake. */
plc_tag_p enip_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                          void *userdata, plc_tag_p src_tag) {
    const char *name = attr_get_str(attribs, "name", NULL);
    const char *gateway = attr_get_str(attribs, "gateway", NULL);

    /* Phase 1: Gateway is required for all ENIP tags (except @connection). */
    if(name && str_cmp_i(name, "@connection") == 0) {
        return enip_connection_tag_create(attribs, tag_callback_func, userdata, src_tag);
    }

    if(src_tag && src_tag->protocol_type == TAG_PROTOCOL_ENIP_CONNECTION) {
        return enip_connection_tag_create(attribs, tag_callback_func, userdata, src_tag);
    }

    /* Phase 1: Validate gateway attribute is present. */
    if(!gateway || str_length(gateway) == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_ERROR, 0, "ENIP: Missing required 'gateway' attribute");
        return NULL;
    }

    return enip_protocol_tag_create(attribs, tag_callback_func, userdata, src_tag);
}
