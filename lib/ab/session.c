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
#include <ab/eip.h>

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

    pdebug(sess->debug, "entering critical block %p",global_session_mut);
    critical_block(global_session_mut) {
        res = (uint16_t)session_get_new_seq_id_unsafe(sess);
    }
    pdebug(sess->debug, "leaving critical block %p", global_session_mut);

    return res;
}

ab_connection_p session_find_connection_by_path_unsafe(ab_session_p session,const char *path)
{
    ab_connection_p connection;

    connection = session->connections;

    while (connection && str_cmp_i(connection->path, path) != 0) {
        connection = connection->next;
    }

    return connection;
}



int session_add_connection_unsafe(ab_session_p session, ab_connection_p connection)
{
    pdebug(session->debug, "Starting");

    /* add the connection to the list in the session */
    connection->next = session->connections;
    session->connections = connection;

    pdebug(session->debug, "Done");

    return PLCTAG_STATUS_OK;
}

int session_add_connection(ab_session_p session, ab_connection_p connection)
{
    int rc = PLCTAG_STATUS_OK;

    if(session) {
        pdebug(session->debug, "Starting");
        pdebug(session->debug,"entering critical block %p",global_session_mut);
        critical_block(global_session_mut) {
            rc = session_add_connection_unsafe(session, connection);
        }
        pdebug(session->debug,"leaving critical block %p", global_session_mut);
        pdebug(session->debug, "Done");
    } else {
        rc = PLCTAG_ERR_NULL_PTR;
    }

    return rc;
}

/* must have the session mutex held here. */
int session_remove_connection_unsafe(ab_session_p session, ab_connection_p connection)
{
    ab_connection_p cur;
    ab_connection_p prev;
    int debug = session->debug;
    int rc;

    pdebug(debug, "Starting");

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

        if (session_is_empty(session)) {
            pdebug(debug, "destroying session");
            session_destroy_unsafe(session);
        }

        rc = PLCTAG_STATUS_OK;
    } else {
        rc = PLCTAG_ERR_NOT_FOUND;
    }

    pdebug(debug, "Done");

    return rc;
}

int session_remove_connection(ab_session_p session, ab_connection_p connection)
{
    int rc = PLCTAG_STATUS_OK;

    if(session) {
        pdebug(session->debug, "Starting");
        pdebug(session->debug,"entering critical block %p", global_session_mut);
        critical_block(global_session_mut) {
            rc = session_remove_connection_unsafe(session, connection);
        }
        pdebug(session->debug,"leaving critical block %p", global_session_mut);
        pdebug(session->debug, "Done");
    } else {
        rc = PLCTAG_ERR_NULL_PTR;
    }

    return rc;
}


int find_or_create_session(ab_session_p *tag_session, attr attribs)
{
    int debug = attr_get_int(attribs,"debug",0);
    const char* session_gw = attr_get_str(attribs, "gateway", "");
    int session_gw_port = attr_get_int(attribs, "gateway_port", AB_EIP_DEFAULT_PORT);
    ab_session_p session = AB_SESSION_NULL;
    int shared_session = attr_get_int(attribs, "share_session", 1); /* share the session by default. */
    int rc = PLCTAG_STATUS_OK;

    pdebug(debug, "Starting");

    pdebug(debug,"entering critical block %p", global_session_mut);
    critical_block(global_session_mut) {
        /* if we are to share sessions, then look for an existing one. */
        if (shared_session) {
            session = find_session_by_host_unsafe(session_gw);
        } else {
            /* no sharing, create a new one */
            session = AB_SESSION_NULL;
        }

        if (session == AB_SESSION_NULL) {
            pdebug(debug,"Creating new session.");
            session = session_create_unsafe(debug, session_gw, session_gw_port);

            if (session == AB_SESSION_NULL) {
                pdebug(debug, "unable to create or find a session!");
                rc = PLCTAG_ERR_BAD_GATEWAY;
            }
        } else {
            pdebug(debug,"Reusing existing session.");
        }
    }
    pdebug(debug, "leaving critical block %p", global_session_mut);

    /* store it into the tag */
    *tag_session = session;

    pdebug(debug, "Done");

    return rc;
}

int add_session_unsafe(ab_session_p n)
{
    if (!n)
        return PLCTAG_ERR_NULL_PTR;

    pdebug(n->debug, "Starting");

    n->prev = NULL;
    n->next = sessions;

    if (sessions) {
        sessions->prev = n;
    }

    sessions = n;

    pdebug(n->debug, "Done");

    return PLCTAG_STATUS_OK;
}

int add_session(ab_session_p s)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(s->debug, "entering critical block %p", global_session_mut);
    critical_block(global_session_mut) {
        rc = add_session_unsafe(s);
    }
    pdebug(s->debug, "leaving critical block %p", global_session_mut);

    return rc;
}

