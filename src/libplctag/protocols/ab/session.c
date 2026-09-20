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
#include <libplctag/protocols/cip/error_codes.h>
#include <libplctag/protocols/cip/request.h>
#include <libplctag/protocols/ab/session.h>
#include <libplctag/protocols/ab/tag.h>
#include <limits.h>
#include <utils/mem.h>
#include <utils/mutex.h>
#include <utils/nap.h>
#include <utils/rc.h>
#include <utils/socket_fd.h>
#include <utils/spinlock.h>
#include <utils/str.h>
#include <utils/thread.h>
#include <utils/time.h>
#include <stdlib.h>
#include <time.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>
#include <utils/random_utils.h>

/* Track active session handler threads for proper shutdown synchronization */
static atomic_int32_t session_handlers_active = ATOMIC_INT_STATIC_INIT;

#define MAX_REQUESTS (400)


#define MAX_CIP_LGX_MSG_SIZE (0x01FF & 504)
#define MAX_CIP_LGX_MSG_SIZE_EX (0xFFFF & 4000)

#define MAX_CIP_MICRO800_MSG_SIZE (0x01FF & 504)
#define MAX_CIP_MICRO800_MSG_SIZE_EX (0xFFFF & 4000)

/* maximum for PCCC embedded within CIP. */
#define MAX_CIP_PLC5_MSG_SIZE (244)
// #define MAX_CIP_SLC_MSG_SIZE (222)
#define MAX_CIP_SLC_MSG_SIZE (244)
#define MAX_CIP_MLGX_MSG_SIZE (244)
#define MAX_CIP_LGX_PCCC_MSG_SIZE (244)

/*
 * Number of milliseconds to wait to try to set up the session again
 * after a failure.
 */
#define RETRY_WAIT_INITIAL_MS (100)
#define RETRY_WAIT_MAX_MS (10000)

/* Idle timeout.  One second less than that negotiated with the PLC. */
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




/* plc-specific session constructors */
static ab_session_p create_plc5_session_unsafe(const char *host, const char *path, int *use_connected_msg,
                                               int connection_group_id);
static ab_session_p create_slc_session_unsafe(const char *host, const char *path, int *use_connected_msg,
                                              int connection_group_id);
static ab_session_p create_mlgx_session_unsafe(const char *host, const char *path, int *use_connected_msg,
                                               int connection_group_id);
static ab_session_p create_lgx_session_unsafe(const char *host, const char *path, int *use_connected_msg,
                                              int connection_group_id);
static ab_session_p create_lgx_pccc_session_unsafe(const char *host, const char *path, int *use_connected_msg,
                                                   int connection_group_id);
static ab_session_p create_micro800_session_unsafe(const char *host, const char *path, int *use_connected_msg,
                                                   int connection_group_id);
// static ab_session_p create_omron_njnx_session_unsafe(const char *host, const char *path, int *use_connected_msg, int
// connection_group_id);

static ab_session_p session_create_unsafe(int max_payload_capacity, bool data_buffer_is_static, const char *host,
                                          const char *path, plc_type_t plc_type, int *use_connected_msg, int connection_group_id);
static int session_init(ab_session_p session);
// static int get_plc_type(attr attribs);
static int add_session_unsafe(ab_session_p n);
static int remove_session_unsafe(ab_session_p n);
static ab_session_p find_session_by_host_unsafe(const char *gateway, const char *path, int connection_group_id);
static int session_match_valid(const char *host, const char *path, ab_session_p session);
// static int session_add_request_unsafe(ab_session_p session, ab_request_p req);
static int session_open_socket(ab_session_p session);
static void session_destroy(void *session);
static int session_register(ab_session_p session);
static int session_close_socket(ab_session_p session);
static int session_unregister(ab_session_p session);
static int64_t calc_retry_time(unsigned int retry_count);
static THREAD_FUNC(session_handler);
static int purge_aborted_requests_unsafe(ab_session_p session);
static int process_requests(ab_session_p session);
// static int check_packing(ab_session_p session, ab_request_p request);
static int get_payload_size(ab_request_p request) { return cip_get_payload_size(request); }
static int pack_requests(ab_session_p session, ab_request_p *requests, int num_requests) {
    return cip_pack_requests((cip_conn_p)session, requests, num_requests);
}
static int prepare_request(ab_session_p session);
static int send_eip_request(ab_session_p session, int timeout);
static int recv_eip_response(ab_session_p session, int timeout);
// static int perform_forward_open(ab_session_p session);
// static int try_forward_open_ex(ab_session_p session, int *max_payload_size_guess);
// static int try_forward_open(ab_session_p session);
// static int send_forward_open_req(ab_session_p session);
// static int send_forward_open_req_ex(ab_session_p session);
// static int recv_forward_open_resp(ab_session_p session, int *max_payload_size_guess);
static int send_forward_open_request(ab_session_p session);
static int receive_forward_open_response(ab_session_p session);
static inline void session_publish_event(ab_session_p session, int32_t event_type, int32_t status, int32_t reason);


static volatile mutex_p mutex = NULL;
static volatile vector_p sessions = NULL;


/* the Forward Open itself is shared; see protocols/cip/conn.h. */
static const cip_conn_io_t conn_io = {
    .send_request = (int (*)(cip_conn_p, int))send_eip_request,
    .recv_response = (int (*)(cip_conn_p, int))recv_eip_response,
};


int session_create_request(ab_session_p session, int tag_id, ab_request_p *req) {
    return cip_conn_create_request((cip_conn_p)session, tag_id, req);
}




/* these are shared; see protocols/cip/conn.h. */
static int unpack_response(ab_session_p session, ab_request_p request, int sub_packet) {
    return cip_unpack_response((cip_conn_p)session, request, sub_packet);
}


static int perform_forward_close(ab_session_p session) { return cip_perform_forward_close((cip_conn_p)session, &conn_io); }


int session_get_available_cip_payload_space(ab_session_p session) {
    return cip_conn_get_available_payload_space((cip_conn_p)session);
}




static int send_forward_open_request(ab_session_p session) { return cip_send_forward_open((cip_conn_p)session, &conn_io); }


static int receive_forward_open_response(ab_session_p session) {
    return cip_receive_forward_open_response((cip_conn_p)session, &conn_io);
}


int session_startup(void) {
    int rc = PLCTAG_STATUS_OK;

    if((rc = mutex_create((mutex_p *)&mutex)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_ERROR, 0, "Unable to create session mutex %s!", plc_tag_decode_error(rc));
        return rc;
    }

    if((sessions = vector_create(25, 5)) == NULL) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_ERROR, 0, "Unable to create session vector!");
        return PLCTAG_ERR_NO_MEM;
    }

    return rc;
}


void session_teardown(void) {
    int remaining_sessions = 0;
    int64_t start_time = 0;
    int64_t timeout_ms = 5000;
    int active_count = 0;
    int64_t elapsed = 0;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    /* flag all open sessions for termination */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Marking all open sessions for termination.");

    if(mutex) {
        critical_block(mutex) {
            if(sessions == NULL) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Session list is already destroyed.");

                break;
            }

            remaining_sessions = vector_length(sessions);

            for(int sess_index = 0; sess_index < remaining_sessions; sess_index++) {
                ab_session_p session = vector_get(sessions, sess_index);

                if(session) { atomic_set_int32(&session->terminating, 1); }
            }
        }
    }

    /* flag the whole library shutting down. */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Setting library shutdown flag.");

    if(sessions && mutex) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Waiting for sessions to terminate.");

        while(1) {
            critical_block(mutex) { remaining_sessions = vector_length(sessions); }

            /* wait for things to terminate. */
            if(remaining_sessions > 0) {
                sleep_ms(50);  // MAGIC
            } else {
                break;
            }
        }

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Sessions all terminated.");

        vector_destroy(sessions);

        sessions = NULL;
    }

    /* Wait for all active session handler threads to complete.
     * Use an atomic counter to track active handlers.
     * Wait up to 5 seconds (5000 ms) with 20ms polling intervals.
     */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Waiting for session handler threads to complete.");
    start_time = time_ms();

    while((active_count = atomic_get_int32(&session_handlers_active)) > 0) {
        elapsed = time_ms() - start_time;

        if(elapsed >= timeout_ms) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Timeout waiting for %d session handler threads to complete.",
                   active_count);
            break;
        }

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
               "Waiting for %d session handler threads to complete. Elapsed: %" PRId64 "ms", active_count, elapsed);
        sleep_ms(20);
    }

    if(active_count == 0) { pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "All session handler threads completed."); }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Destroying session mutex.");

    if(mutex) {
        mutex_destroy((mutex_p *)&mutex);
        mutex = NULL;
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");
}


/* the shared version skips zero on rollover; see protocols/cip/conn.h. */
uint64_t session_get_new_seq_id(ab_session_p session) { return cip_conn_get_new_seq_id((cip_conn_p)session); }


