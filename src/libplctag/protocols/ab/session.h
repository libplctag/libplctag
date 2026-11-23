/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
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
#    define __PLCTAG_AB_SESSION_H__ 1

#    include <stdbool.h>

#    include <libplctag/protocols/ab/ab_common.h>
#    include <libplctag/protocols/ab/defs.h>
#    include <utils/atomic_utils.h>
#    include <utils/rc.h>
#    include <utils/vector.h>

/* #define MAX_SESSION_HOST    (128) */

#    define SESSION_DEFAULT_TIMEOUT (2000)
/* idle timeout, as for Modbus: 2 s under the ~32 s the Forward Open asks for. */
#    define SESSION_DISCONNECT_TIMEOUT (30000)

#    define MAX_PACKET_SIZE_EX (44 + 4002)

/* one buffer size for every CIP PLC: wastes a little on old PLCs, none on Logix. */
#    define SESSION_DATA_CAPACITY (4096)

#    define MAX_CIP_LGX_MSG_SIZE (0x01FF & 504)
#    define MAX_CIP_LGX_MSG_SIZE_EX (0xFFFF & 4000)
#    define MAX_CIP_MICRO800_MSG_SIZE (0x01FF & 504)
#    define MAX_CIP_MICRO800_MSG_SIZE_EX (0xFFFF & 4000)
#    define MAX_CIP_OMRON_MSG_SIZE (0x01FF & 502)
#    define MAX_CIP_OMRON_MSG_SIZE_EX (0xFFFF & 1990)

/* maximum for PCCC embedded within CIP. */
#    define MAX_CIP_PCCC_MSG_SIZE (244)

#    define SESSION_MIN_REQUESTS (10)
#    define SESSION_INC_REQUESTS (10)

#    define MAX_CONN_PATH (260) /* 256 plus padding. */
#    define MAX_IP_ADDR_SEG_LEN (16)

/*
 * Longest gateway string we will copy into a session, NUL included.
 *
 * The attribute is "host[:port]" and is stored whole -- session_open_socket() splits it at
 * connect time rather than at create time.  A DNS name is at most 253 characters in dotted
 * form (the familiar 255 is the wire encoding, which adds a length byte per label and a
 * terminating zero), so 253 + ":65535" + NUL is 260.  Rounded up to keep the following
 * fields aligned.
 */
#    define MAX_SESSION_HOST_LEN (264)

#    define SESSION_CONN_STATUS_RING_SIZE (64)
#    define SESSION_CONN_STATUS_RING_SIZE_MASK (SESSION_CONN_STATUS_RING_SIZE - 1)


/* a connection's state machine; the state it keeps is private to session.c. */
struct session_sm_s;
typedef void (*cip_conn_step_func)(cip_conn_p conn, struct session_sm_s *sm);

typedef struct session_conn_status_entry_s {
    int32_t event_type;
    int32_t status;
    int32_t reason;
} session_conn_status_entry_t;


struct cip_conn_t {
    int on_list;
    struct cip_conn_t *next; /* the global session list, under the module's session_mutex. */

    /* gateway connection related info */
    char *host;
    int port;
    char *path;
    sock_p sock;

    /* connection variables. */
    bool use_connected_msg;
    bool only_use_old_forward_open;
    uint16_t max_payload_guess;
    uint16_t max_payload_size;

    uint32_t orig_connection_id;
    uint32_t targ_connection_id;
    uint16_t conn_seq_num;
    uint16_t conn_serial_number;

    /* AB_PLC_NONE until identify picks it when no plc= was given.  Under session_mutex. */
    plc_type_t plc_type;

    /* identify_step until the type is known, then connected_step or unconnected_step.  Handler thread only after create. */
    cip_conn_step_func step;

    /* why the PLC refused this session for good (identify or the route), or OK.  Under session_mutex. */
    int32_t refused_rc;

    /* the Forward Open path: the route, then the Message Router when connected. */
    uint8_t *conn_path;
    uint8_t conn_path_size;

    /* the longest wait between reconnects, from the connection_retry_max_delay_ms attribute. */
    atomic_int32_t connection_retry_max_delay_ms;

    /* the route alone, which an Unconnected Send carries. */
    uint8_t *route_path;
    uint8_t route_path_size;
    uint16_t dhp_dest;
    int is_dhp;

    int connection_group_id;

    /* registration info */
    uint32_t session_handle;

    /* Sequence ID for requests. */
    lock_t session_seq_id_lock;
    uint64_t session_seq_id;

    /* list of outstanding requests for this session */
    vector_p requests;

    uint64_t resp_seq_id;

