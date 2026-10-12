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
#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/tag.h>
#include <libplctag/modules/cip/wire.h>
#include <libplctag/modules/cip/cip_tag_create.h>
#include <libplctag/modules/omron/omron_common.h>
#include <limits.h>
#include <platform.h>
#include <stdlib.h>
#include <time.h>
#include <utils/atomic_utils.h>
#include <utils/byteorder.h>
#include <utils/debug.h>
#include <utils/random_utils.h>




/*
 * Number of milliseconds to wait to try to set up the conn again
 * after a failure.
 */



/* make sure we try hard to get a good payload size */


static cip_conn_list_t conn_list = {0};


int conn_startup(void) { return session_list_init(&conn_list); }


void conn_teardown(void) { session_list_teardown(&conn_list, DEBUG_MODULE_OMRON_CONN); }


int32_t conn_live_count(void) { return session_list_count(&conn_list); }


int conn_find_or_create(cip_conn_p *tag_conn, attr attribs, int *is_new_conn) {
    /* one family, and an NJ/NX is never DH+ bridged, so the row is fixed. */
    const cip_plc_entry_t *plc_entry = cip_plc_entry_find(CIP_PLC_OMRON_NJNX, CIP_DIALECT_DIRECT);

    if(!plc_entry) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "No PLC table entry for Omron NJ/NX!");
        return PLCTAG_ERR_BAD_DEVICE;
    }

    return cip_conn_find_or_create(&conn_list, plc_entry, attribs, tag_conn, is_new_conn);
}


/*****************************************************************
 **************** Connection handling functions *********************
 ****************************************************************/




/* watch.status and the ring publish must change together under conn->session_mutex:
 * connection_tag_create() takes a paired snapshot of both (watch.ring_write_idx
 * and watch.status) to seed a freshly created connection tag, and needs the same
 * mutex to avoid reading one from before this transition and the other from after it --
 * see the comment there. */
/* new version of Forward Open */