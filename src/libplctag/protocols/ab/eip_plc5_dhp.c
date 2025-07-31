/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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


#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/ab/ab_common.h>
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/ab/eip_plc5_dhp.h>
#include <libplctag/protocols/ab/pccc.h>
#include <libplctag/protocols/ab/session.h>
#include <libplctag/protocols/ab/tag.h>
#include <utils/debug.h>



static int tag_read_start(ab_tag_p tag);
static int tag_write_start(ab_tag_p tag);
static int tag_write_bit_start(ab_tag_p tag);

struct tag_vtable_t eip_plc5_dhp_vtable = {(tag_vtable_func)ab_tag_abort_request, /* shared */
                                           (tag_vtable_func)tag_read_start, (tag_vtable_func)pccc_dhp_tag_status,
                                           (tag_vtable_func)pccc_dhp_tag_tickler, (tag_vtable_func)tag_write_start,
                                           (tag_vtable_func)NULL, /* wake_plc */

                                           /* data accessors */
                                           ab_get_int_attrib, ab_set_int_attrib,

                                           ab_get_byte_array_attrib};

int tag_read_start(ab_tag_p tag) {
    return pccc_dhp_tag_read_start(tag, AB_EIP_PLC5_RANGE_READ_FUNC);
}

int tag_write_start(ab_tag_p tag) {
    return pccc_dhp_tag_write_start(tag, AB_EIP_PLC5_RANGE_WRITE_FUNC);
}