int session_get_max_payload(ab_session_p session) {
    int result = 0;

    if(!session) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Called with null session pointer!");
        return 0;
    }

    critical_block(session->mutex) { result = GET_MAX_PAYLOAD_SIZE(session); }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "max payload size is %d bytes.", result);

    return result;
}

int session_find_or_create(ab_session_p *tag_session, attr attribs, int *is_new_session) {
    /*int debug = attr_get_int(attribs,"debug",0);*/
    const char *session_gw = attr_get_str(attribs, "gateway", "");
    const char *session_path = attr_get_str(attribs, "path", "");
    int use_connected_msg = attr_get_int(attribs, "use_connected_msg", 0);
    // int session_gw_port = attr_get_int(attribs, "gateway_port", EIP_DEFAULT_PORT);
    plc_type_t plc_type = get_plc_type(attribs);
    ab_session_p session = AB_SESSION_NULL;
    int new_session = 0;
    int shared_session = attr_get_int(attribs, "share_session", 1); /* share the session by default. */
    int rc = PLCTAG_STATUS_OK;
    int connection_inactivity_timeout_ms = CIP_DISCONNECT_TIMEOUT;
    int connection_group_id = attr_get_int(attribs, "connection_group_id", 0);
    int only_use_old_forward_open = attr_get_int(attribs, "conn_only_use_old_forward_open", 0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Starting");

    connection_inactivity_timeout_ms = attr_get_int(attribs, "connection_inactivity_timeout_ms", CIP_DISCONNECT_TIMEOUT);
    if(connection_inactivity_timeout_ms < 1 || connection_inactivity_timeout_ms > CIP_DISCONNECT_TIMEOUT) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
               "Invalid connection_inactivity_timeout_ms %d. Must be between 1 and %d. Using default %d.",
               connection_inactivity_timeout_ms, CIP_DISCONNECT_TIMEOUT, CIP_DISCONNECT_TIMEOUT);
        connection_inactivity_timeout_ms = CIP_DISCONNECT_TIMEOUT;
    } else {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Setting connection_inactivity_timeout_ms to %dms.",
               connection_inactivity_timeout_ms);
    }

    // if(plc_type == AB_PLC_PLC5 && str_length(session_path) > 0) {
    //     /* this means it is DH+ */
    //     use_connected_msg = 1;
    //     attr_set_int(attribs, "use_connected_msg", 1);
    // }

    critical_block(mutex) {
        /* if we are to share sessions, then look for an existing one. */
        if(shared_session) {
            session = find_session_by_host_unsafe(session_gw, session_path, connection_group_id);
        } else {
            /* no sharing, create a new one */
            session = AB_SESSION_NULL;
        }

        if(session == AB_SESSION_NULL) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Creating new session.");

            switch(plc_type) {
                case AB_PLC_PLC5:
                    session = create_plc5_session_unsafe(session_gw, session_path, &use_connected_msg, connection_group_id);
                    break;

                case AB_PLC_SLC:
                    session = create_slc_session_unsafe(session_gw, session_path, &use_connected_msg, connection_group_id);
                    break;

                case AB_PLC_MLGX:
                    session = create_mlgx_session_unsafe(session_gw, session_path, &use_connected_msg, connection_group_id);
                    break;

                case AB_PLC_LGX:
                    session = create_lgx_session_unsafe(session_gw, session_path, &use_connected_msg, connection_group_id);
                    break;

                case AB_PLC_LGX_PCCC:
                    session = create_lgx_pccc_session_unsafe(session_gw, session_path, &use_connected_msg, connection_group_id);
                    break;

                case AB_PLC_MICRO800:
                    session = create_micro800_session_unsafe(session_gw, session_path, &use_connected_msg, connection_group_id);
                    break;

                case AB_PLC_GENERIC:
                    /* Generic PLC type uses unconnected messaging for stateless operations */
                    use_connected_msg = 0;
                    session = create_lgx_session_unsafe(session_gw, session_path, &use_connected_msg, connection_group_id);
                    break;

                    // case AB_PLC_OMRON_NJNX:
                    //     session = create_omron_njnx_session_unsafe(session_gw, session_path, &use_connected_msg,
                    //     connection_group_id); break;

                default:
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unknown PLC type %d!", plc_type);
                    session = NULL;
                    break;
            }

            if(session == AB_SESSION_NULL) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "unable to create or find a session!");
                rc = PLCTAG_ERR_BAD_GATEWAY;
            } else {
                atomic_init_int32(&session->connection_inactivity_timeout_ms, connection_inactivity_timeout_ms);

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
     * do this OUTSIDE the mutex in order to let other threads not block if
     * the session creation process blocks.
     */

    if(new_session) {
        rc = session_init(session);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "rc_dec: Releasing session reference.");
            rc_dec(session);
            session = AB_SESSION_NULL;
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


int add_session_unsafe(ab_session_p session) {
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Starting");

    if(!session) { return PLCTAG_ERR_NULL_PTR; }

    vector_set(sessions, vector_length(sessions), session);

    session->on_list = 1;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Done");

    return PLCTAG_STATUS_OK;
}


int add_session(ab_session_p s) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Starting.");

    critical_block(mutex) { rc = add_session_unsafe(s); }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


int remove_session_unsafe(ab_session_p session) {
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Starting");

    if(!session || !sessions) { return 0; }

    for(int i = 0; i < vector_length(sessions); i++) {
        ab_session_p tmp = vector_get(sessions, i);

        /* FIXME potential ABA problem here */
        if(tmp == session) {
            vector_remove(sessions, i);
            break;
        }
    }

    /* no longer on the list */
    session->on_list = 0;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Done");

    return PLCTAG_STATUS_OK;
}

int remove_session(ab_session_p s) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Starting.");

    if(s->on_list) {
        critical_block(mutex) { rc = remove_session_unsafe(s); }
    } else {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Session not on list, skipping removal.");
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


int session_match_valid(const char *host, const char *path, ab_session_p session) {
    if(!session) { return 0; }

    /* don't use sessions that failed immediately. */
    if(session->failed) { return 0; }

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


ab_session_p find_session_by_host_unsafe(const char *host, const char *path, int connection_group_id) {
    for(int i = 0; i < vector_length(sessions); i++) {
        ab_session_p session = vector_get(sessions, i);

        /* is this session in the process of destruction? */
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "rc_inc: Acquiring session reference.");
        session = rc_inc(session);
        if(session) {
            if(session->connection_group_id == connection_group_id && session_match_valid(host, path, session)) {
                return session;
            }

            rc_dec(session);
        }
    }

    return NULL;
}


ab_session_p create_plc5_session_unsafe(const char *host, const char *path, int *use_connected_msg, int connection_group_id) {
    ab_session_p session = NULL;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    do {
        session =
            session_create_unsafe(MAX_CIP_PLC5_MSG_SIZE, true, host, path, AB_PLC_PLC5, use_connected_msg, connection_group_id);
        if(session != NULL) {
            session->only_use_old_forward_open = true;
            session->plc_config.fo_conn_size = MAX_CIP_PLC5_MSG_SIZE;
            session->plc_config.fo_ex_conn_size = 0;
            session->plc_config.min_payload_size = CIP_MIN_PAYLOAD_SIZE_PCCC;
            /* the PCCC families have neither fragmentation nor packing. */
            session->plc_config.supports_fragmented_operations = false;
            session->plc_config.supports_packed_requests = false;
            session->max_payload_size = (uint16_t)session->plc_config.fo_conn_size;
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create PLC/5 session!");
        }
    } while(0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return session;
}


ab_session_p create_slc_session_unsafe(const char *host, const char *path, int *use_connected_msg, int connection_group_id) {
    ab_session_p session = NULL;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    do {
        session =
            session_create_unsafe(MAX_CIP_SLC_MSG_SIZE, true, host, path, AB_PLC_SLC, use_connected_msg, connection_group_id);
        if(session != NULL) {
            session->only_use_old_forward_open = true;
            session->plc_config.fo_conn_size = MAX_CIP_SLC_MSG_SIZE;
            session->plc_config.fo_ex_conn_size = 0;
            session->plc_config.min_payload_size = CIP_MIN_PAYLOAD_SIZE_PCCC;
            /* the PCCC families have neither fragmentation nor packing. */
            session->plc_config.supports_fragmented_operations = false;
            session->plc_config.supports_packed_requests = false;
            session->max_payload_size = (uint16_t)session->plc_config.fo_conn_size;
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create SLC 500 session!");
        }
    } while(0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return session;
}


ab_session_p create_mlgx_session_unsafe(const char *host, const char *path, int *use_connected_msg, int connection_group_id) {
    ab_session_p session = NULL;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    do {
        session =
            session_create_unsafe(MAX_CIP_MLGX_MSG_SIZE, true, host, path, AB_PLC_MLGX, use_connected_msg, connection_group_id);
        if(session != NULL) {
            session->only_use_old_forward_open = true;
            session->plc_config.fo_conn_size = MAX_CIP_MLGX_MSG_SIZE;
            session->plc_config.fo_ex_conn_size = 0;
            session->plc_config.min_payload_size = CIP_MIN_PAYLOAD_SIZE_PCCC;
            /* the PCCC families have neither fragmentation nor packing. */
            session->plc_config.supports_fragmented_operations = false;
            session->plc_config.supports_packed_requests = false;
            session->max_payload_size = (uint16_t)session->plc_config.fo_conn_size;
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create Micrologix session!");
        }
    } while(0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return session;
}


ab_session_p create_lgx_session_unsafe(const char *host, const char *path, int *use_connected_msg, int connection_group_id) {
    ab_session_p session = NULL;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    do {
        session =
            session_create_unsafe(MAX_CIP_LGX_MSG_SIZE_EX, true, host, path, AB_PLC_LGX, use_connected_msg, connection_group_id);
        if(session != NULL) {
            session->only_use_old_forward_open = false;
            session->plc_config.fo_conn_size = MAX_CIP_LGX_MSG_SIZE;
            session->plc_config.fo_ex_conn_size = MAX_CIP_LGX_MSG_SIZE_EX;
            session->plc_config.min_payload_size = CIP_MIN_PAYLOAD_SIZE_CIP;
            session->plc_config.supports_fragmented_operations = true;
            session->plc_config.supports_packed_requests = true;
            session->max_payload_size = (uint16_t)session->plc_config.fo_conn_size;
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create *Logix session!");
        }
    } while(0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return session;
}


ab_session_p create_lgx_pccc_session_unsafe(const char *host, const char *path, int *use_connected_msg, int connection_group_id) {
    ab_session_p session = NULL;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    do {
        session = session_create_unsafe(MAX_CIP_LGX_PCCC_MSG_SIZE, true, host, path, AB_PLC_LGX_PCCC, use_connected_msg,
                                        connection_group_id);
        if(session != NULL) {
            session->only_use_old_forward_open = true;
            session->plc_config.fo_conn_size = MAX_CIP_LGX_PCCC_MSG_SIZE;
            session->plc_config.fo_ex_conn_size = 0;
            session->plc_config.min_payload_size = CIP_MIN_PAYLOAD_SIZE_PCCC;
            /* the PCCC families have neither fragmentation nor packing. */
            session->plc_config.supports_fragmented_operations = false;
            session->plc_config.supports_packed_requests = false;
            session->max_payload_size = (uint16_t)session->plc_config.fo_conn_size;
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create Micrologix session!");
        }
    } while(0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return session;
}


ab_session_p create_micro800_session_unsafe(const char *host, const char *path, int *use_connected_msg, int connection_group_id) {
    ab_session_p session = NULL;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    do {
        session = session_create_unsafe(MAX_CIP_MICRO800_MSG_SIZE_EX, true, host, path, AB_PLC_MICRO800, use_connected_msg,
                                        connection_group_id);
        if(session != NULL) {
            session->only_use_old_forward_open = true;
            session->plc_config.fo_conn_size = MAX_CIP_MICRO800_MSG_SIZE;
            session->plc_config.fo_ex_conn_size = MAX_CIP_MICRO800_MSG_SIZE_EX;
            session->plc_config.min_payload_size = CIP_MIN_PAYLOAD_SIZE_CIP;
            /* Micro800 fragments a single operation but cannot pack several. */
            session->plc_config.supports_fragmented_operations = true;
            session->plc_config.supports_packed_requests = false;
            session->max_payload_size = (uint16_t)session->plc_config.fo_conn_size;
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create Micro800 session!");
        }
    } while(0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return session;
}


ab_session_p session_create_unsafe(int max_payload_capacity, bool data_buffer_is_static, const char *host, const char *path,
                                   plc_type_t plc_type, int *use_connected_msg, int connection_group_id) {
    static volatile uint32_t connection_id = 0;

    int rc = PLCTAG_STATUS_OK;
    ab_session_p session = AB_SESSION_NULL;
    size_t total_allocation_size = sizeof(*session);
    size_t data_buffer_capacity =
        (size_t)EIP_CIP_PREFIX_SIZE + (size_t)max_payload_capacity + (size_t)32;  // MAGIC - FIXME this is just a bandaid.
    size_t data_buffer_offset = 0;
    size_t host_name_offset = 0;
    size_t host_name_size = 0;
    size_t path_offset = 0;
    size_t path_size = 0;
    size_t conn_path_offset = 0;
    uint8_t tmp_conn_path[MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN];
    int tmp_conn_path_size = MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN;
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
        return AB_SESSION_NULL;
    }

    /*
     * The path string is copied in verbatim too, and it is not self-limiting: spaces are
     * skipped everywhere in cip_encode_path(), so an arbitrarily long string can still
     * encode to a valid short path.  Bound it against the encoded path buffer -- a real
     * route is a handful of hops, so this rejects nothing that describes real hardware.
     */
    if(path && str_length(path) >= MAX_CONN_PATH) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Path string is longer than the maximum of %d bytes!", MAX_CONN_PATH - 1);
        return AB_SESSION_NULL;
    }

    if(*use_connected_msg) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Session should use connected messaging.");
    } else {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Session should not use connected messaging.");
    }

    /* add in space for the data buffer. */
    if(data_buffer_is_static) {
        data_buffer_offset = total_allocation_size;
        total_allocation_size += data_buffer_capacity;
    } else {
        data_buffer_offset = 0;
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
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Unable to convert path string to binary path, error %s!",
               plc_tag_decode_error(rc));
        return NULL;
    }

    conn_path_offset = total_allocation_size;
    total_allocation_size += (size_t)tmp_conn_path_size;

    /* allocate the session struct and the buffer in the same allocation. */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
           "Allocating %" PRIu64 " total bytes of memory with %" PRIu64 " bytes for data buffer static data, %" PRId32
           " bytes for the host name, %" PRId32 " bytes for the path, %" PRId32 " bytes for the encoded path.",
           (uint64_t)total_allocation_size, (uint64_t)(data_buffer_is_static ? data_buffer_capacity : 0),
           (int32_t)(str_length(host) + 1), (int32_t)(path_offset == 0 ? 0 : str_length(path) + 1), (int32_t)tmp_conn_path_size);

    session = (ab_session_p)rc_alloc((int)total_allocation_size, session_destroy);
    if(!session) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error allocating new session!");
        return AB_SESSION_NULL;
    }

    /* fill in the interior pointers */

    /* zero is a valid file descriptor, so the handle needs an explicit invalid value. */
    session->sock = SOCKET_FD_INVALID;

    /* fix up the data buffer. */
    session->data_buffer_is_static = data_buffer_is_static;
    session->data_capacity = (uint32_t)data_buffer_capacity;

    if(data_buffer_is_static) {
        session->data = (uint8_t *)(session) + data_buffer_offset;
        // session->data_capacity = max_buffer_size;
    } else {
        session->data = (uint8_t *)mem_alloc((int)data_buffer_capacity);
        if(session->data == NULL) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to allocate the connection data buffer!");
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "rc_dec: Releasing session reference.");
            return rc_dec(session);
        }
    }

    /* point the host pointer just after the data. */
    session->host = (char *)(session) + host_name_offset;
    str_copy(session->host, (int)host_name_size, host);

    if(path_offset) {
        session->path = (char *)(session) + path_offset;
        str_copy(session->path, (int)path_size, path);
    }

    if(conn_path_offset) {
        session->conn_path = (uint8_t *)(session) + conn_path_offset;

        // FIXME - the path length cannot be 8 bits with a buffer length that is over 260.
        session->conn_path_size = (uint8_t)tmp_conn_path_size;
        mem_copy(session->conn_path, tmp_conn_path, tmp_conn_path_size);
    }


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
    session->failed = 0;
    session->conn_serial_number = (uint16_t)(random_u64(UINT16_MAX) + 1);
    session->conn_seq_id = (uint64_t)(random_u64(UINT32_MAX) + 1);
    session->is_dhp = is_dhp;
    session->dhp_dest = dhp_dest;

    /*
     * A PCCC PLC reached over a DH+ bridge needs a fixed connection parameter word
     * rather than the usual CIP_CONN_PARAM plus negotiated size.  Working it out here
     * keeps the Forward Open code free of PLC types -- see cip_plc_config_t.
     */
    if(is_dhp && (plc_type == AB_PLC_PLC5 || plc_type == AB_PLC_SLC || plc_type == AB_PLC_MLGX)) {
        session->plc_config.conn_params_override = AB_EIP_PLC5_PARAM;
    }
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

    /* add the new session to the list. */
    add_session_unsafe(session);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done");

    return session;
}


