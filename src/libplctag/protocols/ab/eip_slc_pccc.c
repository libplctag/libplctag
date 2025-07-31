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
#include <libplctag/protocols/ab/eip_slc_pccc.h>
#include <libplctag/protocols/ab/error_codes.h>
#include <libplctag/protocols/ab/pccc.h>
#include <libplctag/protocols/ab/session.h>
#include <libplctag/protocols/ab/tag.h>
#include <utils/debug.h>


static int tag_read_start(ab_tag_p tag);
static int tag_write_start(ab_tag_p tag);
static int tag_write_bit_start(ab_tag_p tag);

struct tag_vtable_t slc_vtable = {(tag_vtable_func)ab_tag_abort_request, /* shared */
                                   (tag_vtable_func)tag_read_start, (tag_vtable_func)pccc_tag_status, (tag_vtable_func)pccc_tag_tickler,
                                   (tag_vtable_func)tag_write_start, (tag_vtable_func)NULL, /* wake_plc */

                                   /* data accessors */
                                   ab_get_int_attrib, ab_set_int_attrib,

                                   ab_get_byte_array_attrib};



/* default string types used for PLC-5 PLCs. */
tag_byte_order_t slc_tag_byte_order = {.is_allocated = 0,

                                       .int16_order = {0, 1},
                                       .int32_order = {0, 1, 2, 3},
                                       .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                       .float32_order = {0, 1, 2, 3},
                                       .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                       .str_is_defined = 1,
                                       .str_is_counted = 1,
                                       .str_is_fixed_length = 1,
                                       .str_is_zero_terminated = 0,
                                       .str_is_byte_swapped = 1,

                                       .str_pad_to_multiple_bytes = 2,
                                       .str_count_word_bytes = 2,
                                       .str_max_capacity = 82,
                                       .str_total_length = 84,
                                       .str_pad_bytes = 0};

int tag_read_start(ab_tag_p tag) {
    return pccc_tag_read_start((ab_tag_p)tag, AB_EIP_SLC_RANGE_READ_FUNC);
}

int tag_write_start(ab_tag_p tag) {
    return pccc_tag_write_start((ab_tag_p)tag, AB_EIP_SLC_RANGE_WRITE_FUNC);
}
