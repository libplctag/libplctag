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

#ifndef __PLCTAG_AB_TAG_H__
#    define __PLCTAG_AB_TAG_H__ 1

/* do these first */
#    define MAX_TAG_TYPE_INFO (64)

/* they are used in some of these includes */
#    include <libplctag/lib/libplctag.h>
#    include <libplctag/lib/tag.h>
#    include <libplctag/protocols/ab/ab_common.h>
#    include <libplctag/protocols/ab/defs.h>
#    include <libplctag/protocols/ab/pccc.h>
#    include <libplctag/protocols/ab/session.h>
#    include <libplctag/protocols/cip/tag.h>

/* the element type list moved to <libplctag/protocols/cip/tag.h> as cip_elem_type_t. */


struct ab_tag_t {
    TAG_BASE_STRUCT;

    CIP_TAG_STRUCT;

    /* AB specific from here on. */

    /* how do we talk to this device? */
    plc_type_t plc_type;

    /* the connection this tag rides on. */
    ab_session_p conn;

    /*
     * TNS of the PCCC request we last put on the wire.  The response has to carry the same
     * one, otherwise a late reply to a request that already timed out gets applied to
     * whatever operation is in flight now.
     */
    uint16_t req_pccc_seq_num;

    pccc_file_t file_type;
};



#endif