/*
 * session_init
 *
 * This calls several blocking methods and so must not keep the main mutex
 * locked during them.
 */
int session_init(ab_session_p session) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    /* create the session mutex. */
    if((rc = mutex_create(&(session->mutex))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create session mutex!");
        session->failed = 1;
        return rc;
    }

    /* create the session nap. */
    if((rc = nap_create(&(session->nap))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create session condition var!");
        session->failed = 1;
        return rc;
    }

    if((rc = thread_create((thread_p *)&(session->handler_thread), session_handler, 32 * 1024, session)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create session thread!");
        session->failed = 1;
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return rc;
}


/*
 * session_open_socket()
 *
 * Connect to the host/port passed via TCP.
 */

int session_open_socket(ab_session_p session) {
    int rc = PLCTAG_STATUS_OK;
    char **server_port = NULL;
    int port = 0;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    /*
     * A reconnect reuses this session, so drop the identity of the connection that just
     * went away.  Otherwise the checks in recv_eip_response() compare the new session's
     * RegisterSession reply against the old handle and reject it, and the session can
     * never come back up.
     */
    session->conn_handle = 0;
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
        port = EIP_DEFAULT_PORT;

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Using default port %d.", port);
    }

    rc = cip_conn_socket_open((cip_conn_p)session, server_port[0], (int32_t)port);

    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to connect socket for session!");
        mem_free(server_port);
        return rc;
    }

    if(server_port) { mem_free(server_port); }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return rc;
}


int session_register(ab_session_p session) {
    eip_session_reg_req *req;
    eip_encap *resp;
    int rc = PLCTAG_STATUS_OK;

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
    req->encap_command = h2le16(EIP_REGISTER_SESSION);
    req->encap_length = h2le16(sizeof(eip_session_reg_req) - sizeof(eip_encap));
    req->encap_session_handle = h2le32(/*session->conn_handle*/ 0);
    req->encap_status = h2le32(0);
    req->encap_sender_context = h2le64((uint64_t)0);
    req->encap_options = h2le32(0);

    req->eip_version = h2le16(EIP_VERSION);
    req->option_flags = h2le16(0);

    /*
     * socket ops here are _ASYNCHRONOUS_!
     *
     * This is done this way because we do not have everything
     * set up for a request to be handled by the thread.  I think.
     */

    /* send registration to the gateway */
    session->data_size = sizeof(eip_session_reg_req);
    session->data_offset = 0;

    rc = send_eip_request(session, SESSION_DEFAULT_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error sending session registration request %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    /* get the response from the gateway */
    rc = recv_eip_response(session, SESSION_DEFAULT_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error receiving session registration response %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    /* encap header is at the start of the buffer */
    resp = (eip_encap *)(session->data);

    /* check the response status */
    if(le2h16(resp->encap_command) != EIP_REGISTER_SESSION) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "EIP unexpected response packet type: %" PRIu16 "!",
               le2h16(resp->encap_command));
        return PLCTAG_ERR_BAD_DATA;
    }

    if(le2h32(resp->encap_status) != EIP_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "EIP command failed, response code: %d", le2h32(resp->encap_status));
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /*
     * after all that, save the session handle, we will
     * use it in future packets.
     */
    session->conn_handle = le2h32(resp->encap_session_handle);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


int session_unregister(ab_session_p session) {
    (void)session;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    /* nothing to do, perhaps. */

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


int session_close_socket(ab_session_p session) {
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    cip_conn_socket_close((cip_conn_p)session);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


void session_destroy(void *session_arg) {
    ab_session_p session = session_arg;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    if(!session) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Session ptr is null!");

        return;
    }

    /* so remove the session from the list so no one else can reference it. */
    remove_session(session);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Session sent %" PRId64 " packets.", session->packet_count);

    /* terminate the session thread first. */
    atomic_set_int32(&session->terminating, 1);

    /* interrupt the nap in case the handler is sleeping */
    if(session->nap) { nap_interrupt(session->nap); }

    /* get rid of the handler thread. */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Destroying session thread.");
    if(session->handler_thread) {
        /* this cannot be guarded by the mutex since the session thread also locks it. */
        thread_join(&(session->handler_thread));
    }


    /* this needs to be handled in the mutex to prevent double frees due to queued requests. */
    critical_block(session->mutex) {
        /* close off the connection if is one. This helps the PLC clean up. */
        if(session->targ_connection_id) {
            /*
             * we do not want the internal loop to immediately
             * return, so set the flag like we are not terminating.
             * There is still a timeout that applies.
             */
            atomic_set_int32(&session->terminating, 0);
            perform_forward_close(session);
            atomic_set_int32(&session->terminating, 1);
        }

        /* try to be nice and un-register the session */
        if(session->conn_handle) { session_unregister(session); }

        if(session->sock != SOCKET_FD_INVALID) { session_close_socket(session); }

        /* release all the requests that are in the queue. */
        if(session->requests) {
            for(int i = 0; i < vector_length(session->requests); i++) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "rc_dec: Releasing request reference.");
                rc_dec(vector_get(session->requests, i));
            }

            vector_destroy(session->requests);
            session->requests = NULL;
        }
    }

    /* we are done with the nap, finally destroy it. */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Destroying session nap.");
    if(session->nap) {
        nap_destroy(&(session->nap));
        session->nap = NULL;
    }

    /* we are done with the mutex, finally destroy it. */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Destroying session mutex.");
    if(session->mutex) {
        mutex_destroy(&(session->mutex));
        session->mutex = NULL;
    }

    if(!session->data_buffer_is_static) { mem_free(session->data); }

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
int session_add_request(ab_session_p session, ab_request_p req) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, req->tag_id, "Starting. session=%p, req=%p", (void *)session, (void *)req);

    /* this must be checked before the critical block below dereferences it. */
    if(!session) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, req->tag_id, "Session is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    critical_block(session->mutex) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, req->tag_id, "rc_inc: Acquiring request reference.");
        req = rc_inc(req);

        if(!req) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Request is either null or in the process of being deleted.");
            rc = PLCTAG_ERR_NULL_PTR;
            break;
        }

        /* insert into the requests vector */
        vector_set(session->requests, vector_length(session->requests), req);
    }

    if(rc != PLCTAG_STATUS_OK) { return rc; }

    /* wake up the session thread because we added something to process. */
    nap_interrupt(session->nap);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, req->tag_id, "Done.");

    return rc;
}


