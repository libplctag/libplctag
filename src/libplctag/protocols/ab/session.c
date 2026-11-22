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

#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/ab/ab_common.h>
#include <libplctag/protocols/ab/cip.h>
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/ab/error_codes.h>
#include <libplctag/protocols/ab/session.h>
#include <libplctag/protocols/ab/tag.h>
#include <limits.h>
#include <platform.h>
#include <stdlib.h>
#include <time.h>
#include <utils/atomic_utils.h>
#include <utils/attr.h>
#include <utils/backoff.h>
#include <utils/byteorder.h>
#include <utils/debug.h>
#include <utils/random_utils.h>
#include <utils/time_utils.h>

/* Track active session handler threads for proper shutdown synchronization */

#define MAX_REQUESTS (400)

/* the Message Router, which a connected path ends at. */
static uint8_t cip_router_path[] = {0x20, 0x02, 0x24, 0x01};

#define EIP_CIP_PREFIX_SIZE (44) /* bytes of encap header and CFP connected header */


/*
 * Number of milliseconds to wait to try to set up the session again
 * after a failure.
 */
#define RETRY_WAIT_INITIAL_MS (100)
#define RETRY_WAIT_MAX_MS (10000)

/* the bounds of the connection_retry_max_delay_ms attribute. */
#define CONN_RETRY_MAX_DELAY_MIN_MS (100)
#define CONN_RETRY_MAX_DELAY_LIMIT_MS (300000)

/* duplicate connection IDs refused in a row before giving up on the Forward Open. */
#define CIP_FO_MAX_DUP_RETRIES (20)

/* the largest connection the plain Forward Open can ask for. */
#define CIP_FO_PLAIN_MAX_SIZE (504)

/* Idle timeout.  One second less than that negotiated with the PLC. */
#define SESSION_DISCONNECT_TIMEOUT (AB_EIP_CONN_TIMEOUT_MS - 1000)
#define SOCKET_WAIT_TIMEOUT_MS (20)
#define SESSION_IDLE_WAIT_TIME (100)

/*
 * Smallest connection payload we can actually use, by protocol family.
 *
 * A ForwardOpen error can offer a smaller size than we asked for and we accept it, but there
 * is a floor: below the fixed per-request overhead there is no room left for a request, and
 * the "space minus overhead" arithmetic downstream goes negative.  PCCC needs about 92 bytes
 * for its command header and addressing; CIP needs 500 for a minimal fragmented transfer.
 * Both are payload only -- the EIP and CPF encapsulation is accounted for separately.
 */
#define MIN_PAYLOAD_SIZE_PCCC (92)
#define MIN_PAYLOAD_SIZE_CIP (500)

#define MIN_PAYLOAD_SIZE(session)                                                                                   \
    (((session)->plc_type == AB_PLC_PLC5 || (session)->plc_type == AB_PLC_SLC || (session)->plc_type == AB_PLC_MLGX \
      || (session)->plc_type == AB_PLC_LGX_PCCC) ?                                                                  \
         MIN_PAYLOAD_SIZE_PCCC :                                                                                    \
         MIN_PAYLOAD_SIZE_CIP)



/*
 * One request/reply exchange on the socket, run a little on each pass of the handler.  It never
 * waits: the handler waits on the socket between passes, for the events exchange_wait_events()
 * names, until the deadline.
 */
typedef enum { EX_IDLE = 0, EX_SEND, EX_READ } ex_state_t;

typedef enum { EX_PENDING, EX_DONE, EX_FAILED } ex_result_t;

typedef struct {
    ex_state_t state;
    int64_t deadline;
    bool want_reply;
    int32_t rc; /* why it failed */
    bool net_error; /* it failed on the wire: the socket is no good any more. */
} exchange_t;


/* the Forward Open sub-machine's state; reset when the phase ends, either way. */
typedef enum { FO_START = 0, FO_BUILD, FO_EXCHANGE } fo_state_t;

typedef struct {
    fo_state_t state;
    uint16_t size;       /* the connection size being asked for */
    bool offered_tried;  /* a size the PLC offered has been tried */
    uint32_t dup_tries;  /* duplicate connection IDs refused in a row */
} fo_neg_t;


/*
 * The requests in flight.  requests_single_step() sends one bare; requests_packed_step() sends
 * one or more inside a Multiple Service Packet (0x0A).  requests_step() picks one per batch.
 */
typedef enum { REQ_START = 0, REQ_EXCHANGE } req_state_t;

typedef struct {
    req_state_t state;
    bool packed;
    int count;
    ab_request_p reqs[MAX_REQUESTS];
} req_batch_t;


/* plc-specific session constructors */

static cip_conn_p session_create_unsafe(int max_payload_capacity, const char *host,
                                          const char *path, plc_type_t plc_type, int *use_connected_msg, int connection_group_id);
static int add_session_unsafe(cip_conn_p n);
static int remove_session_unsafe(cip_conn_p n);
static cip_conn_p find_session_by_host_unsafe(const char *gateway, const char *path, int connection_group_id, int use_connected_msg);
static int session_match_valid(const char *host, const char *path, cip_conn_p session);
// static int session_add_request_unsafe(cip_conn_p session, ab_request_p req);
static int session_open_socket(cip_conn_p session);
static void session_destroy(void *session);
static int session_register(cip_conn_p session, exchange_t *ex);
static int session_identify(cip_conn_p session, exchange_t *ex);
static int session_close_socket(cip_conn_p session);
static void session_say_goodbye(cip_conn_p session);
static int session_unregister(cip_conn_p session, exchange_t *ex);
static THREAD_FUNC(session_handler);
static int purge_aborted_requests_unsafe(cip_conn_p session);
// static int check_packing(cip_conn_p session, ab_request_p request);
static int get_payload_size(cip_conn_p session, ab_request_p request);
static size_t frame_header_size(cip_conn_p session, bool connected);
static size_t frame_room(cip_conn_p session, bool connected);
static int frame_cip_request(cip_conn_p session, bool connected, size_t cip_size);
static int check_response_framing(cip_conn_p session, size_t *reply_offset);
static int prepare_request(cip_conn_p session);
static uint64_t next_sender_context(cip_conn_p session);
static int send_begin(cip_conn_p session);
static int send_some(cip_conn_p session, int timeout_ms);
static void recv_begin(cip_conn_p session);
static int recv_some(cip_conn_p session, int timeout_ms);
static int recv_finish(cip_conn_p session);
static void exchange_start(cip_conn_p session, exchange_t *ex, bool want_reply);
static ex_result_t exchange_step(cip_conn_p session, exchange_t *ex);
static int exchange_wait_events(const exchange_t *ex);
static int unpack_response(cip_conn_p session, ab_request_p request, int sub_packet, bool packed);
// static int perform_forward_open(cip_conn_p session);
static int session_forward_close(cip_conn_p session, exchange_t *ex);
// static int try_forward_open_ex(cip_conn_p session, int *max_payload_size_guess);
// static int try_forward_open(cip_conn_p session);
// static int send_forward_open_req(cip_conn_p session);
// static int send_forward_open_req_ex(cip_conn_p session);
// static int recv_forward_open_resp(cip_conn_p session, int *max_payload_size_guess);
static uint16_t next_conn_serial_number(uint16_t current);
static void encode_forward_open(cip_conn_p session, bool large);
static void encode_plain_forward_open(cip_conn_p session);
static void encode_large_forward_open(cip_conn_p session);
static void request_destroy(void *req_arg);
static int session_request_increase_buffer(ab_request_p request, int new_capacity);
static inline void session_publish_event(cip_conn_p session, int32_t event_type, int32_t status, int32_t reason);
static void session_apply_plc_type(cip_conn_p session, const cip_plc_type_t *plc);
static void identify_step(cip_conn_p session, struct session_sm_s *sm);
static void connected_step(cip_conn_p session, struct session_sm_s *sm);
static void unconnected_step(cip_conn_p session, struct session_sm_s *sm);


static mutex_t session_mutex;
static cip_conn_p sessions = NULL;


int session_startup(void) {
    int rc = PLCTAG_STATUS_OK;

    /* initialized once and never destroyed: a destructor that runs after shutdown still finds it. */
    if(!session_mutex.initialized && (rc = mutex_init(&session_mutex)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_ERROR, 0, "Unable to create session mutex %s!", plc_tag_decode_error(rc));
        return rc;
    }

    sessions = NULL;

    return rc;
}


/*
 * session_get_new_seq_id
 *
 * A thread-safe function to get a new session sequence ID.
 */

/* the encap sender context; under session_seq_id_lock, which tag threads take for the PCCC TNS. */
static uint64_t next_sender_context(cip_conn_p session) {
    uint64_t res = 0;

    spin_block(&session->session_seq_id_lock) { res = ++session->session_seq_id; }

    return res;
}


uint16_t session_get_new_seq_id(cip_conn_p session) {
    uint16_t res = 0;

    spin_block(&session->session_seq_id_lock) {
        /* check for rollover. */
        if((++session->session_seq_id) == 0) { session->session_seq_id = 1; }
        res = (uint16_t)session->session_seq_id;
    }

    return res;
}


int session_get_available_cip_payload_space(cip_conn_p session) {
    int result = 0;

    if(!session) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Called with null session pointer!");
        return 0;
    }

    critical_block(&session->session_mutex) {
        int max_payload_size = session->max_payload_size;
        result = max_payload_size;

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Session payload calculation: max_payload_size=%d", max_payload_size);

        /* a type that packs sends every request in a Multiple Service Packet, even a lone one. */
        {
            const cip_plc_type_t *plc = ab_plc_type_get(session->plc_type);

            if(plc && plc->allow_packing) { result -= (int)(sizeof(cip_multi_req_header) + sizeof(uint16_le)); }
        }

        /* Account for CPF data item overhead */
        if(session->use_connected_msg) {
            result -= (int)sizeof(cpf_connected_data_item);
        } else {
            result -= (int)sizeof(cpf_unconnected_data_item);
            result -= (int)(session->route_path_size) + 2; /* encoded route size plus two bytes for length and padding */
        }
    }
    if(result < 0) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Available payload space is negative (%d bytes)! This should not happen!",
               result);
        result = 0;
    } else {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Available payload space is %d bytes.", result);
    }

    return result;
}

int session_find_or_create(cip_conn_p *tag_session, attr attribs, int *is_new_session) {
    /*int debug = attr_get_int(attribs,"debug",0);*/
    const char *session_gw = attr_get_str(attribs, "gateway", "");
    const char *session_path = attr_get_str(attribs, "path", "");
    const cip_plc_type_t *plc = ab_plc_type_find(attribs);
    /* data tags set it; an @connection tag gets the same default, so their connections match. */
    int use_connected_msg = attr_get_int(attribs, "use_connected_msg", !plc ? 0 : (plc->connected < 0) ? 1 : plc->connected);
    cip_conn_p session = CIP_CONN_NULL;
    int new_session = 0;
    int shared_session = attr_get_int(attribs, "share_session", 1); /* share the session by default. */
    int rc = PLCTAG_STATUS_OK;
    int connection_inactivity_timeout_ms = SESSION_DISCONNECT_TIMEOUT;
    int connection_group_id = attr_get_int(attribs, "connection_group_id", 0);
    int connection_retry_max_delay_ms = attr_get_int(attribs, "connection_retry_max_delay_ms", RETRY_WAIT_MAX_MS);

    if(connection_retry_max_delay_ms < CONN_RETRY_MAX_DELAY_MIN_MS || connection_retry_max_delay_ms > CONN_RETRY_MAX_DELAY_LIMIT_MS) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "connection_retry_max_delay_ms %d must be between %d and %d, clamping.",
               connection_retry_max_delay_ms, CONN_RETRY_MAX_DELAY_MIN_MS, CONN_RETRY_MAX_DELAY_LIMIT_MS);
        connection_retry_max_delay_ms = (connection_retry_max_delay_ms < CONN_RETRY_MAX_DELAY_MIN_MS) ? CONN_RETRY_MAX_DELAY_MIN_MS
                                                                                                    : CONN_RETRY_MAX_DELAY_LIMIT_MS;
    }
    int only_use_old_forward_open = attr_get_int(attribs, "conn_only_use_old_forward_open", 0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Starting");

    /* share_session is subsumed by connection_group_id.  Warn only if the tag string actually set it. */
    if(attr_get_str(attribs, "share_session", NULL)) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
               "The attribute \"share_session\" is deprecated and will be removed.  Use \"connection_group_id\" instead.");
    }

    connection_inactivity_timeout_ms = attr_get_int(attribs, "connection_inactivity_timeout_ms", SESSION_DISCONNECT_TIMEOUT);
    if(connection_inactivity_timeout_ms < 1 || connection_inactivity_timeout_ms > SESSION_DISCONNECT_TIMEOUT) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
               "Invalid connection_inactivity_timeout_ms %d. Must be between 1 and %d. Using default %d.",
               connection_inactivity_timeout_ms, SESSION_DISCONNECT_TIMEOUT, SESSION_DISCONNECT_TIMEOUT);
        connection_inactivity_timeout_ms = SESSION_DISCONNECT_TIMEOUT;
    } else {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Setting connection_inactivity_timeout_ms to %dms.",
               connection_inactivity_timeout_ms);
    }

    critical_block(&session_mutex) {
        /* if we are to share sessions, then look for an existing one. */
        if(shared_session) {
            /* a tag that names its PLC says how it messages; one that does not takes whatever identify finds. */
            session = find_session_by_host_unsafe(session_gw, session_path, connection_group_id, plc ? use_connected_msg : -1);
        } else {
            /* no sharing, create a new one */
            session = CIP_CONN_NULL;
        }

        if(session == CIP_CONN_NULL) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Creating new session.");

            if(plc) {
                session = session_create_unsafe(plc->max_msg_size, session_gw, session_path, plc->id, &use_connected_msg,
                                                connection_group_id);
            } else {
                /* no PLC named: identify picks it.  Unconnected until then. */
                use_connected_msg = 0;
                session = session_create_unsafe(MAX_CIP_LGX_MSG_SIZE_EX, session_gw, session_path, AB_PLC_NONE,
                                                &use_connected_msg, connection_group_id);
            }

            if(session != CIP_CONN_NULL && plc) { session_apply_plc_type(session, plc); }

            if(session == CIP_CONN_NULL) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "unable to create or find a session!");
                rc = PLCTAG_ERR_BAD_GATEWAY;
            } else {
                atomic_init_int32(&session->connection_inactivity_timeout_ms, connection_inactivity_timeout_ms);
                atomic_init_int32(&session->connection_retry_max_delay_ms, connection_retry_max_delay_ms);

                /* see if we have an attribute set for forcing the use of the older ForwardOpen */
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                       "Passed attribute to prohibit use of extended ForwardOpen is %d.", only_use_old_forward_open);
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                       "Existing attribute to prohibit use of extended ForwardOpen is %d.", session->only_use_old_forward_open);
                session->only_use_old_forward_open = (session->only_use_old_forward_open ? 1 : only_use_old_forward_open);

                new_session = 1;
            }
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Reusing existing session.");
        }
    }

    /*
     * Make sure that we have created the mutex and socket first.
     */

    if(new_session) {
        if((rc = thread_create((thread_p *)&(session->handler_thread), session_handler, 32 * 1024, session))
           != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create session thread!");
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "rc_dec: Releasing session reference.");
            rc_dec(session);
            session = CIP_CONN_NULL;
        } else {
            /* save the status */
            // session->status = rc;
        }
    }

    /* store it into the tag */
    *tag_session = session;

    if(is_new_session) { *is_new_session = new_session; }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Done");

    return rc;
}


