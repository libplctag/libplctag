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

#include <libplctag/lib/tag.h>
#include <stdint.h>

/*
 * State shared by every CIP tag, whatever the family.
 *
 * CIP_TAG_BASE_STRUCT follows the TAG_BASE_STRUCT idiom in lib/tag.h: a macro
 * expanded into each family's tag struct rather than a nested struct, so field
 * accesses stay direct.
 *
 * NOTE: expanding this into two family structs makes their layouts identical by
 * construction.  That is deliberate, but nothing may cast one family's tag to
 * another's -- the fields past this macro differ, and the pointee of `session`
 * differs.  ab_common.c used to rely on the layouts happening to match; that was
 * a bug, not an interface.
 */

/* the longest encoded tag name, and the longest encoded type info, in bytes. */
#define MAX_TAG_NAME (260)
#define MAX_TAG_TYPE_INFO (64)

/*
 * The element type of a tag's data.
 *
 * The last four are not CIP data types at all, but markers for the special tag
 * kinds whose payload the library synthesises.
 */
typedef enum {
    CIP_TYPE_BOOL,
    CIP_TYPE_BOOL_ARRAY,
    CIP_TYPE_CONTROL,
    CIP_TYPE_COUNTER,
    CIP_TYPE_FLOAT32,
    CIP_TYPE_FLOAT64,
    CIP_TYPE_INT8,
    CIP_TYPE_INT16,
    CIP_TYPE_INT32,
    CIP_TYPE_INT64,
    CIP_TYPE_STRING,
    CIP_TYPE_SHORT_STRING,
    CIP_TYPE_TIMER,
    CIP_TYPE_TAG_ENTRY,   /* pseudo type: an entry from a tag listing. */
    CIP_TYPE_TAG_UDT,     /* pseudo type: a UDT definition. */
    CIP_TYPE_TAG_RAW,     /* pseudo type: a raw CIP tag. */
    CIP_TYPE_TAG_IDENTITY /* pseudo type: CIP Identity Object data. */
} cip_elem_type_t;


/*
 * The state every CIP tag carries, whatever the family.
 *
 * Four things are deliberately NOT here because they are still family typed:
 *   plc_type  - until modules/cip owns family classification
 *   req       - until the operation engine replaces the per-request object
 *   session   - the pointee differs until the connection layer is shared
 *   the PCCC fields (file_type, req_pccc_seq_num), which belong to PCCC tags only
 */
#define CIP_TAG_BASE_STRUCT                                                                \
    TAG_BASE_STRUCT;                                                                       \
                                                                                           \
    int use_connected_msg;                                                                 \
                                                                                           \
    /* the encoded symbolic name */                                                        \
    uint8_t encoded_name[MAX_TAG_NAME];                                                    \
    int encoded_name_size;                                                                 \
                                                                                           \
    /* the encoded type, as it came off the wire */                                        \
    uint8_t encoded_type_info[MAX_TAG_TYPE_INFO];                                          \
    int encoded_type_info_size;                                                            \
                                                                                           \
    cip_elem_type_t elem_type;                                                             \
    int elem_count;                                                                        \
    int elem_size;                                                                         \
                                                                                           \
    int special_tag;                                                                       \
                                                                                           \
    /* standard tags: how much data may one packet carry? */                               \
    int write_data_per_packet;                                                             \
                                                                                           \
    uint32_t next_id;       /* listing tags */                                             \
    uint8_t udt_get_fields; /* UDT tags */                                                 \
    uint16_t udt_id;                                                                       \
                                                                                           \
    /*                                                                                     \
     * Consecutive fragment responses that carried no payload.                             \
     *                                                                                     \
     * A partial-transfer status with zero bytes is legitimate: when several requests are  \
     * packed into one packet the earlier ones can consume all the room, leaving the later \
     * ones only a bare CIP header.  It is also what a PLC would send forever to keep us   \
     * asking for the same fragment, so count them and give up rather than loop.  Reset    \
     * whenever a response actually delivers data.                                         \
     */                                                                                    \
    int fragment_retry_count;                                                              \
                                                                                           \
    int pre_write_read;                                                                    \
    int first_read;                                                                        \
    int offset;                                                                            \
    int allow_packing;                                                                     \
                                                                                           \
    int read_in_progress;                                                                  \
    int write_in_progress
