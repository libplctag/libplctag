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


#include <libplctag/lib/conn_timeouts.h>
#include <libplctag/lib/conn_watch.h>
#include <libplctag/modules/cip/plc_type.h>
#include <platform.h>
#include <stdbool.h>
#include <stdint.h>
#include <utils/atomic_utils.h>
#include <utils/vector.h>


/*
 * The most framing the transport ever puts around a CIP message: the unconnected form, which
 * is the largest, plus a route path.  conn_path_size is a uint8_t, so 255 bounds it, and the
 * two extra bytes are the path's own size and pad bytes.
 *
 * Only the connection's assembled-packet buffer needs this.  A request holds its CIP message
 * alone and is sized from the payload.
 */
#define CIP_MAX_FRAMING_SIZE ((int)sizeof(eip_cip_uc_req) + 2 + 255)

/* how long to block in one socket read or write before checking for shutdown */
#define SOCKET_WAIT_TIMEOUT_MS (20)


/* EtherNet/IP encapsulation and Forward Open constants, common to every CIP dialect. */
#define CIP_EIP_DEFAULT_PORT (44818)
#define CIP_EIP_OK (0)
#define CIP_EIP_CONNECTED_SEND ((uint16_t)0x0070)
#define CIP_EIP_UNCONNECTED_SEND ((uint16_t)0x006F)
#define CIP_EIP_ITEM_NAI ((uint16_t)0x0000) /* NULL address item */
#define CIP_EIP_ITEM_UDI ((uint16_t)0x00B2) /* unconnected data item */
#define CIP_EIP_ITEM_CAI ((uint16_t)0x00A1) /* connected address item */
#define CIP_EIP_ITEM_CDI ((uint16_t)0x00B1) /* connected data item */
#define CIP_EIP_CMD_UNCONNECTED_SEND ((uint8_t)0x52)

/*
 * Bytes the Unconnected Send wrapper adds around the embedded CIP message:
 * service code, path size, the four-byte Connection Manager path, the two
 * timeout tick bytes and the embedded length ahead of it, and the pad byte
 * an odd-length message needs after it.
 */
#define CIP_EIP_UC_SEND_OVERHEAD (11)
/*
 * Multiple Service Packet reply accounting.  Packed responses all have to fit
 * in one response packet; the PLC fails the whole exchange if they do not.
 */
#define CIP_MSP_OFFSET_ENTRY_SIZE (2)  /* the uint16 offset stored per packed packet */
#define CIP_MSP_MAX_PACKET_PADDING (8) /* worst-case padding between packed packets */
#define CIP_MSP_REPLY_OVERHEAD (2 + 4) /* the packet count field plus the CIP response header */
#define CIP_MSP_REPLY_SLACK (10)       /* empirical margin, see ephemeral_docs/deferred_fixes.md */


/*
 * What we ask the PLC to hold the connection open for.
 *
 * Derived from the library's own idle timeout, not the other way around.  EtherNet/IP
 * expresses the connection timeout as RPI x 4 x 2^multiplier, and the multiplier is a
 * three-bit field on the wire, so the grantable values are coarse.  Pick the smallest one
 * that covers the library's idle timeout plus the margin we close within; rounding is
 * upward by construction, since the next multiplier down would not reach the required
 * value.  The library therefore always hangs up before the PLC does, whatever
 * CONN_INACTIVITY_TIMEOUT_MAX_MS is set to.
 *
 * RPI is defined here rather than with the other EtherNet/IP constants below because the
 * #if ladder is evaluated where it stands.
 */
#define CIP_EIP_RPI (1000000) /* in microseconds */
#define CIP_EIP_RPI_MS (CIP_EIP_RPI / 1000)

#define CIP_EIP_CONN_TIMEOUT_REQUIRED_MS (CONN_INACTIVITY_TIMEOUT_MAX_MS + CONN_CLOSE_FIRST_MARGIN_MS)