bool session_connected_conflicts(cip_conn_p session, int use_connected_msg) {
    bool conflicts = false;

    critical_block(&session->session_mutex) {
        /* a DH+ route is always connected, whatever the tag's PLC defaults to. */
        conflicts = session->plc_type != AB_PLC_NONE && !session->is_dhp
                    && (use_connected_msg != 0) != (session->use_connected_msg != 0);
    }

    return conflicts;
}


int add_session_unsafe(cip_conn_p session) {
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Starting");

    if(!session) { return PLCTAG_ERR_NULL_PTR; }

    session->next = sessions;
    sessions = session;

    session->on_list = 1;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Done");

    return PLCTAG_STATUS_OK;
}


int remove_session_unsafe(cip_conn_p session) {
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Starting");

    if(!session) { return 0; }

    for(cip_conn_p *link = &sessions; *link; link = &(*link)->next) {
        if(*link == session) {
            *link = session->next;
            session->next = NULL;
            break;
        }
    }

    /* no longer on the list */
    session->on_list = 0;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Done");

    return PLCTAG_STATUS_OK;
}

int remove_session(cip_conn_p s) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Starting.");

    if(s->on_list) {
        critical_block(&session_mutex) { rc = remove_session_unsafe(s); }
    } else {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Session not on list, skipping removal.");
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


int session_match_valid(const char *host, const char *path, cip_conn_p session) {
    if(!session) { return 0; }

    if(!str_length(host)) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "New session host is NULL or zero length!");
        return 0;
    }

    if(!str_length(session->host)) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Session host is NULL or zero length!");
        return 0;
    }

    if(str_cmp_i(host, session->host)) { return 0; }

    if(str_cmp_i(path, session->path)) { return 0; }

    return 1;
}


/*
 * use_connected_msg is 0 or 1 to match only a connection that messages the same way, or -1 for any.
 * A connection is either connected or not, so a connected tag does not match an unconnected one.
 */
cip_conn_p find_session_by_host_unsafe(const char *host, const char *path, int connection_group_id, int use_connected_msg) {
    for(cip_conn_p entry = sessions; entry; entry = entry->next) {
        /* is this session in the process of destruction? */
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "rc_inc: Acquiring session reference.");
        cip_conn_p session = rc_inc(entry);
        if(session) {
            bool messaging_matches = true;

            if(use_connected_msg >= 0) {
                /* identify can change it; a DH+ route is always connected, whatever the tag's PLC defaults to. */
                critical_block(&session->session_mutex) {
                    messaging_matches = session->is_dhp || ((use_connected_msg != 0) == (session->use_connected_msg != 0));
                }
            }

            if(session->connection_group_id == connection_group_id && session_match_valid(host, path, session)
               && messaging_matches) {
                return session;
            }

            rc_dec(session);
        }
    }

    return NULL;
}


cip_conn_p session_create_unsafe(int max_payload_capacity, const char *host, const char *path,
                                   plc_type_t plc_type, int *use_connected_msg, int connection_group_id) {
    static volatile uint32_t connection_id = 0;

    int rc = PLCTAG_STATUS_OK;
    cip_conn_p session = CIP_CONN_NULL;
    size_t total_allocation_size = sizeof(*session);
    size_t data_buffer_capacity = (size_t)EIP_CIP_PREFIX_SIZE + (size_t)max_payload_capacity;
    size_t host_name_offset = 0;
    size_t host_name_size = 0;
    size_t path_offset = 0;
    size_t path_size = 0;
    size_t conn_path_offset = 0;
    uint8_t tmp_conn_path[MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN];
    int tmp_conn_path_size = MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN;
    uint8_t tmp_route_path[MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN];
    int tmp_route_path_size = MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN;
    int route_needs_connection = 0;
    size_t route_path_offset = 0;
    int is_dhp = 0;
    uint16_t dhp_dest = 0;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting");

    /*
     * The host string is copied into this allocation verbatim, so its length is part of the
     * session's size.  It comes straight from the "gateway" attribute with nothing between
     * the application and here, so bound it: without this a caller can size the session
     * object arbitrarily.
     */
    if(!host || str_length(host) >= MAX_SESSION_HOST_LEN) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Gateway string is missing or longer than the maximum of %d bytes!",
               MAX_SESSION_HOST_LEN - 1);
        return CIP_CONN_NULL;
    }

    /*
     * The path string is copied in verbatim too, and it is not self-limiting: spaces are
     * skipped everywhere in cip_encode_path(), so an arbitrarily long string can still
     * encode to a valid short path.  Bound it against the encoded path buffer -- a real
     * route is a handful of hops, so this rejects nothing that describes real hardware.
     */
    if(path && str_length(path) >= MAX_CONN_PATH) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Path string is longer than the maximum of %d bytes!", MAX_CONN_PATH - 1);
        return CIP_CONN_NULL;
    }

    if(*use_connected_msg) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Session should use connected messaging.");
    } else {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Session should not use connected messaging.");
    }

    if(data_buffer_capacity > SESSION_DATA_CAPACITY) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_ERROR, 0, "A %zu byte packet does not fit the %d byte session buffer!",
               data_buffer_capacity, SESSION_DATA_CAPACITY);
        return CIP_CONN_NULL;
    }

    /* add in space for the host name.  + 1 for the NUL terminator. */
    host_name_offset = total_allocation_size;
    host_name_size = (size_t)str_length(host) + 1;
    total_allocation_size += host_name_size;

    /* add in space for the path copy. */
    if(path && str_length(path) > 0) {
        path_offset = total_allocation_size;
        path_size = (size_t)str_length(path) + 1;
        total_allocation_size += path_size;
    } else {
        path_offset = 0;
    }

    /* encode the path */
    rc = cip_encode_path(path, use_connected_msg, plc_type, &tmp_conn_path[0], &tmp_conn_path_size, &is_dhp, &dhp_dest);

    /* the same path without the Message Router, for unconnected requests. */
    if(rc == PLCTAG_STATUS_OK) {
        rc = cip_encode_path(path, &route_needs_connection, plc_type, &tmp_route_path[0], &tmp_route_path_size, &is_dhp,
                             &dhp_dest);
    }
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Unable to convert path string to binary path, error %s!",
               plc_tag_decode_error(rc));
        return NULL;
    }

    conn_path_offset = total_allocation_size;
    /* room for the Message Router that identify may add. */
    total_allocation_size += (size_t)tmp_conn_path_size + sizeof(cip_router_path);

    route_path_offset = total_allocation_size;
    total_allocation_size += (size_t)tmp_route_path_size;

    /* allocate the session struct and the buffer in the same allocation. */
    pdebug(
        DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
        "Allocating %zu total bytes of memory with %d bytes for the data buffer, %d bytes for the host name, %d bytes for the path, %d bytes for the encoded path.",
        total_allocation_size, SESSION_DATA_CAPACITY, str_length(host) + 1,
        (path_offset == 0 ? 0 : str_length(path) + 1), tmp_conn_path_size);

    session = (cip_conn_p)rc_alloc((int)total_allocation_size, session_destroy);
    if(!session) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error allocating new session!");
        return CIP_CONN_NULL;
    }

    /* fill in the interior pointers */

    session->data_capacity = (uint32_t)sizeof(session->data);

    /* point the host pointer just after the data. */
    session->host = (char *)(session) + host_name_offset;
    str_copy(session->host, (int)host_name_size, host);

    if(path_offset) {
        session->path = (char *)(session) + path_offset;
        str_copy(session->path, (int)path_size, path);
    }

    if(conn_path_offset) {
        session->conn_path = (uint8_t *)(session) + conn_path_offset;

        /* cip_encode_path() keeps the path, Message Router included, under 256 bytes. */
        session->conn_path_size = (uint8_t)tmp_conn_path_size;
        mem_copy(session->conn_path, tmp_conn_path, tmp_conn_path_size);
    }

    session->route_path = (uint8_t *)(session) + route_path_offset;
    session->route_path_size = (uint8_t)tmp_route_path_size;
    mem_copy(session->route_path, tmp_route_path, tmp_route_path_size);


    /*
        TO DO
            remove mem_free from destructor for host, path, and conn_path.
    */

    session->requests = vector_create(SESSION_MIN_REQUESTS, SESSION_INC_REQUESTS);
    if(!session->requests) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to allocate vector for requests!");
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "rc_dec: Releasing session reference.");
        rc_dec(session);
        return NULL;
    }

    /* check for ID set up. This does not need to be thread safe since we just need a random value. */
    if(connection_id == 0) { connection_id = (uint32_t)(random_u64(UINT32_MAX) + 1); }

    /* fix up the rest of teh fields */
    session->plc_type = plc_type;
    session->use_connected_msg = *use_connected_msg;
    if(plc_type == AB_PLC_NONE) {
        session->step = identify_step;
    } else {
        session->step = *use_connected_msg ? connected_step : unconnected_step;
    }
    session->conn_serial_number = (uint16_t)(random_u64(UINT16_MAX) + 1);
    session->session_seq_id = (uint64_t)(random_u64(UINT32_MAX) + 1);
    session->is_dhp = is_dhp;
    session->dhp_dest = dhp_dest;
    atomic_init_int32(&session->connection_status, PLCTAG_CONN_STATUS_DOWN);
    atomic_init_int32(&session->connection_status_reason, PLCTAG_STATUS_OK);

    /* conn_status_ring_write_idx always points at the ring slot holding the
     * current state, not the next free slot: session_publish_event() dedups a new
     * event against ring[write_idx] before writing ring[write_idx+1], and a
     * connection tag seeds status_ring_read_idx to this same index to mean "I've
     * already seen this one." Both of those need ring[0] to hold a real DOWN
     * entry, not the zeroed garbage rc_alloc() leaves behind. */
    session->conn_status_ring[0].event_type = PLCTAG_CONN_STATUS_DOWN + PLCTAG_EVENT_CONN_STATUS_OFFSET;
    session->conn_status_ring[0].status = PLCTAG_STATUS_OK;
    atomic_init_int32(&session->conn_status_ring_write_idx, 0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Setting connection_group_id to %d.", connection_group_id);
    session->connection_group_id = connection_group_id;

    /*
     * Why is connection_id global?  Because it looks like the PLC might
     * be treating it globally.  I am seeing ForwardOpen errors that seem
     * to be because of duplicate connection IDs even though the session
     * was closed.
     *
     * So, this is more or less unique across all invocations of the library.
     * FIXME - this could collide.  The probability is low, but it could happen
     * as there are only 32 bits.
     */
    session->orig_connection_id = ++connection_id;

    /* create the session mutex. */
    if((rc = mutex_init(&(session->session_mutex))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create session mutex!");
        return rc_dec(session);
    }

    /*
     * The socket object lives as long as the session: reconnects close and reopen it, and its
     * wake channel is how other threads wake the handler.
     */
    if((rc = socket_create(&(session->sock))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create session socket!");
        return rc_dec(session);
    }

    /* add the new session to the list. */
    add_session_unsafe(session);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done");

    return session;
}


/*
 * session_open_socket()
 *
 * Connect to the host/port passed via TCP.
 */

int session_open_socket(cip_conn_p session) {
    int rc = PLCTAG_STATUS_OK;
    char **server_port = NULL;
    int port = 0;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    /*
     * A reconnect reuses this session, so drop the identity of the connection that just
     * went away.  Otherwise the checks in recv_finish() compare the new session's
     * RegisterSession reply against the old handle and reject it, and the session can
     * never come back up.
     */
    session->session_handle = 0;
    session->req_encap_command = 0;
    session->req_seq_id = 0;
    session->req_sent = false;

    server_port = str_split(session->host, ":");
    if(!server_port) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to split server and port string!");
        return PLCTAG_ERR_BAD_CONFIG;
    }

    if(server_port[0] == NULL) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Server string is malformed or empty!");
        mem_free(server_port);
        return PLCTAG_ERR_BAD_CONFIG;
    }

    if(server_port[1] != NULL) {
        rc = str_to_int(server_port[1], &port);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to extract port number from server string \"%s\"!",
                   session->host);
            mem_free(server_port);
            return PLCTAG_ERR_BAD_CONFIG;
        }

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Using special port %d.", port);
    } else {
        port = AB_EIP_DEFAULT_PORT;

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Using default port %d.", port);
    }

    /* record the effective port, default included, so it can be read back as an attribute. */
    session->port = port;

    rc = socket_connect_tcp_start(session->sock, server_port[0], port);

    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to connect socket for session!");
        mem_free(server_port);
        return rc;
    }

    if(server_port) { mem_free(server_port); }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return rc;
}


/*
 * Register the session.  PLCTAG_STATUS_PENDING until the reply is in; call again on each pass.
 */
int session_register(cip_conn_p session, exchange_t *ex) {
    eip_session_reg_req *req;
    eip_encap *resp;

    if(ex->state != EX_IDLE) {
        switch(exchange_step(session, ex)) {
            case EX_PENDING: return PLCTAG_STATUS_PENDING;
            case EX_FAILED:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Session registration exchange failed %s!",
                       plc_tag_decode_error(ex->rc));
                return ex->rc;
            case EX_DONE: break;
        }

        /* encap header is at the start of the buffer */
        resp = (eip_encap *)(session->data);

        /* check the response status */
        if(le2h16(resp->encap_command) != AB_EIP_REGISTER_SESSION) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "EIP unexpected response packet type: %d!", le2h16(resp->encap_command));
            return PLCTAG_ERR_BAD_DATA;
        }

        if(le2h32(resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "EIP command failed, response code: %d", le2h32(resp->encap_status));
            return PLCTAG_ERR_REMOTE_ERR;
        }

        /* save the session handle, we will use it in future packets. */
        session->session_handle = le2h32(resp->encap_session_handle);

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Registered with handle %" PRIx32 ".", session->session_handle);

        return PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    /*
     * clear the session data.
     *
     * We use the receiving buffer because we do not have a request and nothing can
     * be coming in (we hope) on the socket yet.
     */
    mem_set(session->data, 0, sizeof(eip_session_reg_req));

    req = (eip_session_reg_req *)(session->data);

    /* fill in the fields of the request */
    req->encap_command = h2le16(AB_EIP_REGISTER_SESSION);
    req->encap_length = h2le16(sizeof(eip_session_reg_req) - sizeof(eip_encap));
    req->encap_session_handle = h2le32(/*session->session_handle*/ 0);
    req->encap_status = h2le32(0);
    req->encap_sender_context = h2le64((uint64_t)0);
    req->encap_options = h2le32(0);

    req->eip_version = h2le16(AB_EIP_VERSION);
    req->option_flags = h2le16(0);

    session->data_size = sizeof(eip_session_reg_req);

    exchange_start(session, ex, true);

    return PLCTAG_STATUS_PENDING;
}


/*
 * Unregister the session: send only, as the PLC closes the TCP connection when it sees it.
 * PLCTAG_STATUS_PENDING until the packet is out; call again on each pass.
 */
int session_unregister(cip_conn_p session, exchange_t *ex) {
    eip_encap *req;

    if(ex->state != EX_IDLE) {
        switch(exchange_step(session, ex)) {
            case EX_PENDING: return PLCTAG_STATUS_PENDING;
            case EX_FAILED:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Unregister not sent %s.", plc_tag_decode_error(ex->rc));
                return ex->rc;
            case EX_DONE: break;
        }

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Unregister sent.");
        session->session_handle = 0;
        return PLCTAG_STATUS_OK;
    }

    /* nothing registered, nothing to undo. */
    if(!session->session_handle) { return PLCTAG_STATUS_OK; }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    req = (eip_encap *)(session->data);
    mem_set(req, 0, sizeof(*req));

    req->encap_command = h2le16(AB_EIP_UNREGISTER_SESSION);
    req->encap_length = h2le16(0);
    req->encap_session_handle = h2le32(session->session_handle);

    session->data_size = sizeof(eip_encap);

    exchange_start(session, ex, false);

    return PLCTAG_STATUS_PENDING;
}