int64_t calc_retry_time(unsigned int retry_count) {
    int64_t result = 0;
    result = RETRY_WAIT_INITIAL_MS * (1 << retry_count);

    if(result > RETRY_WAIT_MAX_MS) { result = RETRY_WAIT_MAX_MS; }

    result += (int64_t)random_u64(RETRY_WAIT_INITIAL_MS) - (int64_t)(RETRY_WAIT_INITIAL_MS / 2);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Retry count %u for retry time delay of %" PRId64 "ms.", retry_count,
           result);

    return result;
}


/*****************************************************************
 **************** Session handling functions *********************
 ****************************************************************/


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


static inline void session_publish_event(ab_session_p session, int32_t event_type, int32_t status, int32_t reason) {
    int32_t write_idx = atomic_get_int32(&session->conn_status_ring_write_idx);

    if(session->conn_status_ring[write_idx].event_type == event_type && session->conn_status_ring[write_idx].status == status) {
        return;
    }

    write_idx = (write_idx + 1) & SESSION_CONN_STATUS_RING_SIZE_MASK;

    /* write data to the slot before publishing the new index */
    session->conn_status_ring[write_idx].event_type = event_type;
    session->conn_status_ring[write_idx].status = status;
    session->conn_status_ring[write_idx].reason = reason;

    /* atomic store acts as the release point; readers will not see this slot until after this */
    atomic_set_int32(&session->conn_status_ring_write_idx, write_idx);
    plc_tag_tickler_wake();
}


/* Set connection status and reason atomics, and push a ring buffer entry if the status changed.
 * Must only be called from the session handler thread (single writer).
 *
 * connection_status and the ring publish must change together under mutex:
 * ab_connection_tag_create() takes a paired snapshot of both (conn_status_ring_write_idx
 * and connection_status) to seed a freshly created connection tag, and needs the same
 * mutex to avoid reading one from before this transition and the other from after it --
 * see the comment there. */
static inline void session_set_connection_status(ab_session_p session, int32_t new_status, int32_t new_reason) {
    critical_block(session->mutex) {
        int32_t old_status = atomic_get_int32(&session->connection_status);
        atomic_set_int32(&session->connection_status_reason, new_reason);
        atomic_set_int32(&session->connection_status, new_status);
        if(old_status != new_status) {
            session_publish_event(session, new_status + PLCTAG_EVENT_CONN_STATUS_OFFSET, PLCTAG_STATUS_OK, new_reason);
        }
    }
}


