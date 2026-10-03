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

/* OMRON's wrapper around the shared CIP connection tag.  See cip/connection_tag.c. */

#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/connection_tag.h>
#include <libplctag/modules/omron/conn.h>
#include <libplctag/modules/omron/omron_connection_tag.h>
#include <stdint.h>
#include <utils/attr.h>
#include <utils/debug.h>


plc_tag_p omron_connection_tag_create(attr attribs,
                                      void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                      void *userdata, plc_tag_p src_tag) {
    /* OMRON adopts a connection only from its own data tags, and reports the real error. */
    static const cip_connection_tag_ops_t ops = {.connection_protocol = TAG_PROTOCOL_OMRON_CONNECTION,
                                                 .debug_module = DEBUG_MODULE_OMRON_CONNECTION,
                                                 .source_protocols = {TAG_PROTOCOL_OMRON, TAG_PROTOCOL_UNKNOWN},
                                                 .find_or_create = conn_find_or_create,
                                                 .create_failure_rc = PLCTAG_STATUS_OK};

    return cip_connection_tag_create(attribs, &ops, tag_callback_func, userdata, src_tag);
}