/*
 * Forward Close and unregister, then close the socket.  The exchange deadline bounds each step.
 * The handler does this as it stops, so a shutdown closes every connection in parallel; the
 * destructor calls it again, which then does nothing.  Caller holds the session mutex.
 */
void session_say_goodbye(cip_conn_p session) {
    exchange_t ex = {.state = EX_IDLE};

    if(!session->sock) { return; }

    while(session_forward_close(session, &ex) == PLCTAG_STATUS_PENDING) {
        socket_wait_event(session->sock, SOCK_EVENT_TIMEOUT | exchange_wait_events(&ex), SOCKET_WAIT_TIMEOUT_MS);
    }

    /* a network error is fatal: nothing more can be said on this socket. */
    while(!ex.net_error && session_unregister(session, &ex) == PLCTAG_STATUS_PENDING) {
        socket_wait_event(session->sock, SOCK_EVENT_TIMEOUT | exchange_wait_events(&ex), SOCKET_WAIT_TIMEOUT_MS);
    }

    /* whatever the PLC heard, nothing more is owed on this socket. */
    session->targ_connection_id = 0;
    session->session_handle = 0;

    session_close_socket(session);
}


int session_close_socket(cip_conn_p session) {
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    if(session->sock) { socket_close(session->sock); }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


void session_destroy(void *session_arg) {
    cip_conn_p session = session_arg;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    if(!session) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Session ptr is null!");

        return;
    }

    /* so remove the session from the list so no one else can reference it. */
    remove_session(session);

    /* terminate the session thread first. */
    atomic_set_int32(&session->terminating, 1);

    /* wake the handler in case it is waiting */
    if(session->sock) { socket_wake(session->sock); }

    /* get rid of the handler thread. */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Destroying session thread.");
    if(session->handler_thread) {
        /* this cannot be guarded by the mutex since the session thread also locks it. */
        thread_join(session->handler_thread);

        /* FIXME - is this critical block needed? */
        critical_block(&session->session_mutex) {
            thread_destroy(&(session->handler_thread));
            session->handler_thread = NULL;
        }
    }

    /* after the join: the handler's Forward Close still counts packets. */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Session sent %" PRId64 " packets.", session->packet_count);


    /* this needs to be handled in the mutex to prevent double frees due to queued requests. */
    critical_block(&session->session_mutex) {
        /* the handler has normally said goodbye already; this covers one that never ran. */
        session_say_goodbye(session);

        /* release all the requests that are in the queue. */
        if(session->requests) {
            for(int i = 0; i < vector_length(session->requests); i++) {
                ab_request_p request = vector_get(session->requests, i);

                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "rc_dec: Releasing request reference.");
                if(request) { atomic_set_bool(&request->in_session, false); }
                rc_dec(request);
            }

            vector_destroy(session->requests);
            session->requests = NULL;
        }
    }

    /* the handler is gone, so nothing can wake it any more. */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Destroying session socket.");
    if(session->sock) {
        socket_close(session->sock);
        socket_destroy(&(session->sock));
        session->sock = NULL;
    }

    /* we are done with the mutex, finally destroy it. */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Destroying session mutex.");
    if(session->session_mutex.initialized) {
        mutex_destroy(&(session->session_mutex));
    }


    /* these are all allocated in one large block. */

    // pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, "Cleaning up allocated memory for paths and host name.");
    // if(session->conn_path) {
    //     mem_free(session->conn_path);
    //     session->conn_path = NULL;
    // }

    // if(session->path) {
    //     mem_free(session->path);
    //     session->path = NULL;
    // }

    // if(session->host) {
    //     mem_free(session->host);
    //     session->host = NULL;
    // }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return;
}


/*
 * session_add_request
 *
 * This is a thread-safe version of the above routine.
 */
int session_add_request(cip_conn_p session, ab_request_p req) {
    int rc = PLCTAG_STATUS_OK;

    if(!session || !req) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Session or request is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, req->tag_id, "Starting. session=%p, req=%p", (void *)session, (void *)req);

    /* the session's reference.  A request being destroyed cannot be queued. */
    if(!rc_inc(req)) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Request is in the process of being deleted.");
        return PLCTAG_ERR_NULL_PTR;
    }

    critical_block(&session->session_mutex) {
        /* without the fragmented services an unbudgeted reply could overflow the packed reply. */
        {
            const cip_plc_type_t *plc = ab_plc_type_get(session->plc_type);

            if(plc && !plc->fragmented && req->reply_size <= 0) { req->allow_packing = 0; }
        }

        /* insert into the requests vector */
        atomic_set_bool(&req->in_session, true);
        vector_set(session->requests, vector_length(session->requests), req);
    }

    /* wake up the session thread because we added something to process. */
    socket_wake(session->sock);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, req->tag_id, "Done.");

    return rc;
}


/*****************************************************************
 **************** Session handling functions *********************
 ****************************************************************/


typedef enum {
    SESSION_OPEN_SOCKET = 0,
    SESSION_CONNECT_WAIT,
    SESSION_REGISTER,
    SESSION_IDENTIFY,
    SESSION_IDENTIFIED,
    SESSION_FORWARD_OPEN,
    SESSION_IDLE,
    SESSION_FORWARD_CLOSE,
    SESSION_UNREGISTER,
    SESSION_CLOSE_SOCKET,
    SESSION_BACKOFF_WAIT,
    SESSION_IDLE_WAIT
} session_state_t;

typedef enum { PHASE_PENDING, PHASE_DONE, PHASE_FAILED, PHASE_NET_ERROR } phase_t;

/* what a session's state machine keeps between passes of the handler loop. */
typedef struct session_sm_s {
    session_state_t state;
    int64_t now;
    int64_t timeout_time;
    int64_t auto_disconnect_time;
    int32_t inactivity_timeout_ms;
    backoff_t retry_backoff;
    bool auto_disconnect;
    bool peer_closed;  /* the PLC closed the socket while nothing was in flight */
    exchange_t ex; /* the exchange in flight, if any */
    fo_neg_t fo;   /* the Forward Open negotiation */
    req_batch_t batch; /* the requests in flight */
} session_sm_t;

static int fo_step(cip_conn_p session, session_sm_t *sm);
static int requests_step(cip_conn_p session, session_sm_t *sm);
static void requeue_batch(cip_conn_p session, req_batch_t *b);


/* copy what the PLC type sets on its connection. */
static void session_apply_plc_type(cip_conn_p session, const cip_plc_type_t *plc) {
    session->plc_type = plc->id;
    /* the attribute can only add the restriction, never lift it from a type without the Large FO. */
    if(plc->fo_ex_conn_size == 0) { session->only_use_old_forward_open = true; }
    session->max_payload_size = (uint16_t)plc->fo_conn_size;
}


/* the ring's slots are read and written under session_mutex, so a lagging reader never sees half a slot. */
static inline void session_publish_event(cip_conn_p session, int32_t event_type, int32_t status, int32_t reason) {
    bool published = false;

    critical_block(&session->session_mutex) {
        int32_t write_idx = atomic_get_int32(&session->conn_status_ring_write_idx);

        if(session->conn_status_ring[write_idx].event_type == event_type && session->conn_status_ring[write_idx].status == status) {
            break;
        }

        write_idx = (write_idx + 1) & SESSION_CONN_STATUS_RING_SIZE_MASK;

        session->conn_status_ring[write_idx].event_type = event_type;
        session->conn_status_ring[write_idx].status = status;
        session->conn_status_ring[write_idx].reason = reason;

        atomic_set_int32(&session->conn_status_ring_write_idx, write_idx);
        published = true;
    }

    if(published) { plc_tag_tickler_wake(); }
}


/* Set connection status and reason atomics, and push a ring buffer entry if the status changed.
 * Must only be called from the session handler thread (single writer).
 *
 * connection_status and the ring publish must change together under session_mutex:
 * ab_connection_tag_create() takes a paired snapshot of both (conn_status_ring_write_idx
 * and connection_status) to seed a freshly created connection tag, and needs the same
 * mutex to avoid reading one from before this transition and the other from after it --
 * see the comment there. */
static inline void session_set_connection_status(cip_conn_p session, int32_t new_status, int32_t new_reason) {
    critical_block(&session->session_mutex) {
        int32_t old_status = atomic_get_int32(&session->connection_status);
        atomic_set_int32(&session->connection_status_reason, new_reason);
        atomic_set_int32(&session->connection_status, new_status);
        if(old_status != new_status) {
            session_publish_event(session, new_status + PLCTAG_EVENT_CONN_STATUS_OFFSET, PLCTAG_STATUS_OK, new_reason);
        }
    }
}


/* the PLC refused this session for good. */
static bool session_refused(cip_conn_p session) {
    bool refused = false;

    critical_block(&session->session_mutex) { refused = (session->refused_rc != PLCTAG_STATUS_OK); }

    return refused;
}


/* A refused session answers every request with the refusal, so no tag waits on it. */
static void fail_queued_requests(cip_conn_p session) {
    int32_t refused_rc = PLCTAG_STATUS_OK;
    int failed = 0;

    critical_block(&session->session_mutex) {
        refused_rc = session->refused_rc;

        while(vector_length(session->requests) > 0) {
            ab_request_p request = vector_remove(session->requests, 0);

            if(!request) { continue; }

            spin_block(&request->lock) {
                request->status = refused_rc;
                request->request_size = 0;
                request->resp_received = 1;
            }

            atomic_set_bool(&request->in_session, false);
            rc_dec(request);
            failed++;
        }
    }

    session_set_connection_status(session, PLCTAG_CONN_STATUS_DOWN, refused_rc);

    if(failed) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Failed %d request(s) with %s.", failed, plc_tag_decode_error(refused_rc));
        plc_tag_tickler_wake();
    }
}


/*
 * Connection sizes to fall back to, largest first: every rung is a size some real device is known
 * to accept.  4002 is the measured Logix ceiling, 504 and 500 the pre-extended sizes (500 is what
 * bridging through ControlNet leaves), 244 PCCC embedded in CIP and 92 PCCC on older SLC and
 * MicroLogix.  1900 is for the OMRON models in between.
 */
static const uint16_t forward_open_size_ladder[] = {4002, 1900, 504, 500, 244, 92};


/* the next ladder size at or below ceiling that still clears this protocol's minimum; 0 if none. */
static uint16_t fo_next_size_down(cip_conn_p session, uint16_t ceiling) {
    for(size_t i = 0; i < sizeof(forward_open_size_ladder) / sizeof(forward_open_size_ladder[0]); i++) {
        uint16_t size = forward_open_size_ladder[i];

        if(size > ceiling) { continue; }

        if(size < MIN_PAYLOAD_SIZE(session)) { break; }

        return size;
    }

    return 0;
}


/*
 * The Forward Open phase.  PLCTAG_STATUS_PENDING while it runs, PLCTAG_STATUS_OK with the
 * connection open, or why it gave up; the reason is also in connection_status_reason.
 *
 * A reply classified by classify_fo_reply() either asks again (new ID, offered size, smaller
 * size, plain service) or gives up.  A wrong route or no connected messaging is a refusal: no
 * attempt soon will change it, so it waits the longest and fails the waiting requests.
 */
static int fo_step(cip_conn_p session, session_sm_t *sm) {
    fo_neg_t *fo = &sm->fo;
    eip_forward_open_response_t *resp = NULL;
    uint8_t *status = NULL;
    size_t status_size = 0;
    uint16_t offered = 0;
    uint16_t ceiling = 0;
    bool step_down = false;
    bool refused = false;
    int rc = PLCTAG_STATUS_OK;

    switch(fo->state) {
        case FO_START: {
            const cip_plc_type_t *plc = ab_plc_type_get(session->plc_type);
            int32_t size = CIP_FO_PLAIN_MAX_SIZE;

            if(plc) {
                size = (session->only_use_old_forward_open || plc->fo_ex_conn_size == 0) ? plc->fo_conn_size :
                                                                                          plc->fo_ex_conn_size;
            }

            fo->size = (uint16_t)size;
            fo->offered_tried = false;
            fo->dup_tries = 0;
            fo->state = FO_BUILD;
        }
            /* fall through */

        case FO_BUILD:
            critical_block(&session->session_mutex) { session->max_payload_guess = fo->size; }

            /* the size picks the service: only the Large Forward Open carries more than 504 bytes. */
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Forward Open for %u bytes using the %s service.",
                   (unsigned int)fo->size, (fo->size > CIP_FO_PLAIN_MAX_SIZE ? "Large" : "plain"));

            encode_forward_open(session, fo->size > CIP_FO_PLAIN_MAX_SIZE);
            exchange_start(session, &sm->ex, true);
            fo->state = FO_EXCHANGE;

            return PLCTAG_STATUS_PENDING;

        case FO_EXCHANGE: break;
    }

    switch(exchange_step(session, &sm->ex)) {
        case EX_PENDING: return PLCTAG_STATUS_PENDING;
        case EX_FAILED:
            fo->state = FO_START;
            atomic_set_int32(&session->connection_status_reason, sm->ex.rc);
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Forward Open exchange failed %s!", plc_tag_decode_error(sm->ex.rc));
            return sm->ex.rc;
        case EX_DONE: break;
    }

    resp = (eip_forward_open_response_t *)(session->data);

    /* an error reply stops after the status, so only that much is required here. */
    if((size_t)session->data_size < offsetof(eip_forward_open_response_t, orig_to_targ_conn_id)) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Forward Open response of %u bytes is too short to hold the CIP status!",
               session->data_size);
        fo->state = FO_START;
        atomic_set_int32(&session->connection_status_reason, PLCTAG_ERR_TOO_SMALL);
        return PLCTAG_ERR_TOO_SMALL;
    }

    status = &resp->general_status;
    status_size = cip_error_data_size(status, session->data + session->data_size);

    if(resp->general_status != AB_EIP_OK) {
        uint16_t extended = (uint16_t)((status_size >= 4 && status[1] >= 1) ? (status[2] | (status[3] << 8)) : 0);

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Forward Open failed: %s (0x%02x/0x%04x)",
               decode_cip_error_short(status, status_size), resp->general_status, extended);
    }

    switch(classify_fo_reply(status, status_size, &offered)) {
        case FO_ACCEPTED:
            if((size_t)session->data_size < sizeof(*resp)) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                       "Successful Forward Open response of %u bytes is too short to hold the connection data!",
                       session->data_size);
                rc = PLCTAG_ERR_TOO_SMALL;
                break;
            }

            session->targ_connection_id = le2h32(resp->orig_to_targ_conn_id);
            session->orig_connection_id = le2h32(resp->targ_to_orig_conn_id);

            critical_block(&session->session_mutex) { session->max_payload_size = session->max_payload_guess; }

            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0,
                   "Forward Open succeeded with our connection ID %x and the PLC connection ID %x with packet size %u.",
                   session->orig_connection_id, session->targ_connection_id, session->max_payload_size);

            fo->state = FO_START;
            atomic_set_int32(&session->connection_status_reason, PLCTAG_STATUS_OK);
            return PLCTAG_STATUS_OK;

        case FO_DUP_ID:
            /* the serial number changes on every encode, so asking again is enough. */
            if(++fo->dup_tries <= CIP_FO_MAX_DUP_RETRIES) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Duplicate connection ID, retry %" PRIu32 " of %d with a new one.",
                       fo->dup_tries, CIP_FO_MAX_DUP_RETRIES);
                fo->state = FO_BUILD;
                break;
            }

            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "The PLC refused %d connection IDs in a row; it has no connection to give us.", CIP_FO_MAX_DUP_RETRIES);
            rc = PLCTAG_ERR_NO_RESOURCES;
            break;

        case FO_SIZE_REFUSED:
            /* an offer is tried once; Logix v32 has offered a size and then refused it. */
            if(offered >= MIN_PAYLOAD_SIZE(session) && offered < fo->size && !fo->offered_tried) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "The PLC offered %u bytes.", (unsigned int)offered);
                fo->offered_tried = true;
                fo->size = offered;
                fo->state = FO_BUILD;
                break;
            }

            /* an offer is still a ceiling: one below the protocol's minimum leaves nothing to try. */
            ceiling = (uint16_t)(((offered > 0 && offered < fo->size) ? offered : fo->size) - 1);
            step_down = true;
            break;

        case FO_NO_CONNECTIONS:
            /* some firmware answers an oversized request so, and a full PLC may still take a small one. */
            ceiling = (uint16_t)(fo->size - 1);
            step_down = true;
            break;

        case FO_UNSUPPORTED:
            if(fo->size > CIP_FO_PLAIN_MAX_SIZE) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "No Large Forward Open, trying the plain one.");
                ceiling = CIP_FO_PLAIN_MAX_SIZE;
                step_down = true;
                break;
            }

            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_ERROR, 0,
                   "The PLC refused the plain Forward Open, so it does not support connected messaging.  "
                   "Set the use_connected_msg attribute to 0 for this device.");
            rc = PLCTAG_ERR_UNSUPPORTED;
            refused = true;
            break;

        case FO_ROUTE_REFUSED:
            rc = decode_cip_error_code(status, status_size);
            refused = true;
            break;

        case FO_LINK_OFFLINE: /* the route is right and the PLC is down: the normal backoff. */
        case FO_OTHER: rc = decode_cip_error_code(status, status_size); break;
    }

    if(step_down) {
        uint16_t size = fo_next_size_down(session, ceiling);

        if(size > 0) {
            fo->size = size;
            fo->state = FO_BUILD;
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                   "No connection size left to try at or below %u bytes that clears the %d bytes this protocol needs!",
                   (unsigned int)ceiling, MIN_PAYLOAD_SIZE(session));
            rc = PLCTAG_ERR_TOO_SMALL;
        }
    }

    /* ask again at once. */
    if(fo->state == FO_BUILD) { return fo_step(session, sm); }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Forward Open failed, %s.", plc_tag_decode_error(rc));

    fo->state = FO_START;
    atomic_set_int32(&session->connection_status_reason, rc);

    if(refused) {
        critical_block(&session->session_mutex) { session->refused_rc = rc; }
        backoff_saturate(&sm->retry_backoff);
        fail_queued_requests(session);
    }

    return rc;
}


