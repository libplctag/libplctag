#pragma once

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

/*
 * The EtherNet/IP/CIP connection state that every CIP dialect shares.
 *
 * CIP_CONN_BASE_STRUCT follows the TAG_BASE_STRUCT idiom in lib/tag.h and the
 * CIP_TAG_BASE_STRUCT idiom in modules/cip/tag.h: a macro expanded into each
 * dialect's connection struct rather than a nested struct, so field accesses
 * stay direct.
 *
 * NOTE: expanding this into two dialect structs makes their common prefix
 * identical by construction.  That is deliberate, but nothing may cast one
 * dialect's connection to another's -- the fields past this macro differ, and
 * so does the meaning of plc_type, which each dialect declares itself with its
 * own enum.
 */

#include <libplctag/lib/conn_watch.h>
#include <platform.h>
#include <stdbool.h>
#include <stdint.h>
#include <utils/atomic_utils.h>
#include <utils/vector.h>


/* bytes of encap header and CPF connected header ahead of the CIP payload */
#define EIP_CIP_PREFIX_SIZE (44)

/* connection retry backoff */
#define RETRY_WAIT_INITIAL_MS (100)
#define RETRY_WAIT_MAX_MS (10000)


/* how long to block in one socket read or write before checking for shutdown */
#define SOCKET_WAIT_TIMEOUT_MS (20)

/* default timeout for a connection-level exchange such as Register or Forward Open */
#define SESSION_DEFAULT_TIMEOUT (2000)


/* EtherNet/IP encapsulation and Forward Open constants, common to every CIP dialect. */
#define CIP_EIP_DEFAULT_PORT (44818)
#define CIP_EIP_OK (0)
#define CIP_EIP_CONNECTED_SEND ((uint16_t)0x0070)
#define CIP_EIP_UNCONNECTED_SEND ((uint16_t)0x006F)
#define CIP_EIP_ITEM_NAI ((uint16_t)0x0000) /* NULL address item */
#define CIP_EIP_ITEM_UDI ((uint16_t)0x00B2) /* unconnected data item */
/*
 * Multiple Service Packet reply accounting.  Packed responses all have to fit
 * in one response packet; the PLC fails the whole exchange if they do not.
 */
#define CIP_MSP_OFFSET_ENTRY_SIZE (2)  /* the uint16 offset stored per packed packet */
#define CIP_MSP_MAX_PACKET_PADDING (8) /* worst-case padding between packed packets */
#define CIP_MSP_REPLY_OVERHEAD (2 + 4) /* the packet count field plus the CIP response header */
#define CIP_MSP_REPLY_SLACK (10)       /* empirical margin, see ephemeral_docs/deferred_fixes.md */


/* how long the handler sleeps when nothing else wakes it */
#define SESSION_IDLE_WAIT_TIME (100)


/* the connection handler's state machine */
typedef enum {
    SESSION_OPEN_SOCKET_START,
    SESSION_OPEN_SOCKET_WAIT,
    SESSION_REGISTER,
    SESSION_SEND_FORWARD_OPEN,
    SESSION_RECEIVE_FORWARD_OPEN,
    SESSION_IDLE,
    SESSION_DISCONNECT,
    SESSION_UNREGISTER,
    SESSION_CLOSE_SOCKET,
    SESSION_START_RETRY,
    SESSION_WAIT_ERR_RETRY,
    SESSION_WAIT_IDLE_RECONNECT
} session_state_t;


/* the most requests that may be bundled into one Multiple Service Packet */
#define MAX_REQUESTS (400)

#define CIP_ERR_UNSUPPORTED_SERVICE ((uint8_t)0x08)
#define CIP_ERR_PARTIAL_ERROR ((uint8_t)0x1e)
#define CIP_EIP_VERSION ((uint16_t)0x0001)
#define CIP_EIP_CMD_CIP_MULTI ((uint8_t)0x0A)
#define CIP_EIP_CMD_CIP_OK ((uint8_t)0x80)
#define CIP_EIP_REGISTER_SESSION ((uint16_t)0x0065)
#define CIP_EIP_UNREGISTER_SESSION ((uint16_t)0x0066)
#define CIP_EIP_CMD_FORWARD_CLOSE ((uint8_t)0x4E)
#define CIP_EIP_CMD_FORWARD_OPEN ((uint8_t)0x54)
#define CIP_EIP_CMD_FORWARD_OPEN_EX ((uint8_t)0x5B)
#define CIP_EIP_CONN_PARAM ((uint16_t)0x4200)
#define CIP_EIP_CONN_PARAM_EX ((uint32_t)0x42000000)
#define CIP_EIP_PLC5_PARAM ((uint16_t)0x4302)
#define CIP_EIP_RPI (1000000) /* in microseconds */
#define CIP_EIP_SECS_PER_TICK (0x0A)
#define CIP_EIP_TIMEOUT_TICKS (0x0E)
#define CIP_EIP_TIMEOUT_MULTIPLIER (0x03)
#define CIP_EIP_TRANSPORT_CLASS_T3 ((uint8_t)0xA3)
#define CIP_EIP_VENDOR_ID (0xF33D)      /* tres 1337 */
#define CIP_EIP_VENDOR_SN (0x21504345)  /* the string !PCE */