int remove_session_unsafe(ab_session_p n)
{
    ab_session_p tmp;

    if (!n || !sessions)
        return 0;

    pdebug(n->debug, "Starting");

    tmp = sessions;

    while (tmp && tmp != n)
        tmp = tmp->next;

    if (!tmp || tmp != n) {
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

    pdebug(n->debug, "Done");

    return PLCTAG_STATUS_OK;
}

int remove_session(ab_session_p s)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(s->debug, "entering critical block %p", global_session_mut);
    critical_block(global_session_mut) {
        rc = remove_session_unsafe(s);
    }
    pdebug(s->debug, "leaving critical block %p", global_session_mut);

    return rc;
}

ab_session_p find_session_by_host_unsafe(const char* t)
{
    ab_session_p tmp;

    tmp = sessions;

    while (tmp && str_cmp_i(tmp->host, t)) {
        tmp = tmp->next;
    }

    if (!tmp) {
        return (ab_session_p)NULL;
    }

    return tmp;
}

/* not threadsafe */
int session_add_tag_unsafe(ab_session_p session, ab_tag_p tag)
{
    pdebug(session->debug, "Starting");

    tag->next = session->tags;
    session->tags = tag;

    pdebug(session->debug, "Done");

    return PLCTAG_STATUS_OK;
}

int session_add_tag(ab_session_p session, ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    if(session) {
        pdebug(session->debug, "entering critical block %p", global_session_mut);
        critical_block(global_session_mut) {
            rc = session_add_tag_unsafe(session, tag);
        }
        pdebug(session->debug, "leaving critical block %p", global_session_mut);
    } else {
        rc = PLCTAG_ERR_NULL_PTR;
    }

    return rc;
}

/* not threadsafe */
int session_remove_tag_unsafe(ab_session_p session, ab_tag_p tag)
{
    ab_tag_p tmp, prev;
    int debug = session->debug;

    pdebug(debug, "Starting");

    tmp = session->tags;
    prev = NULL;

    while (tmp && tmp != tag) {
        prev = tmp;
        tmp = tmp->next;
    }

    if (tmp) {
        if (!prev) {
            session->tags = tmp->next;
        } else {
            prev->next = tmp->next;
        }
    }

    /* if the session is empty, get rid of it. */
    if(session_is_empty(session)) {
        session_destroy_unsafe(session);
    }

    pdebug(debug, "Done");

    return PLCTAG_STATUS_OK;
}

int session_remove_tag(ab_session_p session, ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    if(session) {
        int debug = session->debug;
        pdebug(debug, "entering critical block %p", global_session_mut);
        critical_block(global_session_mut) {
            rc = session_remove_tag_unsafe(session, tag);
        }
        pdebug(debug, "leaving critical block %p", global_session_mut);
    } else {
        rc = PLCTAG_ERR_NULL_PTR;
    }

    return rc;
}

ab_session_p session_create_unsafe(int debug, const char* host, int gw_port)
{
    ab_session_p session = AB_SESSION_NULL;

    pdebug(debug, "Starting");

    session = (ab_session_p)mem_alloc(sizeof(struct ab_session_t));

    if (!session) {
        pdebug(debug, "Error allocating new session.");
        return AB_SESSION_NULL;
    }

    session->debug = debug;

    str_copy(session->host, host, MAX_SESSION_HOST);

    /* we must connect to the gateway and register */
    if (!session_connect(session, host)) {
        mem_free(session);
        pdebug(debug, "session connect failed!");
        return AB_SESSION_NULL;
    }

    if (!session_register(session)) {
        session_destroy_unsafe(session);
        pdebug(debug, "session registration failed!");
        return AB_SESSION_NULL;
    }

    /*
     * We assume that we are running in a threaded environment,
     * so every session will have a different address.
     *
     * FIXME - is this needed?
     */
    session->session_seq_id = (uint64_t)(intptr_t)(session);
    session->conn_serial_number = (uint32_t)(intptr_t)(session) + (uint32_t)42; /* MAGIC */

    /* add the new session to the list. */
    add_session_unsafe(session);

    pdebug(debug, "Done.");

    return session;
}

/*
 * ab_session_connect()
 *
 * Connect to the host/port passed via TCP.  Set all the relevant fields in
 * the passed session object.
 */

int session_connect(ab_session_p session, const char* host)
{
    int rc;
    int debug = session->debug;

    pdebug(debug, "Starting.");

    /* Open a socket for communication with the gateway. */
    rc = socket_create(&(session->sock));

    if (rc) {
        pdebug(debug, "Unable to create socket for session!");
        return 0;
    }

    rc = socket_connect_tcp(session->sock, host, AB_EIP_DEFAULT_PORT);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(debug, "Unable to connect socket for session!");
        return 0;
    }

    /* everything is OK.  We have a TCP stream open to a gateway. */
    session->is_connected = 1;

    pdebug(debug, "Done.");

    return 1;
}