/*
 * Phases.  Each runs one non-blocking pass of its part of the connection and says how it went:
 * PHASE_PENDING to be called again, PHASE_DONE or PHASE_FAILED to move on, PHASE_NET_ERROR when
 * the socket is no good.  The machines below decide where each outcome leads.
 */

/* a failure on the wire is fatal to the socket; any other is the PLC's answer. */
static phase_t failure(const session_sm_t *sm) { return sm->ex.net_error ? PHASE_NET_ERROR : PHASE_FAILED; }

/* (re)start the inactivity clock. */
static void inactivity_restart(cip_conn_p session, session_sm_t *sm) {
    sm->inactivity_timeout_ms = atomic_get_int32(&session->connection_inactivity_timeout_ms);
    sm->auto_disconnect_time = sm->now + sm->inactivity_timeout_ms;
}


/* start the connect; CONNECT_WAIT finishes it, even one that completed at once. */
static phase_t open_socket_phase(cip_conn_p session, session_sm_t *sm) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_OPEN_SOCKET state.");

    /* every connect gets the full set of Forward Open retries. */
    sm->fo.state = FO_START;
    session_set_connection_status(session, PLCTAG_CONN_STATUS_CONNECTING, PLCTAG_STATUS_PENDING);

    rc = session_open_socket(session);

    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "session connect failed %s!", plc_tag_decode_error(rc));
        atomic_set_int32(&session->connection_status_reason, rc);
        return PHASE_NET_ERROR;
    }

    return PHASE_DONE;
}


static phase_t connect_wait_phase(cip_conn_p session, session_sm_t *sm) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_CONNECT_WAIT state.");
    session_set_connection_status(session, PLCTAG_CONN_STATUS_CONNECTING, PLCTAG_STATUS_PENDING);

    rc = socket_connect_tcp_check(session->sock, 20); /* MAGIC */

    if(rc == PLCTAG_ERR_TIMEOUT) { return PHASE_PENDING; }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Session connect failed %s!", plc_tag_decode_error(rc));
        atomic_set_int32(&session->connection_status_reason, rc);
        return PHASE_NET_ERROR;
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Socket connection succeeded.");
    inactivity_restart(session, sm);

    return PHASE_DONE;
}


static phase_t register_phase(cip_conn_p session, session_sm_t *sm) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_REGISTER state.");
    session_set_connection_status(session, PLCTAG_CONN_STATUS_CONNECTING, PLCTAG_STATUS_PENDING);

    if((rc = session_register(session, &sm->ex)) == PLCTAG_STATUS_PENDING) { return PHASE_PENDING; }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "session registration failed %s!", plc_tag_decode_error(rc));
        atomic_set_int32(&session->connection_status_reason, rc);
        return failure(sm);
    }

    return PHASE_DONE;
}


/* PHASE_FAILED for a refusal, which is asked again on each reconnect. */
static phase_t identify_phase(cip_conn_p session, session_sm_t *sm) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_IDENTIFY state.");
    session_set_connection_status(session, PLCTAG_CONN_STATUS_CONNECTING, PLCTAG_STATUS_PENDING);

    if((rc = session_identify(session, &sm->ex)) == PLCTAG_STATUS_PENDING) { return PHASE_PENDING; }

    if(rc != PLCTAG_STATUS_OK) {
        atomic_set_int32(&session->connection_status_reason, rc);
        return failure(sm);
    }

    return session_refused(session) ? PHASE_FAILED : PHASE_DONE;
}


static phase_t forward_open_phase(cip_conn_p session, session_sm_t *sm) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_FORWARD_OPEN state.");
    session_set_connection_status(session, PLCTAG_CONN_STATUS_CONNECTING, PLCTAG_STATUS_PENDING);

    if((rc = fo_step(session, sm)) == PLCTAG_STATUS_PENDING) { return PHASE_PENDING; }

    return (rc == PLCTAG_STATUS_OK) ? PHASE_DONE : failure(sm);
}


/*
 * Run requests until the connection has been idle too long (PHASE_DONE) or a batch fails
 * (PHASE_FAILED, its requests back on the queue).
 */
static phase_t idle_phase(cip_conn_p session, session_sm_t *sm) {
    int32_t inactivity_timeout_ms = atomic_get_int32(&session->connection_inactivity_timeout_ms);
    int num_reqs = 0;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_IDLE state.");
    session_set_connection_status(session, PLCTAG_CONN_STATUS_UP, PLCTAG_STATUS_OK);

    /* reaching idle is the only proof of a working connect: the backoff restarts and a refusal is forgotten. */
    backoff_reset(&sm->retry_backoff);
    critical_block(&session->session_mutex) {
        session->refused_rc = PLCTAG_STATUS_OK;
        num_reqs = vector_length(session->requests);
    }

    /* queued work or a changed timeout keeps the connection up. */
    if(num_reqs > 0 || sm->inactivity_timeout_ms != inactivity_timeout_ms) { inactivity_restart(session, sm); }

    /* a batch in flight: nothing else to decide until it is done. */
    if((rc = requests_step(session, sm)) == PLCTAG_STATUS_PENDING) { return PHASE_PENDING; }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error while processing requests %s!", plc_tag_decode_error(rc));
        atomic_set_int32(&session->connection_status_reason, rc);
        return failure(sm);
    }

    if(sm->peer_closed) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "The PLC closed the idle connection.");
        sm->peer_closed = false;
        atomic_set_int32(&session->connection_status_reason, PLCTAG_ERR_READ);
        return PHASE_NET_ERROR;
    }

    if(sm->auto_disconnect_time < sm->now) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Disconnecting due to inactivity.");
        atomic_set_int32(&session->connection_status_reason, PLCTAG_STATUS_OK);
        sm->auto_disconnect = true;
        return PHASE_DONE;
    }

    /* more queued: come straight back. */
    critical_block(&session->session_mutex) { num_reqs = vector_length(session->requests); }
    if(num_reqs > 0) { socket_wake(session->sock); }

    return PHASE_PENDING;
}


/* teardown goes on whatever the PLC says. */
static phase_t forward_close_phase(cip_conn_p session, session_sm_t *sm) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_FORWARD_CLOSE state.");
    session_set_connection_status(session, PLCTAG_CONN_STATUS_DISCONNECTING, atomic_get_int32(&session->connection_status_reason));

    if((rc = session_forward_close(session, &sm->ex)) == PLCTAG_STATUS_PENDING) { return PHASE_PENDING; }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Forward close failed %s!", plc_tag_decode_error(rc));
        if(sm->ex.net_error) { return PHASE_NET_ERROR; }
    }

    return PHASE_DONE;
}


/* teardown goes on whatever happens. */
static phase_t unregister_phase(cip_conn_p session, session_sm_t *sm) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_UNREGISTER state.");
    session_set_connection_status(session, PLCTAG_CONN_STATUS_DISCONNECTING, atomic_get_int32(&session->connection_status_reason));

    if((rc = session_unregister(session, &sm->ex)) == PLCTAG_STATUS_PENDING) { return PHASE_PENDING; }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unregistering session failed %s!", plc_tag_decode_error(rc));
    }

    return PHASE_DONE;
}


/* close the socket and, unless this was an inactivity disconnect, start the backoff. */
static void close_socket_phase(cip_conn_p session, session_sm_t *sm) {
    int64_t retry_wait_ms = 0;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_CLOSE_SOCKET state.");
    session_set_connection_status(session, PLCTAG_CONN_STATUS_DOWN, atomic_get_int32(&session->connection_status_reason));

    session_close_socket(session);

    /* a refusal waits the longest before asking again. */
    if(session_refused(session)) {
        backoff_saturate(&sm->retry_backoff);
        sm->auto_disconnect = false;
    }

    if(sm->auto_disconnect) { return; }

    /* the cap is a live attribute, so it is read on every retry. */
    backoff_set_max_ms(&sm->retry_backoff, atomic_get_int32(&session->connection_retry_max_delay_ms));
    retry_wait_ms = backoff_wait_ms(&sm->retry_backoff);
    sm->timeout_time = sm->now + retry_wait_ms;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Retry %" PRIu32 ", waiting %" PRId64 "ms before reconnecting.",
           sm->retry_backoff.attempts, retry_wait_ms);
}


static phase_t backoff_wait_phase(cip_conn_p session, session_sm_t *sm) {
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_BACKOFF_WAIT state.");

    /* tags waiting on a refused session, or created during the wait, get the refusal at once. */
    if(session_refused(session)) { fail_queued_requests(session); }

    session_set_connection_status(session, PLCTAG_CONN_STATUS_ERR_WAIT, atomic_get_int32(&session->connection_status_reason));

    return (sm->timeout_time < sm->now) ? PHASE_DONE : PHASE_PENDING;
}


/* after an inactivity disconnect: sleep until a request arrives. */
static phase_t idle_wait_phase(cip_conn_p session, session_sm_t *sm) {
    int num_reqs = 0;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_IDLE_WAIT state.");
    session_set_connection_status(session, PLCTAG_CONN_STATUS_IDLE_WAIT, atomic_get_int32(&session->connection_status_reason));

    critical_block(&session->session_mutex) { num_reqs = vector_length(session->requests); }

    if(num_reqs == 0) { return PHASE_PENDING; }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "There are requests waiting, reopening connection to PLC.");
    sm->auto_disconnect = false;

    return PHASE_DONE;
}


/*
 * Move on by the outcome; pending stays put.  A network error is always fatal: nothing more can be
 * said on the socket, so it is closed and the retry backoff starts, from any state.
 */
static void next_state(session_sm_t *sm, phase_t outcome, session_state_t on_done, session_state_t on_failed) {
    switch(outcome) {
        case PHASE_PENDING: break;
        case PHASE_DONE: sm->state = on_done; break;
        case PHASE_FAILED: sm->state = on_failed; break;
        case PHASE_NET_ERROR:
            sm->auto_disconnect = false;
            sm->state = SESSION_CLOSE_SOCKET;
            break;
    }
}


/*
 * The machines.  A network error goes straight to CLOSE_SOCKET from any state (next_state).  Any
 * other exit says goodbye in order: Forward Close if a connection is open, unregister, close.
 */

/* the states every machine has, from the socket up to register and back down from unregister. */
static void common_step(cip_conn_p session, session_sm_t *sm) {
    switch(sm->state) {
        case SESSION_OPEN_SOCKET: next_state(sm, open_socket_phase(session, sm), SESSION_CONNECT_WAIT, SESSION_CLOSE_SOCKET); break;
        case SESSION_CONNECT_WAIT: next_state(sm, connect_wait_phase(session, sm), SESSION_REGISTER, SESSION_CLOSE_SOCKET); break;
        case SESSION_UNREGISTER: next_state(sm, unregister_phase(session, sm), SESSION_CLOSE_SOCKET, SESSION_CLOSE_SOCKET); break;
        case SESSION_CLOSE_SOCKET:
            close_socket_phase(session, sm);
            sm->state = sm->auto_disconnect ? SESSION_IDLE_WAIT : SESSION_BACKOFF_WAIT;
            break;
        case SESSION_BACKOFF_WAIT: next_state(sm, backoff_wait_phase(session, sm), SESSION_OPEN_SOCKET, SESSION_OPEN_SOCKET); break;
        case SESSION_IDLE_WAIT: next_state(sm, idle_wait_phase(session, sm), SESSION_OPEN_SOCKET, SESSION_OPEN_SOCKET); break;
        default:
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_ERROR, 0, "State %d does not belong to this machine!", sm->state);
            sm->state = SESSION_CLOSE_SOCKET;
            break;
    }
}


/*
 * Connected: register, Forward Open, requests over the connection, Forward Close.  Logix (default),
 * Micro800, Omron, and PLC/5 or SLC over DH+ (the connection is to the bridge).
 */
static void connected_step(cip_conn_p session, session_sm_t *sm) {
    switch(sm->state) {
        case SESSION_REGISTER: next_state(sm, register_phase(session, sm), SESSION_FORWARD_OPEN, SESSION_CLOSE_SOCKET); break;
        case SESSION_FORWARD_OPEN: next_state(sm, forward_open_phase(session, sm), SESSION_IDLE, SESSION_UNREGISTER); break;
        case SESSION_IDLE: next_state(sm, idle_phase(session, sm), SESSION_FORWARD_CLOSE, SESSION_FORWARD_CLOSE); break;
        case SESSION_FORWARD_CLOSE: next_state(sm, forward_close_phase(session, sm), SESSION_UNREGISTER, SESSION_UNREGISTER); break;
        default: common_step(session, sm); break;
    }
}


/*
 * Unconnected: register, requests, unregister.  PLC/5, SLC and MicroLogix reached directly, Logix
 * PCCC, generic CIP, and any type with use_connected_msg=0.
 */
static void unconnected_step(cip_conn_p session, session_sm_t *sm) {
    switch(sm->state) {
        case SESSION_REGISTER: next_state(sm, register_phase(session, sm), SESSION_IDLE, SESSION_CLOSE_SOCKET); break;
        case SESSION_IDLE: next_state(sm, idle_phase(session, sm), SESSION_UNREGISTER, SESSION_UNREGISTER); break;
        default: common_step(session, sm); break;
    }
}


