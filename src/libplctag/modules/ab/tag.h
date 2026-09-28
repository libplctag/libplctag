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

#pragma once

/* do these first */

/* they are used in some of these includes */
#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/tag.h>
#include <libplctag/modules/ab/ab_common.h>
#include <libplctag/modules/ab/pccc.h>
#include <libplctag/modules/ab/session.h>


struct ab_tag_t {
    CIP_TAG_BASE_STRUCT;

    /* how do we talk to this device? */
    ab_plc_type_t plc_type;

    /* pointer back to the session */
    ab_session_p session;

    /* the in-flight request object */
    ab_request_p req;

    /* PCCC only: the data file this tag addresses. */
    pccc_file_t file_type;

    /*
     * PCCC only: TNS of the request we last put on the wire.  The response has to carry
     * the same one, otherwise a late reply to a request that already timed out gets
     * applied to whatever operation is in flight now.
     */
    uint16_t req_pccc_seq_num;
};
