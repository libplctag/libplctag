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


#include <platform.h>
#include <ab/ab_common.h>
#include <ab/session.h>
#include <ab/connection.h>
#include <ab/request.h>
#include <ab/defs.h>
#include <ab/eip.h>
#include <util/debug.h>
#include <stdlib.h>
#include <time.h>


static ab_session_p session_create_unsafe(const char* host, int gw_port);
static int session_init(ab_session_p session);
static int add_session_unsafe(ab_session_p n);
//~ static int add_session(ab_session_p s);
static int remove_session_unsafe(ab_session_p n);
//~ static int remove_session(ab_session_p s);
static ab_session_p find_session_by_host_unsafe(const char  *t);
//~ static int session_add_tag_unsafe(ab_session_p session, ab_tag_p tag);
//~ static int session_add_tag(ab_session_p session, ab_tag_p tag);
//~ static int session_remove_tag_unsafe(ab_session_p session, ab_tag_p tag);
//~ static int session_remove_tag(ab_session_p session, ab_tag_p tag);
static int session_connect(ab_session_p session);
//~ static int session_destroy_unsafe(ab_session_p session);
static void session_destroy(void *session);
//~ static int session_is_empty(ab_session_p session);
static int session_register(ab_session_p session);
static int session_unregister_unsafe(ab_session_p session);


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

    //pdebug(DEBUG_DETAIL, "entering critical block %p",global_session_mut);
    critical_block(global_session_mut) {
        res = (uint16_t)session_get_new_seq_id_unsafe(sess);
    }
    //pdebug(DEBUG_DETAIL, "leaving critical block %p", global_session_mut);

    return res;
}


static int connection_is_usable(ab_connection_p connection)
{
    if(!connection) {
        return 0;
    }

    if(connection->exclusive) {
        return 0;
    }

    if(connection->disconnect_in_progress) {
        return 0;
    }

    return 1;
}



ab_connection_p session_find_connection_by_path_unsafe(ab_session_p session,const char *path)
{
    ab_connection_p connection;

    connection = session->connections;

    /*
     * there are a lot of conditions.
     * We do not want to use connections that are in the process of shutting down.
     * We do not want to use connections that are used exclusively by one tag.
     * We want to use connections that have the same path as the tag.
     */

    while(connection) {
        /* scan past any that are in the process of being deleted. */
        while(connection && !rc_inc(connection)) {
            connection = connection->next;
        }

        /* did we walk off the end? */
        if(!connection) {
            break;
        }

        if(connection_is_usable(connection) && str_cmp_i(connection->path, path)==0) {
            /* this is the one! */
            break;
        }

        /* we are not going to use this connection, release it. */
        rc_dec(connection);

        /* try the next one. */
        connection = connection->next;
    }

    /* add to the ref count.  This is safe if connection is null. */
    return connection;
}



int session_add_connection_unsafe(ab_session_p session, ab_connection_p connection)
{
    pdebug(DEBUG_DETAIL, "Starting");

    /* add the connection to the list in the session */
    connection->next = session->connections;
    session->connections = connection;

    pdebug(DEBUG_DETAIL, "Done");

    return PLCTAG_STATUS_OK;
}

int session_add_connection(ab_session_p session, ab_connection_p connection)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting");

    if(session) {
        critical_block(global_session_mut) {
            rc = session_add_connection_unsafe(session, connection);
        }
    } else {
        pdebug(DEBUG_WARN, "Session ptr is null!");
        rc = PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_DETAIL, "Done");

    return rc;
}

/* must have the session mutex held here. */
int session_remove_connection_unsafe(ab_session_p session, ab_connection_p connection)
{
    ab_connection_p cur;
    ab_connection_p prev;
    /* int debug = session->debug; */
    int rc;

    pdebug(DEBUG_DETAIL, "Starting");

    cur = session->connections;
    prev = NULL;

    while (cur && cur != connection) {
        prev = cur;
        cur = cur->next;
    }

    if (cur && cur == connection) {
        if (prev) {
            prev->next = cur->next;
        } else {
            session->connections = cur->next;
        }

        rc = PLCTAG_STATUS_OK;
    } else {
        rc = PLCTAG_ERR_NOT_FOUND;
    }

    pdebug(DEBUG_DETAIL, "Done");

    return rc;
}