/*
 * No plc=: register, then read the Identity object.  Once the type is known the connection hands
 * over to its flow, which takes it from the same registered socket: connected at Forward Open,
 * unconnected at idle.  A refusal tears down and is asked again on each reconnect.
 */
static void identify_step(cip_conn_p session, session_sm_t *sm) {
    switch(sm->state) {
        case SESSION_REGISTER: next_state(sm, register_phase(session, sm), SESSION_IDENTIFY, SESSION_CLOSE_SOCKET); break;
        case SESSION_IDENTIFY: next_state(sm, identify_phase(session, sm), SESSION_IDENTIFIED, SESSION_UNREGISTER); break;
        case SESSION_IDENTIFIED:
            /* hand over to the type's flow, on the same registered socket. */
            session->step = session->use_connected_msg ? connected_step : unconnected_step;
            sm->state = session->use_connected_msg ? SESSION_FORWARD_OPEN : SESSION_IDLE;
            break;
        default: common_step(session, sm); break;
    }
}


THREAD_FUNC(session_handler) {
    cip_conn_p session = arg;
    session_sm_t sm = {.state = SESSION_OPEN_SOCKET};
    int64_t wait_until_time = 0;

    sm.inactivity_timeout_ms = atomic_get_int32(&session->connection_inactivity_timeout_ms);
    backoff_init(&sm.retry_backoff, RETRY_WAIT_INITIAL_MS, RETRY_WAIT_MAX_MS);
    sm.auto_disconnect_time = time_ms() + sm.inactivity_timeout_ms;


    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting thread for session %p", (void *)session);

    /* Increment the count of active session handlers */

    while(!atomic_get_int32(&session->terminating) && atomic_get_bool(&lib_active)) {
        sm.now = time_ms();

        /* how long should we wait if nothing wakes us? */
        wait_until_time = sm.now + SESSION_IDLE_WAIT_TIME;

        /*
         * Do this on every cycle.   This keeps the queue clean(ish).
         *
         * Make sure we get rid of all the aborted requests queued.
         * This keeps the overall memory usage lower.
         */

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_SPEW, 0, "Critical block.");
        critical_block(&session->session_mutex) { purge_aborted_requests_unsafe(session); }

        {
            session_state_t prev_state = sm.state;
            cip_conn_step_func prev_step = session->step;

            session->step(session, &sm);

            /* a new state runs at once. */
            if(sm.state != prev_state || session->step != prev_step) { continue; }
        }

        {
            int64_t time_left = 0;

            /* an exchange in flight waits on the socket, up to its deadline. */
            if(sm.ex.state != EX_IDLE && sm.ex.deadline < wait_until_time) { wait_until_time = sm.ex.deadline; }

            time_left = wait_until_time - sm.now;

            if(time_left > 0) {
                /* idle with nothing in flight: only watching the socket shows the PLC closing it. */
                bool idle = (sm.state == SESSION_IDLE && sm.ex.state == EX_IDLE);
                int events = 0;

                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Waiting up to %" PRId64 "ms for something to happen.",
                       time_left);
                events = socket_wait_event(session->sock,
                                           SOCK_EVENT_WAKE_UP | SOCK_EVENT_TIMEOUT | exchange_wait_events(&sm.ex)
                                               | (idle ? SOCK_EVENT_DISCONNECT : 0),
                                           (int)time_left);

                if(idle && events > 0 && (events & SOCK_EVENT_DISCONNECT)) { sm.peer_closed = true; }
            }
        }
    }

    /* a batch in flight goes back on the queue, where the destructor releases it. */
    requeue_batch(session, &sm.batch);

    /* each handler says its own goodbye, so a shutdown closes every connection at once. */
    critical_block(&session->session_mutex) { session_say_goodbye(session); }

    /*
     * One last time before we exit.
     */
    critical_block(&session->session_mutex) { purge_aborted_requests_unsafe(session); }

    /* Decrement the count of active session handlers */

    THREAD_RETURN(0);
}


/*
 * This must be called with the session mutex held!
 */
int purge_aborted_requests_unsafe(cip_conn_p session) {
    int purge_count = 0;
    ab_request_p request = NULL;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_SPEW, 0, "Starting.");

    /* remove the aborted requests. */
    for(int i = 0; i < vector_length(session->requests); i++) {
        request = vector_get(session->requests, i);

        /* filter out the aborts. */
        if(request && atomic_get_int32(&request->abort_request)) {
            purge_count++;

            /* remove it from the queue. */
            vector_remove(session->requests, i);

            /* set the debug tag to the owning tag. */

            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Session thread releasing aborted request %p.", (void *)request);

            /* the same lock as every other hand-off, though the aborting tag has already let go. */
            spin_block(&request->lock) {
                request->status = PLCTAG_ERR_ABORT;
                request->request_size = 0;
                request->resp_received = 1;
            }

            /* release our hold on it. */
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "rc_dec: Releasing request reference.");
            atomic_set_bool(&request->in_session, false);
            rc_dec(request);

            /* vector size has changed, back up one. */
            i--;
        }
    }

    if(purge_count > 0) { pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Removed %d aborted requests.", purge_count); }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_SPEW, 0, "Done.");

    return purge_count;
}


/*
 * The Multiple Service Packet reply as a whole: its status, then a count and offsets that are
 * inside the packet.  Each request's own status is that request's business.
 */
static int check_packed_reply(cip_conn_p session, req_batch_t *b) {
    int rc = PLCTAG_STATUS_OK;
    cip_multi_resp_header *multi_resp = NULL;
    size_t reply_offset = 0;

    if((rc = check_response_framing(session, &reply_offset)) != PLCTAG_STATUS_OK) { return rc; }

    multi_resp = (cip_multi_resp_header *)(session->data + reply_offset);

    /* punt if we got an overall error or it is not a partial/bundled error. */
    if(multi_resp->status != AB_EIP_OK && multi_resp->status != AB_CIP_ERR_PARTIAL_ERROR) {
        size_t status_size = cip_error_data_size(&multi_resp->status, session->data + session->data_size);

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Received CIP error %s.",
               decode_cip_error_short(&multi_resp->status, status_size));
        return decode_cip_error_code(&multi_resp->status, status_size);
    }

    /*
     * The count word and the offset array that follows it are both past the
     * fixed part of the response we checked above, and the array is sized by a
     * count the PLC controls.  A short response with a large count would have us
     * reading offsets out of the buffer to decide whether the offsets are in the
     * buffer, so bound the whole header before touching any of it.
     */
    {
        size_t offsets_start =
            (size_t)((uint8_t *)multi_resp - session->data) + offsetof(cip_multi_resp_header, request_offsets);
        size_t offsets_size = (size_t)b->count * sizeof(uint16_le);

        if(offsets_start > (size_t)session->data_size
           || offsets_size > (size_t)session->data_size - offsets_start) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                   "Response of %d bytes is too short to hold %d packed response offsets!", session->data_size,
                   b->count);
            return PLCTAG_ERR_TOO_SMALL;
        }
    }

    /* we have multiple requests, sanity check the data. */
    if(le2h16(multi_resp->request_count) == b->count) {
        size_t offset_base = (size_t)((uint8_t *)(&multi_resp->request_count) - session->data);

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "offset_base=%zu", offset_base);

        /* check all the offsets */
        for(int resp_index = 0; resp_index < b->count; resp_index++) {
            size_t resp_offset = (size_t)le2h16(multi_resp->request_offsets[resp_index]) + offset_base;

            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Response %d starts at byte offset %zu", resp_index,
                   resp_offset);

            if(resp_offset >= (size_t)session->data_size) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                       "Response %d has offset %zu which is outside the session data!", resp_index, resp_offset);
                return PLCTAG_ERR_OUT_OF_BOUNDS;
            }
        }
    } else {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Expected %d packed responses back but got %zu!",
               b->count, (size_t)le2h16(multi_resp->request_count));
        return PLCTAG_ERR_BAD_DATA;
    }

    return PLCTAG_STATUS_OK;
}


/* put a batch back at the front of the queue, in order, after a failure. */
static void requeue_batch(cip_conn_p session, req_batch_t *b) {
    if(b->count > 0) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Pushing %d requests back into the queue.", b->count);
    }

    /* session->requests is also written by session_add_request() under the same mutex. */
    critical_block(&session->session_mutex) {
        for(int i = b->count - 1; i >= 0; i--) {
            if(b->reqs[i]) { vector_insert(session->requests, 0, b->reqs[i]); }
            b->reqs[i] = NULL;
        }
    }

    b->count = 0;
    b->state = REQ_START;
}


/*
 * Take the next batch off the queue: one request, or for a packed batch as many as fit with the
 * same framing.  The available space already allows for one Multiple Service Packet header on a
 * type that packs.  Returns the number taken.
 */
static int take_batch(cip_conn_p session, req_batch_t *b) {
    critical_block(&session->session_mutex) {
        ab_request_p first = NULL;
        int remaining_space = 0;
        int remaining_reply_space = 0;
        int first_size = 0;

        purge_aborted_requests_unsafe(session);

        if(!vector_length(session->requests)) { break; }

        remaining_space = session_get_available_cip_payload_space(session);
        first = vector_get(session->requests, 0);
        first_size = get_payload_size(session, first);

        if(first_size > remaining_space) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, first->tag_id,
                   "First request size %d exceeds remaining space %d, cannot process any requests.", first_size,
                   remaining_space);
            break;
        }

        b->reqs[b->count++] = first;
        vector_remove(session->requests, 0);

        if(!b->packed) { break; }

        /* the replies share one packet too, so they are budgeted the same way. */
        remaining_reply_space = remaining_space - first->reply_size;
        remaining_space -= first_size;

        /* the pad before the route path depends on the whole packet's length. */
        if(!first->connected && session->route_path_size > 0) { remaining_space--; }

        while(vector_length(session->requests) > 0 && b->count < MAX_REQUESTS) {
            ab_request_p next = vector_get(session->requests, 0);
            int next_size = next->request_size + (int)sizeof(uint16_le);

            /* a request that must go bare, or with other framing, starts the next batch. */
            if(!next->allow_packing || next->connected != first->connected) { break; }

            if(next->request_size <= 0 || next_size > remaining_space) { break; }

            /* a reply that has to come back whole has to fit too. */
            if(next->reply_size > 0 && next->reply_size + (int)sizeof(uint16_le) > remaining_reply_space) { break; }

            b->reqs[b->count++] = next;
            remaining_space -= next_size;
            remaining_reply_space -= next->reply_size + (int)sizeof(uint16_le);
            vector_remove(session->requests, 0);
        }
    }

    return b->count;
}


/* build the batch into the session buffer, framed and ready, and start the exchange. */
static int start_batch(cip_conn_p session, session_sm_t *sm) {
    req_batch_t *b = &sm->batch;
    bool connected = (b->reqs[0]->connected != 0);
    uint8_t *cip = session->data + frame_header_size(session, connected);
    size_t room = frame_room(session, connected);
    size_t cip_size = 0;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "%d request(s) to send%s.", b->count, (b->packed ? ", packed" : ""));

    if(!b->packed) {
        ab_request_p req = b->reqs[0];

        if(req->request_size < 0 || (size_t)req->request_size > room) { return PLCTAG_ERR_TOO_LARGE; }

        mem_copy(cip, req->data, req->request_size);
        cip_size = (size_t)req->request_size;
    } else {
        cip_multi_req_header *msp = (cip_multi_req_header *)cip;
        size_t offset = sizeof(uint16_le) * (size_t)(b->count + 1); /* from the count word */

        cip_size = sizeof(cip_multi_req_header) + sizeof(uint16_le) * (size_t)b->count;

        if(cip_size > room) { return PLCTAG_ERR_TOO_LARGE; }

        msp->service_code = AB_EIP_CMD_CIP_MULTI;
        msp->req_path_size = 0x02; /* words */
        msp->req_path[0] = 0x20;   /* class */
        msp->req_path[1] = 0x02;   /* Message Router */
        msp->req_path[2] = 0x24;   /* instance */
        msp->req_path[3] = 0x01;   /* #1 */
        msp->request_count = h2le16((uint16_t)b->count);

        for(int i = 0; i < b->count; i++) {
            ab_request_p req = b->reqs[i];

            if(req->request_size < 0 || (size_t)req->request_size > room - cip_size) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, req->tag_id, "Packed requests do not fit the session buffer!");
                return PLCTAG_ERR_TOO_LARGE;
            }

            msp->request_offsets[i] = h2le16((uint16_t)offset);
            mem_copy(cip + cip_size, req->data, req->request_size);
            cip_size += (size_t)req->request_size;
            offset += (size_t)req->request_size;
        }
    }

    if((rc = frame_cip_request(session, connected, cip_size)) != PLCTAG_STATUS_OK) { return rc; }

    if((rc = prepare_request(session)) != PLCTAG_STATUS_OK) { return rc; }

    exchange_start(session, &sm->ex, true);
    b->state = REQ_EXCHANGE;

    return PLCTAG_STATUS_PENDING;
}


/* hand each request its reply.  One that cannot take it goes back on the queue with the rest. */
static int deliver_batch(cip_conn_p session, req_batch_t *b) {
    int rc = PLCTAG_STATUS_OK;

    for(int i = 0; i < b->count; i++) {
        if((rc = unpack_response(session, b->reqs[i], i, b->packed)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, b->reqs[i]->tag_id, "Unable to unpack response!");
            break;
        }

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, b->reqs[i]->tag_id, "rc_dec: Releasing request reference.");
        atomic_set_bool(&b->reqs[i]->in_session, false);
        b->reqs[i] = rc_dec(b->reqs[i]);
    }

    /* tickle the main tickler thread to note that we have responses. */
    plc_tag_tickler_wake();

    if(rc != PLCTAG_STATUS_OK) {
        requeue_batch(session, b);
        return rc;
    }

    b->count = 0;
    b->state = REQ_START;

    return PLCTAG_STATUS_OK;
}


/*
 * One request, bare.  REQ_START takes it and sends it; REQ_EXCHANGE waits for the reply and
 * hands it over.  PLCTAG_STATUS_PENDING while one is in flight.
 */
static int requests_single_step(cip_conn_p session, session_sm_t *sm) {
    req_batch_t *b = &sm->batch;
    int rc = PLCTAG_STATUS_OK;

    if(b->state == REQ_START) {
        if(!take_batch(session, b)) { return PLCTAG_STATUS_OK; }

        if((rc = start_batch(session, sm)) != PLCTAG_STATUS_PENDING) { requeue_batch(session, b); }

        return rc;
    }

    switch(exchange_step(session, &sm->ex)) {
        case EX_PENDING: return PLCTAG_STATUS_PENDING;
        case EX_FAILED: requeue_batch(session, b); return sm->ex.rc;
        case EX_DONE: break;
    }

    return deliver_batch(session, b);
}


/*
 * Requests inside a Multiple Service Packet, one or more.  REQ_START fills and sends it;
 * REQ_EXCHANGE waits for the reply, checks the packet as a whole, and hands each request its
 * reply.  A bad packet sends the batch back to the queue and drops the connection.
 */
