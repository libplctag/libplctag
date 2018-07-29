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

#ifndef __PLCTAG_AB_REQUEST_H__
#define __PLCTAG_AB_REQUEST_H__ 1

#include <ab/ab_common.h>
#include <util/rc.h>
#include <ab/defs.h>


//#define MAX_REQ_RESP_SIZE   (MAX_EIP_CIP_PACKET_SIZE_EX) /* enough? */

/*
 * this structure contains data necessary to set up a request and hold
 * the resulting response.
 */

struct ab_request_t {
    ab_request_p next;  /* for linked list */

    int req_id;         /* which request is this for the tag? */
    int data_size;      /* how many bytes did we get? */

    /* flags for communicating with background thread */
    int send_request;
    int send_in_progress;
    int resp_received;
    int recv_in_progress;
    int _abort_request;
    int abort_after_send; /* for one shot packets */
    int no_resend; /* do not resend if this is set. */
    int connected_request; /* serialize this packet with respect to other serialized packets. */

    int status;

    /* used when processing a response */
    int processed;

    mutex_p request_mutex;
<<<<<<< HEAD
=======
    ab_tag_p tag;
>>>>>>> Added tickler function and session-specific thread.
    ab_session_p session;
//    ab_connection_p connection;
    int allow_packing;
    int packing_num;

    uint64_t session_seq_id;
    uint32_t conn_id;
    uint16_t conn_seq;

    /* time stamps for rate calculations */
    int64_t time_sent;
    int send_count;

    /* used by the background thread for incrementally getting data */
    int current_offset;
    int request_size; /* total bytes, not just data */
    int request_capacity;
    uint8_t data[];
};





extern int request_create(ab_request_p *req, int max_payload_size);
extern int request_abort(ab_request_p req);
extern int request_check_abort(ab_request_p req);
//extern ab_tag_p request_get_tag(ab_request_p req);
extern int request_allow_packing(ab_request_p req);
extern int request_check_packing(ab_request_p req);

#endif