/* must have the session mutex held here */
int session_destroy_unsafe(ab_session_p session)
{
    if (!session)
        return 1;

    int debug = session->debug;
    ab_request_p req;

    pdebug(debug, "Starting.");

    /* do not destroy the session if there are
     * tags or connections still */
    if(!session_is_empty(session)) {
        pdebug(debug, "Attempt to destroy session while open tags or connections exist!");
        return 0;
    }

    /* this is a best effort attempt */
    session_unregister(session);

    /* close the socket. */
    if (session->is_connected) {
        socket_close(session->sock);
        socket_destroy(&(session->sock));
        session->is_connected = 0;
    }

    /* need the mutex-protected version */
    remove_session_unsafe(session);

    /* remove any remaining requests, they are dead */
    req = session->requests;

    while(req) {
        request_remove_unsafe(session, req);
        request_destroy_unsafe(&req);
        req = session->requests;
    }

    mem_free(session);

    pdebug(debug, "Done.");

    return 1;
}

int session_destroy(ab_session_p session)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(session->debug, "entering critical block %p", global_session_mut);
    critical_block(global_session_mut) {
        rc = session_destroy_unsafe(session);
    }
    pdebug(session->debug, "leaving critical block %p", global_session_mut);

    return rc;
}

int session_is_empty(ab_session_p session)
{
    return (session->tags == NULL) && (session->connections == NULL);
}

int session_register(ab_session_p session)
{
    int debug = session->debug;
    eip_session_reg_req* req;
    eip_encap_t* resp;
    int rc;
    int data_size = 0;
    uint64_t timeout_time;

    pdebug(debug, "Starting.");

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
    timeout_time = time_ms() + 5000; /* MAGIC */

    pdebug(debug, "sending data:");
    pdebug_dump_bytes(debug, session->recv_data, data_size);

    while (timeout_time > time_ms() && session->recv_offset < data_size) {
        rc = socket_write(session->sock, session->recv_data + session->recv_offset, data_size - session->recv_offset);

        if (rc < 0) {
            pdebug(debug, "Unable to send session registration packet! rc=%d", rc);
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
        ;
        return PLCTAG_ERR_TIMEOUT;
    }

    /* get the response from the gateway */

    /* ready the input buffer */
    session->recv_offset = 0;
    mem_set(session->recv_data, 0, MAX_REQ_RESP_SIZE);

    timeout_time = time_ms() + 5000; /* MAGIC */

    while (timeout_time > time_ms()) {
        if (session->recv_offset < sizeof(eip_encap_t)) {
            data_size = sizeof(eip_encap_t);
        } else {
            data_size = sizeof(eip_encap_t) + ((eip_encap_t*)(session->recv_data))->encap_length;
        }

        if (session->recv_offset < data_size) {
            rc =
                socket_read(session->sock, session->recv_data + session->recv_offset, data_size - session->recv_offset);

            if (rc < 0) {
                if (rc != PLCTAG_ERR_NO_DATA) {
                    /* error! */
                    pdebug(debug, "Error reading socket! rc=%d", rc);
                    return rc;
                }
            } else {
                session->recv_offset += rc;

                /* recalculate the amount of data needed if we have just completed the read of an encap header */
                if (session->recv_offset >= sizeof(eip_encap_t)) {
                    data_size = sizeof(eip_encap_t) + ((eip_encap_t*)(session->recv_data))->encap_length;
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

    pdebug(debug, "received response:");
    pdebug_dump_bytes(debug, session->recv_data, data_size);

    /* encap header is at the start of the buffer */
    resp = (eip_encap_t*)(session->recv_data);

    /* check the response status */
    if (le2h16(resp->encap_command) != AB_EIP_REGISTER_SESSION) {
        pdebug(debug, "EIP unexpected response packet type: %d!", resp->encap_command);
        return PLCTAG_ERR_BAD_DATA;
    }

    if (le2h16(resp->encap_status) != AB_EIP_OK) {
        pdebug(debug, "EIP command failed, response code: %d", resp->encap_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* after all that, save the session handle, we will
     * use it in future packets.
     */
    session->session_handle = resp->encap_session_handle; /* opaque to us */

    pdebug(debug, "Done.");

    return 1;
}

int session_unregister(ab_session_p session)
{
    if (session->sock) {
        socket_close(session->sock);
        socket_destroy(&(session->sock));
        session->sock = NULL;
        session->is_connected = 0;
    }

    return PLCTAG_STATUS_OK;
}