static int requests_packed_step(cip_conn_p session, session_sm_t *sm) {
    req_batch_t *b = &sm->batch;
    int rc = PLCTAG_STATUS_OK;

    if(b->state == REQ_START) {
        if(!take_batch(session, b)) { return PLCTAG_STATUS_OK; }

        if((rc = start_batch(session, sm)) != PLCTAG_STATUS_PENDING) { requeue_batch(session, b); }

        return rc;
    }

    switch(exchange_step(session, &sm->ex)) {
        case EX_PENDING: return PLCTAG_STATUS_PENDING;
        case EX_FAILED: requeue_batch(session, b); return sm->ex.rc;
        case EX_DONE: break;
    }

    if((rc = check_packed_reply(session, b)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Bad packed reply, %s!", plc_tag_decode_error(rc));
        requeue_batch(session, b);
        return rc;
    }

    return deliver_batch(session, b);
}


/*
 * The requests phase, run from IDLE.  A type with the Multiple Service Packet service packs every
 * request that allows it, even a lone one; anything else goes bare.
 */
static int requests_step(cip_conn_p session, session_sm_t *sm) {
    if(sm->batch.state == REQ_START) {
        const cip_plc_type_t *plc = ab_plc_type_get(session->plc_type);
        bool front_packs = false;

        critical_block(&session->session_mutex) {
            ab_request_p front = vector_length(session->requests) ? vector_get(session->requests, 0) : NULL;

            front_packs = (front && front->allow_packing);
        }

        sm->batch.packed = (plc && plc->allow_packing && front_packs);
    }

    return sm->batch.packed ? requests_packed_step(session, sm) : requests_single_step(session, sm);
}


int unpack_response(cip_conn_p session, ab_request_p request, int sub_packet, bool packed) {
    int rc = PLCTAG_STATUS_OK;
    uint8_t *buf_end = session->data + session->data_size;
    size_t reply_offset = 0;
    uint8_t *reply = NULL;
    uint8_t *pkt_start = NULL;
    uint8_t *pkt_end = NULL;
    int pkt_len = 0;
    int new_len = 0;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, request->tag_id, "Starting.");

    /* a bad frame fails this request, not the session: the tag gets the error. */
    rc = check_response_framing(session, &reply_offset);
    if(rc != PLCTAG_STATUS_OK) {
        spin_block(&request->lock) {
            request->status = rc;
            request->request_size = 0;
            request->resp_received = 1;
        }

        return PLCTAG_STATUS_OK;
    }

    if(reply_offset + sizeof(cip_header) > (size_t)session->data_size) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, request->tag_id, "Response of %u bytes is too short for a CIP reply!",
               session->data_size);
        return PLCTAG_ERR_TOO_SMALL;
    }

    reply = session->data + reply_offset;

    if(!packed) {
        pkt_start = reply;
        pkt_end = buf_end;
    } else {
        cip_multi_resp_header *multi = (cip_multi_resp_header *)reply;
        uint16_t total_responses = le2h16(multi->request_count);

        /*
         * The response count, the offsets and encap_length all come from the wire.  Check
         * that the offset array itself is inside the data we received BEFORE reading any
         * offset out of it -- otherwise the read that decides whether the array is in bounds
         * is itself out of bounds.
         */
        size_t offsets_start = reply_offset + offsetof(cip_multi_resp_header, request_offsets);
        size_t offsets_size = (size_t)total_responses * sizeof(uint16_le);

        if(sub_packet < 0 || sub_packet >= (int)total_responses || offsets_start > (size_t)session->data_size
           || offsets_size > (size_t)session->data_size - offsets_start) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, request->tag_id,
                   "Packed response offset table for sub-packet %d of %u is out of bounds!", sub_packet,
                   (unsigned int)total_responses);
            return PLCTAG_ERR_OUT_OF_BOUNDS;
        }

        pkt_start = ((uint8_t *)(&multi->request_count) + le2h16(multi->request_offsets[sub_packet]));

        /* calculate the end of the data. */
        if((sub_packet + 1) < total_responses) {
            pkt_end = (uint8_t *)(&multi->request_count) + le2h16(multi->request_offsets[sub_packet + 1]);
        } else {
            pkt_end = buf_end;
        }

        /*
         * Bound pkt_start/pkt_end against the bytes we actually received before trusting
         * them as a memcpy source range.  Comparing pointers directly is UB, so compare the
         * integer values instead.
         */
        if((intptr_t)pkt_start < (intptr_t)(&multi->request_count) || (intptr_t)pkt_start > (intptr_t)buf_end
           || (intptr_t)pkt_end < (intptr_t)pkt_start || (intptr_t)pkt_end > (intptr_t)buf_end) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, request->tag_id, "Packed sub-packet %d is out of bounds!", sub_packet);
            return PLCTAG_ERR_OUT_OF_BOUNDS;
        }
    }

    pkt_len = (int)(pkt_end - pkt_start);

    /* only the CIP reply goes back to the tag. */
    new_len = pkt_len;

    if(new_len > request->request_capacity) {
        int request_capacity = 0;

        critical_block(&session->session_mutex) {
            request_capacity = (int)(session->max_payload_size + EIP_CIP_PREFIX_SIZE);
        }

        /* make sure it will fit. */
        if(new_len > request_capacity) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, request->tag_id,
                   "Response of %d bytes is larger than the maximum request buffer of %d bytes!", new_len, request_capacity);
            return PLCTAG_ERR_TOO_LARGE;
        }

        rc = session_request_increase_buffer(request, request_capacity);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, request->tag_id, "Unable to grow the request buffer, %s!",
                   plc_tag_decode_error(rc));
            return rc;
        }
    }

    /* clear out the request data. */
    mem_set(request->data, 0, request->request_capacity);

    mem_copy(request->data, pkt_start, pkt_len);

    /* notify the reading thread that the request is ready */
    spin_block(&request->lock) {
        request->status = PLCTAG_STATUS_OK;
        request->request_size = new_len;
        request->resp_received = 1;
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, request->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * Check the EIP and CPF framing of the response in the session buffer: EIP status, CPF items,
 * our connection ID, and a data item length that matches what arrived.  *reply_offset is where
 * the CIP reply starts.
 */
int check_response_framing(cip_conn_p session, size_t *reply_offset) {
    eip_encap *encap = (eip_encap *)(session->data);
    const char *kind = NULL;
    size_t data_item_start = 0;
    size_t data_item_length = 0;

    if((size_t)session->data_size < sizeof(eip_encap)) { return PLCTAG_ERR_TOO_SMALL; }

    if(le2h32(encap->encap_status) != AB_EIP_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "EIP command failed, response code: %d", le2h32(encap->encap_status));
        return PLCTAG_ERR_REMOTE_ERR;
    }

    if(le2h16(encap->encap_command) == AB_EIP_CONNECTED_SEND) {
        eip_cip_co_resp *resp = (eip_cip_co_resp *)(session->data);

        kind = "Connected";

        if((size_t)session->data_size < sizeof(eip_cip_co_resp)) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Connected response of %u bytes is too short for a CIP response!",
                   session->data_size);
            return PLCTAG_ERR_TOO_SMALL;
        }

        if(le2h16(resp->cpf_item_count) != 2) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Connected response has %u CPF items, expected 2!",
                   le2h16(resp->cpf_item_count));
            return PLCTAG_ERR_BAD_DATA;
        }

        if(le2h16(resp->cpf_cai_item_type) != AB_EIP_ITEM_CAI || le2h16(resp->cpf_cdi_item_type) != AB_EIP_ITEM_CDI) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                   "Connected response CPF item types are %04" PRIx16 "/%04" PRIx16 ", expected %04" PRIx16 "/%04" PRIx16 "!",
                   le2h16(resp->cpf_cai_item_type), le2h16(resp->cpf_cdi_item_type), AB_EIP_ITEM_CAI, AB_EIP_ITEM_CDI);
            return PLCTAG_ERR_BAD_DATA;
        }

        /*
         * Only meaningful once ForwardOpen has actually negotiated a connection.  Until then
         * orig_connection_id is just the local placeholder, we send connection ID zero on the
         * wire, and the target echoes zero back -- there is no connection identity to check.
         */
        if(session->targ_connection_id != 0 && le2h32(resp->cpf_orig_conn_id) != session->orig_connection_id) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                   "Connected response is for connection %" PRIx32 " but ours is %" PRIx32 "!", le2h32(resp->cpf_orig_conn_id),
                   session->orig_connection_id);
            return PLCTAG_ERR_BAD_DATA;
        }

        data_item_start = (size_t)((uint8_t *)(&resp->cpf_conn_seq_num) - session->data);
        data_item_length = (size_t)le2h16(resp->cpf_cdi_item_length);
        *reply_offset = offsetof(eip_cip_co_resp, reply_service);
    } else if(le2h16(encap->encap_command) == AB_EIP_UNCONNECTED_SEND) {
        eip_cip_uc_resp *resp = (eip_cip_uc_resp *)(session->data);

        kind = "Unconnected";

        if((size_t)session->data_size < sizeof(eip_cip_uc_resp)) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unconnected response of %u bytes is too short for a CIP response!",
                   session->data_size);
            return PLCTAG_ERR_TOO_SMALL;
        }

        if(le2h16(resp->cpf_item_count) != 2) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unconnected response has %u CPF items, expected 2!",
                   le2h16(resp->cpf_item_count));
            return PLCTAG_ERR_BAD_DATA;
        }

        if(le2h16(resp->cpf_nai_item_type) != AB_EIP_ITEM_NAI || le2h16(resp->cpf_udi_item_type) != AB_EIP_ITEM_UDI) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                   "Unconnected response CPF item types are %04" PRIx16 "/%04" PRIx16 ", expected %04" PRIx16 "/%04" PRIx16 "!",
                   le2h16(resp->cpf_nai_item_type), le2h16(resp->cpf_udi_item_type), AB_EIP_ITEM_NAI, AB_EIP_ITEM_UDI);
            return PLCTAG_ERR_BAD_DATA;
        }

        data_item_start = (size_t)((uint8_t *)(&resp->reply_service) - session->data);
        data_item_length = (size_t)le2h16(resp->cpf_udi_item_length);
        *reply_offset = offsetof(eip_cip_uc_resp, reply_service);
    } else {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unexpected EIP packet type %04x!", le2h16(encap->encap_command));
        return PLCTAG_ERR_BAD_DATA;
    }

    /*
     * Require the data item to match what we actually received rather than merely fit,
     * otherwise the PLC can shorten the item and leave the handlers reading bytes it never sent.
     */
    if(data_item_start + data_item_length != (size_t)session->data_size) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
               "%s data item claims %zu bytes but the response is %u bytes with the item starting at %zu!", kind,
               data_item_length, session->data_size, data_item_start);
        return PLCTAG_ERR_BAD_DATA;
    }

    return PLCTAG_STATUS_OK;
}


/*
 * Write the EIP and CPF framing around a CIP message already in the session buffer at
 * frame_header_size().  prepare_request() fills in the session handle, sequence numbers and
 * connection ID.
 *
 *   connected                  EIP, CPF connected address and data items, CIP
 *   unconnected, with a path   EIP, CPF null address and data items, Unconnected Send wrapping
 *                              the CIP, then the route path
 *   unconnected, no path       EIP, CPF null address and data items, CIP to the device itself
 */
/* where the CIP message starts in the session buffer, after the framing in front of it. */
static size_t frame_header_size(cip_conn_p session, bool connected) {
    if(connected) { return sizeof(eip_cip_co_req); }

    return (session->route_path_size > 0) ? sizeof(eip_cip_uc_req) : sizeof(eip_cpf_uc_header);
}


/* how many CIP bytes fit in the session buffer, leaving room for the framing on both sides. */
static size_t frame_room(cip_conn_p session, bool connected) {
    size_t used = frame_header_size(session, connected);

    /* the route path behind an Unconnected Send, its size word, and a pad byte. */
    if(!connected && session->route_path_size > 0) { used += (size_t)session->route_path_size + 3; }

    return (used < (size_t)session->data_capacity) ? (size_t)session->data_capacity - used : 0;
}


int frame_cip_request(cip_conn_p session, bool connected, size_t cip_size) {
    size_t header_size = frame_header_size(session, connected);
    size_t total_size = 0;
    uint8_t *cip = session->data + header_size;
    bool wrapped = (!connected && session->route_path_size > 0);
    /* an odd-length embedded request is padded to a word before the route path; the length does not count it. */
    size_t pad_size = ((wrapped && (cip_size & 1)) ? 1 : 0);
    size_t path_size = (wrapped ? (size_t)session->route_path_size + 2 + pad_size : 0);

    total_size = header_size + cip_size + path_size;

    if(cip_size > frame_room(session, connected)) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Request of %zu bytes does not fit the session buffer of %u bytes!",
               cip_size, session->data_capacity);
        return PLCTAG_ERR_TOO_LARGE;
    }

    mem_set(session->data, 0, (int)header_size);

    if(connected) {
        eip_cip_co_req *co = (eip_cip_co_req *)(session->data);

        co->encap_command = h2le16(AB_EIP_CONNECTED_SEND);
        co->router_timeout = h2le16(1);
        co->cpf_item_count = h2le16(2);
        co->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);
        co->cpf_cai_item_length = h2le16(4);
        co->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);
        co->cpf_cdi_item_length = h2le16((uint16_t)(cip_size + sizeof(uint16_le)));
    } else {
        eip_cpf_uc_header *uc = (eip_cpf_uc_header *)(session->data);

        uc->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND);
        uc->router_timeout = h2le16(1);
        uc->cpf_item_count = h2le16(2);
        uc->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI);
        uc->cpf_nai_item_length = h2le16(0);
        uc->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI);
        uc->cpf_udi_item_length = h2le16((uint16_t)(total_size - sizeof(eip_cpf_uc_header)));

        if(wrapped) {
            eip_cip_uc_req *wrap = (eip_cip_uc_req *)(session->data);
            uint8_t *path = cip + cip_size + pad_size;

            wrap->cm_service_code = AB_EIP_CMD_UNCONNECTED_SEND;
            wrap->cm_req_path_size = 2;
            wrap->cm_req_path[0] = 0x20; /* class */
            wrap->cm_req_path[1] = 0x06; /* Connection Manager */
            wrap->cm_req_path[2] = 0x24; /* instance */
            wrap->cm_req_path[3] = 0x01; /* instance 1 */
            wrap->secs_per_tick = AB_EIP_SECS_PER_TICK;
            wrap->timeout_ticks = AB_EIP_TIMEOUT_TICKS;
            wrap->uc_cmd_length = h2le16((uint16_t)cip_size);

            if(pad_size) { cip[cip_size] = 0; }

            /* route path: size in words, a reserved byte, then the path. */
            path[0] = (uint8_t)(session->route_path_size / 2);
            path[1] = 0;
            mem_copy(path + 2, session->route_path, session->route_path_size);
        }
    }

    session->data_size = (uint32_t)total_size;

    return PLCTAG_STATUS_OK;
}


int get_payload_size(cip_conn_p session, ab_request_p request) {
    bool wrapped = false;

    if(!request || !request->data || request->request_size <= 0) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, request ? request->tag_id : 0, "Null request pointer or empty request data!");
        return INT_MAX;
    }

    /* the available space already allows for the route path; the Unconnected Send wrapper is the rest. */
    wrapped = (!request->connected && session->route_path_size > 0);

    return request->request_size + (wrapped ? CIP_UC_SEND_WRAPPER_SIZE + (request->request_size & 1) : 0);
}