/*
 * The smallest payload a plain CIP connection can still issue a request in.
 * Every family has a fixed per-request overhead; below this there is no room
 * for a request at all and the size arithmetic downstream underflows.
 */
#define MIN_PAYLOAD_SIZE_CIP (500)


/*
 * A module's set of live connections.  Each dialect owns one and passes it in,
 * so AB and Omron keep separate connection pools: session_match_valid() keys
 * only on host and path, and the two dialects build a connection differently
 * for the same address.
 */
typedef struct {
    mutex_p mutex;
    vector_p conns;

    /* handler threads still running for this list, so teardown can wait them out */
    atomic_int32_t handler_count;
} cip_conn_list_t;


#define CIP_CONN_BASE_STRUCT                                                                        \
    int on_list;                                                                                    \
    cip_conn_list_t *owner_list; /* the module list this connection belongs to */                   \
                                                                                                    \
    /* gateway connection related info */                                                           \
    char *host;                                                                                     \
    int port;                                                                                       \
    char *path;                                                                                     \
    sock_p sock;                                                                                    \
                                                                                                    \
    /* connection variables. */                                                                     \
    bool use_connected_msg;                                                                         \
    bool only_use_old_forward_open;                                                                 \
    int fo_conn_size;    /* old FO max connection size */                                           \
    int fo_ex_conn_size; /* extended FO max connection size */                                      \
    uint16_t max_payload_guess;                                                                     \
    uint16_t max_payload_size;                                                                      \
                                                                                                    \
    uint32_t orig_connection_id;                                                                    \
    uint32_t targ_connection_id;                                                                    \
    uint16_t conn_seq_num;                                                                          \
    uint16_t conn_serial_number;                                                                    \
                                                                                                    \
    uint8_t *conn_path;                                                                             \
    uint8_t conn_path_size;                                                                         \
    uint16_t dhp_dest;                                                                              \
    int is_dhp;                                                                                     \
                                                                                                    \
    int connection_group_id;                                                                        \
                                                                                                    \
    /*                                                                                              \
     * What the dialect's PLC family implies, derived by the dialect's constructor.                 \
     * The shared code reads these rather than the dialect's own plc_type enum, so it never         \
     * has to know which family this is -- see cip_encode_path()'s CIP_PLC_KIND_* for the same      \
     * idea applied to path encoding.                                                               \
     */                                                                                             \
    uint16_t min_payload_size; /* smallest payload worth negotiating down to */                     \
    bool dhp_capable;          /* family can bridge to DH+ */                                       \
                                                                                                    \
    /*                                                                                              \
     * The dialect's own PLC family enum, stored opaquely.  Shared code must never                  \
     * interpret it: the two dialects' enums are deliberately distinct types (see A2), and          \
     * only the dialect that wrote this value may cast it back.  It is here so a tag created        \
     * from an @connection tag can inherit the family from the connection.                          \
     */                                                                                             \
    int32_t plc_type;                                                                               \
                                                                                                    \
    /* EIP session handle, from RegisterSession */                                                  \
    uint32_t session_handle;                                                                        \
                                                                                                    \
    /* sequence ID for requests */                                                                  \
    uint64_t session_seq_id;                                                                        \
                                                                                                    \
    /* list of outstanding requests for this connection */                                          \
    vector_p requests;                                                                              \
                                                                                                    \
    uint64_t resp_seq_id;                                                                           \
                                                                                                    \
    /*                                                                                              \
     * What we last put on the wire.  The response has to be an answer to the request we            \
     * actually sent, so these are snapshotted from the outgoing packet in send_eip_request()       \
     * and checked against the incoming one in recv_eip_response().                                 \
     */                                                                                             \
    uint16_t req_encap_command;                                                                     \
    uint64_t req_seq_id;                                                                            \
    bool req_sent;                                                                                  \
                                                                                                    \
    /* data for receiving messages */                                                               \
    uint32_t data_offset;                                                                           \
    uint32_t data_capacity;                                                                         \
    uint32_t data_size;                                                                             \
    uint8_t *data;                                                                                  \
    bool data_buffer_is_static;                                                                     \
                                                                                                    \
    uint64_t packet_count;                                                                          \
                                                                                                    \
    thread_p handler_thread;                                                                        \
    atomic_int32_t terminating;                                                                     \
    mutex_p session_mutex;                                                                          \
    cond_p session_wait_cond;                                                                       \
                                                                                                    \
    /* connection status and event ring - what connection tags observe */                           \
    conn_watch_t watch;                                                                             \
                                                                                                    \
    /* connection inactivity timeout - readable/writable by tags via atomics */                     \
    atomic_int32_t connection_inactivity_timeout_ms /* milliseconds */


