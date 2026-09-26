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

#include <libplctag/api/libplctag.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * CIP connection-path and data-type helpers shared by every CIP family.
 *
 * Nothing here touches a tag: these operate on caller-owned buffers so that the
 * same code serves Rockwell, OMRON and anything else speaking CIP.
 */

/*
 * Parse a textual connection path into CIP path segments.
 *
 * plc_type selects DH+ bridging, which only the PLC-5/SLC/MicroLogix families
 * support: a DH+ segment against any other family is rejected with
 * PLCTAG_ERR_BAD_PARAM rather than silently ignored.
 */
extern int cip_encode_path(const char *path, int *needs_connection, int plc_type, uint8_t *tmp_conn_path, int *tmp_conn_path_size,
                           int *is_dhp, uint16_t *dhp_dest);

/* PLC families that support DH+ bridging, for cip_encode_path()'s plc_type. */
#define CIP_PLC_KIND_OTHER (0)
#define CIP_PLC_KIND_DHP_CAPABLE (1)

/* look up the type size in bytes based on the first byte */
extern int cip_lookup_encoded_type_size(uint8_t type_byte, int *type_size);

/* look up the element size in bytes based on the first byte */
extern int cip_lookup_data_element_size(uint8_t type_byte, int *element_size);

/*
 * Everything the symbolic-name encoder needs from a tag, so that the encoder
 * itself does not depend on any family's tag structure.
 *
 * The caller owns encoded_name and states its capacity; the encoder never writes
 * past it.  bit, is_bit and encoded_name_size are outputs, copied back by the
 * caller because is_bit is a bitfield in the tag and cannot be written through a
 * pointer.
 */
typedef struct {
    int32_t tag_id;            /* in: for logging only */
    int elem_count;            /* in */
    uint8_t *encoded_name;     /* in: caller-owned destination buffer */
    int encoded_name_capacity; /* in */
    int encoded_name_size;     /* out */
    int bit;                   /* out */
    bool is_bit;               /* out */
} cip_name_t;

/* Encode a symbolic tag name into ctx->encoded_name. */
extern int cip_encode_name(cip_name_t *ctx, const char *name);