int session_remove_connection(ab_session_p session, ab_connection_p connection)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting");

    if(session) {
        critical_block(global_session_mut) {
            rc = session_remove_connection_unsafe(session, connection);
        }
    } else {
        rc = PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_DETAIL, "Done");

    return rc;
}


int session_find_or_create(ab_session_p *tag_session, attr attribs)
{
    /*int debug = attr_get_int(attribs,"debug",0);*/
    const char* session_gw = attr_get_str(attribs, "gateway", "");
    int session_gw_port = attr_get_int(attribs, "gateway_port", AB_EIP_DEFAULT_PORT);
    ab_session_p session = AB_SESSION_NULL;
    int new_session = 0;
    int shared_session = attr_get_int(attribs, "share_session", 1); /* share the session by default. */
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting");

    critical_block(global_session_mut) {
        /* if we are to share sessions, then look for an existing one. */
        if (shared_session) {
            session = find_session_by_host_unsafe(session_gw);
        } else {
            /* no sharing, create a new one */
            session = AB_SESSION_NULL;
        }

        if (session == AB_SESSION_NULL) {
            pdebug(DEBUG_DETAIL,"Creating new session.");
            session = session_create_unsafe(session_gw, session_gw_port);

            if (session == AB_SESSION_NULL) {
                pdebug(DEBUG_WARN, "unable to create or find a session!");
                rc = PLCTAG_ERR_BAD_GATEWAY;
            } else {
                new_session = 1;
            }
        } else {
            pdebug(DEBUG_DETAIL,"Reusing existing session.");
        }
    }

    /*
     * do this OUTSIDE the mutex in order to let other threads not block if
     * the session creation process blocks.
     */

    if(new_session) {
        rc = session_init(session);

        if(rc != PLCTAG_STATUS_OK) {
            /* failed to set up the session! */
            //session_destroy(session);
            rc_dec(session);
            session = AB_SESSION_NULL;
        } else {
            /* save the status */
            session->status = rc;
        }
    }

    /* store it into the tag */
    *tag_session = session;

    pdebug(DEBUG_DETAIL, "Done");

    return rc;
}

int add_session_unsafe(ab_session_p n)
{
    pdebug(DEBUG_DETAIL, "Starting");

    if (!n) {
        return PLCTAG_ERR_NULL_PTR;
    }

    n->prev = NULL;
    n->next = sessions;

    if (sessions) {
        sessions->prev = n;
    }

    sessions = n;

    pdebug(DEBUG_DETAIL, "Done");

    return PLCTAG_STATUS_OK;
}

int add_session(ab_session_p s)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    critical_block(global_session_mut) {
        rc = add_session_unsafe(s);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}

int remove_session_unsafe(ab_session_p n)
{
    ab_session_p tmp;

    pdebug(DEBUG_DETAIL, "Starting");

    if (!n || !sessions) {
        return 0;
    }

    tmp = sessions;

    while (tmp && tmp != n)
        tmp = tmp->next;

    if (!tmp || tmp != n) {
        pdebug(DEBUG_WARN, "Session not found!");
        return PLCTAG_ERR_NOT_FOUND;
    }

    if (n->next) {
        n->next->prev = n->prev;
    }

    if (n->prev) {
        n->prev->next = n->next;
    } else {
        sessions = n->next;
    }

    n->next = NULL;
    n->prev = NULL;

    pdebug(DEBUG_DETAIL, "Done");

    return PLCTAG_STATUS_OK;
}

