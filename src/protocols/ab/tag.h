/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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
#define __PLCTAG_AB_TAG_H__ 1

/* do these first */
#define MAX_TAG_NAME        (260)
#define MAX_TAG_TYPE_INFO   (64)
#define MAX_CONN_PATH       (260)   /* 256 plus padding. */

/* they are used in some of these includes */
#include <lib/libplctag.h>
#include <lib/tag.h>
#include <ab/ab_common.h>
#include <ab/session.h>
#include <ab/pccc.h>

typedef enum {
    AB_TYPE_BOOL,
    AB_TYPE_BOOL_ARRAY,
    AB_TYPE_CONTROL,
    AB_TYPE_COUNTER,
    AB_TYPE_FLOAT32,
    AB_TYPE_FLOAT64,
    AB_TYPE_INT8,
    AB_TYPE_INT16,
    AB_TYPE_INT32,
    AB_TYPE_INT64,
    AB_TYPE_STRING,
    AB_TYPE_SHORT_STRING,
    AB_TYPE_TIMER,
    AB_TYPE_TAG_ENTRY,  /* not a real AB type, but a pseudo UDT. */
    AB_TYPE_TAG_RAW     /* raw CIP tag */
} elem_type_t;


struct ab_tag_t {
    /*struct plc_tag_t p_tag;*/
    TAG_BASE_STRUCT;

    /* how do we talk to this device? */
    plc_type_t plc_type;

    /* pointers back to session */
    ab_session_p session;
    int use_connected_msg;

    /* this contains the encoded name */
    uint8_t encoded_name[MAX_TAG_NAME];
    int encoded_name_size;

//    const char *read_group;

    /* storage for the encoded type. */
    uint8_t encoded_type_info[MAX_TAG_TYPE_INFO];
    int encoded_type_info_size;

    /* how much data can we send per packet? */
    int write_data_per_packet;

    /* number of elements and size of each in the tag. */
    pccc_file_t file_type;
    elem_type_t elem_type;

    int elem_count;
    int elem_size;

    int special_tag;
    uint32_t next_id;

    //int is_bit;
    //uint8_t bit;

    /* requests */
    int pre_write_read;
    int first_read;
    ab_request_p req;
    int offset;

    int allow_packing;

    /* flags for operations */
    int read_in_progress;
    int write_in_progress;
    /*int connect_in_progress;*/
};




#endif
