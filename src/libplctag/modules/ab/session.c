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
#include <libplctag/modules/ab/pccc.h>
#include <libplctag/modules/ab/session.h>
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


#define MAX_CIP_LGX_MSG_SIZE (0x01FF & 504)
#define MAX_CIP_LGX_MSG_SIZE_EX (0xFFFF & 4000)

#define MAX_CIP_MICRO800_MSG_SIZE (0x01FF & 504)
/*
 * Micro800 supports the Extended Forward Open and can negotiate a comms buffer
 * as large as 60000 bytes.  This is the opening ask; a PLC that supports less
 * answers with its own size and receive_forward_open_response() retries at that
 * size, down to min_payload_size.
 */
#define MAX_CIP_MICRO800_MSG_SIZE_EX (0xFFFF & 60000)

/* Omron is special */

/* maximum for PCCC embedded within CIP. */
#define MAX_CIP_PLC5_MSG_SIZE (244)
#define MAX_CIP_SLC_MSG_SIZE (244)
#define MAX_CIP_MLGX_MSG_SIZE (244)
#define MAX_CIP_LGX_PCCC_MSG_SIZE (244)

/*
 * Number of milliseconds to wait to try to set up the session again
 * after a failure.
 */

/* Idle timeout.  One second less than that negotiated with the PLC. */

/*
 * Smallest connection payload we can actually use, by protocol family.
 *
 * A ForwardOpen error can offer a smaller size than we asked for and we accept it, but there
 * is a floor: below the fixed per-request overhead there is no room left for a request, and
 * the "space minus overhead" arithmetic downstream goes negative.  PCCC needs about 92 bytes
 * for its command header and addressing; CIP needs 500 for a minimal fragmented transfer.
 * Both are payload only -- the EIP and CPF encapsulation is accounted for separately.
 */
#define MIN_PAYLOAD_SIZE_PCCC (92)

/* make sure we try hard to get a good payload size */


/* plc-specific session constructors */


static cip_conn_list_t conn_list = {0};

/*
 * One row per AB PLC family.  Adding a family is adding a row.
 */
static const cip_conn_profile_t ab_conn_profiles[] = {
    /*                                       capacity                  fo_size                    fo_ex_size                   old_fo  min payload            dh+    unconn */
    {CIP_PLC_PLC5,     "PLC/5",               MAX_CIP_PLC5_MSG_SIZE,    MAX_CIP_PLC5_MSG_SIZE,     0,                           true,   MIN_PAYLOAD_SIZE_PCCC, true,  false},
    {CIP_PLC_SLC,      "SLC 500",             MAX_CIP_SLC_MSG_SIZE,     MAX_CIP_SLC_MSG_SIZE,      0,                           true,   MIN_PAYLOAD_SIZE_PCCC, true,  false},
    {CIP_PLC_MLGX,     "MicroLogix",          MAX_CIP_MLGX_MSG_SIZE,    MAX_CIP_MLGX_MSG_SIZE,     0,                           true,   MIN_PAYLOAD_SIZE_PCCC, true,  false},
    {CIP_PLC_LGX_PCCC, "*Logix PCCC",         MAX_CIP_LGX_PCCC_MSG_SIZE, MAX_CIP_LGX_PCCC_MSG_SIZE, 0,                          true,   MIN_PAYLOAD_SIZE_PCCC, false, false},
    {CIP_PLC_LGX,      "*Logix",              MAX_CIP_LGX_MSG_SIZE_EX,  MAX_CIP_LGX_MSG_SIZE,      MAX_CIP_LGX_MSG_SIZE_EX,     false,  MIN_PAYLOAD_SIZE_CIP,  false, false},
    {CIP_PLC_MICRO800, "Micro800",            MAX_CIP_MICRO800_MSG_SIZE_EX, MAX_CIP_MICRO800_MSG_SIZE, MAX_CIP_MICRO800_MSG_SIZE_EX, false, MIN_PAYLOAD_SIZE_CIP, false, false},

    /* generic CIP is *Logix sizing, but stateless: it never uses connected messaging */
    {CIP_PLC_GENERIC,  "generic CIP device",  MAX_CIP_LGX_MSG_SIZE_EX,  MAX_CIP_LGX_MSG_SIZE,      MAX_CIP_LGX_MSG_SIZE_EX,     false,  MIN_PAYLOAD_SIZE_CIP,  false, true},
};


static const cip_conn_profile_t *ab_conn_profile(cip_plc_type_t plc_type) {
    for(size_t i = 0; i < (sizeof(ab_conn_profiles) / sizeof(ab_conn_profiles[0])); i++) {
        if(ab_conn_profiles[i].plc_type == plc_type) { return &ab_conn_profiles[i]; }
    }

    return NULL;
}


int session_startup(void) { return session_list_init(&conn_list); }


void session_teardown(void) { session_list_teardown(&conn_list, DEBUG_MODULE_AB_SESSION); }
int session_find_or_create(cip_conn_p *tag_session, attr attribs, int *is_new_session) {
    const cip_conn_profile_t *profile = ab_conn_profile(get_plc_type(attribs));

    if(!profile) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unknown PLC type %d!", get_plc_type(attribs));
        return PLCTAG_ERR_BAD_DEVICE;
    }

    return cip_conn_find_or_create(&conn_list, profile, attribs, tag_session, is_new_session);
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