THREAD_FUNC(session_handler) {
    ab_session_p session = arg;
    int rc = PLCTAG_STATUS_OK;
    session_state_t state = SESSION_OPEN_SOCKET_START;
    int64_t now = 0;
    int64_t timeout_time = 0;
    int64_t wait_until_time = 0;
    int32_t inactivity_timeout_ms = atomic_get_int32(&session->connection_inactivity_timeout_ms);
    int64_t auto_disconnect_time = time_ms() + inactivity_timeout_ms;
    unsigned int retry_count = 0;
    int64_t retry_wait_ms = 0;
    int auto_disconnect = 0;


    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting thread for session %p", (void *)session);

    /* Increment the count of active session handlers */
    atomic_add_int32(&session_handlers_active, 1);

    while(!atomic_get_int32(&session->terminating) && atomic_get_bool(&lib_active)) {
        now = time_ms();

        /* how long should we wait if nothing wakes us? */
        wait_until_time = now + SESSION_IDLE_WAIT_TIME;

        /*
         * Do this on every cycle.   This keeps the queue clean(ish).
         *
         * Make sure we get rid of all the aborted requests queued.
         * This keeps the overall memory usage lower.
         */

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_SPEW, 0, "Critical block.");
        critical_block(session->mutex) { purge_aborted_requests_unsafe(session); }

        switch(state) {
            case SESSION_OPEN_SOCKET_START:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_OPEN_SOCKET_START state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_CONNECTING, PLCTAG_STATUS_PENDING);

                /* we must connect to the gateway*/
                rc = session_open_socket(session);
                if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "session connect failed %s!", plc_tag_decode_error(rc));
                    atomic_set_int32(&session->connection_status_reason, rc);
                    state = SESSION_CLOSE_SOCKET;
                } else {
                    if(rc == PLCTAG_STATUS_OK) {
                        /* bump auto disconnect time into the future so that we do not accidentally disconnect immediately. */
                        inactivity_timeout_ms = atomic_get_int32(&session->connection_inactivity_timeout_ms);
                        auto_disconnect_time = now + inactivity_timeout_ms;

                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "Connect complete immediately, going to state SESSION_REGISTER.");

                        state = SESSION_REGISTER;

                        retry_count = 0;
                    } else {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "Connect started, going to state SESSION_OPEN_SOCKET_WAIT.");

                        state = SESSION_OPEN_SOCKET_WAIT;
                    }
                }

                /* in all cases, don't wait. */
                nap_interrupt(session->nap);

                break;

            case SESSION_OPEN_SOCKET_WAIT:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_OPEN_SOCKET_WAIT state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_CONNECTING, PLCTAG_STATUS_PENDING);

                /* we must connect to the gateway */
                rc = cip_conn_connect_check((cip_conn_p)session, SOCKET_WAIT_TIMEOUT_MS);
                if(rc == PLCTAG_STATUS_OK) {
                    /* connected! */
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Socket connection succeeded.");

                    /* calculate the disconnect time. */
                    inactivity_timeout_ms = atomic_get_int32(&session->connection_inactivity_timeout_ms);
                    auto_disconnect_time = now + inactivity_timeout_ms;

                    state = SESSION_REGISTER;
                } else if(rc == PLCTAG_ERR_TIMEOUT) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Still waiting for connection to succeed.");

                    /* don't wait more.  The TCP connect check will wait in select(). */
                } else {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Session connect failed %s!", plc_tag_decode_error(rc));
                    atomic_set_int32(&session->connection_status_reason, rc);

                    state = SESSION_CLOSE_SOCKET;
                }

                /* in all cases, don't wait. */
                nap_interrupt(session->nap);

                break;

            case SESSION_REGISTER:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_REGISTER state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_CONNECTING, PLCTAG_STATUS_PENDING);

                if((rc = session_register(session)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "session registration failed %s!", plc_tag_decode_error(rc));
                    atomic_set_int32(&session->connection_status_reason, rc);
                    state = SESSION_CLOSE_SOCKET;
                } else {
                    retry_wait_ms = RETRY_WAIT_INITIAL_MS;

                    if(session->use_connected_msg) {
                        state = SESSION_SEND_FORWARD_OPEN;
                    } else {
                        state = SESSION_IDLE;
                    }
                }
                nap_interrupt(session->nap);
                break;

            case SESSION_SEND_FORWARD_OPEN:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_SEND_FORWARD_OPEN state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_CONNECTING, PLCTAG_STATUS_PENDING);

                if((rc = send_forward_open_request(session)) != PLCTAG_STATUS_OK) {
                    atomic_set_int32(&session->connection_status_reason, rc);
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Send Forward Open failed %s!", plc_tag_decode_error(rc));
                    state = SESSION_UNREGISTER;
                } else {
                    retry_wait_ms = RETRY_WAIT_INITIAL_MS;

                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                           "Send Forward Open succeeded, going to SESSION_RECEIVE_FORWARD_OPEN state.");
                    state = SESSION_RECEIVE_FORWARD_OPEN;
                }
                nap_interrupt(session->nap);
                break;

            case SESSION_RECEIVE_FORWARD_OPEN:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_RECEIVE_FORWARD_OPEN state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_CONNECTING, PLCTAG_STATUS_PENDING);

                if((rc = receive_forward_open_response(session)) != PLCTAG_STATUS_OK) {
                    if(rc == PLCTAG_ERR_DUPLICATE) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "Duplicate connection error received, trying again with different connection ID.");
                        state = SESSION_SEND_FORWARD_OPEN;
                    } else if(rc == PLCTAG_ERR_TOO_LARGE) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "Requested packet size too large, retrying with smaller size.");
                        state = SESSION_SEND_FORWARD_OPEN;
                    } else if(rc == PLCTAG_ERR_UNSUPPORTED && !session->only_use_old_forward_open) {
                        /* if we got an unsupported error and we are trying with ForwardOpenEx, then try the old command. */
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "PLC does not support ForwardOpenEx, trying old ForwardOpen.");
                        session->only_use_old_forward_open = 1;
                        state = SESSION_SEND_FORWARD_OPEN;
                    } else {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Receive Forward Open failed %s!",
                               plc_tag_decode_error(rc));
                        state = SESSION_UNREGISTER;
                    }
                } else {
                    retry_wait_ms = RETRY_WAIT_INITIAL_MS;
                    atomic_set_int32(&session->connection_status_reason, PLCTAG_STATUS_OK);
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Send Forward Open succeeded, going to SESSION_IDLE state.");
                    state = SESSION_IDLE;
                }
                nap_interrupt(session->nap);
                break;

            case SESSION_IDLE:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_IDLE state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_UP, PLCTAG_STATUS_OK);

                /* make sure that our timeout period has not changed */
                if(inactivity_timeout_ms != atomic_get_int32(&session->connection_inactivity_timeout_ms)) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                           "Inactivity timeout changed from %" PRId32 "ms to %" PRId32 "ms, updating auto disconnect time.",
                           inactivity_timeout_ms, atomic_get_int32(&session->connection_inactivity_timeout_ms));
                    inactivity_timeout_ms = atomic_get_int32(&session->connection_inactivity_timeout_ms);
                    auto_disconnect_time = now + inactivity_timeout_ms;
                }

                /* if there is work to do, make sure we do not disconnect. */
                critical_block(session->mutex) {
                    int num_reqs = vector_length(session->requests);
                    if(num_reqs > 0) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "There are %d requests pending before cleanup and sending.", num_reqs);
                        inactivity_timeout_ms = atomic_get_int32(&session->connection_inactivity_timeout_ms);
                        auto_disconnect_time = now + inactivity_timeout_ms;
                    }
                }

                if((rc = process_requests(session)) != PLCTAG_STATUS_OK) {
                    atomic_set_int32(&session->connection_status_reason, rc);
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error while processing requests %s!",
                           plc_tag_decode_error(rc));
                    if(session->use_connected_msg) {
                        state = SESSION_DISCONNECT;
                    } else {
                        state = SESSION_UNREGISTER;
                    }

                    nap_interrupt(session->nap);
                }

                /* check if we should disconnect */
                if(auto_disconnect_time < now) {
                    atomic_set_int32(&session->connection_status_reason, PLCTAG_STATUS_OK);
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Disconnecting due to inactivity.");

                    auto_disconnect = 1;

                    if(session->use_connected_msg) {
                        state = SESSION_DISCONNECT;
                    } else {
                        state = SESSION_UNREGISTER;
                    }
                    nap_interrupt(session->nap);
                }

                /* if there is work to do, make sure we signal the condition var. */
                critical_block(session->mutex) {
                    int num_reqs = vector_length(session->requests);
                    if(num_reqs > 0) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "There are %d requests still pending after abort purge and sending.", num_reqs);
                        nap_interrupt(session->nap);
                    }
                }

                break;

            case SESSION_DISCONNECT:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_DISCONNECT state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_DISCONNECTING,
                                              atomic_get_int32(&session->connection_status_reason));

                if((rc = perform_forward_close(session)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Forward close failed %s!", plc_tag_decode_error(rc));
                }

                state = SESSION_UNREGISTER;
                nap_interrupt(session->nap);
                break;

            case SESSION_UNREGISTER:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_UNREGISTER state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_DISCONNECTING,
                                              atomic_get_int32(&session->connection_status_reason));

                if((rc = session_unregister(session)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unregistering session failed %s!", plc_tag_decode_error(rc));
                }

                state = SESSION_CLOSE_SOCKET;
                nap_interrupt(session->nap);
                break;

            case SESSION_CLOSE_SOCKET:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_CLOSE_SOCKET state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_DOWN,
                                              atomic_get_int32(&session->connection_status_reason));

                if((rc = session_close_socket(session)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Closing session socket failed %s!", plc_tag_decode_error(rc));
                }

                if(auto_disconnect) {
                    state = SESSION_WAIT_IDLE_RECONNECT;
                } else {
                    state = SESSION_START_RETRY;
                }
                nap_interrupt(session->nap);
                break;

            case SESSION_START_RETRY:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_START_RETRY state.");

                /* FIXME - make this a tag attribute. */
                timeout_time = now + calc_retry_time(retry_count);
                retry_count++;

                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Waiting %dms before trying to reconnect.",
                       (int)(retry_wait_ms));

                /* start waiting. */
                state = SESSION_WAIT_ERR_RETRY;

                nap_interrupt(session->nap);
                break;

            case SESSION_WAIT_ERR_RETRY:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_WAIT_ERR_RETRY state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_ERR_WAIT,
                                              atomic_get_int32(&session->connection_status_reason));

                if(timeout_time < now) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Transitioning to SESSION_OPEN_SOCKET_START.");
                    state = SESSION_OPEN_SOCKET_START;
                    nap_interrupt(session->nap);
                } else {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Wait not complete, still %dms to go.",
                           (int)(timeout_time - now));
                }

                break;

            case SESSION_WAIT_IDLE_RECONNECT:
                /* wait for at least one request to queue before reconnecting. */
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_WAIT_IDLE_RECONNECT state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_IDLE_WAIT,
                                              atomic_get_int32(&session->connection_status_reason));

                auto_disconnect = 0;

                /* if there is work to do, reconnect.. */
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_SPEW, 0, "Critical block.");
                critical_block(session->mutex) {
                    if(vector_length(session->requests) > 0) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "There are requests waiting, reopening connection to PLC.");

                        state = SESSION_OPEN_SOCKET_START;
                        nap_interrupt(session->nap);
                    }
                }

                break;


            default:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_ERROR, 0, "Unknown state %d!", state);

                /* FIXME - this logic is not complete.  We might be here without
                 * a connected session or a registered session. */

                atomic_set_int32(&session->connection_status_reason, PLCTAG_ERR_UNSUPPORTED);
                if(session->use_connected_msg) {
                    state = SESSION_DISCONNECT;
                } else {
                    state = SESSION_UNREGISTER;
                }

                nap_interrupt(session->nap);
                break;
        }

        /*
         * give up the CPU a bit, but only if we are not
         * doing some linked states.
         */
        if(wait_until_time > 0) {
            int64_t time_left = wait_until_time - now;

            if(time_left > 0) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Waiting up to %" PRId64 "ms for something to happen.",
                       time_left);
                nap_wait(session->nap, (int)time_left);
            }
        }
    }

    /*
     * One last time before we exit.
     */
    critical_block(session->mutex) { purge_aborted_requests_unsafe(session); }

    /* Decrement the count of active session handlers */
    atomic_add_int32(&session_handlers_active, -1);

    THREAD_RETURN(0);
}