#if (CIP_EIP_RPI_MS * 4) >= CIP_EIP_CONN_TIMEOUT_REQUIRED_MS
#    define CIP_EIP_TIMEOUT_MULTIPLIER (0)
#elif (CIP_EIP_RPI_MS * 8) >= CIP_EIP_CONN_TIMEOUT_REQUIRED_MS
#    define CIP_EIP_TIMEOUT_MULTIPLIER (1)
#elif (CIP_EIP_RPI_MS * 16) >= CIP_EIP_CONN_TIMEOUT_REQUIRED_MS
#    define CIP_EIP_TIMEOUT_MULTIPLIER (2)
#elif (CIP_EIP_RPI_MS * 32) >= CIP_EIP_CONN_TIMEOUT_REQUIRED_MS
#    define CIP_EIP_TIMEOUT_MULTIPLIER (3)
#elif (CIP_EIP_RPI_MS * 64) >= CIP_EIP_CONN_TIMEOUT_REQUIRED_MS
#    define CIP_EIP_TIMEOUT_MULTIPLIER (4)
#elif (CIP_EIP_RPI_MS * 128) >= CIP_EIP_CONN_TIMEOUT_REQUIRED_MS
#    define CIP_EIP_TIMEOUT_MULTIPLIER (5)
#elif (CIP_EIP_RPI_MS * 256) >= CIP_EIP_CONN_TIMEOUT_REQUIRED_MS
#    define CIP_EIP_TIMEOUT_MULTIPLIER (6)
#elif (CIP_EIP_RPI_MS * 512) >= CIP_EIP_CONN_TIMEOUT_REQUIRED_MS
#    define CIP_EIP_TIMEOUT_MULTIPLIER (7)
#else
#    error "CONN_INACTIVITY_TIMEOUT_MAX_MS is longer than an EtherNet/IP connection timeout can express.  Lower it or raise CIP_EIP_RPI."
#endif

/* what the PLC actually grants: the required value rounded up to the chosen multiplier. */
#define CIP_EIP_CONN_TIMEOUT_MS (CIP_EIP_RPI_MS * 4 * (1 << CIP_EIP_TIMEOUT_MULTIPLIER))

/*
 * How long to wait for the answer to a connection-level exchange: Register Session,
 * Forward Open, and the request path.
 *
 * The same budget we give the PLC.  CIP_EIP_CONN_TIMEOUT_MS is what goes on the wire as
 * the Unconnected Send route timeout and what the connection is negotiated with, so
 * waiting any less means giving up before the deadline we ourselves granted -- a slow but
 * healthy PLC gets abandoned while it is still entitled to answer.  This was 2000 ms
 * against a 32000 ms budget.
 */
#define SESSION_DEFAULT_TIMEOUT (CIP_EIP_CONN_TIMEOUT_MS)

/*
 * The shortest deadline a request on an established connection may be given.
 *
 * process_requests() takes its deadline from connection_inactivity_timeout_ms, which
 * conn_create() has already clamped to CONN_INACTIVITY_TIMEOUT_MIN_MS and above, so this
 * does not bind today.  It is a backstop against that clamp moving or a future caller
 * reaching the request path without one, and it is set where a reply is physically
 * impossible rather than merely slow: a current PLC on a local gigabit network answers in
 * well under a millisecond, so 10ms is still an order of magnitude of headroom.
 */
#define CIP_REQUEST_MIN_TIMEOUT_MS (10)


/*
 * Connection retry backoff bounds.  The cap is the same budget we give the PLC for a
 * connection-level exchange: there is no point retrying faster than the deadline we
 * ourselves grant, and none in waiting longer.  CIP_EIP_CONN_TIMEOUT_MS is 32000 ms.
 *
 * The growth and jitter are backoff_wait_ms()'s, not ours.
 */
#define RETRY_WAIT_INITIAL_MS (100)
#define RETRY_WAIT_MAX_MS (SESSION_DEFAULT_TIMEOUT)


/* how long teardown waits for connections and then handler threads to finish */
#define SESSION_TEARDOWN_TIMEOUT_MS (5000)


/* initial size and growth step of a connection's outstanding-request vector */
#define SESSION_MIN_REQUESTS (10)
#define SESSION_INC_REQUESTS (10)


/* limits on the strings copied into a connection's allocation */
#define MAX_SESSION_HOST_LEN (264)
#define MAX_CONN_PATH (260) /* 256 plus padding. */
#define MAX_IP_ADDR_SEG_LEN (16)


/* how long the handler sleeps when nothing else wakes it */
#define SESSION_IDLE_WAIT_TIME (100)