int remove_session(ab_session_p s)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    critical_block(global_session_mut) {
        rc = remove_session_unsafe(s);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


static int session_match_valid(const char *host, ab_session_p session)
{
    if(!session) {
        return 0;
    }

    if(session->status !=  PLCTAG_STATUS_OK && session->status != PLCTAG_STATUS_PENDING) {
        return 0;
    }

    if(str_cmp_i(host,session->host)) {
        return 0;
    }

    return 1;
}


ab_session_p find_session_by_host_unsafe(const char* host)
{
    ab_session_p session;

    session = sessions;

    while(session) {
        /* scan past any that are in the process of being deleted. */
        while(session && !rc_inc(session)) {
            session = session->next;
        }

        if(!session) {
            break;
        }

        if(session_match_valid(host, session)) {
            /* this is the one! */
            break;
        }

        /* we are not going to use this session, release it. */
        rc_dec(session);

        /* try the next one. */
        session = session->next;
    }

    return session;
}



ab_session_p session_create_unsafe(const char* host, int gw_port)
{
    ab_session_p session = AB_SESSION_NULL;
    static volatile uint32_t srand_setup = 0;
    static volatile uint32_t connection_id = 0;

    pdebug(DEBUG_INFO, "Starting");

    pdebug(DEBUG_DETAIL, "Warning: not using passed port %d", gw_port);

    session = (ab_session_p)rc_alloc(sizeof(struct ab_session_t), session_destroy);

    if (!session) {
        pdebug(DEBUG_WARN, "Error allocating new session.");
        return AB_SESSION_NULL;
    }

    session->recv_capacity = EIP_CIP_PREFIX_SIZE + MAX_CIP_MSG_SIZE_EX;

    str_copy(session->host, MAX_SESSION_HOST, host);

    session->status = PLCTAG_STATUS_PENDING;

    /* check for ID set up */
    if(srand_setup == 0) {
        srand((unsigned int)time_ms());
        srand_setup = 1;
    }

    if(connection_id == 0) {
        connection_id = rand();
    }

    session->session_seq_id =  rand();

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
    session->conn_serial_number = ++connection_id;

    /* set up the packet interval to a reasonable default */
    //session->next_packet_interval_us = SESSION_DEFAULT_PACKET_INTERVAL;

    /* set up packet round trip information */
    for(int index=0; index < SESSION_NUM_ROUND_TRIP_SAMPLES; index++) {
        session->round_trip_samples[index] = SESSION_DEFAULT_RESEND_INTERVAL_MS;
    }
    session->retry_interval = SESSION_DEFAULT_RESEND_INTERVAL_MS;

    /* set up the ref count */
    //session->rc = refcount_init(1, session, session_destroy);

    /* FIXME */
    //pdebug(DEBUG_DETAIL, "Refcount is now %d", session->rc.count);

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

    /* we must connect to the gateway and register */
    if ((rc = session_connect(session)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "session connect failed!");
        session->status = rc;
        return rc;
    }

    if ((rc = session_register(session)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "session registration failed!");
        session->status = rc;
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}

/*
 * session_connect()
 *
 * Connect to the host/port passed via TCP.
 */

int session_connect(ab_session_p session)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /* Open a socket for communication with the gateway. */
    rc = socket_create(&(session->sock));

    if (rc) {
        pdebug(DEBUG_WARN, "Unable to create socket for session!");
        return 0;
    }

    rc = socket_connect_tcp(session->sock, session->host, AB_EIP_DEFAULT_PORT);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to connect socket for session!");
        return rc;
    }

    /* everything is OK.  We have a TCP stream open to a gateway. */
    session->is_connected = 1;

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}

/* must have the session mutex held here */
void session_destroy(void *session_arg)
{
    ab_session_p session = session_arg;
    int really_destroy = 1;

    pdebug(DEBUG_INFO, "Starting.");

    if (!session) {
        pdebug(DEBUG_WARN, "Session ptr is null!");

        return;
    }

    /* do not destroy the session if there are
     * tags or connections still */
     /* removed due to refcount code
      *
    if(!session_is_empty(session)) {
        pdebug(DEBUG_WARN, "Attempt to destroy session while open tags or connections exist!");
        return 0;
    }
    */

    /*
     * Work around the race condition here.  Another thread could have looked up the
     * session before this thread got to this point, but after this thread saw the
     * session's ref count go to zero.  So, we need to check again after preventing
     * other threads from getting a reference.
     */

    critical_block(global_session_mut) {
//        if(refcount_get_count(&session->rc) > 0) {
//            pdebug(DEBUG_WARN,"Another thread got a reference to this session before it could be deleted.  Aborting deletion");
//            really_destroy = 0;
//            break;
//        }

        /* still good, so remove the session from the list so no one else can reference it. */
        remove_session_unsafe(session);
    }

    /*
     * if we are really destroying the session then we know that this is
     * the last reference.  So, we can use the unsafe variants.
     */
    if(really_destroy) {
        ab_request_p req;

        /* unregister and close the socket. */
        session_unregister_unsafe(session);

        /* remove any remaining requests, they are dead */
        req = session->requests;

        while(req) {
            session_remove_request_unsafe(session, req);
            //~ request_destroy_unsafe(&req);
            //request_release(req);
            req = session->requests;
        }

        mem_free(session);
    }

    pdebug(DEBUG_INFO, "Done.");

    return;
}