int prepare_request(cip_conn_p session) {
    eip_encap *encap = NULL;
    int payload_size = 0;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    if(!session) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Called with null session!");
        return PLCTAG_ERR_NULL_PTR;
    }

    encap = (eip_encap *)(session->data);
    payload_size = (int)session->data_size - (int)sizeof(eip_encap);

    /* fill in the fields of the request. */

    encap->encap_length = h2le16((uint16_t)payload_size);
    encap->encap_session_handle = h2le32(session->session_handle);
    encap->encap_status = h2le32(0);
    encap->encap_options = h2le32(0);

    /* FIXME - support other kinds of requests? */

    /* set up the session sequence ID for this transaction */
    if(le2h16(encap->encap_command) == AB_EIP_UNCONNECTED_SEND) {
        /* get new ID */
        uint64_t seq_id = next_sender_context(session);

        // request->session_seq_id = session->session_seq_id;
        encap->encap_sender_context = h2le64(seq_id); /* link up the request seq ID and the packet seq ID */

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Preparing unconnected packet with session sequence ID %llx",
               seq_id);
    } else if(le2h16(encap->encap_command) == AB_EIP_CONNECTED_SEND) {
        eip_cip_co_req *conn_req = (eip_cip_co_req *)(session->data);

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "cpf_targ_conn_id=%x", session->targ_connection_id);

        /* set up the connection information */
        conn_req->cpf_targ_conn_id = h2le32(session->targ_connection_id);

        session->conn_seq_num++;
        conn_req->cpf_conn_seq_num = h2le16(session->conn_seq_num);

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Preparing connected packet with connection ID %x and sequence ID %u(%x)",
               session->orig_connection_id, session->conn_seq_num, session->conn_seq_num);
    } else {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unsupported packet type %x!", le2h16(encap->encap_command));
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* display the data */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Prepared packet of size %d", session->data_size);
    pdebug_dump_bytes(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, session->data, (int)session->data_size);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * Get ready to send the packet in session->data: remember what we are asking for, so that
 * recv_finish() can tell an answer to this request from an unrelated packet.  The only identity
 * the encapsulation layer gives us is the command and the sender context we echo back.
 */
int send_begin(cip_conn_p session) {
    session_publish_event(session, TAG_CONN_EVENT_SEND_REQUEST_STARTED, PLCTAG_STATUS_OK, PLCTAG_STATUS_OK);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Sending packet of size %d", session->data_size);
    pdebug_dump_bytes(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, session->data, (int)(session->data_size));

    session->data_offset = 0;
    session->packet_count++;

    if(session->data_size < sizeof(eip_encap)) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Packet of %u bytes is too small to hold an EIP header!",
               session->data_size);
        session_publish_event(session, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, PLCTAG_ERR_TOO_SMALL, PLCTAG_ERR_TOO_SMALL);
        return PLCTAG_ERR_TOO_SMALL;
    }

    {
        eip_encap *out_header = (eip_encap *)(session->data);

        session->req_encap_command = le2h16(out_header->encap_command);
        session->req_seq_id = le2h64(out_header->encap_sender_context);
        session->req_sent = true;
    }

    return PLCTAG_STATUS_OK;
}


/* write what is left of the packet.  PLCTAG_STATUS_PENDING while some is left. */
int send_some(cip_conn_p session, int timeout_ms) {
    int rc = socket_write(session->sock, session->data + session->data_offset,
                          (int)session->data_size - (int)session->data_offset, timeout_ms);

    if(rc == PLCTAG_ERR_TIMEOUT) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Socket not yet ready to write.");
        rc = 0;
    }

    if(rc < 0) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error, %d, writing socket!", rc);
        session_publish_event(session, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, rc, rc);
        return rc;
    }

    session->data_offset += (uint32_t)rc;

    if(session->data_offset < session->data_size) { return PLCTAG_STATUS_PENDING; }

    session_publish_event(session, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, PLCTAG_STATUS_OK, PLCTAG_STATUS_OK);

    return PLCTAG_STATUS_OK;
}


/*
 * Get ready to read a reply.
 *
 * The buffer is cleared before reading into it.  Response handlers cast this buffer to header
 * structs, and a short response leaves whatever the previous response put there.  Every length
 * check guarding those casts is then the only thing between a truncated packet and a
 * PLC-groomed value being read as a status or connection ID.  Zeroing makes that failure mode
 * boring instead of exploitable.
 */
void recv_begin(cip_conn_p session) {
    session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_STARTED, PLCTAG_STATUS_OK, PLCTAG_STATUS_OK);

    session->data_offset = 0;
    session->data_size = 0;

    mem_set(session->data, 0, (int)session->data_capacity);
}


/*
 * Read what we can of the reply: the EIP header first, then the length it gives.
 * PLCTAG_STATUS_PENDING until the whole packet is in.
 */
int recv_some(cip_conn_p session, int timeout_ms) {
    uint32_t data_needed = sizeof(eip_encap);
    int rc = PLCTAG_STATUS_OK;

    if(session->data_offset >= sizeof(eip_encap)) {
        data_needed = (uint32_t)(sizeof(eip_encap) + le2h16(((eip_encap *)(session->data))->encap_length));
    }

    rc = socket_read(session->sock, session->data + session->data_offset, (int)(data_needed - session->data_offset),
                     timeout_ms);

    if(rc == PLCTAG_ERR_TIMEOUT) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Socket not yet ready to read.");
        rc = 0;
    }

    if(rc < 0) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error reading socket! rc=%d", rc);
        session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, rc, rc);
        return rc;
    }

    session->data_offset += (uint32_t)rc;

    /* the header just completed: now we know how much more there is. */
    if(session->data_offset == sizeof(eip_encap) && data_needed == sizeof(eip_encap)) {
        data_needed = (uint32_t)(sizeof(eip_encap) + le2h16(((eip_encap *)(session->data))->encap_length));

        if(data_needed > session->data_capacity) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Packet response (%d) is larger than possible buffer size (%d)!",
                   data_needed, session->data_capacity);
            session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, PLCTAG_ERR_TOO_LARGE,
                                  PLCTAG_ERR_TOO_LARGE);
            return PLCTAG_ERR_TOO_LARGE;
        }
    }

    if(session->data_offset < data_needed) { return PLCTAG_STATUS_PENDING; }

    session->data_size = data_needed;

    return PLCTAG_STATUS_OK;
}


/*
 * The whole reply is in.  Everything downstream of here decides how to parse this buffer from
 * fields the PLC chose.  Before any of that, make sure this packet is actually the answer to the
 * request we sent: the encapsulation layer gives us three things to check and all three are free.
 *
 * The command matters most.  The connected and unconnected CPF headers are different lengths, so
 * a reply that changes the command out from under us moves every field the handlers read,
 * including the CIP status they branch on.
 */
int recv_finish(cip_conn_p session) {
    int rc = PLCTAG_STATUS_OK;

    session->resp_seq_id = le2h64(((eip_encap *)(session->data))->encap_sender_context);

    {
        eip_encap *resp_header = (eip_encap *)(session->data);
        uint16_t resp_command = le2h16(resp_header->encap_command);
        uint32_t resp_handle = le2h32(resp_header->encap_session_handle);

        if(session->req_sent && resp_command != session->req_encap_command) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                   "Received EIP command %04" PRIx16 " in response to command %04" PRIx16 "!", resp_command,
                   session->req_encap_command);
            session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, PLCTAG_ERR_BAD_DATA,
                                  PLCTAG_ERR_BAD_DATA);
            return PLCTAG_ERR_BAD_DATA;
        }

        /*
         * Once the session is registered every packet carries our handle.  A zero handle means
         * we are still registering, so there is nothing to compare against yet.
         */
        if(session->session_handle != 0 && resp_handle != session->session_handle) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                   "Received a response for session handle %" PRIx32 " but this session is %" PRIx32 "!", resp_handle,
                   session->session_handle);
            session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, PLCTAG_ERR_BAD_DATA,
                                  PLCTAG_ERR_BAD_DATA);
            return PLCTAG_ERR_BAD_DATA;
        }

        /*
         * The target echoes the sender context on SendRRData.  That is what tells a reply to
         * the request we are waiting on from a late reply to one that already timed out --
         * without it a stale response gets applied to whichever tag is in flight now.
         *
         * Connected sends do not get this check: we do not fill the field in for them, so there
         * is nothing meaningful to echo.  Their identity is the connection ID and the connection
         * sequence number in the CPF header, which the tag layer checks instead.
         */
        if(session->req_sent && resp_command == AB_EIP_UNCONNECTED_SEND && session->resp_seq_id != session->req_seq_id) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                   "Received a response with sender context %" PRIx64 " but we sent %" PRIx64 "!", session->resp_seq_id,
                   session->req_seq_id);
            session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, PLCTAG_ERR_BAD_DATA,
                                  PLCTAG_ERR_BAD_DATA);
            return PLCTAG_ERR_BAD_DATA;
        }
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "request received all needed data (%d bytes).", session->data_offset);

    pdebug_dump_bytes(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, session->data, (int)(session->data_offset));

    /* check status. */
    if(le2h32(((eip_encap *)(session->data))->encap_status) != AB_EIP_OK) { rc = PLCTAG_ERR_BAD_STATUS; }

    session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, rc, rc);

    return rc;
}


/* start an exchange of the packet in session->data. */
void exchange_start(cip_conn_p session, exchange_t *ex, bool want_reply) {
    ex->want_reply = want_reply;
    ex->deadline = time_ms() + SESSION_DEFAULT_TIMEOUT;
    ex->rc = send_begin(session);
    ex->net_error = false;
    ex->state = EX_SEND;
}


/* one non-blocking pass of the exchange. */
ex_result_t exchange_step(cip_conn_p session, exchange_t *ex) {
    int rc = ex->rc;

    if(rc == PLCTAG_STATUS_OK && ex->state == EX_SEND) {
        rc = send_some(session, 0);

        if(rc == PLCTAG_STATUS_OK) {
            if(!ex->want_reply) {
                ex->state = EX_IDLE;
                return EX_DONE;
            }

            recv_begin(session);
            ex->state = EX_READ;
        }
    }

    /* a reply may well be in already; try for it at once. */
    if(rc == PLCTAG_STATUS_OK && ex->state == EX_READ) {
        rc = recv_some(session, 0);

        if(rc == PLCTAG_STATUS_OK) {
            ex->state = EX_IDLE;
            ex->rc = recv_finish(session);
            /* a reply to something else means the stream is out of step: as bad as a broken socket. */
            ex->net_error = (ex->rc != PLCTAG_STATUS_OK && ex->rc != PLCTAG_ERR_BAD_STATUS);
            return (ex->rc == PLCTAG_STATUS_OK) ? EX_DONE : EX_FAILED;
        }
    }

    if(rc == PLCTAG_STATUS_PENDING && time_ms() >= ex->deadline) {
        rc = PLCTAG_ERR_TIMEOUT;
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Timed out waiting on the socket!");
        session_publish_event(session,
                              (ex->state == EX_SEND) ? TAG_CONN_EVENT_SEND_REQUEST_COMPLETED :
                                                       TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED,
                              rc, rc);
    }

    if(rc == PLCTAG_STATUS_PENDING) { return EX_PENDING; }

    /* a packet we could not build never reached the wire. */
    ex->net_error = (ex->rc == PLCTAG_STATUS_OK);
    ex->state = EX_IDLE;
    ex->rc = rc;

    return EX_FAILED;
}


/* what the handler waits on while an exchange is in flight. */
int exchange_wait_events(const exchange_t *ex) {
    switch(ex->state) {
        case EX_SEND: return SOCK_EVENT_CAN_WRITE;
        case EX_READ: return SOCK_EVENT_CAN_READ;
        default: return SOCK_EVENT_NONE;
    }
}


/*
 * Advance the connection serial number, skipping zero.
 *
 * The field is 16 bits and is meant to cycle, but zero is not a usable serial
 * number -- conn setup seeds it into 1..65535 for exactly that reason -- so the
 * wrap has to land on 1 rather than 0. Doing the arithmetic in uint16_t here
 * also keeps -fsanitize=implicit-integer-truncation quiet: a bare ++ on a
 * uint16_t promotes to int and narrows again on store.
 */
static uint16_t next_conn_serial_number(uint16_t current) {
    uint16_t next = (uint16_t)(current + 1);

    if(next == 0) { next = 1; }

    return next;
}


/* build a Forward Open, Large or plain, for max_payload_guess bytes in session->data. */
void encode_forward_open(cip_conn_p session, bool large) {
    if(large) {
        encode_large_forward_open(session);
    } else {
        encode_plain_forward_open(session);
    }
}


