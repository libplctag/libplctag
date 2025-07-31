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

#ifndef __PLCTAG_OMRON_CONN_H__
#define __PLCTAG_OMRON_CONN_H__ 1

#include <stdbool.h>

#include <libplctag/protocols/omron/defs.h>
#include <libplctag/protocols/omron/omron_common.h>
#include <utils/rc.h>
#include <utils/vector.h>

/* #define MAX_CONN_HOST    (128) */

#define CONN_DEFAULT_TIMEOUT (2000)

#define MAX_PACKET_SIZE_EX (44 + 4002)

#define CONN_MIN_REQUESTS (10)
#define CONN_INC_REQUESTS (10)

#define MAX_CONN_PATH (260) /* 256 plus padding. */
#define MAX_IP_ADDR_SEG_LEN (16)


struct omron_conn_t {
    //    int status;
    int failed;
    int on_list;

    /* gateway connection related info */
    char *host;
    int port;
    char *path;
    sock_p sock;

    /* connection variables. */
    bool use_connected_msg;
    bool only_use_old_forward_open;
    int fo_conn_size;    /* old FO max connection size */
    int fo_ex_conn_size; /* extended FO max connection size */
    uint16_t max_payload_guess;
    uint16_t max_payload_size;

    uint32_t orig_connection_id;
    uint32_t targ_connection_id;
    uint16_t conn_seq_num;
    uint16_t conn_serial_number;

    plc_type_t plc_type;

    uint8_t *conn_path;
    uint8_t conn_path_size;
    uint16_t dhp_dest;
    int is_dhp;

    int connection_group_id;

    /* registration info */
    uint32_t conn_handle;

    /* Sequence ID for requests. */
    uint64_t conn_seq_id;

    /* list of outstanding requests for this conn */
    vector_p requests;

    uint64_t resp_seq_id;

    /* data for receiving messages */
    uint32_t data_offset;
    uint32_t data_capacity;
    uint32_t data_size;
    uint8_t *data;
    bool data_buffer_is_static;
    // uint8_t data[MAX_PACKET_SIZE_EX];

    uint64_t packet_count;

    thread_p handler_thread;
    volatile int terminating;
    mutex_p mutex;
    cond_p wait_cond;

    /* disconnect handling */
    int auto_disconnect_enabled;
    int auto_disconnect_timeout_ms;
};


struct omron_request_t {
    /* used to force interlocks with other threads. */
    lock_t lock;

    int status;

    /* flags for communicating with background thread */
    int resp_received;
    int abort_request;

    /* debugging info */
    int tag_id;

    /* allow requests to be packed in the conn */
    int allow_packing;
    int packing_num;

    /* time stamp for debugging output */
    int64_t time_sent;

    /* used by the background thread for incrementally getting data */
    int request_size; /* total bytes, not just data */
    int request_capacity;
    int response_size; /* size of data we expect to be returned by this request */

    int first_read;               /* whether this tag is being read for the first time and its size is therefor unknown*/
    int supports_fragmented_read; /* if fragmented read is supported then we do not need to worry about the response*/
    uint8_t *data;
};


uint64_t conn_get_new_seq_id_unsafe(omron_conn_p sess);
uint64_t conn_get_new_seq_id(omron_conn_p sess);

extern int conn_startup(void);
extern void conn_teardown(void);

extern int conn_find_or_create(omron_conn_p *conn, attr attribs);
extern int conn_get_max_payload(omron_conn_p conn);
extern int conn_get_available_cip_payload_space(omron_conn_p conn);
extern int conn_create_request(omron_conn_p conn, int tag_id, omron_request_p *request);
extern int conn_add_request(omron_conn_p sess, omron_request_p req);

#endif