    /*
     * What we last put on the wire.  The response has to be an answer to the request we
     * actually sent, so these are snapshotted from the outgoing packet in send_eip_request()
     * and checked against the incoming one in recv_eip_response().
     */
    uint16_t req_encap_command;
    uint64_t req_seq_id;
    bool req_sent;

    /* data for receiving messages */
    uint32_t data_offset;
    uint32_t data_capacity;
    uint32_t data_size;

    uint64_t packet_count;

    thread_p handler_thread;
    atomic_int32_t terminating;
    mutex_t session_mutex;

    /* connection status - readable by tags via atomics */
    atomic_int32_t connection_status;        /* plc_tag_conn_status_t values */
    atomic_int32_t connection_status_reason; /* additional info about the connection status, such as error codes */

    /* ring buffer of connection status changes; single writer (session thread), multiple independent readers */
    session_conn_status_entry_t conn_status_ring[SESSION_CONN_STATUS_RING_SIZE];
    atomic_int32_t
        conn_status_ring_write_idx; /* index of last written entry; wraps via & 0x07; tags drain by advancing their read idx */

    /* connection inactivity timeout - readable/writable by tags via atomics */
    atomic_int32_t connection_inactivity_timeout_ms; /* milliseconds */

    /* the packet being sent or received.  Big enough for the largest PLC type. */
    uint8_t data[SESSION_DATA_CAPACITY];
};


struct ab_request_t {
    /* used to force interlocks with other threads. */
    lock_t lock;

    int status;

    /* flags for communicating with background thread */
    int resp_received;
    atomic_int32_t abort_request;

    /* true while the session holds it, queued or in a batch.  Cleared just before the session's rc_dec. */
    atomic_bool in_session;

    /* debugging info */
    int tag_id;

    /*
     * data holds only the CIP request, and gets back only the CIP reply.  The session writes
     * the EIP and CPF framing.  connected sends it over the CIP connection.
     */
    bool connected;

    /* allow requests to be packed in the session */
    int allow_packing;

    /*
     * The CIP reply this request expects, header included, or 0 when unknown.  Only a PLC
     * without the fragmented services needs it: its reply cannot be cut short and continued,
     * so a packed request must leave room for every reply too.
     */
    int reply_size;
    int packing_num;

    /* time stamp for debugging output */
    int64_t time_sent;

    /* used by the background thread for incrementally getting data */
    int request_size; /* total bytes, not just data */
    int request_capacity;
    uint8_t *data;
};

/*
 * A tag's spare request is off limits until session_reuse_request() or the tag's destroy takes it.
 * Under ASan it is poisoned, so a stale pointer into it is reported instead of reading a reused
 * request.  The rc header before it stays unpoisoned: rc_inc()/rc_dec() still work.
 */
#if defined(__SANITIZE_ADDRESS__)
#    include <sanitizer/asan_interface.h>
#    define REQUEST_SPARE_POISON(r)                                                         \
        do {                                                                                \
            ASAN_POISON_MEMORY_REGION((r)->data, (size_t)(r)->request_capacity);            \
            ASAN_POISON_MEMORY_REGION((r), sizeof(*(r)));                                   \
        } while(0)
#    define REQUEST_SPARE_UNPOISON(r)                                                       \
        do {                                                                                \
            ASAN_UNPOISON_MEMORY_REGION((r), sizeof(*(r)));                                 \
            ASAN_UNPOISON_MEMORY_REGION((r)->data, (size_t)(r)->request_capacity);          \
        } while(0)
#else
#    define REQUEST_SPARE_POISON(r) ((void)(r))
#    define REQUEST_SPARE_UNPOISON(r) ((void)(r))
#endif


/* the next PCCC TNS: 16 bits, never 0. */
uint16_t session_get_new_seq_id(cip_conn_p sess);

extern int session_startup(void);

extern int session_find_or_create(cip_conn_p *session, attr attribs, int *is_new_session);
/* true when an identified connection is connected and the tag asks for unconnected, or the reverse. */
extern bool session_connected_conflicts(cip_conn_p session, int use_connected_msg);
/* true when the connection is identified and connected.  An unidentified one is unconnected for now. */
extern bool session_is_connected(cip_conn_p session);
extern int session_get_available_cip_payload_space(cip_conn_p session);
extern int session_create_request(cip_conn_p session, int tag_id, ab_request_p *request);
/* reuses spare, a request the session no longer holds, when it is big enough; else releases it and creates one. */
extern int session_reuse_request(cip_conn_p session, ab_request_p spare, int tag_id, ab_request_p *request);
extern int session_add_request(cip_conn_p sess, ab_request_p req);

/* The PLC type, AB_PLC_NONE while identify runs; *refused_rc is set when the PLC refused the session. */
extern plc_type_t session_plc_type(cip_conn_p session, int32_t *refused_rc);

#endif
