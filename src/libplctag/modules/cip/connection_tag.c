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

/* The @connection tag body shared by the CIP families.  See connection_tag.h. */

#include <libplctag/api/libplctag.h>
#include <libplctag/lib/connection_tag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/cip/connection_tag.h>
#include <libplctag/modules/cip/tag.h>
#include <stdbool.h>
#include <stddef.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/rc.h>


/* plc_tag_p::protocol_type is a bare int, so compare as one. */
static bool is_source_protocol(const cip_connection_tag_ops_t *ops, int protocol) {
    for(size_t i = 0; i < (sizeof(ops->source_protocols) / sizeof(ops->source_protocols[0])); i++) {
        if((int)ops->source_protocols[i] == protocol) { return true; }
    }

    return false;
}


plc_tag_p cip_connection_tag_create(attr attribs, const cip_connection_tag_ops_t *ops,
                                    void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                    void *userdata, plc_tag_p src_tag) {
    connection_tag_args_t args = {
        .protocol_type = ops->connection_protocol, .debug_module = ops->debug_module, .conn_rc = PLCTAG_STATUS_OK};
    cip_conn_p conn = NULL;

    pdebug(ops->debug_module, DEBUG_DETAIL, 0, "Starting.");

    if(src_tag) {
        if(is_source_protocol(ops, src_tag->protocol_type)) {
            conn = rc_inc(((cip_tag_p)src_tag)->session);
        } else if(src_tag->protocol_type == (int)ops->connection_protocol) {
            conn = rc_inc(((connection_tag_p)src_tag)->conn);
        }

        args.conn_rc = (conn ? PLCTAG_STATUS_OK : PLCTAG_ERR_NOT_ALLOWED);
    } else {
        int new_conn = 0;

        args.conn_rc = ops->find_or_create(&conn, attribs, &new_conn);
        if(args.conn_rc != PLCTAG_STATUS_OK) {
            conn = NULL;
            if(ops->create_failure_rc != PLCTAG_STATUS_OK) { args.conn_rc = ops->create_failure_rc; }
        }

        args.conn_is_new = (new_conn != 0);
    }

    if(conn) {
        args.conn = conn;
        args.watch = &conn->watch;
        args.conn_mutex = conn->session_mutex;
    }

    pdebug(ops->debug_module, DEBUG_DETAIL, 0, "Done.");

    return connection_tag_create(attribs, &args, tag_callback_func, userdata);
}
