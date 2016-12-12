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
 * 2012-06-20  KRH - Created file.                                        *
 *                                                                        *
 **************************************************************************/


#ifndef __AB_AB_COMMON_H__
#define __AB_AB_COMMON_H__

#define MAX_CONN_PATH       (128)


#include <platform.h>
#include <ab/ab_common.h>
#include <util/attr.h>
#include <util/refcount.h>
#include <ab/session.h>
#include <ab/tag.h>

#define CONNECTION_MAX_IN_FLIGHT (7)

struct ab_connection_t {
    ab_connection_p next;

    char path[MAX_CONN_PATH];

    ab_session_p session;

    uint32_t targ_connection_id; /* the ID the target uses for this connection. */
    uint32_t orig_connection_id; /* the ID we use for this connection */
    uint16_t packet;
    uint16_t conn_serial_number;
    uint16_t conn_seq_num;

    /* need to save the connection path for later */
    uint8_t conn_path[MAX_CONN_PATH];
    uint8_t conn_path_size;

    /* how do we talk to this device? */
    int protocol_type;
    int use_dhp_direct;
    uint8_t dhp_src;
    uint8_t dhp_dest;
    uint16_t conn_params;

    /* useful status */
    int is_connected;
    int connect_in_progress;
    int disconnect_in_progress;
    int exclusive;
    int status;

    /* flag to avoid packet loss */
    //int request_in_flight[CONNECTION_MAX_IN_FLIGHT];
    //uint16_t seq_in_flight[CONNECTION_MAX_IN_FLIGHT];

    /* maintain a ref count. */
    refcount rc;

    /* list of tags that belong to this connection */
    //ab_tag_p tags;
};



extern int connection_find_or_create(ab_tag_p tag, attr attribs);
//extern int connection_acquire(ab_connection_p connection);
extern int connection_acquire(ab_connection_p connection);
extern int connection_release(ab_connection_p connection);



#endif
