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

#ifndef __PLCTAG_AB_SESSION_H__
#define __PLCTAG_AB_SESSION_H__ 1

#include <ab/ab_common.h>
#include <ab/request.h>

#define MAX_SESSION_HOST    (128)

struct ab_session_t {
    ab_session_p next;
    ab_session_p prev;

    /* gateway connection related info */
    char host[MAX_SESSION_HOST];
    int port;
    sock_p sock;
    int is_connected;
    int status;

    /* registration info */
    uint32_t session_handle;
    int registered;

    /* Sequence ID for requests. */
    uint64_t session_seq_id;

    /* current request being sent, only one at a time */
    ab_request_p current_request;

    /* list of outstanding requests for this session */
    ab_request_p requests;

    /* counter for number of messages in flight */
    int num_reqs_in_flight;

    /* data for receiving messages */
    uint64_t resp_seq_id;
    int has_response;
    uint32_t recv_offset;
    uint8_t recv_data[MAX_REQ_RESP_SIZE];

    /*int recv_size;*/

    /* tags for this session */
    ab_tag_p tags;

    /* connections for this session */
    ab_connection_p connections;
    uint32_t conn_serial_number; /* id for the next connection */
};

uint64_t session_get_new_seq_id_unsafe(ab_session_p sess);
uint64_t session_get_new_seq_id(ab_session_p sess);

int find_or_create_session(ab_session_p *session, attr attribs);
int add_session_unsafe(ab_session_p n);
int add_session(ab_session_p s);
int remove_session_unsafe(ab_session_p n);
int remove_session(ab_session_p s);
ab_session_p find_session_by_host_unsafe(const char  *t);
int session_add_connection_unsafe(ab_session_p session, ab_connection_p connection);
int session_add_connection(ab_session_p session, ab_connection_p connection);
int session_remove_connection_unsafe(ab_session_p session, ab_connection_p connection);
int session_remove_connection(ab_session_p session, ab_connection_p connection);
int session_add_tag_unsafe(ab_session_p session, ab_tag_p tag);
int session_add_tag(ab_session_p session, ab_tag_p tag);
int session_remove_tag_unsafe(ab_session_p session, ab_tag_p tag);
int session_remove_tag(ab_session_p session, ab_tag_p tag);
ab_session_p session_create_unsafe(const char* host, int gw_port);
int session_init(ab_session_p session);
int session_connect(ab_session_p session);
int session_destroy_unsafe(ab_session_p session);
int session_destroy(ab_session_p session);
int session_is_empty(ab_session_p session);
int session_register(ab_session_p session);
int session_unregister_unsafe(ab_session_p session);

#endif
