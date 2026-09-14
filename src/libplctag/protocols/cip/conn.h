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
 * The connection state both CIP modules keep.
 *
 * ab_session_t and omron_conn_t held the same thirty-nine fields; a module pastes
 * this after nothing and adds its own afterwards, the same arrangement as
 * CIP_TAG_STRUCT.  What stays module specific is the DH+ routing AB needs, and
 * each module's own way of publishing connection status to its tags -- AB has a
 * status ring with a reason code, Omron an event ring.
 *
 * NOTE: plc_type is not here.  Its type is a different enum in each module.
 */

#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/cip/tag.h>
#include <stdbool.h>
#include <stdint.h>
#include <utils/atomic_utils.h>
#include <utils/mutex.h>
#include <utils/nap.h>
#include <utils/socket.h>
#include <utils/spinlock.h>
#include <utils/thread.h>
#include <utils/vector.h>


#define CIP_CONN_STRUCT                                                                          \
    int failed;                                                                                  \
    int on_list;                                                                                 \
                                                                                                 \
    /* gateway connection related info */                                                        \
    char *host;                                                                                  \
    int port;                                                                                    \
    char *path;                                                                                  \
    sock_p sock;                                                                                 \
                                                                                                 \
    /* connection variables. */                                                                  \
    bool use_connected_msg;                                                                       \
    bool only_use_old_forward_open;                                                              \
                                                                                                 \
    /* what this PLC model can do and how big its payloads may be. */                             \
    cip_plc_config_t plc_config;                                                                 \
    uint16_t max_payload_guess;                                                                  \
    uint16_t max_payload_size;                                                                   \
                                                                                                 \
    uint32_t orig_connection_id;                                                                 \
    uint32_t targ_connection_id;                                                                 \
    uint16_t conn_seq_num;                                                                       \
    uint16_t conn_serial_number;                                                                 \
                                                                                                 \
    uint8_t *conn_path;                                                                          \
    uint8_t conn_path_size;                                                                      \
                                                                                                 \
    int connection_group_id;                                                                     \
                                                                                                 \
    /* registration info */                                                                      \
    uint32_t conn_handle;                                                                        \
                                                                                                 \
    /* Sequence ID for requests; guarded by mutex, see cip_conn_get_new_seq_id(). */              \
    uint64_t conn_seq_id;                                                                        \
                                                                                                 \
    /* list of outstanding requests for this connection */                                       \
    vector_p requests;                                                                           \
                                                                                                 \
    uint64_t resp_seq_id;                                                                        \
                                                                                                 \
    /*                                                                                           \
     * What we last put on the wire.  The response has to be an answer to the request we         \
     * actually sent, so these are snapshotted from the outgoing packet in send_eip_request()    \
     * and checked against the incoming one in recv_eip_response().                              \
     */                                                                                          \
    uint16_t req_encap_command;                                                                  \
    uint64_t req_seq_id;                                                                         \
    bool req_sent;                                                                               \
                                                                                                 \
    /* data for receiving messages */                                                            \
    uint32_t data_offset;                                                                        \
    uint32_t data_capacity;                                                                      \
    uint32_t data_size;                                                                          \
    uint8_t *data;                                                                               \
    bool data_buffer_is_static;                                                                  \
                                                                                                 \
    uint64_t packet_count;                                                                       \
                                                                                                 \
    thread_p handler_thread;                                                                     \
    atomic_int32_t terminating;                                                                  \
    mutex_p mutex;                                                                               \
    nap_p nap;                                                                                   \
                                                                                                 \
    /* connection status - readable by tags via atomics */                                       \
    atomic_int32_t connection_status;                                                            \
                                                                                                 \
    /* connection inactivity timeout - readable/writable by tags via atomics */                  \
    atomic_int32_t connection_inactivity_timeout_ms


/*
 * A connection holding just the shared fields.  Both modules' structs begin with
 * this sequence, so shared code takes one of these and each module casts.
 */
struct cip_conn_t {
    CIP_CONN_STRUCT;
};


/*
 * The next sequence ID for a request on this connection.
 *
 * The ID is what matches a response to the request that asked for it, so zero is
 * skipped on rollover: a zero could collide with an unset sender context.
 */
extern uint64_t cip_conn_get_new_seq_id(cip_conn_p conn);


/*
 * The socket belongs to the module, so the shared code asks it to move bytes.
 * Both calls keep the signature the modules already use.
 */
typedef struct {
    int (*send_request)(cip_conn_p conn, int timeout);
    int (*recv_response)(cip_conn_p conn, int timeout);
} cip_conn_io_t;

/* the timeout both modules used for connection setup traffic. */
#define CIP_CONN_DEFAULT_TIMEOUT (2000)

/*
 * The OFFSET of the payload within a connected request frame, not the size of any
 * struct.  It is offsetof(eip_cip_co_req, cpf_conn_seq_num), and the request
 * builders measure a payload from that same field onwards -- so
 *
 *     EIP_CIP_PREFIX_SIZE + payload == the bytes written
 *
 * holds by construction, and a buffer of available_payload + EIP_CIP_PREFIX_SIZE is
 * exactly the worst case with no slack.
 *
 * CAUTION: sizeof(eip_cip_co_req) is 46 and sizeof(eip_cip_uc_req) is 50, so this
 * constant looks two and six bytes short against them.  It is not; those trailing
 * bytes are counted as part of the payload.  Do not "correct" it to a struct size
 * without also changing how the builders measure, or every request buffer grows.
 *
 * The unconnected frame puts its payload at offset 40 rather than 44, and
 * available_payload_unsafe() makes up the difference by subtracting the encoded
 * connection path plus its length and padding bytes.
 */
#define EIP_CIP_PREFIX_SIZE (44)

/*
 * The payload size to work with: what the PLC agreed to if a Forward Open has
 * completed, otherwise the size we intend to ask for.
 */
#define GET_MAX_PAYLOAD_SIZE(conn)                                                  \
    (((conn)->max_payload_size > 0)                                                 \
         ? ((conn)->max_payload_size)                                               \
         : (((conn)->plc_config.fo_conn_size > 0) ? ((conn)->plc_config.fo_conn_size) \
                                                  : ((conn)->plc_config.fo_ex_conn_size)))


/*
 * Forward Open.  send_forward_open() picks the old or the extended form and sends
 * it; receive_forward_open_response() handles the reply, including negotiating the
 * payload size down and retrying, which the caller sees as PLCTAG_ERR_TOO_LARGE.
 */
extern int cip_send_forward_open(cip_conn_p conn, const cip_conn_io_t *io);
extern int cip_receive_forward_open_response(cip_conn_p conn, const cip_conn_io_t *io);


/* payload left for a request once the CPF framing is accounted for. */
extern int cip_conn_get_available_payload_space(cip_conn_p conn);

/* allocate a request and a buffer big enough for one packet on this connection. */
extern int cip_conn_create_request(cip_conn_p conn, int tag_id, cip_request_p *req);

/* pull one response out of a packed multi-response packet. */
extern int cip_unpack_response(cip_conn_p conn, cip_request_p request, int sub_packet);

/* tear the CIP connection down. */
extern int cip_perform_forward_close(cip_conn_p conn, const cip_conn_io_t *io);

/* payload size of a built request, or INT_MAX if it cannot be measured. */
extern int cip_get_payload_size(cip_request_p request);
