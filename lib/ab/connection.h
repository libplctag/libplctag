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

#define MAX_CONN_PATH 		(128)


#include <platform.h>
#include <ab/ab_common.h>
#include <util/attr.h>
#include <ab/session.h>
#include <ab/tag.h>


struct ab_connection_t {
    ab_connection_p next;

    char path[MAX_CONN_PATH];

    ab_session_p session;

    int is_connected;
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

    int connect_in_progress;
    int status;
    int debug;

    ab_tag_p tags;
};



int find_or_create_connection(ab_tag_p tag, ab_session_p session, attr attribs);
ab_connection_p session_find_connection_by_path_unsafe(ab_session_p session,const char *path);
ab_connection_p connection_create_unsafe(int debug, const char* path, ab_session_p session);
int connection_perform_forward_open(ab_connection_p connection);
int send_forward_open_req(ab_connection_p connection, ab_request_p req);
int recv_forward_open_resp(ab_connection_p connection, ab_request_p req);
int connection_add_tag_unsafe(ab_connection_p connection, ab_tag_p tag);
int connection_add_tag(ab_connection_p connection, ab_tag_p tag);
int connection_remove_tag_unsafe(ab_connection_p connection, ab_tag_p tag);
int connection_remove_tag(ab_connection_p connection, ab_tag_p tag);
int connection_empty_unsafe(ab_connection_p connection);
int connection_is_empty(ab_connection_p connection);
int connection_destroy_unsafe(ab_connection_p connection);
int connection_close(ab_connection_p connection);
int send_forward_close_req(ab_connection_p connection, ab_request_p req);




#endif