/*
 * This must be called with the session mutex held!
 */
int purge_aborted_requests_unsafe(ab_session_p session) {
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

            request->status = PLCTAG_ERR_ABORT;
            request->request_size = 0;
            request->resp_received = 1;

            /* release our hold on it. */
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "rc_dec: Releasing request reference.");
            rc_dec(request);

            /* vector size has changed, back up one. */
            i--;
        }
    }

    if(purge_count > 0) { pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Removed %d aborted requests.", purge_count); }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_SPEW, 0, "Done.");

    return purge_count;
}


int process_requests(ab_session_p session) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p request = NULL;
    ab_request_p bundled_requests[MAX_REQUESTS] = {NULL};
    int num_bundled_requests = 0;
    int remaining_space = 0;


    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_SPEW, 0, "Starting.");

    if(!session) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Null session pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_SPEW, 0, "Checking for requests to process.");

    rc = PLCTAG_STATUS_OK;
    request = NULL;
    session->data_size = 0;
    session->data_offset = 0;

    /* grab a request off the front of the list. */
    critical_block(session->mutex) {
        // FIXME - no logging in a mutex!
        // pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, "FIXME: available payload space %d", available_payload);

        /* is there anything to do? */
        if(vector_length(session->requests)) {
            /* get rid of all aborted requests. */
            purge_aborted_requests_unsafe(session);

            /* if there are still requests after purging all the aborted requests, process them. */

            /*
             * The total allowed space for requests is the negotiated packet capacity
             * less the overhead of the CPF data item. The rest of the space is for
             * the EIP encapsulation header and the CPF header and the CPF address item,
             * which are already accounted for in the buffer structure.
             */
            remaining_space = session_get_available_cip_payload_space(session);

            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Available payload space is %d bytes.", remaining_space);

            /*
             * The logic below is a bit convoluted.
             *
             * - If the first request takes up all the space, we cannot pack any more requests.
             *
             * - If the first request is packable, we can keep packing requests
             *   until we run out of space or we reach the maximum number of requests.  We need to make sure
             *   that the overhead of the CIP packed request header is accounted for in the remaining space as well as the
             *   two-byte offset entry for each request.
             *
             * - If we are packing requests, and the next one is not packable, we stop packing.
             *
             * - If the first request is not packable, we can only pack it
             *   if it is the first one in the queue. And then can pack no more
             *   requests after that.
             */

            if(vector_length(session->requests)) {
                /* Always process the first request, regardless of packability */
                request = vector_get(session->requests, 0);
                int first_request_size = get_payload_size(request);

                /* Check if the first request fits at all */
                if(first_request_size <= remaining_space) {
                    bundled_requests[num_bundled_requests] = request;
                    num_bundled_requests++;
                    remaining_space -= first_request_size;
                    vector_remove(session->requests, 0);

                    /* If the first request is packable, try to pack more requests */
                    if(request->allow_packing && vector_length(session->requests) > 0) {
                        /* Account for CIP multi-request overhead now that we know we'll have multiple requests */
                        remaining_space -= (int)sizeof(cip_multi_req_header);

                        /* Account for 2-byte offset entry per request (including the first one already processed) */
                        int multi_request_overhead = 2;            /* 2-byte offset entry per additional request */
                        remaining_space -= multi_request_overhead; /* for the first request */

                        while(vector_length(session->requests) > 0 && num_bundled_requests < MAX_REQUESTS) {

                            request = vector_get(session->requests, 0);

                            /* Only pack if this request is packable */
                            if(!request->allow_packing) { break; }

                            int next_request_size = get_payload_size(request) + multi_request_overhead;

                            /* Check if this request fits in remaining space */
                            if(next_request_size > remaining_space) { break; }

                            bundled_requests[num_bundled_requests] = request;
                            num_bundled_requests++;
                            remaining_space -= next_request_size;
                            vector_remove(session->requests, 0);
                        }
                    }
                    /* If first request is not packable, we stop here (only the first request is packed) */
                } else {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                           "First request size %d exceeds remaining space %d, cannot process any requests.", first_request_size,
                           remaining_space);
                }
            } else {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "All requests in queue were aborted, nothing to do.");
            }
        }
    }

    if(num_bundled_requests > 0) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "%d requests to process.", num_bundled_requests);

        do {
            /* copy and pack the requests into the session buffer. */
            /* FIXME - pack_requests() only returns PLCTAG_STATUS_OK */
            rc = pack_requests(session, bundled_requests, num_bundled_requests);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error while packing requests, %s!", plc_tag_decode_error(rc));
                break;
            }

            /* fill in all the necessary parts to the request. */
            if((rc = prepare_request(session)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to prepare request, %s!", plc_tag_decode_error(rc));
                break;
            }

            /* send the request */
            if((rc = send_eip_request(session, SESSION_DEFAULT_TIMEOUT)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error sending packet %s!", plc_tag_decode_error(rc));
                break;
            }

            /* wait for the response */
            if((rc = recv_eip_response(session, SESSION_DEFAULT_TIMEOUT)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error receiving packet response %s!", plc_tag_decode_error(rc));
                break;
            }

            /*
             * check the CIP status, but only if this is a bundled
             * response.   If it is a singleton, then we pass the
             * status back to the tag.
             */
            if(num_bundled_requests > 1) {
                cip_multi_resp_header *multi_resp = NULL;

                if(le2h16(((eip_encap *)(session->data))->encap_command) == EIP_UNCONNECTED_SEND) {
                    eip_cip_uc_resp *resp = (eip_cip_uc_resp *)(session->data);
                    uint16_t udi_item_length = 0;
                    size_t response_overhead = 0;
                    size_t response_size = 0;

                    /* we only know we got an EIP header, so check before reading CPF/CIP fields. */
                    if((size_t)session->data_size < sizeof(*resp)) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                               "Unconnected response of %u bytes is too short to hold a CIP response of %d bytes!",
                               session->data_size, (int)sizeof(*resp));
                        rc = PLCTAG_ERR_TOO_SMALL;
                        break;
                    }

                    udi_item_length = le2h16(resp->cpf_udi_item_length);

                    multi_resp = (cip_multi_resp_header *)(&(resp->reply_service));

                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0,
                           "Received unconnected packet with session sequence ID %" PRIx64 ".",
                           le2h64(resp->encap_sender_context));

                    /* punt if we got an overall error or it is not a partial/bundled error. */
                    if(resp->status != EIP_OK && resp->status != CIP_ERR_PARTIAL_ERROR) {
                        rc = decode_cip_error_code(&(resp->status),
                                                   cip_error_data_size(&resp->status, session->data + session->data_size));
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Command failed! (%d/%d) %s", resp->status, rc,
                               plc_tag_decode_error(rc));
                        break;
                    }

                    response_overhead = (size_t)((uint8_t *)multi_resp - session->data);
                    response_size = (size_t)session->data_size - response_overhead;

                    /* check the passed UDI data item size against what we really got. */
                    if((size_t)udi_item_length != response_size) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                               "Incorrectly constructed response! UDI data length field is %" PRIu64
                               " but actual size is %" PRIu64 "!",
                               (uint64_t)udi_item_length, (uint64_t)response_size);

                        rc = PLCTAG_ERR_BAD_DATA;
                        break;
                    }
                } else if(le2h16(((eip_encap *)(session->data))->encap_command) == EIP_CONNECTED_SEND) {
                    eip_cip_co_resp *resp = (eip_cip_co_resp *)(session->data);
                    uint16_t cdi_item_length = 0;
                    size_t response_overhead = 0;
                    size_t response_size = 0;

                    /* we only know we got an EIP header, so check before reading CPF/CIP fields. */
                    if((size_t)session->data_size < sizeof(*resp)) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                               "Connected response of %u bytes is too short to hold a CIP response of %d bytes!",
                               session->data_size, (int)sizeof(*resp));
                        rc = PLCTAG_ERR_TOO_SMALL;
                        break;
                    }

                    cdi_item_length = le2h16(resp->cpf_cdi_item_length);

                    multi_resp = (cip_multi_resp_header *)(&(resp->reply_service));

                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0,
                           "Received connected packet with connection ID %x and sequence ID %u(%x)",
                           le2h32(resp->cpf_orig_conn_id), le2h16(resp->cpf_conn_seq_num), le2h16(resp->cpf_conn_seq_num));

                    /* punt if we got an overall error or it is not a partial/bundled error. */
                    if(resp->status != EIP_OK && resp->status != CIP_ERR_PARTIAL_ERROR) {
                        size_t status_size = cip_error_data_size(&resp->status, session->data + session->data_size);

                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Response status=%u", resp->status);
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Received CIP error %s (%s).",
                               decode_cip_error_long(&resp->status, status_size),
                               decode_cip_error_short(&resp->status, status_size));
                        rc = decode_cip_error_code(&(resp->status), status_size);
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Command failed! (%d/%d) %s", resp->status, rc,
                               plc_tag_decode_error(rc));
                        break;
                    }

                    response_overhead = (size_t)((uint8_t *)(&resp->cpf_conn_seq_num) - session->data);
                    response_size = (size_t)session->data_size - response_overhead;

                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "response_overhead=%" PRIu64, (uint64_t)response_overhead);
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "response_size=%" PRIu64, (uint64_t)response_size);

                    /* check the passed CDI data item size against what we really got. */
                    if((size_t)cdi_item_length != response_size) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                               "Incorrectly constructed response! CDI data length field is %" PRIu64
                               " but actual size is %" PRIu64 "!",
                               (uint64_t)cdi_item_length, (uint64_t)response_size);

                        rc = PLCTAG_ERR_BAD_DATA;
                        break;
                    }
                } else {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unexpected EIP packet type, %04x!",
                           le2h16(((eip_encap *)(session->data))->encap_command));
                    rc = PLCTAG_ERR_BAD_DATA;
                    break;
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
                    size_t offsets_size = (size_t)num_bundled_requests * sizeof(uint16_le);

                    /* FIXME - session->data_size is uint32_t, so this test is always false.  Check carefully
                     * before removing it: the guard that follows depends on data_size being sane. */
                    if(session->data_size < 0 || offsets_start > (size_t)session->data_size
                       || offsets_size > (size_t)session->data_size - offsets_start) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                               "Response of %d bytes is too short to hold %d packed response offsets!", session->data_size,
                               num_bundled_requests);
                        rc = PLCTAG_ERR_TOO_SMALL;
                        break;
                    }
                }

                /* we have multiple requests, sanity check the data. */
                if(le2h16(multi_resp->request_count) == num_bundled_requests) {
                    size_t offset_base = (size_t)((uint8_t *)(&multi_resp->request_count) - session->data);

                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "offset_base=%" PRIu64, (uint64_t)offset_base);

                    /* check all the offsets */
                    for(int resp_index = 0; resp_index < num_bundled_requests; resp_index++) {
                        size_t resp_offset = (size_t)le2h16(multi_resp->request_offsets[resp_index]) + offset_base;

                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Response %d starts at byte offset %" PRIu64, resp_index,
                               (uint64_t)resp_offset);

                        if(resp_offset >= (size_t)session->data_size) {
                            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                                   "Response %d has offset %" PRIu64 " which is outside the session data!", resp_index,
                                   (uint64_t)resp_offset);
                            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                            break;
                        }
                    }
                } else {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Expected %d packed responses back but got %" PRIu64 "!",
                           num_bundled_requests, (uint64_t)le2h16(multi_resp->request_count));
                    rc = PLCTAG_ERR_BAD_DATA;
                    break;
                }
            }

            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Got error %s when processing incoming response(s)!",
                       plc_tag_decode_error(rc));
                break;
            }

            /* copy the results back out. Every request gets a copy. */
            for(int i = 0; i < num_bundled_requests; i++) {
                rc = unpack_response(session, bundled_requests[i], i);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, bundled_requests[i]->tag_id, "Unable to unpack response!");
                    break;
                }

                /* release our reference */
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, bundled_requests[i]->tag_id,
                       "rc_dec: Releasing request reference.");
                bundled_requests[i] = rc_dec(bundled_requests[i]);
            }

            rc = PLCTAG_STATUS_OK;
        } while(0);

        /* problem? push the requests back on the queue. */
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error sending or receiving requests!");

            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Pushing %d requests back into the queue.", num_bundled_requests);

            /* session->requests is also written by session_add_request() (tickler thread)
             * under session->mutex, so this push-back needs the same lock. */
            critical_block(session->mutex) {
                for(int i = num_bundled_requests - 1; i >= 0; i--) {
                    if(bundled_requests[i]) { vector_insert(session->requests, 0, bundled_requests[i]); }
                }
            }
        }

        /* tickle the main tickler thread to note that we have responses. */
        plc_tag_tickler_wake();
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_SPEW, 0, "Done.");

    return rc;
}


