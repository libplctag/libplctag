/***************************************************************************
 *   Copyright (C) 2015 by OmanTek                                         *
 *   Author Kyle Hayes  kylehayes@omantek.com                              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/**************************************************************************
 * CHANGE LOG                                                             *
 *                                                                        *
 * 2015-09-12  KRH - Created file.                                        *
 *                                                                        *
 **************************************************************************/

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
//#include <ab/connection.h>
#include <ab/request.h>


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
    AB_TYPE_TIMER
} elem_type_t;


struct ab_tag_t {
    /*struct plc_tag_t p_tag;*/
    TAG_BASE_STRUCT;

    /* how do we talk to this device? */
    int protocol_type;
//    int use_dhp_direct;
//    uint8_t dhp_src;
//    uint8_t dhp_dest;

    /* pointers back to session */
    ab_session_p session;
    int use_connected_msg;

    /* this contains the encoded name */
    uint8_t encoded_name[MAX_TAG_NAME];
    int encoded_name_size;

    const char *read_group;

    /* the connection IOI path */
//    uint8_t conn_path[MAX_CONN_PATH];
//    uint8_t conn_path_size;

    /* storage for the encoded type. */
    uint8_t encoded_type_info[MAX_TAG_TYPE_INFO];
    int encoded_type_info_size;

    /* how much data can we send per packet? */
    int write_data_per_packet;

    /* number of elements and size of each in the tag. */
    elem_type_t elem_type;
    int elem_count;
    int elem_size;

    /* requests */
    int pre_write_read;
    int first_read;
    ab_request_p req;
    int byte_offset;

    int allow_packing;

    /* flags for operations */
    int read_in_progress;
    int write_in_progress;
    /*int connect_in_progress;*/
};




#endif
