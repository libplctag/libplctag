#pragma once

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


/*
 * CIP path and tag-name encoding, and the CIP data-type table.
 *
 * Split out of ab/cip.c and omron/cip.c, which held the same ten functions and
 * the same 260-entry type table under different names.  Everything here is
 * defined by the CIP specification.
 *
 * The tag-name encoder needed six fields from a tag and nothing else, so it
 * takes those instead of a vendor tag pointer.  That is the whole of the seam:
 * fill one in, call, copy the three outputs back.
 */

#include <stddef.h>
#include <stdint.h>

#include <libplctag/protocols/cip/defs.h>


typedef struct {
    int32_t tag_id;        /* in: for log messages only */
    int elem_count;        /* in */
    uint8_t *encoded_name; /* in: buffer of MAX_TAG_NAME bytes, filled on success */
    int encoded_name_size; /* out */
    int is_bit;            /* out */
    int bit;               /* out */
} cip_tag_name_t;


extern int cip_encode_tag_name(cip_tag_name_t *ctx, const char *name);

/* look up the type size in bytes based on the first byte */
extern int cip_lookup_encoded_type_size(uint8_t type_byte, int *type_size);

/* look up the element size in bytes based on the first byte */
extern int cip_lookup_data_element_size(uint8_t type_byte, int *element_size);