int session_register(ab_session_p session)
{
    eip_session_reg_req* req;
    eip_encap_t* resp;
    int rc = PLCTAG_STATUS_OK;
    uint32_t data_size = 0;
    int64_t timeout_time;

    pdebug(DEBUG_INFO, "Starting.");

    /*
     * clear the session data.
     *
     * We use the receiving buffer because we do not have a request and nothing can
     * be coming in (we hope) on the socket yet.
     */
    mem_set(session->recv_data, 0, sizeof(eip_session_reg_req));

    req = (eip_session_reg_req*)(session->recv_data);

    /* fill in the fields of the request */
    req->encap_command = h2le16(AB_EIP_REGISTER_SESSION);
    req->encap_length = h2le16(sizeof(eip_session_reg_req) - sizeof(eip_encap_t));
    req->encap_session_handle = session->session_handle;
    req->encap_status = h2le32(0);
    req->encap_sender_context = (uint64_t)0;
    req->encap_options = h2le32(0);

    req->eip_version = h2le16(AB_EIP_VERSION);
    req->option_flags = 0;

    /*
     * socket ops here are _ASYNCHRONOUS_!
     *
     * This is done this way because we do not have everything
     * set up for a request to be handled by the thread.  I think.
     */

    /* send registration to the gateway */
    data_size = sizeof(eip_session_reg_req);
    session->recv_offset = 0;
    timeout_time = time_ms() + SESSION_REGISTRATION_TIMEOUT;

    pdebug(DEBUG_INFO, "sending data:");
    pdebug_dump_bytes(DEBUG_INFO, session->recv_data, data_size);

    while (timeout_time > time_ms() && session->recv_offset < data_size) {
        rc = socket_write(session->sock, session->recv_data + session->recv_offset, data_size - session->recv_offset);

        if (rc < 0) {
            pdebug(DEBUG_WARN, "Unable to send session registration packet! rc=%d", rc);
            session->recv_offset = 0;
            return rc;
        }

        session->recv_offset += rc;

        /* don't hog the CPU */
        if (session->recv_offset < data_size) {
            sleep_ms(1);
        }
    }

    if (session->recv_offset != data_size) {
        session->recv_offset = 0;
        return PLCTAG_ERR_TIMEOUT;
    }

    /* get the response from the gateway */

    /* ready the input buffer */
    session->recv_offset = 0;
    mem_set(session->recv_data, 0, session->recv_capacity);

    timeout_time = time_ms() + SESSION_REGISTRATION_TIMEOUT;

    while (timeout_time > time_ms()) {
        if (session->recv_offset < sizeof(eip_encap_t)) {
            data_size = sizeof(eip_encap_t);
        } else {
            data_size = sizeof(eip_encap_t) + le2h16(((eip_encap_t*)(session->recv_data))->encap_length);
        }

        if (session->recv_offset < data_size) {
            rc = socket_read(session->sock, session->recv_data + session->recv_offset, data_size - session->recv_offset);

            if (rc < 0) {
                if (rc != PLCTAG_ERR_NO_DATA) {
                    /* error! */
                    pdebug(DEBUG_WARN, "Error reading socket! rc=%d", rc);
                    return rc;
                }
            } else {
                session->recv_offset += rc;

                /* recalculate the amount of data needed if we have just completed the read of an encap header */
                if (session->recv_offset >= sizeof(eip_encap_t)) {
                    data_size = sizeof(eip_encap_t) + le2h16(((eip_encap_t*)(session->recv_data))->encap_length);
                }
            }
        }

        /* did we get all the data? */
        if (session->recv_offset == data_size) {
            break;
        } else {
            /* do not hog the CPU */
            sleep_ms(1);
        }
    }

    if (session->recv_offset != data_size) {
        session->recv_offset = 0;
        return PLCTAG_ERR_TIMEOUT;
    }

    /* set the offset back to zero for the next packet */
    session->recv_offset = 0;

    pdebug(DEBUG_INFO, "received response:");
    pdebug_dump_bytes(DEBUG_INFO, session->recv_data, data_size);

    /* encap header is at the start of the buffer */
    resp = (eip_encap_t*)(session->recv_data);

    /* check the response status */
    if (le2h16(resp->encap_command) != AB_EIP_REGISTER_SESSION) {
        pdebug(DEBUG_WARN, "EIP unexpected response packet type: %d!", resp->encap_command);
        return PLCTAG_ERR_BAD_DATA;
    }

    if (le2h16(resp->encap_status) != AB_EIP_OK) {
        pdebug(DEBUG_WARN, "EIP command failed, response code: %d", resp->encap_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /*
     * after all that, save the session handle, we will
     * use it in future packets.
     */
    session->session_handle = resp->encap_session_handle; /* opaque to us */
    session->registered = 1;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}

int session_unregister_unsafe(ab_session_p session)
{
    if (session->sock) {
        session->is_connected = 0;
        session->registered = 0;
        socket_close(session->sock);
        socket_destroy(&(session->sock));
        session->sock = NULL;
    }

    return PLCTAG_STATUS_OK;
}



/*
 * session_add_request_unsafe
 *
 * You must hold the mutex before calling this!
 */
int session_add_request_unsafe(ab_session_p sess, ab_request_p req)
{
    int rc = PLCTAG_STATUS_OK;
    ab_request_p cur, prev;
    int total_requests = 0;

    pdebug(DEBUG_INFO, "Starting.");

    if(!sess) {
        pdebug(DEBUG_WARN, "Session is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* make sure the request points to the session */
    req->session = sess;

    /* we add the request to the end of the list. */
    cur = sess->requests;
    prev = NULL;

    while (cur) {
        prev = cur;
        cur = cur->next;
        total_requests++;
    }

    if (!prev) {
        sess->requests = rc_inc(req);
    } else {
        prev->next = rc_inc(req);
    }

    /* update the request's refcount as we point to it. */
//    rc_inc(req);

    pdebug(DEBUG_INFO,"Total requests in the queue: %d",total_requests);

    pdebug(DEBUG_INFO, "Done.");

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

    pdebug(DEBUG_DETAIL, "Starting. sess=%p, req=%p", sess, req);

    critical_block(global_session_mut) {
        rc = session_add_request_unsafe(sess, req);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


/*
 * session_remove_request_unsafe
 *
 * You must hold the mutex before calling this!
 */
int session_remove_request_unsafe(ab_session_p sess, ab_request_p req)
{
    int rc = PLCTAG_STATUS_OK;
    ab_request_p cur, prev;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(sess == NULL || req == NULL) {
        return rc;
    }

    /* find the request and remove it from the list. */
    cur = sess->requests;
    prev = NULL;

    while (cur && cur != req) {
        prev = cur;
        cur = cur->next;
    }

    if (cur == req) {
        if (!prev) {
            sess->requests = cur->next;
        } else {
            prev->next = cur->next;
        }
    } /* else not found */

    req->next = NULL;
    req->session = NULL;

    /* release the request refcount */
    rc_dec(req);

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



/*
 * session_remove_request
 *
 * This is a thread-safe version of the above routine.
 */
int session_remove_request(ab_session_p sess, ab_request_p req)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.  session=%p, req=%p", sess, req);

    if(sess == NULL || req == NULL) {
        return rc;
    }

    critical_block(global_session_mut) {
        rc = session_remove_request_unsafe(sess, req);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}




//int session_acquire(ab_session_p session)
//{
//    pdebug(DEBUG_INFO, "Acquire session=%p", session);
//
//    if(!session) {
//        return PLCTAG_ERR_NULL_PTR;
//    }
//
//    return refcount_acquire(&session->rc);
//}
//
//
//int session_release(ab_session_p session)
//{
//    pdebug(DEBUG_INFO, "Release session=%p", session);
//
//    if(!session) {
//        return PLCTAG_ERR_NULL_PTR;
//    }
//
//    return refcount_release(&session->rc);
//}
