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
#include <ab/defs.h>
#include <util/rc.h>
#include <util/vector.h>

#define MAX_SESSION_HOST    (128)

#define MAX_PACKET_SIZE_EX  (44 + 4002)

/* the following are in microseconds*/
#define SESSION_DEFAULT_PACKET_INTERVAL (5000)
#define SESSION_MAX_PACKET_INTERVAL (100000)
#define SESSION_MIN_PACKET_INTERVAL (3000)
#define SESSION_PACKET_LOSS_INTERVAL_INC (5000)
#define SESSION_PACKET_RECEIVE_INTERVAL_DEC (1000)


/* resend interval in milliseconds*/
#define SESSION_DEFAULT_RESEND_INTERVAL_MS (50)
#define SESSION_MIN_RESEND_INTERVAL  (10)

#define SESSION_NUM_ROUND_TRIP_SAMPLES (5)

/* how long to wait for session registration before timing out. In milliseconds. */
//#define SESSION_REGISTRATION_TIMEOUT (1500)
#define SESSION_DEFAULT_TIMEOUT      (2000)

#define SESSION_MIN_REQUESTS    (10)
#define SESSION_INC_REQUESTS    (10)

/*
 * the queue depth depends on the type of the request.
 */

#define SESSION_MAX_CONNECTED_REQUESTS_IN_FLIGHT (1)
#define SESSION_MAX_UNCONNECTED_REQUESTS_IN_FLIGHT (4)

struct ab_session_t {
    int status;

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

    thread_p handler_thread;
    int terminating;
    mutex_p mutex;
};

struct ab_request_t {
    /* used to force interlocks with other threads. */
    lock_t lock;

    int status;

    /* flags for communicating with background thread */
    int resp_received;
    int abort_request;

    /* allow requests to be packed in the session */
    int allow_packing;
    int packing_num;

    /* time stamp for debugging output */
    int64_t time_sent;

    /* used by the background thread for incrementally getting data */
    int request_size; /* total bytes, not just data */
    int request_capacity;
    uint8_t data[];
};



uint64_t session_get_new_seq_id_unsafe(ab_session_p sess);
uint64_t session_get_new_seq_id(ab_session_p sess);

extern int session_startup();
extern void session_teardown();

extern int session_find_or_create(ab_session_p *session, attr attribs);
extern int session_create_request(ab_session_p session, ab_request_p *request);
extern int session_add_request(ab_session_p sess, ab_request_p req);

#endif
