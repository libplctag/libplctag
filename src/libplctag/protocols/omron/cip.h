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

#pragma once

#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/omron/defs.h>
#include <libplctag/protocols/omron/omron_common.h>


/* fake up some generics */
typedef struct {
    enum {
        GET_ATTRIBUTES_ALL = 0x01,
        GET_ATTRIBUTE_LIST = 0x03,
        GET_ATTRIBUTE_SINGLE = 0x1E,

        AB_READ_TAG = 0x4C,
        AB_READ_TAG_FRAG = 0x52,
        AB_WRITE_TAG = 0x4D,
        AB_WRITE_TAG_FRAG = 0x53,
        AB_MULTI_REQUEST = 0x0A,
        AB_LIST_TAGS = 0x55,

        OMRON_READ_TAG = AB_READ_TAG,
        OMRON_WRITE_TAG = AB_WRITE_TAG,
        OMRON_MULTI_REQUEST = AB_MULTI_REQUEST,
        OMRON_LIST_TAGS = 0x5F,


    } services;

    int32_t (*encode_path)(const char *path, int *needs_connection, uint8_t *tmp_conn_path, int *tmp_conn_path_size, int *is_dhp,
                           uint16_t *dhp_dest);
    int32_t (*encode_tag_name)(omron_tag_p tag, const char *name);
    int32_t (*lookup_encoded_type_size)(uint8_t type_byte, int *type_size);
    int32_t (*lookup_data_element_size)(uint8_t type_byte, int *element_size);

    const char *(*decode_cip_error_short)(uint8_t *data);
    const char *(*decode_cip_error_long)(uint8_t *data);
    int (*decode_cip_error_code)(uint8_t *data);

    // int (*decode_error)(uint8_t *buf, uint32_t buf_size, uint16_le *extended_status, uint32_le *extended_status_size, const
    // char **short_desc, const char **long_desc);
} cip_generic_t;

extern cip_generic_t CIP;
