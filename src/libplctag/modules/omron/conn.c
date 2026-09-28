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
#include <libplctag/modules/omron/cip.h>
#include <libplctag/modules/omron/conn.h>
#include <libplctag/modules/omron/defs.h>
#include <libplctag/modules/omron/omron_common.h>
#include <libplctag/modules/omron/tag.h>
#include <limits.h>
#include <platform.h>
#include <stdlib.h>
#include <time.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>
#include <utils/random_utils.h>


/* Omron is special */
#define MAX_CIP_OMRON_MSG_SIZE_EX (0xFFFF & 1990)
#define MAX_CIP_OMRON_MSG_SIZE (0x01FF & 502)


/*
 * Number of milliseconds to wait to try to set up the conn again
 * after a failure.
 */



/* make sure we try hard to get a good payload size */


static cip_conn_list_t conn_list = {0};


/* Omron speaks one dialect, so the table has one row. */
static const cip_conn_profile_t omron_conn_profiles[] = {
    /*                                         capacity                   fo_size                 fo_ex_size                 old_fo  min payload           dh+    unconn */
    {OMRON_PLC_OMRON_NJNX, "Omron NJ/NX", MAX_CIP_OMRON_MSG_SIZE_EX, MAX_CIP_OMRON_MSG_SIZE, MAX_CIP_OMRON_MSG_SIZE_EX, false, MIN_PAYLOAD_SIZE_CIP, false, false},
};


static const cip_conn_profile_t *omron_conn_profile(omron_plc_type_t plc_type) {
    for(size_t i = 0; i < (sizeof(omron_conn_profiles) / sizeof(omron_conn_profiles[0])); i++) {
        if(omron_conn_profiles[i].plc_type == (int32_t)plc_type) { return &omron_conn_profiles[i]; }
    }

    return NULL;
}


int conn_startup(void) { return session_list_init(&conn_list); }


void conn_teardown(void) { session_list_teardown(&conn_list, DEBUG_MODULE_OMRON_CONN); }
int conn_find_or_create(omron_conn_p *tag_conn, attr attribs, int *is_new_conn) {
    const cip_conn_profile_t *profile = omron_conn_profile(OMRON_PLC_OMRON_NJNX);

    if(!profile) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "No connection profile for Omron NJ/NX!");
        return PLCTAG_ERR_BAD_DEVICE;
    }

    return cip_conn_find_or_create(&conn_list, profile, attribs, tag_conn, is_new_conn);
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