/* the connection handler's state machine */
typedef enum {
    SESSION_OPEN_SOCKET_START,
    SESSION_OPEN_SOCKET_WAIT,
    SESSION_REGISTER,
    SESSION_IDENTIFY,
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

/* the most tags the handler starts auto-sync work for in one pass */
#define CIP_TICKLE_BATCH (64)

#define CIP_ERR_UNSUPPORTED_SERVICE ((uint8_t)0x08)
#define CIP_ERR_PARTIAL_ERROR ((uint8_t)0x1e)

/*
 * The general and extended status codes a Forward Open can fail with, from the CIP
 * specification's Connection Manager object.  They are what decides whether a failure is
 * worth another attempt, so they are named rather than written as numbers at the use site.
 */
#define CIP_ERR_CONN_FAILURE ((uint8_t)0x01)
#define CIP_ERR_RESOURCE_UNAVAILABLE ((uint8_t)0x02)

#define CIP_EXT_ERR_DUPLICATE_CONN ((uint16_t)0x0100)
#define CIP_EXT_ERR_BAD_SIZE ((uint16_t)0x0109)
#define CIP_EXT_ERR_CONN_LIMIT ((uint16_t)0x0113)
#define CIP_EXT_ERR_UNCONNECTED_TIMEOUT ((uint16_t)0x0203)
#define CIP_EXT_ERR_CONN_TIMEOUT ((uint16_t)0x0204)
#define CIP_EXT_ERR_BAD_PORT ((uint16_t)0x0311)
#define CIP_EXT_ERR_BAD_LINK_ADDR ((uint16_t)0x0312)
#define CIP_EXT_ERR_BAD_SEGMENT ((uint16_t)0x0315)
#define CIP_EXT_ERR_LINK_OFFLINE ((uint16_t)0x0800)
#define CIP_EXT_ERR_BAD_OT_SIZE ((uint16_t)0x0127)
#define CIP_EXT_ERR_BAD_TO_SIZE ((uint16_t)0x0128)

/* matches whatever extended status came back, including none at all */
#define CIP_EXT_STATUS_ANY ((uint16_t)0xFFFF)

/*
 * The largest connection a plain Forward Open can carry.  Above this the extended service
 * is required, so the size the negotiation settles on decides which service is used.
 */
#define CIP_FO_PLAIN_MAX_SIZE ((uint16_t)504)

/*
 * How many times to retry a Forward Open the PLC rejected for a duplicate connection ID.
 * Each attempt uses a fresh serial number, so a bounded number clears a stale connection
 * the PLC has not timed out yet; past that the PLC is refusing for some other reason and
 * retrying at full speed only makes it worse.
 */
#define CIP_FO_MAX_DUP_RETRIES (20)
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
#define CIP_EIP_TRANSPORT_CLASS_T3 ((uint8_t)0xA3)
#define CIP_EIP_VENDOR_ID (0xF33D)     /* tres 1337 */
#define CIP_EIP_VENDOR_SN (0x21504345) /* the string !PCE */


/*
 * The smallest payload a plain CIP connection can still issue a request in.
 * Every family has a fixed per-request overhead; below this there is no room
 * for a request at all and the size arithmetic downstream underflows.
 */
#define MIN_PAYLOAD_SIZE_CIP (500)


/*
 * The PLC family, cip/family.h.  Named only as an opaque pointer here, so this file still
 * knows nothing about any particular family.
 */
typedef struct cip_family_t cip_family_t;

/* a CIP tag, defined in cip/tag.h, which includes this file */
struct cip_tag_t;


/*
 * The set of live connections.  There is one, in conn.c, for every CIP family;
 * a connection records it so it can remove itself.
 */
typedef struct {
    mutex_p mutex;
    vector_p conns;

    /* handler threads still running for this list, so teardown can wait them out */
    atomic_int32_t handler_count;
} cip_conn_list_t;


/*
 * The EtherNet/IP connection object.  One type for every CIP dialect: the
 * dialects differ in how they construct one (see each module's create_*), not
 * in what one is.
 */
typedef struct cip_conn_t {
    /*
     * The handler thread's state machine position.  It lives on the object rather than on
     * the handler's stack so that something outside the handler can observe it.  Only the
     * handler writes it.
     */
    session_state_t state;

    int on_list;
    cip_conn_list_t *owner_list; /* the module list this connection belongs to */

    /* gateway connection related info */
    char *host;
    int port;
    char *path;
    sock_p sock;

    /* connection variables. */
    bool use_connected_msg;

    /*
     * Which Forward Open service to use.  It starts at what the family says and the
     * negotiation can change it, so unlike everything else the family supplies, this is
     * state and not a constant.
     */
    bool only_use_old_forward_open;

    uint16_t max_payload_guess;
    uint16_t max_payload_size;

    /*
     * Where the Forward Open negotiation has got to.  The opening ask comes from the
     * family's own sizes above; these track the retries after it is refused.
     */
    uint16_t fo_offered_size; /* size the PLC named as supported, 0 if it named none */
    int32_t fo_ladder_idx;    /* rung of the fallback size ladder, -1 before the first step */
    uint32_t fo_dup_retries;  /* duplicate connection ID retries spent */
    bool fo_tried_plain;      /* the plain Forward Open has already been refused */
    bool fo_plc_answered;     /* the last Forward Open got a reply, as opposed to no reply at all */
    uint16_t fo_ext_status;   /* extended status of the last Forward Open refusal, 0 if it had none */

    uint32_t orig_connection_id;
    uint32_t targ_connection_id;
    uint16_t conn_seq_num;
    uint16_t conn_serial_number;

    uint8_t *conn_path;
    uint8_t conn_path_size;
    uint16_t dhp_dest;
    int is_dhp;

    int connection_group_id;

    /*
     * The PLC type the tag string named, CIP_PLC_NONE when it named none.  Part of the match
     * key, and fixed for the connection's life: a probed connection stays NONE after it has
     * identified the device, so later tags that also name no PLC still find it.
     */
    cip_plc_type_t plc_type;

    /*
     * Why the Identity probe refused the device, PLCTAG_STATUS_OK if it has not.  A refusal is
     * an answer about the device rather than a network failure, so the tags waiting on the
     * connection fail with it instead of waiting out their timeouts.  Read and written under
     * session_mutex, as family is on a probed connection.
     */
    int identify_rc;

    /*
     * The family this connection was created for -- the DH+ one for a DH+ path -- with its
     * sizing and its tag setup.  Families are static constants with program lifetime, so this
     * is never dangling and never written through.
     *
     * NULL on a probed connection until the handler has read the Identity object.  The
     * handler writes it once, under session_mutex, before any request goes out; a tag reads
     * it through cip_conn_family().  Everything else that reads it runs after a tag has
     * been set up, or on the handler thread itself.
     *
     * Shared code reads what the family implies -- min_payload_size, DH+ -- rather than
     * plc_type, so it never has to know which family this is.  A tag asks the connection
     * whether it is DH+ instead of re-reading a path attribute it may not have -- a cloned
     * tag has no path of its own.
     */
    const cip_family_t *family;

    /* EIP session handle, from RegisterSession */
    uint32_t session_handle;

    /* sequence ID for requests */
    uint64_t session_seq_id;

    /*
     * The tags the handler has to act on, in order of when: a tag with an operation to send is
     * due when it was started, and a tag armed for auto-sync is due at its next deadline.  Kept
     * sorted with lib/tag_schedule.h.  Raw pointers under session_mutex: a tag unlinks itself
     * before it is freed (cip_conn_forget_tag()), so the list never holds a reference.
     */
    vector_p active;

    /*
     * Tags with events to report, each holding a reference, whose callbacks run once the handler
     * has released session_mutex.  Only the handler thread touches it.
     */
    vector_p notify;

    /*
     * The exchange being built or in flight.  The encoders write the requests back to back
     * into staging, and slots records where each one is.  A slot whose tag is NULL was
     * aborted or destroyed while the exchange was on the wire; its reply is dropped.
     */
    uint8_t *staging;
    struct {
        struct cip_tag_t *tag;
        int32_t offset;
        int32_t size;
        bool is_read;
    } slots[MAX_REQUESTS];
    int32_t slot_count;
    bool exchange_unrouted;

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
    uint8_t *data;
    bool data_buffer_is_static;

    uint64_t packet_count;

    thread_p handler_thread;
    atomic_int32_t terminating;
    mutex_p session_mutex;
    cond_p session_wait_cond;

    /* connection status and event ring - what connection tags observe */
    conn_event_ring_t watch;

    /* connection inactivity timeout - readable/writable by tags via atomics */
    atomic_int32_t connection_inactivity_timeout_ms; /* milliseconds */

    /* longest delay between reconnect attempts after an error - likewise live via atomics */
    atomic_int32_t connection_retry_max_delay_ms /* milliseconds */;
} cip_conn_t;

typedef cip_conn_t *cip_conn_p;




/*
 * The negotiated payload for this connection: what the PLC agreed to, else
 * what the Forward Open asked for.
 */
#define GET_MAX_PAYLOAD_SIZE(conn)    \
    (((conn)->max_payload_size > 0) ? \
         (conn)->max_payload_size :   \
         (((conn)->family->fo_conn_size > 0) ? (conn)->family->fo_conn_size : (conn)->family->fo_ex_conn_size))


/* shared connection handling, see modules/cip/conn.c */

extern int session_list_add_unsafe(cip_conn_list_t *list, cip_conn_p conn);
extern int session_list_remove(cip_conn_list_t *list, cip_conn_p conn);

extern int session_list_remove_unsafe(cip_conn_list_t *list, cip_conn_p conn);
extern cip_conn_p session_list_find_by_host_unsafe(cip_conn_list_t *list, const char *host, const char *path,
                                                   int connection_group_id, cip_plc_type_t plc_type);

extern uint16_t next_conn_serial_number(uint16_t current);
extern uint64_t session_get_new_seq_id_unsafe(cip_conn_p conn);
extern uint64_t session_get_new_seq_id(cip_conn_p conn);
extern int session_match_valid(const char *host, const char *path, cip_conn_p conn);
extern int session_close_socket(cip_conn_p conn);
extern int session_get_available_cip_payload_space(cip_conn_p conn);

/*
 * The largest CIP message this connection can carry in one request once the
 * transport has added its framing: the CPF data item, and for unconnected
 * messaging the Unconnected Send wrapper and the route path as well.
 *
 * This is the bound for payload-only requests.  Builders that still frame their
 * own packets use session_get_available_cip_payload_space() and account for the
 * rest themselves.
 */
extern int cip_conn_max_cip_payload(cip_conn_p conn);

extern int send_eip_request(cip_conn_p conn, int timeout);
extern int recv_eip_response(cip_conn_p conn, int timeout);
extern int send_forward_open_request(cip_conn_p conn);
extern int send_forward_close_req(cip_conn_p conn);
extern int recv_forward_close_resp(cip_conn_p conn);
extern int perform_forward_close(cip_conn_p conn);
extern int session_open_socket(cip_conn_p conn);
extern int prepare_request(cip_conn_p conn);
extern int session_unregister(cip_conn_p conn);

/* frame the exchange in conn->slots, and find one reply in what came back. */
extern int pack_exchange(cip_conn_p conn);
extern int find_reply(cip_conn_p conn, int32_t index, uint8_t **reply, int32_t *reply_size);

/*
 * Make a tag due now and wake the handler; or take it off the list and out of any exchange in
 * flight.  The handler encodes, sends and decodes for a tag with an operation started, and runs
 * the library's auto-sync for one without.
 */
extern int cip_conn_queue_tag(cip_conn_p conn, struct cip_tag_t *tag);
extern void cip_conn_forget_tag(cip_conn_p conn, struct cip_tag_t *tag);

extern int session_register(cip_conn_p conn);
extern void session_destroy(void *conn_arg);
extern int receive_forward_open_response(cip_conn_p conn);

/*
 * How much of the single reply packet a request expecting reply_size bytes will consume: its
 * own reply plus the offset entry and the worst-case inter-packet padding.  A reply size of
 * zero is not known yet -- typically the tag has never been read -- so it draws nothing here,
 * and the encoder keeps such a request out of a bundle by marking it not packable.
 */
static inline int32_t reply_budget_cost(int32_t reply_size) {
    if(reply_size <= 0) { return 0; }

    return reply_size + CIP_MSP_MAX_PACKET_PADDING + CIP_MSP_OFFSET_ENTRY_SIZE;
}
extern int process_requests(cip_conn_p conn);
extern THREAD_FUNC(session_handler);
extern void session_set_connection_status(cip_conn_p conn, int32_t new_status);


/* the one connection list, for every CIP family.  Started and torn down with the library. */
extern int cip_conn_startup(void);
extern void cip_conn_teardown(void);

/* find a shared connection matching the tag's attributes, or create one. */
extern int cip_conn_find_or_create(cip_conn_p *tag_conn, attr attribs, int *is_new_conn);

/*
 * The connection's family, safe to call from any thread.  NULL while a probed connection is
 * still identifying the device; *identify_rc then says whether it refused the device.
 */
extern const cip_family_t *cip_conn_family(cip_conn_p conn, int *identify_rc);

/*
 * How many connections the list currently holds.  Nothing in the library needs this; it
 * exists so a test can assert that a failed tag create leaks no connection.
 */
extern int32_t cip_conn_live_count(void);
