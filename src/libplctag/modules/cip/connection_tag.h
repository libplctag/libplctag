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
 * The @connection tag body shared by the CIP families.
 *
 * AB and OMRON differ only in which protocol constants they answer to and which
 * find-or-create they call, so those are parameters rather than a second copy of the
 * function.  Each family keeps its own entry point so that its debug module ID -- which
 * is public API that callers filter on -- stays its own.
 */

#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/conn.h>
#include <stdint.h>
#include <utils/attr.h>
#include <utils/debug.h>


typedef struct {
    tag_protocol_t connection_protocol; /* this family's @connection protocol */
    debug_module_t debug_module;

    /*
     * Data-tag protocols this family will adopt a connection from.  AB accepts an OMRON
     * tag as well as its own, since both hang a cip_conn_p off the same field; OMRON
     * accepts only its own.  TAG_PROTOCOL_UNKNOWN pads an unused slot.
     */
    tag_protocol_t source_protocols[2];

    int (*find_or_create)(cip_conn_p *conn, attr attribs, int *is_new);

    /*
     * What to report when find_or_create() fails.  AB reports PLCTAG_ERR_BAD_GATEWAY
     * whatever went wrong; OMRON passes the underlying error through.  Left as
     * PLCTAG_STATUS_OK to pass through.
     */
    int32_t create_failure_rc;
} cip_connection_tag_ops_t;


extern plc_tag_p cip_connection_tag_create(attr attribs, const cip_connection_tag_ops_t *ops,
                                           void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                           void *userdata, plc_tag_p src_tag);
