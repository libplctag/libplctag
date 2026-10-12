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

#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <libplctag/lib/conn_watch.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/ab/ab_common.h>
#include <libplctag/modules/cip/cip_tag_create.h>
#include <libplctag/modules/ab/pccc.h>
#include <libplctag/modules/cip/error_codes.h>
#include <libplctag/modules/cip/tag.h>
#include <libplctag/modules/cip/wire.h>
#include <limits.h>
#include <platform.h>
#include <stdlib.h>
#include <time.h>
#include <utils/atomic_utils.h>
#include <utils/byteorder.h>
#include <utils/debug.h>
#include <utils/random_utils.h>

/* Track active session handler threads for proper shutdown synchronization */



/*
 * Number of milliseconds to wait to try to set up the session again
 * after a failure.
 */

/* Idle timeout.  One second less than that negotiated with the PLC. */


/* make sure we try hard to get a good payload size */


/* plc-specific session constructors */


static cip_conn_list_t conn_list = {0};

int session_startup(void) { return session_list_init(&conn_list); }


void session_teardown(void) { session_list_teardown(&conn_list, DEBUG_MODULE_AB_SESSION); }


int32_t session_live_count(void) { return session_list_count(&conn_list); }


int session_find_or_create(cip_conn_p *tag_session, attr attribs, int *is_new_session) {
    cip_plc_type_t plc_type = cip_plc_type_from_attributes(attribs);

    /*
     * The row is resolved from the same attributes the connection is about to parse its own
     * path from, so the dialect stored on the connection and the path it encodes cannot
     * disagree.
     */
    const cip_plc_entry_t *plc_entry = cip_plc_entry_find(plc_type, cip_dialect_from_attributes(plc_type, attribs));

    if(!plc_entry || plc_type == CIP_PLC_OMRON_NJNX) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unknown PLC type %d!", plc_type);
        return PLCTAG_ERR_BAD_DEVICE;
    }

    return cip_conn_find_or_create(&conn_list, plc_entry, attribs, tag_session, is_new_session);
}


/*****************************************************************
 **************** Session handling functions *********************
 ****************************************************************/


/* Set connection status and reason atomics, and push a ring buffer entry if the status changed.
 * Must only be called from the session handler thread (single writer).
 *
 * watch.status and the ring publish must change together under conn_list.mutex:
 * connection_tag_create() takes a paired snapshot of both (watch.ring_write_idx
 * and watch.status) to seed a freshly created connection tag, and needs the same
 * mutex to avoid reading one from before this transition and the other from after it --
 * see the comment there. */


/* new version of Forward Open */