int prepare_request(ab_session_p session) {
    eip_encap *encap = NULL;
    int payload_size = 0;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    encap = (eip_encap *)(session->data);
    payload_size = (int)session->data_size - (int)sizeof(eip_encap);

    /* FIXME - why is this check here? Haven't we checked this up the call chain? */
    if(!session) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Called with null session!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* fill in the fields of the request. */

    encap->encap_length = h2le16((uint16_t)payload_size);
    encap->encap_session_handle = h2le32(session->conn_handle);
    encap->encap_status = h2le32(0);
    encap->encap_options = h2le32(0);

    /* FIXME - support other kinds of requests? */

    /* set up the session sequence ID for this transaction */
    if(le2h16(encap->encap_command) == EIP_UNCONNECTED_SEND) {
        /* get new ID */
        session->conn_seq_id++;

        // request->conn_seq_id = session->conn_seq_id;
        encap->encap_sender_context = h2le64(session->conn_seq_id); /* link up the request seq ID and the packet seq ID */

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Preparing unconnected packet with session sequence ID %" PRIx64,
               session->conn_seq_id);
    } else if(le2h16(encap->encap_command) == EIP_CONNECTED_SEND) {
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


int send_eip_request(ab_session_p session, int timeout) {
    int rc = PLCTAG_STATUS_OK;
    int32_t final_rc = PLCTAG_STATUS_OK;
    int64_t timeout_time = 0;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    if(!session) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Session pointer is null.");
        return PLCTAG_ERR_NULL_PTR;
    }

    session_publish_event(session, TAG_CONN_EVENT_SEND_REQUEST_STARTED, PLCTAG_STATUS_OK, PLCTAG_STATUS_OK);

    if(timeout > 0) {
        timeout_time = time_ms() + timeout;
    } else {
        timeout_time = INT64_MAX;
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Sending packet of size %d", session->data_size);
    pdebug_dump_bytes(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, session->data, (int)(session->data_size));

    session->data_offset = 0;
    session->packet_count++;

    /*
     * Remember what we are asking for.  recv_eip_response() has to be able to tell an answer
     * to this request from an unrelated packet, and the only identity the encapsulation layer
     * gives us is the command and the sender context we echo back.
     */
    if(session->data_size >= sizeof(eip_encap)) {
        eip_encap *out_header = (eip_encap *)(session->data);

        session->req_encap_command = le2h16(out_header->encap_command);
        session->req_seq_id = le2h64(out_header->encap_sender_context);
        session->req_sent = true;
    } else {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Packet of %u bytes is too small to hold an EIP header!",
               session->data_size);
        session_publish_event(session, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, PLCTAG_ERR_TOO_SMALL, PLCTAG_ERR_TOO_SMALL);
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* send the packet */
    do {
        int32_t bytes_written = 0;

        rc = cip_conn_send((cip_conn_p)session, session->data + session->data_offset,
                           (int32_t)(session->data_size - session->data_offset), &bytes_written, SOCKET_WAIT_TIMEOUT_MS);

        if(rc == PLCTAG_STATUS_OK) {
            if(bytes_written == 0) { pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Socket not yet ready to write."); }

            session->data_offset += (uint32_t)bytes_written;
        }

        /* give up the CPU if we still are looping */
        // if(!session->terminating && rc >= 0 && session->data_offset < session->data_size) {
        //     sleep_ms(1);
        // }
    } while(!atomic_get_int32(&session->terminating) && rc == PLCTAG_STATUS_OK && session->data_offset < session->data_size
            && timeout_time > time_ms());

    if(atomic_get_int32(&session->terminating)) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Session is terminating.");
        final_rc = PLCTAG_ERR_ABORT;
        session_publish_event(session, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, final_rc, final_rc);
        return final_rc;
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error, %d, writing socket!", rc);
        final_rc = rc;
        session_publish_event(session, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, final_rc, final_rc);
        return final_rc;
    }

    if(timeout_time <= time_ms()) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Timed out waiting to send data!");
        final_rc = PLCTAG_ERR_TIMEOUT;
        session_publish_event(session, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, final_rc, final_rc);
        return final_rc;
    }

    session_publish_event(session, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, PLCTAG_STATUS_OK, PLCTAG_STATUS_OK);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * recv_eip_response
 *
 * Look at the passed session and read any data we can
 * to fill in a packet.  If we already have a full packet,
 * punt.
 */
int recv_eip_response(ab_session_p session, int timeout) {
    uint32_t data_needed = 0;
    int rc = PLCTAG_STATUS_OK;
    int32_t final_rc = PLCTAG_STATUS_OK;
    int64_t timeout_time = 0;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    if(!session) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Called with null session!");
        return PLCTAG_ERR_NULL_PTR;
    }

    session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_STARTED, PLCTAG_STATUS_OK, PLCTAG_STATUS_OK);


    if(timeout > 0) {
        timeout_time = time_ms() + timeout;
    } else {
        timeout_time = INT64_MAX;
    }

    session->data_offset = 0;
    session->data_size = 0;
    data_needed = sizeof(eip_encap);

    /*
     * Clear the buffer before reading into it.  Response handlers cast this buffer to
     * header structs, and a short response leaves whatever the previous response put
     * there.  Every length check guarding those casts is then the only thing between a
     * truncated packet and a PLC-groomed value being read as a status or connection ID.
     * Zeroing makes that failure mode boring instead of exploitable.
     */
    mem_set(session->data, 0, (int)session->data_capacity);

    do {
        int32_t bytes_read = 0;

        rc = cip_conn_recv((cip_conn_p)session, session->data + session->data_offset,
                           (int32_t)(data_needed - session->data_offset), &bytes_read, SOCKET_WAIT_TIMEOUT_MS);

        if(rc == PLCTAG_STATUS_OK) {
            if(bytes_read == 0) { pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Socket not yet ready to read."); }

            session->data_offset += (uint32_t)bytes_read;

            /*pdebug_dump_bytes(session->debug, session->data, session->data_offset);*/

            /* recalculate the amount of data needed if we have just completed the read of an encap header */
            if(session->data_offset >= sizeof(eip_encap)) {
                data_needed = (uint32_t)(sizeof(eip_encap) + le2h16(((eip_encap *)(session->data))->encap_length));

                if(data_needed > session->data_capacity) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                           "Packet response (%d) is larger than possible buffer size (%d)!", data_needed, session->data_capacity);
                    final_rc = PLCTAG_ERR_TOO_LARGE;
                    session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, final_rc, final_rc);
                    return final_rc;
                }
            }
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error reading socket! rc=%d", rc);
            final_rc = rc;
            session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, final_rc, final_rc);
            return final_rc;
        }
    } while(!atomic_get_int32(&session->terminating) && session->data_offset < data_needed && timeout_time > time_ms());

    if(atomic_get_int32(&session->terminating)) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Session is terminating, returning...");
        final_rc = PLCTAG_ERR_ABORT;
        session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, final_rc, final_rc);
        return final_rc;
    }

    if(timeout_time <= time_ms()) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Timed out waiting for data to read!");
        final_rc = PLCTAG_ERR_TIMEOUT;
        session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, final_rc, final_rc);
        return final_rc;
    }

    session->resp_seq_id = le2h64(((eip_encap *)(session->data))->encap_sender_context);
    session->data_size = data_needed;

    /*
     * Everything downstream of here decides how to parse this buffer from fields the PLC
     * chose.  Before any of that, make sure this packet is actually the answer to the request
     * we sent: the encapsulation layer gives us three things to check and all three are free.
     *
     * The command matters most.  The connected and unconnected CPF headers are different
     * lengths, so a reply that changes the command out from under us moves every field the
     * handlers read, including the CIP status they branch on.
     */
    {
        eip_encap *resp_header = (eip_encap *)(session->data);
        uint16_t resp_command = le2h16(resp_header->encap_command);
        uint32_t resp_handle = le2h32(resp_header->encap_session_handle);

        if(session->req_sent && resp_command != session->req_encap_command) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                   "Received EIP command %04" PRIx16 " in response to command %04" PRIx16 "!", resp_command,
                   session->req_encap_command);
            final_rc = PLCTAG_ERR_BAD_DATA;
            session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, final_rc, final_rc);
            return final_rc;
        }

        /*
         * Once the session is registered every packet carries our handle.  A zero handle means
         * we are still registering, so there is nothing to compare against yet.
         */
        if(session->conn_handle != 0 && resp_handle != session->conn_handle) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                   "Received a response for session handle %" PRIx32 " but this session is %" PRIx32 "!", resp_handle,
                   session->conn_handle);
            final_rc = PLCTAG_ERR_BAD_DATA;
            session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, final_rc, final_rc);
            return final_rc;
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
        if(session->req_sent && resp_command == EIP_UNCONNECTED_SEND && session->resp_seq_id != session->req_seq_id) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                   "Received a response with sender context %" PRIx64 " but we sent %" PRIx64 "!", session->resp_seq_id,
                   session->req_seq_id);
            final_rc = PLCTAG_ERR_BAD_DATA;
            session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, final_rc, final_rc);
            return final_rc;
        }
    }

    rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "request received all needed data (%d bytes of %d).", session->data_offset,
           data_needed);

    pdebug_dump_bytes(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, session->data, (int)(session->data_offset));

    /* check status. */
    if(le2h32(((eip_encap *)(session->data))->encap_status) != EIP_OK) { rc = PLCTAG_ERR_BAD_STATUS; }

    session_publish_event(session, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, rc, rc);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return rc;
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
/* new version of Forward Open */
/*
 * cip_request_destroy
 *
 * The request must be removed from any lists before this!
 */
