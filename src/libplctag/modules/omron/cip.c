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

#include <ctype.h>
#include <errno.h>
#include <libplctag/api/libplctag.h>
#include <libplctag/modules/cip/path.h>
#include <libplctag/modules/cip/error_codes.h>
#include <libplctag/modules/omron/cip.h>
#include <libplctag/modules/omron/defs.h>
#include <libplctag/modules/omron/omron_common.h>
#include <libplctag/modules/omron/tag.h>
#include <platform.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <utils/debug.h>


static int omron_encode_tag_name(omron_tag_p tag, const char *name);


/* public access point */
cip_generic_t CIP = {
    .encode_path = cip_encode_path,
    .encode_tag_name = omron_encode_tag_name,
    .lookup_data_element_size = cip_lookup_data_element_size,
    .lookup_encoded_type_size = cip_lookup_encoded_type_size,
    .decode_cip_error_code = decode_cip_error_code,
    .decode_cip_error_long = decode_cip_error_long,
    .decode_cip_error_short = decode_cip_error_short,
};


// #define MAX_IP_ADDR_SEG_LEN (16)


/*
 * match symbolic IP address segments.
 *  18,10.206.10.14 - port 2/A -> 10.206.10.14
 *  19,10.206.10.14 - port 3/B -> 10.206.10.14
 */


/*
 * match DH+ address segments.
 *  A:1:2 - port 2/A -> DH+ node 2
 *  B:1:2 - port 3/B -> DH+ node 2
 *
 * A and B can be lowercase or numeric.
 */


/*
 * The EBNF is:
 *
 * tag ::= SYMBOLIC_SEG ( tag_seg )* ( bit_seg )?
 *
 * tag_seg ::= '.' SYMBOLIC_SEG
 *             '[' array_seg ']'
 *
 * bit_seg ::= '.' [0-9]+
 *
 * array_seg ::= NUMERIC_SEG ( ',' NUMERIC_SEG )*
 *
 * SYMBOLIC_SEG ::= [a-zA-Z]([a-zA-Z0-9_]*)
 *
 * NUMERIC_SEG ::= [0-9]+
 *
 */


/*
 * A bit segment is simply an integer from 0 to 63 (inclusive). */


int omron_encode_tag_name(omron_tag_p tag, const char *name) {
    cip_name_t ctx = {.tag_id = tag->tag_id,
                      .elem_count = tag->elem_count,
                      .encoded_name = &tag->encoded_name[0],
                      .encoded_name_capacity = (int)sizeof(tag->encoded_name),
                      .encoded_name_size = 0,
                      .bit = tag->bit,
                      .is_bit = (tag->is_bit ? true : false)};
    int rc = cip_encode_name(&ctx, name);

    /* copy the outputs back; is_bit is a bitfield, so it cannot be written through a pointer. */
    tag->encoded_name_size = ctx.encoded_name_size;
    tag->bit = ctx.bit;
    tag->is_bit = (ctx.is_bit ? (uint8_t)1 : (uint8_t)0);

    return rc;
}
