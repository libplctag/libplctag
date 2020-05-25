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

#ifndef __PLCTAG_AB_SESSION_H__
#define __PLCTAG_AB_SESSION_H__ 1

#include <ab/ab_common.h>
#include <ab/defs.h>
#include <util/rc.h>
#include <util/vector.h>

/* #define MAX_SESSION_HOST    (128) */

#define SESSION_DEFAULT_TIMEOUT (2000)

#define MAX_PACKET_SIZE_EX  (44 + 4002)

#define SESSION_MIN_REQUESTS    (10)
#define SESSION_INC_REQUESTS    (10)


struct ab_session_t {
//    int status;
    int failed;

    /* gateway connection related info */
    char *host;
    int port;
    char *path;
    sock_p sock;

    /* connection variables. */
    int use_connected_msg;
    uint32_t orig_connection_id;
    uint32_t targ_connection_id;
    uint16_t conn_seq_num;
    uint16_t conn_serial_number;

    plc_type_t plc_type;

    uint16_t max_payload_size;
    uint8_t *conn_path;
    uint8_t conn_path_size;
    uint16_t dhp_dest;

    /* registration info */
    uint32_t session_handle;

    /* Sequence ID for requests. */
    uint64_t session_seq_id;

    /* list of outstanding requests for this session */
    vector_p requests;

    /* data for receiving messages */
    uint64_t resp_seq_id;
    uint32_t data_offset;
    uint32_t data_capacity;
    uint32_t data_size;
    uint8_t data[MAX_PACKET_SIZE_EX];

    uint64_t packet_count;

    thread_p handler_thread;
    volatile int terminating;
    mutex_p mutex;

    /* disconnect handling */
    int auto_disconnect_enabled;
    int auto_disconnect_timeout_ms;
};

struct ab_request_t {
    /* used to force interlocks with other threads. */
    lock_t lock;

    int status;

    /* flags for communicating with background thread */
    int resp_received;
    int abort_request;

    /* debugging info */
    int tag_id;

    /* allow requests to be packed in the session */
    int allow_packing;
    int packing_num;

    /* time stamp for debugging output */
    int64_t time_sent;

    /* used by the background thread for incrementally getting data */
    int request_size; /* total bytes, not just data */
    int request_capacity;
    uint8_t *data;
};



uint64_t session_get_new_seq_id_unsafe(ab_session_p sess);
uint64_t session_get_new_seq_id(ab_session_p sess);

extern int session_startup();
extern void session_teardown();

extern int session_find_or_create(ab_session_p *session, attr attribs);
extern int session_get_max_payload(ab_session_p session);
extern int session_create_request(ab_session_p session, int tag_id, ab_request_p *request);
extern int session_add_request(ab_session_p sess, ab_request_p req);

#endif