void encode_plain_forward_open(cip_conn_p session) {
    eip_forward_open_request_t *fo = NULL;
    uint8_t *data;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting");

    mem_set(session->data, 0, (int)(sizeof(*fo) + session->conn_path_size));

    fo = (eip_forward_open_request_t *)(session->data);

    /* point to the end of the struct */
    data = (session->data) + sizeof(eip_forward_open_request_t);

    /* set up the path information. */
    mem_copy(data, session->conn_path, session->conn_path_size);
    data += session->conn_path_size;

    /* fill in the static parts */

    /* encap header parts */
    fo->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND); /* 0x006F EIP Send RR Data command */
    fo->encap_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->interface_handle))); /* total length of packet except for encap header */
    fo->encap_session_handle = h2le32(session->session_handle);
    fo->encap_sender_context = h2le64(next_sender_context(session));
    fo->router_timeout = h2le16(1); /* one second is enough ? */

    /* CPF parts */
    fo->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    fo->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI); /* null address item type */
    fo->cpf_nai_item_length = h2le16(0);             /* no data, zero length */
    fo->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fo->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->cm_service_code))); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fo->cm_service_code = AB_EIP_CMD_FORWARD_OPEN; /* 0x54 Forward Open Request or 0x5B for Forward Open Extended */
    fo->cm_req_path_size = 2;                      /* size of path in 16-bit words */
    fo->cm_req_path[0] = 0x20;                     /* class */
    fo->cm_req_path[1] = 0x06;                     /* CM class */
    fo->cm_req_path[2] = 0x24;                     /* instance */
    fo->cm_req_path[3] = 0x01;                     /* instance 1 */

    /* Forward Open Params */
    fo->secs_per_tick = AB_EIP_SECS_PER_TICK; /* seconds per tick, no used? */
    fo->timeout_ticks = AB_EIP_TIMEOUT_TICKS; /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
    fo->orig_to_targ_conn_id = h2le32(0);     /* is this right?  Our connection id on the other machines? */
    fo->targ_to_orig_conn_id = h2le32(session->orig_connection_id); /* Our connection id in the other direction. */
    /* this might need to be globally unique */
    session->conn_serial_number = next_conn_serial_number(session->conn_serial_number);
    fo->conn_serial_number = h2le16(session->conn_serial_number); /* our connection SEQUENCE number. */
    fo->orig_vendor_id = h2le16(AB_EIP_VENDOR_ID);                /* our unique :-) vendor ID */
    fo->orig_serial_number = h2le32(AB_EIP_VENDOR_SN);            /* our serial number. */
    fo->conn_timeout_multiplier = AB_EIP_TIMEOUT_MULTIPLIER;      /* timeout = mult * RPI */

    fo->orig_to_targ_rpi = h2le32(AB_EIP_RPI); /* us to target RPI - Request Packet Interval in microseconds */

    /* screwy logic if this is a DH+ route! */
    if((session->plc_type == AB_PLC_PLC5 || session->plc_type == AB_PLC_SLC || session->plc_type == AB_PLC_MLGX)
       && session->is_dhp) {
        fo->orig_to_targ_conn_params = h2le16(AB_EIP_PLC5_PARAM);
    } else {
        fo->orig_to_targ_conn_params = h2le16(
            AB_EIP_CONN_PARAM | session->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    }

    fo->targ_to_orig_rpi = h2le32(AB_EIP_RPI); /* target to us RPI - not really used for explicit messages? */

    /* screwy logic if this is a DH+ route! */
    if((session->plc_type == AB_PLC_PLC5 || session->plc_type == AB_PLC_SLC || session->plc_type == AB_PLC_MLGX)
       && session->is_dhp) {
        fo->targ_to_orig_conn_params = h2le16(AB_EIP_PLC5_PARAM);
    } else {
        fo->targ_to_orig_conn_params = h2le16(
            AB_EIP_CONN_PARAM | session->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    }

    fo->transport_class = AB_EIP_TRANSPORT_CLASS_T3; /* 0xA3, server transport, class 3, application trigger */
    fo->path_size = session->conn_path_size / 2;     /* size in 16-bit words */

    /* set the size of the request */
    session->data_size = (uint32_t)(data - (session->data));

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done");
}


/* new version of Forward Open */
void encode_large_forward_open(cip_conn_p session) {
    eip_forward_open_request_ex_t *fo = NULL;
    uint8_t *data;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting");

    mem_set(session->data, 0, (int)(sizeof(*fo) + session->conn_path_size));

    fo = (eip_forward_open_request_ex_t *)(session->data);

    /* point to the end of the struct */
    data = (session->data) + sizeof(*fo);

    /* set up the path information. */
    mem_copy(data, session->conn_path, session->conn_path_size);
    data += session->conn_path_size;

    /* fill in the static parts */

    /* encap header parts */
    fo->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND); /* 0x006F EIP Send RR Data command */
    fo->encap_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->interface_handle))); /* total length of packet except for encap header */
    fo->encap_session_handle = h2le32(session->session_handle);
    fo->encap_sender_context = h2le64(next_sender_context(session));
    fo->router_timeout = h2le16(1); /* one second is enough ? */

    /* CPF parts */
    fo->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    fo->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI); /* null address item type */
    fo->cpf_nai_item_length = h2le16(0);             /* no data, zero length */
    fo->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fo->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->cm_service_code))); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fo->cm_service_code = AB_EIP_CMD_FORWARD_OPEN_EX; /* 0x54 Forward Open Request or 0x5B for Forward Open Extended */
    fo->cm_req_path_size = 2;                         /* size of path in 16-bit words */
    fo->cm_req_path[0] = 0x20;                        /* class */
    fo->cm_req_path[1] = 0x06;                        /* CM class */
    fo->cm_req_path[2] = 0x24;                        /* instance */
    fo->cm_req_path[3] = 0x01;                        /* instance 1 */

    /* Forward Open Params */
    fo->secs_per_tick = AB_EIP_SECS_PER_TICK; /* seconds per tick, no used? */
    fo->timeout_ticks = AB_EIP_TIMEOUT_TICKS; /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
    fo->orig_to_targ_conn_id = h2le32(0);     /* is this right?  Our connection id on the other machines? */
    fo->targ_to_orig_conn_id = h2le32(session->orig_connection_id); /* Our connection id in the other direction. */
    /* this might need to be globally unique */
    session->conn_serial_number = next_conn_serial_number(session->conn_serial_number);
    fo->conn_serial_number = h2le16(session->conn_serial_number); /* our connection ID/serial number. */
    fo->orig_vendor_id = h2le16(AB_EIP_VENDOR_ID);                /* our unique :-) vendor ID */
    fo->orig_serial_number = h2le32(AB_EIP_VENDOR_SN);            /* our serial number. */
    fo->conn_timeout_multiplier = AB_EIP_TIMEOUT_MULTIPLIER;      /* timeout = mult * RPI */
    fo->orig_to_targ_rpi = h2le32(AB_EIP_RPI);                    /* us to target RPI - Request Packet Interval in microseconds */
    fo->orig_to_targ_conn_params_ex = h2le32(
        AB_EIP_CONN_PARAM_EX | session->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    fo->targ_to_orig_rpi = h2le32(AB_EIP_RPI);              /* target to us RPI - not really used for explicit messages? */
    fo->targ_to_orig_conn_params_ex = h2le32(
        AB_EIP_CONN_PARAM_EX | session->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    fo->transport_class = AB_EIP_TRANSPORT_CLASS_T3;        /* 0xA3, server transport, class 3, application trigger */
    fo->path_size = session->conn_path_size / 2;            /* size in 16-bit words */

    /* set the size of the request */
    session->data_size = (uint32_t)(data - (session->data));

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done");
}


/*
 * Close the CIP connection.  PLCTAG_STATUS_PENDING until the reply is in; call again on each
 * pass.  Teardown goes on whatever this returns.
 */
int session_forward_close(cip_conn_p session, exchange_t *ex) {
    eip_forward_close_req_t *fc;
    eip_forward_close_resp_t *fo_resp;
    uint8_t *data;

    if(ex->state != EX_IDLE) {
        switch(exchange_step(session, ex)) {
            case EX_PENDING: return PLCTAG_STATUS_PENDING;
            case EX_FAILED:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Forward Close exchange failed %s!", plc_tag_decode_error(ex->rc));
                session->targ_connection_id = 0;
                return ex->rc;
            case EX_DONE: break;
        }

        session->targ_connection_id = 0;
        fo_resp = (eip_forward_close_resp_t *)(session->data);

        /*
         * As in the Forward Open case, we only know we got an EIP header so far.  We read
         * no further than general_status here, so the CIP reply status prefix is enough.
         */
        if((size_t)session->data_size < offsetof(eip_forward_close_resp_t, conn_serial_number)) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                   "Forward Close response of %u bytes is too short to hold the CIP reply status at %d bytes!",
                   session->data_size, (int)offsetof(eip_forward_close_resp_t, conn_serial_number));
            return PLCTAG_ERR_TOO_SMALL;
        }

        if(fo_resp->general_status != AB_EIP_OK) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Forward Close command failed, response code: %d",
                   fo_resp->general_status);
            return PLCTAG_ERR_REMOTE_ERR;
        }

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Connection close succeeded.");

        return PLCTAG_STATUS_OK;
    }

    /* no connection, nothing to close. */
    if(!session->targ_connection_id) { return PLCTAG_STATUS_OK; }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting");

    /* the buffer still holds the last packet; every field not set below must be zero. */
    mem_set(session->data, 0, (int)(sizeof(*fc) + session->conn_path_size));

    fc = (eip_forward_close_req_t *)(session->data);

    /* point to the end of the struct */
    data = (session->data) + sizeof(*fc);

    /* set up the path information. */
    mem_copy(data, session->conn_path, session->conn_path_size);
    data += session->conn_path_size;

    /* FIXME DEBUG */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Forward Close connection path:");
    pdebug_dump_bytes(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, session->conn_path, session->conn_path_size);

    /* fill in the static parts */

    /* encap header parts */
    fc->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND); /* 0x006F EIP Send RR Data command */
    fc->encap_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fc->interface_handle))); /* total length of packet except for encap header */
    fc->encap_session_handle = h2le32(session->session_handle);
    fc->encap_sender_context = h2le64(next_sender_context(session));
    fc->router_timeout = h2le16(1); /* one second is enough ? */

    /* CPF parts */
    fc->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    fc->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI); /* null address item type */
    fc->cpf_nai_item_length = h2le16(0);             /* no data, zero length */
    fc->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fc->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fc->cm_service_code))); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fc->cm_service_code = AB_EIP_CMD_FORWARD_CLOSE; /* 0x4E Forward Close Request */
    fc->cm_req_path_size = 2;                       /* size of path in 16-bit words */
    fc->cm_req_path[0] = 0x20;                      /* class */
    fc->cm_req_path[1] = 0x06;                      /* CM class */
    fc->cm_req_path[2] = 0x24;                      /* instance */
    fc->cm_req_path[3] = 0x01;                      /* instance 1 */

    /* Forward Open Params */
    fc->secs_per_tick = AB_EIP_SECS_PER_TICK;                     /* seconds per tick, no used? */
    fc->timeout_ticks = AB_EIP_TIMEOUT_TICKS;                     /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
    fc->conn_serial_number = h2le16(session->conn_serial_number); /* our connection SEQUENCE number. */
    fc->orig_vendor_id = h2le16(AB_EIP_VENDOR_ID);                /* our unique :-) vendor ID */
    fc->orig_serial_number = h2le32(AB_EIP_VENDOR_SN);            /* our serial number. */
    fc->path_size = session->conn_path_size / 2;                  /* size in 16-bit words */
    fc->reserved = (uint8_t)0;                                    /* padding for the path. */

    /* set the size of the request */
    session->data_size = (uint32_t)(data - (session->data));

    exchange_start(session, ex, true);

    return PLCTAG_STATUS_PENDING;
}


plc_type_t session_plc_type(cip_conn_p session, int32_t *refused_rc) {
    plc_type_t plc_type = AB_PLC_NONE;

    critical_block(&session->session_mutex) {
        plc_type = session->plc_type;
        *refused_rc = session->refused_rc;
    }

    return plc_type;
}


/*
 * Read the Identity object and take the PLC type from the product name.  Runs on the handler
 * thread before any tag request, so nothing else is using the buffer.  A network failure is
 * returned to be retried like any connect failure.  An answer is final: a CIP error or a device
 * the classifier refuses is kept in refused_rc so the waiting tags fail with it.
 */
int session_identify(cip_conn_p session, exchange_t *ex) {
    static uint8_t get_identity[] = {0x01, 0x02, 0x20, 0x01, 0x24, 0x01}; /* Get_Attributes_All, Identity, 1 */
    uint8_t *reply = NULL;
    size_t reply_offset = 0;
    uint8_t *data_end = NULL;
    uint8_t *data = NULL;
    uint16_t vendor_id = 0;
    char product_name[256];
    size_t name_len = 0;
    plc_type_t plc_type = AB_PLC_NONE;
    const cip_plc_type_t *plc = NULL;
    int rc = PLCTAG_STATUS_OK;

    if(ex->state == EX_IDLE) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

        mem_copy(session->data + frame_header_size(session, false), get_identity, (int)sizeof(get_identity));

        rc = frame_cip_request(session, false, sizeof(get_identity));
        if(rc == PLCTAG_STATUS_OK) { rc = prepare_request(session); }
        if(rc != PLCTAG_STATUS_OK) { return rc; }

        exchange_start(session, ex, true);

        return PLCTAG_STATUS_PENDING;
    }

    switch(exchange_step(session, ex)) {
        case EX_PENDING: return PLCTAG_STATUS_PENDING;
        case EX_FAILED:
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to exchange the Identity request, %s!",
                   plc_tag_decode_error(ex->rc));
            return ex->rc;
        case EX_DONE: break;
    }

    do {
        if((rc = check_response_framing(session, &reply_offset)) != PLCTAG_STATUS_OK) { break; }

        reply = session->data + reply_offset;
        data_end = session->data + session->data_size;

        /* a routed request may come back inside a successful Unconnected Send reply. */
        if(reply[0] == (AB_EIP_CMD_UNCONNECTED_SEND | AB_EIP_CMD_CIP_OK) && reply[2] == AB_CIP_STATUS_OK) {
            reply += sizeof(cip_header);
        }

        if((data_end - reply) < (ptrdiff_t)sizeof(cip_header)) {
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        if(reply[2] != AB_CIP_STATUS_OK) {
            size_t status_size = cip_error_data_size(reply + 2, data_end);

            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Identity request failed: %s",
                   decode_cip_error_short(reply + 2, status_size));
            rc = decode_cip_error_code(reply + 2, status_size);
            break;
        }

        if(reply[0] != (0x01 | AB_EIP_CMD_CIP_OK)) {
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        /* vendor (2), device type (2), product code (2), revision (2), status (2), serial (4), name */
        /* check the length before pointing past the additional status words. */
        if((size_t)(data_end - reply) < sizeof(cip_header) + ((size_t)reply[3] * 2) + 15) {
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        data = reply + sizeof(cip_header) + ((size_t)reply[3] * 2);

        vendor_id = (uint16_t)(data[0] | (data[1] << 8));
        name_len = data[14];

        if((size_t)(data_end - (data + 15)) < name_len) {
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        mem_copy(product_name, data + 15, (int)name_len);
        product_name[name_len] = 0;

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Device is vendor %u, \"%s\".", (unsigned int)vendor_id, product_name);

        rc = ab_plc_type_from_identity(vendor_id, product_name, session->route_path_size > 0, &plc_type);
    } while(0);

    plc = (rc == PLCTAG_STATUS_OK) ? ab_plc_type_get(plc_type) : NULL;

    critical_block(&session->session_mutex) {
        if(!plc) {
            session->refused_rc = (rc != PLCTAG_STATUS_OK) ? rc : PLCTAG_ERR_BAD_DEVICE;
        } else {
            session_apply_plc_type(session, plc);
            session->use_connected_msg = (plc->connected != 0);

            /* a connected PLC's path ends at its Message Router; it could not be known before. */
            if(session->use_connected_msg) {
                mem_copy(session->conn_path + session->conn_path_size, cip_router_path, (int)sizeof(cip_router_path));
                session->conn_path_size = (uint8_t)(session->conn_path_size + sizeof(cip_router_path));
            }
        }
    }

    if(!plc) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Device refused, %s!", plc_tag_decode_error(session->refused_rc));
    } else {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Identified a %s, %s.", plc->name,
               (session->use_connected_msg ? "connected" : "unconnected"));
    }

    /* an answer, even a refusal, ends identify. */
    return PLCTAG_STATUS_OK;
}


int session_create_request(cip_conn_p session, int tag_id, ab_request_p *req) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p res;
    size_t request_capacity = 0;
    uint8_t *buffer = NULL;

    critical_block(&session->session_mutex) {
        int available_payload = session_get_available_cip_payload_space(session);

        // FIXME: no logging in a mutex!
        // pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, "FIXME: available payload space %d", available_payload);

        request_capacity = (size_t)(available_payload + EIP_CIP_PREFIX_SIZE);
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Starting.");

    buffer = (uint8_t *)mem_alloc((int)request_capacity);
    if(!buffer) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to allocate request buffer!");
        *req = NULL;
        return PLCTAG_ERR_NO_MEM;
    }

    res = (ab_request_p)rc_alloc((int)sizeof(struct ab_request_t), request_destroy);
    if(!res) {
        mem_free(buffer);
        *req = NULL;
        rc = PLCTAG_ERR_NO_MEM;
    } else {
        res->data = buffer;
        res->tag_id = tag_id;
        res->request_capacity = (int)request_capacity;
        res->lock = LOCK_INIT;

        *req = res;
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


extern int session_reuse_request(cip_conn_p session, ab_request_p spare, int tag_id, ab_request_p *req) {
    int request_capacity = 0;
    uint8_t *data = NULL;

    if(!spare) { return session_create_request(session, tag_id, req); }

    REQUEST_SPARE_UNPOISON(spare);

    critical_block(&session->session_mutex) {
        request_capacity = session_get_available_cip_payload_space(session) + EIP_CIP_PREFIX_SIZE;
    }

    /* the connection's payload can grow after a new Forward Open. */
    if(spare->request_capacity < request_capacity) {
        rc_dec(spare);
        return session_create_request(session, tag_id, req);
    }

    /* back to how session_create_request() leaves it.  The session no longer touches it. */
    data = spare->data;
    request_capacity = spare->request_capacity;
    mem_set(spare, 0, (int32_t)sizeof(*spare));
    spare->data = data;
    spare->request_capacity = request_capacity;
    spare->tag_id = tag_id;
    spare->lock = LOCK_INIT;

    *req = spare;

    return PLCTAG_STATUS_OK;
}


/*
 * request_destroy
 *
 * The request must be removed from any lists before this!
 */

void request_destroy(void *req_arg) {
    ab_request_p req = req_arg;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Starting.");

    atomic_set_int32(&req->abort_request, 1);

    if(req->data) {
        mem_free(req->data);
        req->data = NULL;
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Done.");
}


int session_request_increase_buffer(ab_request_p request, int new_capacity) {
    uint8_t *old_buffer = NULL;
    uint8_t *new_buffer = NULL;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, request->tag_id, "Starting.");

    new_buffer = (uint8_t *)mem_alloc(new_capacity);
    if(!new_buffer) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, request->tag_id, "Unable to allocate larger request buffer!");
        return PLCTAG_ERR_NO_MEM;
    }

    spin_block(&request->lock) {
        old_buffer = request->data;
        request->request_capacity = new_capacity;
        request->data = new_buffer;
    }

    mem_free(old_buffer);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, request->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}