/*
 * The EtherNet/IP connection object.  One type for every CIP dialect: the
 * dialects differ in how they construct one (see each module's create_*), not
 * in what one is.
 */
typedef struct cip_conn_t {
    CIP_CONN_BASE_STRUCT;
} cip_conn_t;

typedef cip_conn_t *cip_conn_p;


/* One request in flight on a cip_conn_t. */
struct cip_request_t {
    /* used to force interlocks with other threads. */
    lock_t lock;

    int status;

    /* flags for communicating with background thread */
    int resp_received;
    atomic_int32_t abort_request;

    /* debugging info */
    int tag_id;

    /* allow requests to be packed into one packet */
    int allow_packing;
    int packing_num;

    /* time stamp for debugging output */
    int64_t time_sent;

    /* used by the background thread for incrementally getting data */
    int request_size; /* total bytes, not just data */
    int request_capacity;

    /* how much reply space this request expects to consume, for packing against the reply budget */
    int response_size;

    /* a first read does not know its own size yet, so it cannot be packed */
    int first_read;

    uint8_t *data;
};

typedef struct cip_request_t cip_request_t;
typedef cip_request_t *cip_request_p;


/*
 * The negotiated payload for this connection: what the PLC agreed to, else
 * what the Forward Open asked for.
 */
#define GET_MAX_PAYLOAD_SIZE(conn) \
    (((conn)->max_payload_size > 0) ? (conn)->max_payload_size : (((conn)->fo_conn_size > 0) ? (conn)->fo_conn_size : (conn)->fo_ex_conn_size))


/* shared connection handling, see modules/cip/conn.c */

extern int session_list_init(cip_conn_list_t *list);
extern int session_list_add(cip_conn_list_t *list, cip_conn_p conn);
extern int session_list_add_unsafe(cip_conn_list_t *list, cip_conn_p conn);
extern int session_list_remove(cip_conn_list_t *list, cip_conn_p conn);
extern int session_list_remove_unsafe(cip_conn_list_t *list, cip_conn_p conn);
extern cip_conn_p session_list_find_by_host_unsafe(cip_conn_list_t *list, const char *host, const char *path,
                                                   int connection_group_id);

extern uint16_t next_conn_serial_number(uint16_t current);
extern uint64_t session_get_new_seq_id_unsafe(cip_conn_p conn);
extern uint64_t session_get_new_seq_id(cip_conn_p conn);
extern int session_match_valid(const char *host, const char *path, cip_conn_p conn);
extern int session_close_socket(cip_conn_p conn);
extern void cip_request_destroy(void *req_arg);
extern int session_request_increase_buffer(cip_request_p request, int new_capacity);
extern int session_get_available_cip_payload_space(cip_conn_p conn);

extern int send_eip_request(cip_conn_p conn, int timeout);
extern int recv_eip_response(cip_conn_p conn, int timeout);
extern int send_extended_forward_open_request(cip_conn_p conn);
extern int send_old_forward_open_request(cip_conn_p conn);
extern int send_forward_open_request(cip_conn_p conn);
extern int send_forward_close_req(cip_conn_p conn);
extern int recv_forward_close_resp(cip_conn_p conn);
extern int perform_forward_close(cip_conn_p conn);
extern int session_open_socket(cip_conn_p conn);
extern int prepare_request(cip_conn_p conn);
extern int session_unregister(cip_conn_p conn);
extern int get_payload_size(cip_request_p request);
extern int purge_aborted_requests_unsafe(cip_conn_p conn);
extern int pack_requests(cip_conn_p conn, cip_request_p *requests, int num_requests);
extern int unpack_response(cip_conn_p conn, cip_request_p request, int sub_packet);
extern int session_add_request_unsafe(cip_conn_p conn, cip_request_p req);
extern int session_add_request(cip_conn_p conn, cip_request_p req);
extern int64_t calc_retry_time(unsigned int retry_count);
extern int session_create_request(cip_conn_p conn, int tag_id, cip_request_p *req);
extern int session_register(cip_conn_p conn);
extern void session_destroy(void *conn_arg);
extern int receive_forward_open_response(cip_conn_p conn);

/*
 * How much of the single reply packet this request will consume: its own reply
 * plus the offset entry and the worst-case inter-packet padding.  A request
 * whose reply size is not yet known -- response_size of zero, typically because
 * the tag has never been read -- cannot be budgeted, so it draws nothing here
 * and is kept out of a bundle by the first_read gate instead.
 */
static inline int reply_budget_cost(cip_request_p request) {
    if(request->response_size <= 0) { return 0; }

    return request->response_size + CIP_MSP_MAX_PACKET_PADDING + CIP_MSP_OFFSET_ENTRY_SIZE;
}
extern int process_requests(cip_conn_p conn);
extern THREAD_FUNC(session_handler);
extern void session_set_connection_status(cip_conn_p conn, int32_t new_status);
