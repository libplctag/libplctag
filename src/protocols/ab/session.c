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

#include <platform.h>
#include <ab/ab_common.h>
#include <ab/cip.h>
#include <ab/defs.h>
#include <ab/error_codes.h>
#include <ab/session.h>
#include <util/debug.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>

#define MAX_REQUESTS (200)

#define EIP_CIP_PREFIX_SIZE (44) /* bytes of encap header and CFP connected header */

/* WARNING: this must fit within 9 bits! */
#define MAX_CIP_MSG_SIZE        (0x01FF & 508)

/* Warning, this must fit within 16 bits */
#define MAX_CIP_MSG_SIZE_EX     (0xFFFF & 4002)

/* maximum for PCCC embedded within CIP. */
#define MAX_CIP_PLC5_MSG_SIZE (244)
// #define MAX_CIP_SLC_MSG_SIZE (222)
#define MAX_CIP_SLC_MSG_SIZE (244)
#define MAX_CIP_MLGX_MSG_SIZE (244)

/*
 * Number of milliseconds to wait to try to set up the session again
 * after a failure.
 */
#define RETRY_WAIT_MS (5000)

#define SESSION_DISCONNECT_TIMEOUT (5000)

#define SOCKET_WAIT_TIMEOUT_MS (20)
#define SESSION_IDLE_WAIT_TIME (100)



static ab_session_p session_create_unsafe(const char *host, const char *path, plc_type_t plc_type, int *use_connected_msg);
static int session_init(ab_session_p session);
//static int get_plc_type(attr attribs);
static int add_session_unsafe(ab_session_p n);
static int remove_session_unsafe(ab_session_p n);
static ab_session_p find_session_by_host_unsafe(const char *gateway, const char *path);
static int session_match_valid(const char *host, const char *path, ab_session_p session);
static int session_add_request_unsafe(ab_session_p sess, ab_request_p req);
static int session_open_socket(ab_session_p session);
static void session_destroy(void *session);
static int session_register(ab_session_p session);
static int session_close_socket(ab_session_p session);
static int session_unregister(ab_session_p session);
static THREAD_FUNC(session_handler);
static int purge_aborted_requests_unsafe(ab_session_p session);
static int process_requests(ab_session_p session);
//static int check_packing(ab_session_p session, ab_request_p request);
static int get_payload_size(ab_request_p request);
static int pack_requests(ab_session_p session, ab_request_p *requests, int num_requests);
static int prepare_request(ab_session_p session);
static int send_eip_request(ab_session_p session, int timeout);
static int recv_eip_response(ab_session_p session, int timeout);
static int unpack_response(ab_session_p session, ab_request_p request, int sub_packet);
// static int perform_forward_open(ab_session_p session);
static int perform_forward_close(ab_session_p session);
// static int try_forward_open_ex(ab_session_p session, int *max_payload_size_guess);
// static int try_forward_open(ab_session_p session);
// static int send_forward_open_req(ab_session_p session);
// static int send_forward_open_req_ex(ab_session_p session);
// static int recv_forward_open_resp(ab_session_p session, int *max_payload_size_guess);
static int send_forward_close_req(ab_session_p session);
static int recv_forward_close_resp(ab_session_p session);
static int send_forward_open_request(ab_session_p session);
static int send_old_forward_open_request(ab_session_p session);
static int send_extended_forward_open_request(ab_session_p session);
static int receive_forward_open_response(ab_session_p session);
static void request_destroy(void *req_arg);
static int session_request_increase_buffer(ab_request_p request, int new_capacity);


static volatile mutex_p session_mutex = NULL;
static volatile vector_p sessions = NULL;




