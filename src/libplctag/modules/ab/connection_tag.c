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

/* AB's wrapper around the shared connection tag.  See lib/connection_tag.c. */

#include "connection_tag.h"
#include "session.h"
#include <libplctag/api/libplctag.h>
#include <libplctag/lib/connection_tag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/ab/tag.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/rc.h>


plc_tag_p ab_connection_tag_create(attr attribs,
                                   void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                   void *userdata, plc_tag_p src_tag) {
    connection_tag_args_t args = {.protocol_type = TAG_PROTOCOL_AB_CONNECTION,
                                  .debug_module = DEBUG_MODULE_AB_CONNECTION,
                                  .conn_rc = PLCTAG_STATUS_OK};
    ab_session_p session = NULL;

    pdebug(DEBUG_MODULE_AB_CONNECTION, DEBUG_DETAIL, 0, "Starting.");

    if(src_tag) {
        switch(src_tag->protocol_type) {
            case TAG_PROTOCOL_AB:
            case TAG_PROTOCOL_OMRON: session = rc_inc(((ab_tag_p)src_tag)->session); break;

            case TAG_PROTOCOL_AB_CONNECTION: session = rc_inc(((connection_tag_p)src_tag)->conn); break;

            default: session = NULL; break;
        }

        args.conn_rc = (session ? PLCTAG_STATUS_OK : PLCTAG_ERR_NOT_ALLOWED);
    } else {
        int new_session = 0;

        args.conn_rc = session_find_or_create(&session, attribs, &new_session);
        if(args.conn_rc != PLCTAG_STATUS_OK) {
            session = NULL;
            args.conn_rc = PLCTAG_ERR_BAD_GATEWAY;
        }

        args.conn_is_new = (new_session != 0);
    }

    if(session) {
        args.conn = session;
        args.watch = &session->watch;
        args.conn_mutex = session->session_mutex;
    }

    pdebug(DEBUG_MODULE_AB_CONNECTION, DEBUG_DETAIL, 0, "Done.");

    return connection_tag_create(attribs, &args, tag_callback_func, userdata);
}