int session_startup()
{
    int rc = PLCTAG_STATUS_OK;

    if((rc = mutex_create((mutex_p *)&session_mutex)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to create session mutex %s!", plc_tag_decode_error(rc));
        return rc;
    }

    if((sessions = vector_create(25, 5)) == NULL) {
        pdebug(DEBUG_ERROR, "Unable to create session vector!");
        return PLCTAG_ERR_NO_MEM;
    }

    return rc;
}


void session_teardown()
{
    if(sessions) {
        for(int i=0; i < vector_length(sessions); i++) {
            ab_session_p session = vector_get(sessions, i);
            if(session) {
                rc_dec(session);
            }
        }

        vector_destroy(sessions);
        sessions = NULL;
    }


    if(session_mutex) {
        mutex_destroy((mutex_p *)&session_mutex);
        session_mutex = NULL;
    }
}







/*
 * session_get_new_seq_id_unsafe
 *
 * A wrapper to get a new session sequence ID.  Not thread safe.
 *
 * Note that this is dangerous to use in threaded applications
 * because 32-bit processors will not implement a 64-bit
 * integer as an atomic entity.
 */

uint64_t session_get_new_seq_id_unsafe(ab_session_p sess)
{
    return sess->session_seq_id++;
}

/*
 * session_get_new_seq_id
 *
 * A thread-safe function to get a new session sequence ID.
 */

uint64_t session_get_new_seq_id(ab_session_p sess)
{
    uint16_t res = 0;

    //pdebug(DEBUG_DETAIL, "entering critical block %p",session_mutex);
    critical_block(sess->mutex) {
        res = (uint16_t)session_get_new_seq_id_unsafe(sess);
    }
    //pdebug(DEBUG_DETAIL, "leaving critical block %p", session_mutex);

    return res;
}



int session_get_max_payload(ab_session_p session)
{
    int result = 0;

    if(!session) {
        pdebug(DEBUG_WARN, "Called with null session pointer!");
        return 0;
    }

    critical_block(session->mutex) {
        result = session->max_payload_size;
    }

    return result;
}

int session_find_or_create(ab_session_p *tag_session, attr attribs)
{
    /*int debug = attr_get_int(attribs,"debug",0);*/
    const char *session_gw = attr_get_str(attribs, "gateway", "");
    const char *session_path = attr_get_str(attribs, "path", "");
    int use_connected_msg = attr_get_int(attribs, "use_connected_msg", 0);
    //int session_gw_port = attr_get_int(attribs, "gateway_port", AB_EIP_DEFAULT_PORT);
    plc_type_t plc_type = get_plc_type(attribs);
    ab_session_p session = AB_SESSION_NULL;
    int new_session = 0;
    int shared_session = attr_get_int(attribs, "share_session", 1); /* share the session by default. */
    int rc = PLCTAG_STATUS_OK;
    int auto_disconnect_enabled = 0;
    int auto_disconnect_timeout_ms = INT_MAX;

    pdebug(DEBUG_DETAIL, "Starting");

    auto_disconnect_timeout_ms = attr_get_int(attribs, "auto_disconnect_ms", INT_MAX);
    if(auto_disconnect_timeout_ms != INT_MAX) {
        pdebug(DEBUG_DETAIL, "Setting auto-disconnect after %dms.", auto_disconnect_timeout_ms);
        auto_disconnect_enabled = 1;
    }

    // if(plc_type == AB_PLC_PLC5 && str_length(session_path) > 0) {
    //     /* this means it is DH+ */
    //     use_connected_msg = 1;
    //     attr_set_int(attribs, "use_connected_msg", 1);
    // }

    critical_block(session_mutex) {
        /* if we are to share sessions, then look for an existing one. */
        if (shared_session) {
            session = find_session_by_host_unsafe(session_gw, session_path);
        } else {
            /* no sharing, create a new one */
            session = AB_SESSION_NULL;
        }

        if (session == AB_SESSION_NULL) {
            pdebug(DEBUG_DETAIL, "Creating new session.");
            session = session_create_unsafe(session_gw, session_path, plc_type, &use_connected_msg);

            if (session == AB_SESSION_NULL) {
                pdebug(DEBUG_WARN, "unable to create or find a session!");
                rc = PLCTAG_ERR_BAD_GATEWAY;
            } else {
                session->auto_disconnect_enabled = auto_disconnect_enabled;
                session->auto_disconnect_timeout_ms = auto_disconnect_timeout_ms;

                new_session = 1;
            }
        } else {
            /* turn on auto disconnect if we need to. */
            if(!session->auto_disconnect_enabled && auto_disconnect_enabled) {
                session->auto_disconnect_enabled = auto_disconnect_enabled;
            }

            /* disconnect period always goes down. */
            if(session->auto_disconnect_enabled && session->auto_disconnect_timeout_ms > auto_disconnect_timeout_ms) {
                session->auto_disconnect_timeout_ms = auto_disconnect_timeout_ms;
            }

            pdebug(DEBUG_DETAIL, "Reusing existing session.");
        }
    }

    /*
     * do this OUTSIDE the mutex in order to let other threads not block if
     * the session creation process blocks.
     */

    if(new_session) {
        rc = session_init(session);
        if(rc != PLCTAG_STATUS_OK) {
            rc_dec(session);
            session = AB_SESSION_NULL;
        } else {
            /* save the status */
            //session->status = rc;
        }
    }

    /* store it into the tag */
    *tag_session = session;

    pdebug(DEBUG_DETAIL, "Done");

    return rc;
}




int add_session_unsafe(ab_session_p session)
{
    pdebug(DEBUG_DETAIL, "Starting");

    if (!session) {
        return PLCTAG_ERR_NULL_PTR;
    }

    vector_put(sessions, vector_length(sessions), session);

    session->on_list = 1;

    pdebug(DEBUG_DETAIL, "Done");

    return PLCTAG_STATUS_OK;
}



int add_session(ab_session_p s)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    critical_block(session_mutex) {
        rc = add_session_unsafe(s);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}




int remove_session_unsafe(ab_session_p session)
{
    pdebug(DEBUG_DETAIL, "Starting");

    if (!session || !sessions) {
        return 0;
    }

    for(int i=0; i < vector_length(sessions); i++) {
        ab_session_p tmp = vector_get(sessions, i);

        if(tmp == session) {
            vector_remove(sessions, i);
            break;
        }
    }

    pdebug(DEBUG_DETAIL, "Done");

    return PLCTAG_STATUS_OK;
}

int remove_session(ab_session_p s)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(s->on_list) {
        critical_block(session_mutex) {
            rc = remove_session_unsafe(s);
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int session_match_valid(const char *host, const char *path, ab_session_p session)
{
    if(!session) {
        return 0;
    }

    /* don't use sessions that failed immediately. */
    if(session->failed) {
        return 0;
    }

    if(!str_length(host)) {
        pdebug(DEBUG_WARN, "New session host is NULL or zero length!");
        return 0;
    }

    if(!str_length(session->host)) {
        pdebug(DEBUG_WARN, "Session host is NULL or zero length!");
        return 0;
    }

    if(str_cmp_i(host, session->host)) {
        return 0;
    }

    if(str_cmp_i(path, session->path)) {
        return 0;
    }

    return 1;
}


ab_session_p find_session_by_host_unsafe(const char *host, const char *path)
{
    for(int i=0; i < vector_length(sessions); i++) {
        ab_session_p session = vector_get(sessions, i);

        /* is this session in the process of destruction? */
        session = rc_inc(session);
        if(session) {
            if(session_match_valid(host, path, session)) {
                return session;
            }

            rc_dec(session);
        }
    }

    return NULL;
}



ab_session_p session_create_unsafe(const char *host, const char *path, plc_type_t plc_type, int *use_connected_msg)
{
    static volatile uint32_t connection_id = 0;

    int rc = PLCTAG_STATUS_OK;
    ab_session_p session = AB_SESSION_NULL;

    pdebug(DEBUG_INFO, "Starting");

    if(*use_connected_msg) {
        pdebug(DEBUG_DETAIL, "Session should use connected messaging.");
    } else {
        pdebug(DEBUG_DETAIL, "Session should not use connected messaging.");
    }

    session = (ab_session_p)rc_alloc(sizeof(struct ab_session_t), session_destroy);
    if (!session) {
        pdebug(DEBUG_WARN, "Error allocating new session.");
        return AB_SESSION_NULL;
    }

    session->host = str_dup(host);
    if(!session->host) {
        pdebug(DEBUG_WARN, "Unable to duplicate host string!");
        rc_dec(session);
        return NULL;
    }

    rc = cip_encode_path(path, use_connected_msg, plc_type, &session->conn_path, &session->conn_path_size, &session->dhp_dest);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Unable to convert path links strings to binary path!");
        rc_dec(session);
        return NULL;
    }

    if(path && str_length(path)) {
        session->path = str_dup(path);
        if(path && str_length(path) && !session->path) {
            pdebug(DEBUG_WARN, "Unable to duplicate path string!");
            rc_dec(session);
            return NULL;
        }
    }

    session->requests = vector_create(SESSION_MIN_REQUESTS, SESSION_INC_REQUESTS);
    if(!session->requests) {
        pdebug(DEBUG_WARN, "Unable to allocate vector for requests!");
        rc_dec(session);
        return NULL;
    }

    /* check for ID set up. This does not need to be thread safe since we just need a random value. */
    if(connection_id == 0) {
        connection_id = (uint32_t)rand();
    }

    session->plc_type = plc_type;
    session->data_capacity = MAX_PACKET_SIZE_EX;
    session->use_connected_msg = *use_connected_msg;
    session->failed = 0;
    session->conn_serial_number = (uint16_t)(uintptr_t)(intptr_t)rand();

    session->session_seq_id = (uint64_t)rand();

    /* guess the max CIP payload size. */
    switch(plc_type) {
    case AB_PLC_SLC:
        session->max_payload_size = MAX_CIP_SLC_MSG_SIZE;
        break;

    case AB_PLC_MLGX:
        session->max_payload_size = MAX_CIP_MLGX_MSG_SIZE;
        break;

    case AB_PLC_PLC5:
    case AB_PLC_LGX_PCCC:
        session->max_payload_size = MAX_CIP_PLC5_MSG_SIZE;
        break;

    case AB_PLC_LGX:
        session->max_payload_size = MAX_CIP_MSG_SIZE;
        break;

    case AB_PLC_MICRO800:
        session->max_payload_size = MAX_CIP_MSG_SIZE;
        break;

    case AB_PLC_OMRON_NJNX:
        session->max_payload_size = MAX_CIP_MSG_SIZE;
        break;

    default:
        pdebug(DEBUG_WARN, "Unknown protocol/cpu type!");
        rc_dec(session);
        return NULL;
        break;
    }


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

    pdebug(DEBUG_INFO, "Done");

    return session;
}


/*
 * session_init
 *
 * This calls several blocking methods and so must not keep the main mutex
 * locked during them.
 */
int session_init(ab_session_p session)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /* create the session mutex. */
    if((rc = mutex_create(&(session->mutex))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create session mutex!");
        session->failed = 1;
        return rc;
    }

    /* create the session condition variable. */
    if((rc = cond_create(&(session->wait_cond))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create session condition var!");
        session->failed = 1;
        return rc;
    }

    if((rc = thread_create((thread_p *)&(session->handler_thread), session_handler, 32*1024, session)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create session thread!");
        session->failed = 1;
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


/*
 * session_open_socket()
 *
 * Connect to the host/port passed via TCP.
 */

int session_open_socket(ab_session_p session)
{
    int rc = PLCTAG_STATUS_OK;
    char **server_port = NULL;
    int port = 0;

    pdebug(DEBUG_INFO, "Starting.");

    /* Open a socket for communication with the gateway. */
    rc = socket_create(&(session->sock));

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create socket for session!");
        return rc;
    }

    server_port = str_split(session->host, ":");
    if(!server_port) {
        pdebug(DEBUG_WARN, "Unable to split server and port string!");
        return PLCTAG_ERR_BAD_CONFIG;
    }

    if(server_port[0] == NULL) {
        pdebug(DEBUG_WARN, "Server string is malformed or empty!");
        mem_free(server_port);
        return PLCTAG_ERR_BAD_CONFIG;
    }

    if(server_port[1] != NULL) {
        rc = str_to_int(server_port[1], &port);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to extract port number from server string \"%s\"!", session->host);
            mem_free(server_port);
            return PLCTAG_ERR_BAD_CONFIG;
        }

        pdebug(DEBUG_DETAIL, "Using special port %d.", port);
    } else {
        port = AB_EIP_DEFAULT_PORT;

        pdebug(DEBUG_DETAIL, "Using default port %d.", port);
    }

    rc = socket_connect_tcp(session->sock, server_port[0], port);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to connect socket for session!");
        mem_free(server_port);
        return rc;
    }

    if(server_port) {
        mem_free(server_port);
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



int session_register(ab_session_p session)
{
    eip_session_reg_req *req;
    eip_encap *resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

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
        pdebug(DEBUG_WARN, "Error sending session registration request %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* get the response from the gateway */
    rc = recv_eip_response(session, SESSION_DEFAULT_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error receiving session registration response %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* encap header is at the start of the buffer */
    resp = (eip_encap *)(session->data);

    /* check the response status */
    if (le2h16(resp->encap_command) != AB_EIP_REGISTER_SESSION) {
        pdebug(DEBUG_WARN, "EIP unexpected response packet type: %d!", resp->encap_command);
        return PLCTAG_ERR_BAD_DATA;
    }

    if (le2h32(resp->encap_status) != AB_EIP_OK) {
        pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(resp->encap_status));
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /*
     * after all that, save the session handle, we will
     * use it in future packets.
     */
    session->session_handle = le2h32(resp->encap_session_handle);

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int session_unregister(ab_session_p session)
{
    (void)session;

    pdebug(DEBUG_INFO, "Starting.");

    /* nothing to do, perhaps. */

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



int session_close_socket(ab_session_p session)
{
    pdebug(DEBUG_INFO, "Starting.");

    if (session->sock) {
        socket_close(session->sock);
        socket_destroy(&(session->sock));
        session->sock = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



void session_destroy(void *session_arg)
{
    ab_session_p session = session_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if (!session) {
        pdebug(DEBUG_WARN, "Session ptr is null!");

        return;
    }

    /* so remove the session from the list so no one else can reference it. */
    remove_session(session);

    pdebug(DEBUG_INFO, "Session sent %" PRId64 " packets.", session->packet_count);

    /* terminate the session thread first. */
    session->terminating = 1;

    /* signal the condition variable in case it is waiting */
    if(session->wait_cond) {
        cond_signal(session->wait_cond);
    }

    /* get rid of the handler thread. */
    pdebug(DEBUG_DETAIL, "Destroying session thread.");
    if (session->handler_thread) {
        /* this cannot be guarded by the mutex since the session thread also locks it. */
        thread_join(session->handler_thread);

        /* FIXME - is this critical block needed? */
        critical_block(session->mutex) {
            thread_destroy(&(session->handler_thread));
            session->handler_thread = NULL;
        }
    }


    /* this needs to be handled in the mutex to prevent double frees due to queued requests. */
    critical_block(session->mutex) {
        /* close off the connection if is one. This helps the PLC clean up. */
        if (session->targ_connection_id) {
            /*
             * we do not want the internal loop to immediately
             * return, so set the flag like we are not terminating.
             * There is still a timeout that applies.
             */
            session->terminating = 0;
            perform_forward_close(session);
            session->terminating = 1;
        }

        /* try to be nice and un-register the session */
        if (session->session_handle) {
            session_unregister(session);
        }

        if (session->sock) {
            session_close_socket(session);
        }

        /* release all the requests that are in the queue. */
        if (session->requests) {
            for (int i = 0; i < vector_length(session->requests); i++) {
                rc_dec(vector_get(session->requests, i));
            }

            vector_destroy(session->requests);
            session->requests = NULL;
        }
    }

    /* we are done with the condition variable, finally destroy it. */
    pdebug(DEBUG_DETAIL, "Destroying session condition variable.");
    if(session->wait_cond) {
        cond_destroy(&(session->wait_cond));
        session->wait_cond = NULL;
    }

    /* we are done with the mutex, finally destroy it. */
    pdebug(DEBUG_DETAIL, "Destroying session mutex.");
    if(session->mutex) {
        mutex_destroy(&(session->mutex));
        session->mutex = NULL;
    }

    pdebug(DEBUG_DETAIL, "Cleaning up allocated memory for paths and host name.");
    if(session->conn_path) {
        mem_free(session->conn_path);
        session->conn_path = NULL;
    }

    if(session->path) {
        mem_free(session->path);
        session->path = NULL;
    }

    if(session->host) {
        mem_free(session->host);
        session->host = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");

    return;
}




/*
 * session_add_request_unsafe
 *
 * You must hold the mutex before calling this!
 */
int session_add_request_unsafe(ab_session_p session, ab_request_p req)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!session) {
        pdebug(DEBUG_WARN, "Session is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    req = rc_inc(req);

    if(!req) {
        pdebug(DEBUG_WARN, "Request is either null or in the process of being deleted.");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* make sure the request points to the session */

    /* insert into the requests vector */
    vector_put(session->requests, vector_length(session->requests), req);

    pdebug(DEBUG_DETAIL, "Total requests in the queue: %d", vector_length(session->requests));

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}

/*
 * session_add_request
 *
 * This is a thread-safe version of the above routine.
 */
int session_add_request(ab_session_p sess, ab_request_p req)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting. sess=%p, req=%p", sess, req);

    critical_block(sess->mutex) {
        rc = session_add_request_unsafe(sess, req);
    }

    cond_signal(sess->wait_cond);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


/*
 * session_remove_request_unsafe
 *
 * You must hold the mutex before calling this!
 */
int session_remove_request_unsafe(ab_session_p session, ab_request_p req)
{
    int rc = PLCTAG_STATUS_OK;
//    ab_request_p cur, prev;

    pdebug(DEBUG_INFO, "Starting.");

    if(session == NULL || req == NULL) {
        return rc;
    }

    for(int i=0; i < vector_length(session->requests); i++) {
        if(vector_get(session->requests, i) == req) {
            vector_remove(session->requests, i);
            break;
        }
    }

    /* release the request refcount */
    rc_dec(req);

    cond_signal(session->wait_cond);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



/*****************************************************************
 **************** Session handling functions *********************
 ****************************************************************/


typedef enum { SESSION_OPEN_SOCKET, SESSION_REGISTER, SESSION_SEND_FORWARD_OPEN,
               SESSION_RECEIVE_FORWARD_OPEN, SESSION_IDLE, SESSION_DISCONNECT,
               SESSION_UNREGISTER, SESSION_CLOSE_SOCKET, SESSION_START_RETRY,
               SESSION_WAIT_RETRY, SESSION_WAIT_RECONNECT
             } session_state_t;


THREAD_FUNC(session_handler)
{
    ab_session_p session = arg;
    int rc = PLCTAG_STATUS_OK;
    session_state_t state = SESSION_OPEN_SOCKET;
    int64_t timeout_time = 0;
    int64_t wait_until_time = 0;
    int64_t auto_disconnect_time = time_ms() + SESSION_DISCONNECT_TIMEOUT;
    int auto_disconnect = 0;


    pdebug(DEBUG_INFO, "Starting thread for session %p", session);

    while(!session->terminating) {
        /* how long should we wait if nothing wakes us? */
        wait_until_time = time_ms() + SESSION_IDLE_WAIT_TIME;

        /*
         * Do this on every cycle.   This keeps the queue clean(ish).
         *
         * Make sure we get rid of all the aborted requests queued.
         * This keeps the overall memory usage lower.
         */

        pdebug(DEBUG_SPEW,"Critical block.");
        critical_block(session->mutex) {
            purge_aborted_requests_unsafe(session);
        }

        switch(state) {
        case SESSION_OPEN_SOCKET:
            pdebug(DEBUG_DETAIL, "in SESSION_OPEN_SOCKET state.");

            /* we must connect to the gateway*/
            if ((rc = session_open_socket(session)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "session connect failed %s!", plc_tag_decode_error(rc));
                state = SESSION_CLOSE_SOCKET;
            } else {
                /* set the timeout for disconnect. */
                //if(session->auto_disconnect_enabled) {
                auto_disconnect_time = time_ms() + SESSION_DISCONNECT_TIMEOUT;
                //}

                state = SESSION_REGISTER;
            }
            cond_signal(session->wait_cond);
            break;

        case SESSION_REGISTER:
            pdebug(DEBUG_DETAIL, "in SESSION_REGISTER state.");

            if ((rc = session_register(session)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "session registration failed %s!", plc_tag_decode_error(rc));
                state = SESSION_CLOSE_SOCKET;
            } else {
                if(session->use_connected_msg) {
                    state = SESSION_SEND_FORWARD_OPEN;
                } else {
                    state = SESSION_IDLE;
                }
            }
            cond_signal(session->wait_cond);
            break;

        case SESSION_SEND_FORWARD_OPEN:
            pdebug(DEBUG_DETAIL, "in SESSION_SEND_FORWARD_OPEN state.");

            if((rc = send_forward_open_request(session)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Send Forward Open failed %s!", plc_tag_decode_error(rc));
                state = SESSION_UNREGISTER;
            } else {
                pdebug(DEBUG_DETAIL, "Send Forward Open succeeded, going to SESSION_RECEIVE_FORWARD_OPEN state.");
                state = SESSION_RECEIVE_FORWARD_OPEN;
            }
            cond_signal(session->wait_cond);
            break;

        case SESSION_RECEIVE_FORWARD_OPEN:
            pdebug(DEBUG_DETAIL, "in SESSION_RECEIVE_FORWARD_OPEN state.");

            if((rc = receive_forward_open_response(session)) != PLCTAG_STATUS_OK) {
                if(rc == PLCTAG_ERR_DUPLICATE) {
                    pdebug(DEBUG_DETAIL, "Duplicate connection error received, trying again with different connection ID.");
                    state = SESSION_SEND_FORWARD_OPEN;
                } else if(rc == PLCTAG_ERR_TOO_LARGE) {
                    pdebug(DEBUG_DETAIL, "Requested packet size too large, retrying with smaller size.");
                    state = SESSION_SEND_FORWARD_OPEN;
                } else if(rc == PLCTAG_ERR_UNSUPPORTED && !session->only_use_old_forward_open) {
                    /* if we got an unsupported error and we are trying with ForwardOpenEx, then try the old command. */
                    pdebug(DEBUG_DETAIL, "PLC does not support ForwardOpenEx, trying old ForwardOpen.");
                    session->only_use_old_forward_open = 1;
                    state = SESSION_SEND_FORWARD_OPEN;
                } else {
                    pdebug(DEBUG_WARN, "Receive Forward Open failed %s!", plc_tag_decode_error(rc));
                    state = SESSION_UNREGISTER;
                }
            } else {
                pdebug(DEBUG_DETAIL, "Send Forward Open succeeded, going to SESSION_IDLE state.");
                state = SESSION_IDLE;
            }
            cond_signal(session->wait_cond);
            break;

        case SESSION_IDLE:
            pdebug(DEBUG_DETAIL, "in SESSION_IDLE state.");

            /* if there is work to do, make sure we do not disconnect. */
            pdebug(DEBUG_DETAIL,"Critical block.");
            critical_block(session->mutex) {
                if(vector_length(session->requests) > 0) {
                    auto_disconnect_time = time_ms() + SESSION_DISCONNECT_TIMEOUT;
                }
            }

            if((rc = process_requests(session)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error while processing requests %s!", plc_tag_decode_error(rc));
                if(session->use_connected_msg) {
                    state = SESSION_DISCONNECT;
                } else {
                    state = SESSION_UNREGISTER;
                }
                cond_signal(session->wait_cond);
            }

            /* check if we should disconnect */
            if(auto_disconnect_time < time_ms()) {
                pdebug(DEBUG_DETAIL, "Disconnecting due to inactivity.");

                auto_disconnect = 1;

                if(session->use_connected_msg) {
                    state = SESSION_DISCONNECT;
                } else {
                    state = SESSION_UNREGISTER;
                }
                cond_signal(session->wait_cond);
            }

            break;

        case SESSION_DISCONNECT:
            pdebug(DEBUG_DETAIL, "in SESSION_DISCONNECT state.");

            if((rc = perform_forward_close(session)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Forward close failed %s!", plc_tag_decode_error(rc));
            }

            state = SESSION_UNREGISTER;
            cond_signal(session->wait_cond);
            break;

        case SESSION_UNREGISTER:
            pdebug(DEBUG_DETAIL, "in SESSION_UNREGISTER state.");

            if((rc = session_unregister(session)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unregistering session failed %s!", plc_tag_decode_error(rc));
            }

            state = SESSION_CLOSE_SOCKET;
            cond_signal(session->wait_cond);
            break;

        case SESSION_CLOSE_SOCKET:
            pdebug(DEBUG_DETAIL, "in SESSION_CLOSE_SOCKET state.");

            if((rc = session_close_socket(session)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Closing session socket failed %s!", plc_tag_decode_error(rc));
            }

            if(auto_disconnect) {
                state = SESSION_WAIT_RECONNECT;
            } else {
                state = SESSION_START_RETRY;
            }
            cond_signal(session->wait_cond);
            break;

        case SESSION_START_RETRY:
            pdebug(DEBUG_DETAIL, "in SESSION_START_RETRY state.");

            /* FIXME - make this a tag attribute. */
            timeout_time = time_ms() + RETRY_WAIT_MS;

            /* start waiting. */
            state = SESSION_WAIT_RETRY;

            cond_signal(session->wait_cond);
            break;

        case SESSION_WAIT_RETRY:
            pdebug(DEBUG_DETAIL, "in SESSION_WAIT_RETRY state.");

            if(timeout_time < time_ms()) {
                pdebug(DEBUG_DETAIL, "Transitioning to SESSION_OPEN_SOCKET.");
                state = SESSION_OPEN_SOCKET;
                cond_signal(session->wait_cond);
            }

            break;

        case SESSION_WAIT_RECONNECT:
            /* wait for at least one request to queue before reconnecting. */
            pdebug(DEBUG_DETAIL, "in SESSION_WAIT_RECONNECT state.");

            auto_disconnect = 0;

            /* if there is work to do, reconnect.. */
            pdebug(DEBUG_SPEW,"Critical block.");
            critical_block(session->mutex) {
                if(vector_length(session->requests) > 0) {
                    pdebug(DEBUG_DETAIL, "There are requests waiting, reopening connection to PLC.");

                    state = SESSION_OPEN_SOCKET;
                    cond_signal(session->wait_cond);
                }
            }

            break;


        default:
            pdebug(DEBUG_ERROR, "Unknown state %d!", state);

            /* FIXME - this logic is not complete.  We might be here without
             * a connected session or a registered session. */
            if(session->use_connected_msg) {
                state = SESSION_DISCONNECT;
            } else {
                state = SESSION_UNREGISTER;
            }

            cond_signal(session->wait_cond);
            break;
        }

        /*
         * give up the CPU a bit, but only if we are not
         * doing some linked states.
         */
        if(wait_until_time > 0) {
            int64_t time_left = wait_until_time - time_ms();

            if(time_left > 0) {
                cond_wait(session->wait_cond, (int)time_left);
            }
        }
    }

    /*
     * One last time before we exit.
     */
    pdebug(DEBUG_DETAIL,"Critical block.");
    critical_block(session->mutex) {
        purge_aborted_requests_unsafe(session);
    }

    THREAD_RETURN(0);
}



/*
 * This must be called with the session mutex held!
 */
int purge_aborted_requests_unsafe(ab_session_p session)
{
    int purge_count = 0;
    ab_request_p request = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    /* remove the aborted requests. */
    for(int i=0; i < vector_length(session->requests); i++) {
        request = vector_get(session->requests, i);

        /* filter out the aborts. */
        if(request && request->abort_request) {
            purge_count++;

            /* remove it from the queue. */
            vector_remove(session->requests, i);

            /* set the debug tag to the owning tag. */
            debug_set_tag_id(request->tag_id);

            pdebug(DEBUG_DETAIL, "Session thread releasing aborted request %p.", request);

            request->status = PLCTAG_ERR_ABORT;
            request->request_size = 0;
            request->resp_received = 1;

            /* release our hold on it. */
            request = rc_dec(request);

            /* vector size has changed, back up one. */
            i--;
        }
    }

    if(purge_count > 0) {
        pdebug(DEBUG_DETAIL, "Removed %d aborted requests.", purge_count);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return purge_count;
}


int process_requests(ab_session_p session)
{
    int rc = PLCTAG_STATUS_OK;
    ab_request_p request = NULL;
    ab_request_p bundled_requests[MAX_REQUESTS] = {NULL};
    int num_bundled_requests = 0;
    int remaining_space = 0;

    debug_set_tag_id(0);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!session) {
        pdebug(DEBUG_WARN, "Null session pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_SPEW, "Checking for requests to process.");

    rc = PLCTAG_STATUS_OK;
    request = NULL;
    session->data_size = 0;
    session->data_offset = 0;

    /* grab a request off the front of the list. */
    critical_block(session->mutex) {
        /* is there anything to do? */
        if(vector_length(session->requests)) {
            /* get rid of all aborted requests. */
            purge_aborted_requests_unsafe(session);

            /* if there are still requests after purging all the aborted requests, process them. */

            /* how much space do we have to work with. */
            remaining_space = session->max_payload_size - (int)sizeof(cip_multi_req_header);

            if(vector_length(session->requests)) {
                do {
                    request = vector_get(session->requests, 0);

                    remaining_space = remaining_space - get_payload_size(request);

                    /*
                     * If we have a non-packable request, only queue it if it is the first one.
                     * If the request is packable, keep queuing as long as there is space.
                     */

                    if(num_bundled_requests == 0 || (request->allow_packing && remaining_space > 0)) {
                        //pdebug(DEBUG_DETAIL, "packed %d requests with remaining space %d", num_bundled_requests+1, remaining_space);
                        bundled_requests[num_bundled_requests] = request;
                        num_bundled_requests++;

                        /* remove it from the queue. */
                        vector_remove(session->requests, 0);
                    }
                } while(vector_length(session->requests) && remaining_space > 0 && num_bundled_requests < MAX_REQUESTS && request->allow_packing);
            } else {
                pdebug(DEBUG_DETAIL, "All requests in queue were aborted, nothing to do.");
            }
        }
    }

    /* output debug display as no particular tag. */
    debug_set_tag_id(0);

    if(num_bundled_requests > 0) {

        pdebug(DEBUG_INFO, "%d requests to process.", num_bundled_requests);

        do {
            /* copy and pack the requests into the session buffer. */
            rc = pack_requests(session, bundled_requests, num_bundled_requests);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error while packing requests, %s!", plc_tag_decode_error(rc));
                break;
            }

            /* fill in all the necessary parts to the request. */
            if((rc = prepare_request(session)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to prepare request, %s!", plc_tag_decode_error(rc));
                break;
            }

            /* send the request */
            if((rc = send_eip_request(session, SESSION_DEFAULT_TIMEOUT)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error sending packet %s!", plc_tag_decode_error(rc));
                break;
            }

            /* wait for the response */
            if((rc = recv_eip_response(session, SESSION_DEFAULT_TIMEOUT)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error receiving packet response %s!", plc_tag_decode_error(rc));
                break;
            }

            /*
             * check the CIP status, but only if this is a bundled
             * response.   If it is a singleton, then we pass the
             * status back to the tag.
             */
            if(num_bundled_requests > 1) {
                if(le2h16(((eip_encap *)(session->data))->encap_command) == AB_EIP_UNCONNECTED_SEND) {
                    eip_cip_uc_resp *resp = (eip_cip_uc_resp *)(session->data);
                    pdebug(DEBUG_INFO, "Received unconnected packet with session sequence ID %llx", resp->encap_sender_context);

                    /* punt if we got an overall error or it is not a partial/bundled error. */
                    if(resp->status != AB_EIP_OK && resp->status != AB_CIP_ERR_PARTIAL_ERROR) {
                        rc = decode_cip_error_code(&(resp->status));
                        pdebug(DEBUG_WARN, "Command failed! (%d/%d) %s", resp->status, rc, plc_tag_decode_error(rc));
                        break;
                    }
                } else if(le2h16(((eip_encap *)(session->data))->encap_command) == AB_EIP_CONNECTED_SEND) {
                    eip_cip_co_resp *resp = (eip_cip_co_resp *)(session->data);
                    pdebug(DEBUG_INFO, "Received connected packet with connection ID %x and sequence ID %u(%x)", le2h32(resp->cpf_orig_conn_id), le2h16(resp->cpf_conn_seq_num), le2h16(resp->cpf_conn_seq_num));

                    /* punt if we got an overall error or it is not a partial/bundled error. */
                    if(resp->status != AB_EIP_OK && resp->status != AB_CIP_ERR_PARTIAL_ERROR) {
                        rc = decode_cip_error_code(&(resp->status));
                        pdebug(DEBUG_WARN, "Command failed! (%d/%d) %s", resp->status, rc, plc_tag_decode_error(rc));
                        break;
                    }
                }
            }

            /* copy the results back out. Every request gets a copy. */
            for(int i=0; i < num_bundled_requests; i++) {
                debug_set_tag_id(bundled_requests[i]->tag_id);

                rc = unpack_response(session, bundled_requests[i], i);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Unable to unpack response!");
                    break;
                }

                /* release our reference */
                bundled_requests[i] = rc_dec(bundled_requests[i]);
            }

            rc = PLCTAG_STATUS_OK;
        } while(0);

        /* problem? clean up the pending requests and dump everything. */
        if(rc != PLCTAG_STATUS_OK) {
            for(int i=0; i < num_bundled_requests; i++) {
                if(bundled_requests[i]) {
                    bundled_requests[i]->status = rc;
                    bundled_requests[i]->request_size = 0;
                    bundled_requests[i]->resp_received = 1;

                    bundled_requests[i] = rc_dec(bundled_requests[i]);
                }
            }
        }

        /* tickle the main tickler thread to note that we have responses. */
        plc_tag_tickler_wake();
    }

    debug_set_tag_id(0);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


int unpack_response(ab_session_p session, ab_request_p request, int sub_packet)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp *packed_resp = (eip_cip_co_resp *)(session->data);
    eip_cip_co_resp *unpacked_resp = NULL;
    uint8_t *pkt_start = NULL;
    uint8_t *pkt_end = NULL;
    int new_eip_len = 0;

    pdebug(DEBUG_INFO, "Starting.");

    /* clear out the request data. */
    mem_set(request->data, 0, request->request_capacity);

    /* change what we do depending on the type. */
    if(packed_resp->reply_service != (AB_EIP_CMD_CIP_MULTI | AB_EIP_CMD_CIP_OK)) {
        /* copy the data back into the request buffer. */
        new_eip_len = (int)session->data_size;
        pdebug(DEBUG_INFO, "Got single response packet.  Copying %d bytes unchanged.", new_eip_len);

        if(new_eip_len > request->request_capacity) {
            int request_capacity = 0;

            pdebug(DEBUG_INFO, "Request buffer too small, allocating larger buffer.");

            critical_block(session->mutex) {
                request_capacity = (int)(session->max_payload_size + EIP_CIP_PREFIX_SIZE);
            }

            /* make sure it will fit. */
            if(new_eip_len > request_capacity) {
                pdebug(DEBUG_WARN, "something is very wrong, packet length is %d but allowable capacity is %d!", new_eip_len, request_capacity);
                return PLCTAG_ERR_TOO_LARGE;
            }

            rc = session_request_increase_buffer(request, request_capacity);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to increase request buffer size to %d bytes!", request_capacity);
                return rc;
            }
        }

        mem_copy(request->data, session->data, new_eip_len);
    } else {
        cip_multi_resp_header *multi = (cip_multi_resp_header *)(&packed_resp->reply_service);
        uint16_t total_responses = le2h16(multi->request_count);
        int pkt_len = 0;

        /* this is a packed response. */
        pdebug(DEBUG_INFO, "Got multiple response packet, subpacket %d", sub_packet);

        pdebug(DEBUG_INFO, "Our result offset is %d bytes.", (int)le2h16(multi->request_offsets[sub_packet]));

        pkt_start = ((uint8_t *)(&multi->request_count) + le2h16(multi->request_offsets[sub_packet]));

        /* calculate the end of the data. */
        if((sub_packet + 1) < total_responses) {
            /* not the last response */
            pkt_end = (uint8_t *)(&multi->request_count) + le2h16(multi->request_offsets[sub_packet + 1]);
        } else {
            pkt_end = (session->data + le2h16(packed_resp->encap_length) + sizeof(eip_encap));
        }

        pkt_len = (int)(pkt_end - pkt_start);

        /* replace the request buffer if it is not big enough. */
        new_eip_len = pkt_len + (int)sizeof(eip_cip_co_generic_response);
        if(new_eip_len > request->request_capacity) {
            int request_capacity = 0;

            pdebug(DEBUG_INFO, "Request buffer too small, allocating larger buffer.");

            critical_block(session->mutex) {
                request_capacity = (int)(session->max_payload_size + EIP_CIP_PREFIX_SIZE);
            }

            /* make sure it will fit. */
            if(new_eip_len > request_capacity) {
                pdebug(DEBUG_WARN, "something is very wrong, packet length is %d but allowable capacity is %d!", new_eip_len, request_capacity);
                return PLCTAG_ERR_TOO_LARGE;
            }

            rc = session_request_increase_buffer(request, request_capacity);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to increase request buffer size to %d bytes!", request_capacity);
                return rc;
            }
        }

        /* point to the response buffer in a structured way. */
        unpacked_resp = (eip_cip_co_resp *)(request->data);

        /* copy the header down */
        mem_copy(request->data, session->data, (int)sizeof(eip_cip_co_resp));

        /* size of the new packet */
        new_eip_len = (uint16_t)(((uint8_t *)(&unpacked_resp->reply_service) + pkt_len) /* end of the packet */
                                 - (uint8_t *)(request->data));                         /* start of the packet */

        /* now copy the packet over that. */
        mem_copy(&unpacked_resp->reply_service, pkt_start, pkt_len);

        /* stitch up the packet sizes. */
        unpacked_resp->cpf_cdi_item_length = h2le16((uint16_t)(pkt_len + (int)sizeof(uint16_le))); /* extra for the connection sequence */
        unpacked_resp->encap_length = h2le16((uint16_t)(new_eip_len - (uint16_t)sizeof(eip_encap)));
    }

    pdebug(DEBUG_INFO, "Unpacked packet:");
    pdebug_dump_bytes(DEBUG_INFO, request->data, new_eip_len);

    /* notify the reading thread that the request is ready */
    spin_block(&request->lock) {
        request->status = PLCTAG_STATUS_OK;
        request->request_size = new_eip_len;
        request->resp_received = 1;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}



int get_payload_size(ab_request_p request)
{
    int request_data_size = 0;
    eip_encap *header = (eip_encap *)(request->data);
    eip_cip_co_req *co_req = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(le2h16(header->encap_command) == AB_EIP_CONNECTED_SEND) {
        co_req = (eip_cip_co_req *)(request->data);
        /* get length of new request */
        request_data_size = le2h16(co_req->cpf_cdi_item_length)
                            - 2  /* for connection sequence ID */
                            + 2  /* for multipacket offset */
                            ;
    } else {
        pdebug(DEBUG_DETAIL, "Not a supported type EIP packet type %d to get the payload size.", le2h16(header->encap_command));
        request_data_size = INT_MAX;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return request_data_size;
}




int pack_requests(ab_session_p session, ab_request_p *requests, int num_requests)
{
    eip_cip_co_req *new_req = NULL;
    eip_cip_co_req *packed_req = NULL;
    /* FIXME - is this the right way to check? */
    int header_size = 0;
    cip_multi_req_header *multi_header = NULL;
    int current_offset = 0;
    uint8_t *pkt_start = NULL;
    int pkt_len = 0;
    uint8_t *first_pkt_data = NULL;
    uint8_t *next_pkt_data = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    debug_set_tag_id(requests[0]->tag_id);

    /* get the header info from the first request. Just copy the whole thing. */
    mem_copy(session->data, requests[0]->data, requests[0]->request_size);
    session->data_size = (uint32_t)requests[0]->request_size;

    /* special case the case where there is just one request. */
    if(num_requests == 1) {
        pdebug(DEBUG_INFO, "Only one request, so done.");

        debug_set_tag_id(0);

        return PLCTAG_STATUS_OK;
    }

    /* set up multi-packet header. */

    header_size = (int)(sizeof(cip_multi_req_header)
                        + (sizeof(uint16_le) * (size_t)num_requests)); /* offsets for each request. */

    pdebug(DEBUG_INFO, "header size %d", header_size);

    packed_req = (eip_cip_co_req *)(session->data);

    /* make room in the request packet in the session for the header. */
    pkt_start = (uint8_t *)(&packed_req->cpf_conn_seq_num) + sizeof(packed_req->cpf_conn_seq_num);
    pkt_len = (int)le2h16(packed_req->cpf_cdi_item_length) - (int)sizeof(packed_req->cpf_conn_seq_num);

    pdebug(DEBUG_INFO, "packet 0 is of length %d.", pkt_len);

    /* point to where we want the current packet to start. */
    first_pkt_data = pkt_start + header_size;

    /* move the data over to make room */
    mem_move(first_pkt_data, pkt_start, pkt_len);

    /* now fill in the header. Use pkt_start as it is pointing to the right location. */
    multi_header = (cip_multi_req_header *)pkt_start;
    multi_header->service_code = AB_EIP_CMD_CIP_MULTI;
    multi_header->req_path_size = 0x02; /* length of path in words */
    multi_header->req_path[0] = 0x20; /* Class */
    multi_header->req_path[1] = 0x02; /* CM */
    multi_header->req_path[2] = 0x24; /* Instance */
    multi_header->req_path[3] = 0x01; /* #1 */
    multi_header->request_count = h2le16((uint16_t)num_requests);

    /* set up the offset for the first request. */
    current_offset = (int)(sizeof(uint16_le) + (sizeof(uint16_le) * (size_t)num_requests));
    multi_header->request_offsets[0] = h2le16((uint16_t)current_offset);

    next_pkt_data = first_pkt_data + pkt_len;
    current_offset = current_offset + pkt_len;

    /* now process the rest of the requests. */
    for(int i=1; i<num_requests; i++) {
        debug_set_tag_id(requests[i]->tag_id);

        /* set up the offset */
        multi_header->request_offsets[i] = h2le16((uint16_t)current_offset);

        /* get a pointer to the request. */
        new_req = (eip_cip_co_req *)(requests[i]->data);

        /* calculate the request start and length */
        pkt_start = (uint8_t *)(&new_req->cpf_conn_seq_num) + sizeof(new_req->cpf_conn_seq_num);
        pkt_len = (int)le2h16(new_req->cpf_cdi_item_length) - (int)sizeof(new_req->cpf_conn_seq_num);

        pdebug(DEBUG_INFO, "packet %d is of length %d.", i, pkt_len);

        /* copy the request into the session buffer. */
        mem_copy(next_pkt_data, pkt_start, pkt_len);

        /* calculate the next packet info. */
        next_pkt_data += pkt_len;
        current_offset += pkt_len;
    }

    /* stitch up the CPF packet length */
    packed_req->cpf_cdi_item_length = h2le16((uint16_t)(next_pkt_data - (uint8_t *)(&packed_req->cpf_conn_seq_num)));

    /* stick up the EIP packet length */
    packed_req->encap_length = h2le16((uint16_t)((size_t)(next_pkt_data - session->data) - sizeof(eip_encap)));

    /* set the total data size */
    session->data_size = (uint32_t)(next_pkt_data - session->data);

    debug_set_tag_id(0);

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



int prepare_request(ab_session_p session)
{
    eip_encap *encap = NULL;
    int payload_size = 0;

    pdebug(DEBUG_INFO, "Starting.");

    encap = (eip_encap *)(session->data);
    payload_size = (int)session->data_size - (int)sizeof(eip_encap);

    if(!session) {
        pdebug(DEBUG_WARN, "Called with null session!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* fill in the fields of the request. */

    encap->encap_length = h2le16((uint16_t)payload_size);
    encap->encap_session_handle = h2le32(session->session_handle);
    encap->encap_status = h2le32(0);
    encap->encap_options = h2le32(0);

    /* set up the session sequence ID for this transaction */
    if(le2h16(encap->encap_command) == AB_EIP_UNCONNECTED_SEND) {
        /* get new ID */
        session->session_seq_id++;

        //request->session_seq_id = session->session_seq_id;
        encap->encap_sender_context = h2le64(session->session_seq_id); /* link up the request seq ID and the packet seq ID */

        pdebug(DEBUG_INFO, "Preparing unconnected packet with session sequence ID %llx", session->session_seq_id);
    } else if(le2h16(encap->encap_command) == AB_EIP_CONNECTED_SEND) {
        eip_cip_co_req *conn_req = (eip_cip_co_req *)(session->data);

        pdebug(DEBUG_DETAIL, "cpf_targ_conn_id=%x", session->targ_connection_id);

        /* set up the connection information */
        conn_req->cpf_targ_conn_id = h2le32(session->targ_connection_id);

        session->conn_seq_num++;
        conn_req->cpf_conn_seq_num = h2le16(session->conn_seq_num);

        pdebug(DEBUG_INFO, "Preparing connected packet with connection ID %x and sequence ID %u(%x)", session->orig_connection_id, session->conn_seq_num, session->conn_seq_num);
    } else {
        pdebug(DEBUG_WARN, "Unsupported packet type %x!", le2h16(encap->encap_command));
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* display the data */
    pdebug(DEBUG_INFO, "Prepared packet of size %d", session->data_size);
    pdebug_dump_bytes(DEBUG_INFO, session->data, (int)session->data_size);

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}




int send_eip_request(ab_session_p session, int timeout)
{
    int rc = PLCTAG_STATUS_OK;
    int64_t timeout_time = 0;

    pdebug(DEBUG_INFO, "Starting.");

    if(!session) {
        pdebug(DEBUG_WARN, "Session pointer is null.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(timeout > 0) {
        timeout_time = time_ms() + timeout;
    } else {
        timeout_time = INT64_MAX;
    }

    pdebug(DEBUG_INFO, "Sending packet of size %d", session->data_size);
    pdebug_dump_bytes(DEBUG_INFO, session->data, (int)(session->data_size));

    session->data_offset = 0;
    session->packet_count++;

    /* send the packet */
    do {
        rc = socket_write(session->sock,
                          session->data + session->data_offset,
                          (int)session->data_size - (int)session->data_offset,
                          SOCKET_WAIT_TIMEOUT_MS);

        if(rc >= 0) {
            session->data_offset += (uint32_t)rc;
        } else {
            if(rc == PLCTAG_ERR_TIMEOUT) {
                pdebug(DEBUG_DETAIL, "Socket not yet ready to write.");
                rc = 0;
            }
        }

        /* give up the CPU if we still are looping */
        // if(!session->terminating && rc >= 0 && session->data_offset < session->data_size) {
        //     sleep_ms(1);
        // }
    } while(!session->terminating && rc >= 0 && session->data_offset < session->data_size && timeout_time > time_ms());

    if(session->terminating) {
        pdebug(DEBUG_WARN, "Session is terminating.");
        return PLCTAG_ERR_ABORT;
    }

    if(rc < 0) {
        pdebug(DEBUG_WARN, "Error, %d, writing socket!", rc);
        return rc;
    }

    if(timeout_time <= time_ms()) {
        pdebug(DEBUG_WARN, "Timed out waiting to send data!");
        return PLCTAG_ERR_TIMEOUT;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



/*
 * recv_eip_response
 *
 * Look at the passed session and read any data we can
 * to fill in a packet.  If we already have a full packet,
 * punt.
 */
int recv_eip_response(ab_session_p session, int timeout)
{
    uint32_t data_needed = 0;
    int rc = PLCTAG_STATUS_OK;
    int64_t timeout_time = 0;

    pdebug(DEBUG_INFO, "Starting.");

    if(!session) {
        pdebug(DEBUG_WARN, "Called with null session!");
        return PLCTAG_ERR_NULL_PTR;
    }


    if(timeout > 0) {
        timeout_time = time_ms() + timeout;
    } else {
        timeout_time = INT64_MAX;
    }

    session->data_offset = 0;
    session->data_size = 0;
    data_needed = sizeof(eip_encap);

    do {
        rc = socket_read(session->sock,
                         session->data + session->data_offset,
                         (int)(data_needed - session->data_offset),
                         SOCKET_WAIT_TIMEOUT_MS);

        if(rc >= 0) {
            session->data_offset += (uint32_t)rc;

            /*pdebug_dump_bytes(session->debug, session->data, session->data_offset);*/

            /* recalculate the amount of data needed if we have just completed the read of an encap header */
            if(session->data_offset >= sizeof(eip_encap)) {
                data_needed = (uint32_t)(sizeof(eip_encap) + le2h16(((eip_encap *)(session->data))->encap_length));

                if(data_needed > session->data_capacity) {
                    pdebug(DEBUG_WARN, "Packet response (%d) is larger than possible buffer size (%d)!", data_needed, session->data_capacity);
                    return PLCTAG_ERR_TOO_LARGE;
                }
            }
        } else {
            if(rc == PLCTAG_ERR_TIMEOUT) {
                pdebug(DEBUG_DETAIL, "Socket not yet ready to read.");
                rc = 0;
            } else {
                /* error! */
                pdebug(DEBUG_WARN, "Error reading socket! rc=%d", rc);
                return rc;
            }
        }

        // /* did we get all the data? */
        // if(!session->terminating && session->data_offset < data_needed) {
        //     /* do not hog the CPU */
        //     sleep_ms(1);
        // }
    } while(!session->terminating && session->data_offset < data_needed && timeout_time > time_ms());

    if(session->terminating) {
        pdebug(DEBUG_INFO, "Session is terminating, returning...");
        return PLCTAG_ERR_ABORT;
    }

    if(timeout_time <= time_ms()) {
        pdebug(DEBUG_WARN, "Timed out waiting for data to read!");
        return PLCTAG_ERR_TIMEOUT;
    }

    session->resp_seq_id = le2h64(((eip_encap *)(session->data))->encap_sender_context);
    session->data_size = data_needed;

    rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "request received all needed data (%d bytes of %d).", session->data_offset, data_needed);

    pdebug_dump_bytes(DEBUG_INFO, session->data, (int)(session->data_offset));

    /* check status. */
    if(le2h32(((eip_encap *)(session->data))->encap_status) != AB_EIP_OK) {
        rc = PLCTAG_ERR_BAD_STATUS;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



int perform_forward_close(ab_session_p session)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    do {
        rc = send_forward_close_req(session);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Sending forward close failed, %s!", plc_tag_decode_error(rc));
            break;
        }

        rc = recv_forward_close_resp(session);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Forward close response not received, %s!", plc_tag_decode_error(rc));
            break;
        }
    } while(0);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



int send_forward_open_request(ab_session_p session)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    /* if this is the first time we are called set up a default guess size. */
    if(!session->max_payload_guess) {
        if(session->plc_type == AB_PLC_LGX && session->use_connected_msg && !session->only_use_old_forward_open) {
            session->max_payload_guess = MAX_CIP_MSG_SIZE_EX;
        } else {
            session->max_payload_guess = session->max_payload_size;
        }

        pdebug(DEBUG_DETAIL, "Setting initial payload size guess to %u.", (unsigned int)session->max_payload_guess);
    }

    /*
     * double check the message size.   If we just transitioned to using the old ForwardOpen command
     * then the maximum payload guess might be too large.
     */
    if(session->plc_type == AB_PLC_LGX && session->only_use_old_forward_open && session->max_payload_guess > MAX_CIP_MSG_SIZE) {
        session->max_payload_guess = MAX_CIP_MSG_SIZE;
    }

    if(session->only_use_old_forward_open) {
        rc = send_old_forward_open_request(session);
    } else {
        rc = send_extended_forward_open_request(session);
    }

    pdebug(DEBUG_INFO, "Done");

    return rc;
}


int send_old_forward_open_request(ab_session_p session)
{
    eip_forward_open_request_t *fo = NULL;
    uint8_t *data;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

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
    fo->encap_length = h2le16((uint16_t)(data - (uint8_t *)(&fo->interface_handle))); /* total length of packet except for encap header */
    fo->encap_session_handle = h2le32(session->session_handle);
    fo->encap_sender_context = h2le64(++session->session_seq_id);
    fo->router_timeout = h2le16(1);                       /* one second is enough ? */

    /* CPF parts */
    fo->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    fo->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI); /* null address item type */
    fo->cpf_nai_item_length = h2le16(0);             /* no data, zero length */
    fo->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fo->cpf_udi_item_length = h2le16((uint16_t)(data - (uint8_t *)(&fo->cm_service_code))); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fo->cm_service_code = AB_EIP_CMD_FORWARD_OPEN; /* 0x54 Forward Open Request or 0x5B for Forward Open Extended */
    fo->cm_req_path_size = 2;                      /* size of path in 16-bit words */
    fo->cm_req_path[0] = 0x20;                     /* class */
    fo->cm_req_path[1] = 0x06;                     /* CM class */
    fo->cm_req_path[2] = 0x24;                     /* instance */
    fo->cm_req_path[3] = 0x01;                     /* instance 1 */

    /* Forward Open Params */
    fo->secs_per_tick = AB_EIP_SECS_PER_TICK;         /* seconds per tick, no used? */
    fo->timeout_ticks = AB_EIP_TIMEOUT_TICKS;         /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
    fo->orig_to_targ_conn_id = h2le32(0);             /* is this right?  Our connection id on the other machines? */
    fo->targ_to_orig_conn_id = h2le32(session->orig_connection_id); /* Our connection id in the other direction. */
    /* this might need to be globally unique */
    fo->conn_serial_number = h2le16(++(session->conn_serial_number)); /* our connection SEQUENCE number. */
    fo->orig_vendor_id = h2le16(AB_EIP_VENDOR_ID);               /* our unique :-) vendor ID */
    fo->orig_serial_number = h2le32(AB_EIP_VENDOR_SN);           /* our serial number. */
    fo->conn_timeout_multiplier = AB_EIP_TIMEOUT_MULTIPLIER;     /* timeout = mult * RPI */

    fo->orig_to_targ_rpi = h2le32(AB_EIP_RPI); /* us to target RPI - Request Packet Interval in microseconds */

    /* screwy logic if this is a DH+ route! */
    if((session->plc_type == AB_PLC_PLC5 || session->plc_type == AB_PLC_SLC || session->plc_type == AB_PLC_MLGX) && session->dhp_dest != 0) {
        fo->orig_to_targ_conn_params = h2le16(AB_EIP_PLC5_PARAM);
    } else {
        fo->orig_to_targ_conn_params = h2le16(AB_EIP_CONN_PARAM | session->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    }

    fo->targ_to_orig_rpi = h2le32(AB_EIP_RPI); /* target to us RPI - not really used for explicit messages? */

    /* screwy logic if this is a DH+ route! */
    if((session->plc_type == AB_PLC_PLC5 || session->plc_type == AB_PLC_SLC || session->plc_type == AB_PLC_MLGX) && session->dhp_dest != 0) {
        fo->targ_to_orig_conn_params = h2le16(AB_EIP_PLC5_PARAM);
    } else {
        fo->targ_to_orig_conn_params = h2le16(AB_EIP_CONN_PARAM | session->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    }

    fo->transport_class = AB_EIP_TRANSPORT_CLASS_T3; /* 0xA3, server transport, class 3, application trigger */
    fo->path_size = session->conn_path_size/2; /* size in 16-bit words */

    /* set the size of the request */
    session->data_size = (uint32_t)(data - (session->data));

    rc = send_eip_request(session, 0);

    pdebug(DEBUG_INFO, "Done");

    return rc;
}


/* new version of Forward Open */
int send_extended_forward_open_request(ab_session_p session)
{
    eip_forward_open_request_ex_t *fo = NULL;
    uint8_t *data;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    mem_set(session->data, 0, (int)(sizeof(*fo) + session->conn_path_size));

    fo = (eip_forward_open_request_ex_t *)(session->data);

    /* point to the end of the struct */
    data = (session->data) + sizeof(eip_forward_open_request_ex_t);

    /* set up the path information. */
    mem_copy(data, session->conn_path, session->conn_path_size);
    data += session->conn_path_size;

    /* fill in the static parts */

    /* encap header parts */
    fo->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND); /* 0x006F EIP Send RR Data command */
    fo->encap_length = h2le16((uint16_t)(data - (uint8_t *)(&fo->interface_handle))); /* total length of packet except for encap header */
    fo->encap_session_handle = h2le32(session->session_handle);
    fo->encap_sender_context = h2le64(++session->session_seq_id);
    fo->router_timeout = h2le16(1);                       /* one second is enough ? */

    /* CPF parts */
    fo->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    fo->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI); /* null address item type */
    fo->cpf_nai_item_length = h2le16(0);             /* no data, zero length */
    fo->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fo->cpf_udi_item_length = h2le16((uint16_t)(data - (uint8_t *)(&fo->cm_service_code))); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fo->cm_service_code = AB_EIP_CMD_FORWARD_OPEN_EX; /* 0x54 Forward Open Request or 0x5B for Forward Open Extended */
    fo->cm_req_path_size = 2;                      /* size of path in 16-bit words */
    fo->cm_req_path[0] = 0x20;                     /* class */
    fo->cm_req_path[1] = 0x06;                     /* CM class */
    fo->cm_req_path[2] = 0x24;                     /* instance */
    fo->cm_req_path[3] = 0x01;                     /* instance 1 */

    /* Forward Open Params */
    fo->secs_per_tick = AB_EIP_SECS_PER_TICK;         /* seconds per tick, no used? */
    fo->timeout_ticks = AB_EIP_TIMEOUT_TICKS;         /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
    fo->orig_to_targ_conn_id = h2le32(0);             /* is this right?  Our connection id on the other machines? */
    fo->targ_to_orig_conn_id = h2le32(session->orig_connection_id); /* Our connection id in the other direction. */
    /* this might need to be globally unique */
    fo->conn_serial_number = h2le16(++(session->conn_serial_number)); /* our connection ID/serial number. */
    fo->orig_vendor_id = h2le16(AB_EIP_VENDOR_ID);               /* our unique :-) vendor ID */
    fo->orig_serial_number = h2le32(AB_EIP_VENDOR_SN);           /* our serial number. */
    fo->conn_timeout_multiplier = AB_EIP_TIMEOUT_MULTIPLIER;     /* timeout = mult * RPI */
    fo->orig_to_targ_rpi = h2le32(AB_EIP_RPI); /* us to target RPI - Request Packet Interval in microseconds */
    fo->orig_to_targ_conn_params_ex = h2le32(AB_EIP_CONN_PARAM_EX | session->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    fo->targ_to_orig_rpi = h2le32(AB_EIP_RPI); /* target to us RPI - not really used for explicit messages? */
    fo->targ_to_orig_conn_params_ex = h2le32(AB_EIP_CONN_PARAM_EX | session->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    fo->transport_class = AB_EIP_TRANSPORT_CLASS_T3; /* 0xA3, server transport, class 3, application trigger */
    fo->path_size = session->conn_path_size/2; /* size in 16-bit words */

    /* set the size of the request */
    session->data_size = (uint32_t)(data - (session->data));

    rc = send_eip_request(session, SESSION_DEFAULT_TIMEOUT);

    pdebug(DEBUG_INFO, "Done");

    return rc;
}




int receive_forward_open_response(ab_session_p session)
{
    eip_forward_open_response_t *fo_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    rc = recv_eip_response(session, 0);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to receive Forward Open response.");
        return rc;
    }

    fo_resp = (eip_forward_open_response_t *)(session->data);

    do {
        if(le2h16(fo_resp->encap_command) != AB_EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", fo_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(fo_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", fo_resp->encap_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(fo_resp->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "Forward Open command failed, response code: %s (%d)", decode_cip_error_short(&fo_resp->general_status), fo_resp->general_status);
            if(fo_resp->general_status == AB_CIP_ERR_UNSUPPORTED_SERVICE) {
                /* this type of command is not supported! */
                pdebug(DEBUG_WARN, "Received CIP command unsupported error from the PLC!");
                rc = PLCTAG_ERR_UNSUPPORTED;
            } else {
                rc = PLCTAG_ERR_REMOTE_ERR;

                if(fo_resp->general_status == 0x01 && fo_resp->status_size >= 2) {
                    /* we might have an error that tells us the actual size to use. */
                    uint8_t *data = &fo_resp->status_size;
                    int extended_status = data[1] | (data[2] << 8);
                    uint16_t supported_size = (uint16_t)((uint16_t)data[3] | (uint16_t)((uint16_t)data[4] << (uint16_t)8));

                    if(extended_status == 0x109) { /* MAGIC */
                        pdebug(DEBUG_WARN, "Error from forward open request, unsupported size, but size %d is supported.", supported_size);
                        session->max_payload_guess = supported_size;
                        rc = PLCTAG_ERR_TOO_LARGE;
                    } else if(extended_status == 0x100) { /* MAGIC */
                        pdebug(DEBUG_WARN, "Error from forward open request, duplicate connection ID.  Need to try again.");
                        rc = PLCTAG_ERR_DUPLICATE;
                    } else {
                        pdebug(DEBUG_WARN, "CIP extended error %s (%s)!", decode_cip_error_short(&fo_resp->general_status), decode_cip_error_long(&fo_resp->general_status));
                    }
                } else {
                    pdebug(DEBUG_WARN, "CIP error code %s (%s)!", decode_cip_error_short(&fo_resp->general_status), decode_cip_error_long(&fo_resp->general_status));
                }
            }

            break;
        }

        /* success! */
        session->targ_connection_id = le2h32(fo_resp->orig_to_targ_conn_id);
        session->orig_connection_id = le2h32(fo_resp->targ_to_orig_conn_id);

        session->max_payload_size = session->max_payload_guess;

        pdebug(DEBUG_INFO, "ForwardOpen succeeded with our connection ID %x and the PLC connection ID %x with packet size %u.", session->orig_connection_id, session->targ_connection_id, session->max_payload_size);

        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


int send_forward_close_req(ab_session_p session)
{
    eip_forward_close_req_t *fc;
    uint8_t *data;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    fc = (eip_forward_close_req_t *)(session->data);

    /* point to the end of the struct */
    data = (session->data) + sizeof(*fc);

    /* set up the path information. */
    mem_copy(data, session->conn_path, session->conn_path_size);
    data += session->conn_path_size;

    /* FIXME DEBUG */
    pdebug(DEBUG_DETAIL, "Forward Close connection path:");
    pdebug_dump_bytes(DEBUG_DETAIL, session->conn_path, session->conn_path_size);

    /* fill in the static parts */

    /* encap header parts */
    fc->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND); /* 0x006F EIP Send RR Data command */
    fc->encap_length = h2le16((uint16_t)(data - (uint8_t *)(&fc->interface_handle))); /* total length of packet except for encap header */
    fc->encap_sender_context = h2le64(++session->session_seq_id);
    fc->router_timeout = h2le16(1);                       /* one second is enough ? */

    /* CPF parts */
    fc->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    fc->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI); /* null address item type */
    fc->cpf_nai_item_length = h2le16(0);             /* no data, zero length */
    fc->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fc->cpf_udi_item_length = h2le16((uint16_t)(data - (uint8_t *)(&fc->cm_service_code))); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fc->cm_service_code = AB_EIP_CMD_FORWARD_CLOSE;/* 0x4E Forward Close Request */
    fc->cm_req_path_size = 2;                      /* size of path in 16-bit words */
    fc->cm_req_path[0] = 0x20;                     /* class */
    fc->cm_req_path[1] = 0x06;                     /* CM class */
    fc->cm_req_path[2] = 0x24;                     /* instance */
    fc->cm_req_path[3] = 0x01;                     /* instance 1 */

    /* Forward Open Params */
    fc->secs_per_tick = AB_EIP_SECS_PER_TICK;         /* seconds per tick, no used? */
    fc->timeout_ticks = AB_EIP_TIMEOUT_TICKS;         /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
    fc->conn_serial_number = h2le16(session->conn_serial_number); /* our connection SEQUENCE number. */
    fc->orig_vendor_id = h2le16(AB_EIP_VENDOR_ID);               /* our unique :-) vendor ID */
    fc->orig_serial_number = h2le32(AB_EIP_VENDOR_SN);           /* our serial number. */
    fc->path_size = session->conn_path_size/2; /* size in 16-bit words */
    fc->reserved = (uint8_t)0; /* padding for the path. */

    /* set the size of the request */
    session->data_size = (uint32_t)(data - (session->data));

    rc = send_eip_request(session, 100);

    pdebug(DEBUG_INFO, "Done");

    return rc;
}


int recv_forward_close_resp(ab_session_p session)
{
    eip_forward_close_resp_t *fo_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    rc = recv_eip_response(session, 150);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to receive Forward Close response, %s!", plc_tag_decode_error(rc));
        return rc;
    }

    fo_resp = (eip_forward_close_resp_t *)(session->data);

    do {
        if(le2h16(fo_resp->encap_command) != AB_EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", fo_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(fo_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", fo_resp->encap_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(fo_resp->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "Forward Close command failed, response code: %d", fo_resp->general_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        pdebug(DEBUG_INFO, "Connection close succeeded.");

        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}



int session_create_request(ab_session_p session, int tag_id, ab_request_p *req)
{
    int rc = PLCTAG_STATUS_OK;
    ab_request_p res;
    size_t request_capacity = 0;
    uint8_t *buffer = NULL;

    critical_block(session->mutex) {
        request_capacity = (size_t)(session->max_payload_size + EIP_CIP_PREFIX_SIZE);
    }

    pdebug(DEBUG_DETAIL, "Starting.");

    buffer = (uint8_t *)mem_alloc((int)request_capacity);
    if(!buffer) {
        pdebug(DEBUG_WARN, "Unable to allocate request buffer!");
        *req = NULL;
        return PLCTAG_ERR_NO_MEM;
    }

    res = (ab_request_p)rc_alloc((int)sizeof(struct ab_request_t), request_destroy);
    if (!res) {
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

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}





/*
 * request_destroy
 *
 * The request must be removed from any lists before this!
 */

void request_destroy(void *req_arg)
{
    ab_request_p req = req_arg;

    pdebug(DEBUG_DETAIL, "Starting.");

    req->abort_request = 1;

    if(req->data) {
        mem_free(req->data);
        req->data = NULL;
    }

    pdebug(DEBUG_DETAIL, "Done.");
}


int session_request_increase_buffer(ab_request_p request, int new_capacity)
{
    uint8_t *old_buffer = NULL;
    uint8_t *new_buffer = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    new_buffer = (uint8_t *)mem_alloc(new_capacity);
    if(!new_buffer) {
        pdebug(DEBUG_WARN, "Unable to allocate larger request buffer!");
        return PLCTAG_ERR_NO_MEM;
    }

    spin_block(&request->lock) {
        old_buffer = request->data;
        request->request_capacity = new_capacity;
        request->data = new_buffer;
    }

    mem_free(old_buffer);

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}
