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
 * 2013-11-19  KRH - Created file.                                        *
 **************************************************************************/

/*#ifdef __cplusplus
extern "C"
{
#endif
*/

/*#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
*/
#include <errno.h>
#include <platform.h>
#include <lib/libplctag.h>
#include <lib/libplctag_tag.h>
#include <ab/connection.h>
#include <ab/session.h>
#include <ab/tag.h>
#include <ab/eip.h>
#include <ab/ab.h>
#include <util/attr.h>
#include <util/debug.h>


/*
 * Shared global data
 */

//~ static ab_connection_p session_find_connection_by_path_unsafe(ab_session_p session,const char *path);
static ab_connection_p connection_create_unsafe(const char* path, ab_tag_p tag, int shared);
static int connection_perform_forward_open(ab_connection_p connection);
static int send_forward_open_req(ab_connection_p connection, ab_request_p req);
static int recv_forward_open_resp(ab_connection_p connection, ab_request_p req);
//~ static int connection_add_tag_unsafe(ab_connection_p connection, ab_tag_p tag);
//~ static int connection_add_tag(ab_connection_p connection, ab_tag_p tag);
//~ static int connection_remove_tag_unsafe(ab_connection_p connection, ab_tag_p tag);
//~ static int connection_remove_tag(ab_connection_p connection, ab_tag_p tag);
//~ static int connection_empty_unsafe(ab_connection_p connection);
//~ static int connection_is_empty(ab_connection_p connection);
//static int connection_destroy_unsafe(ab_connection_p connection);
static void connection_destroy(void *connection);
static int connection_close(ab_connection_p connection);
static int send_forward_close_req(ab_connection_p connection, ab_request_p req);
static int recv_forward_close_resp(ab_connection_p connection, ab_request_p req);


/*
 * EXTERNAL functions
 *
 */



int connection_find_or_create(ab_tag_p tag, attr attribs)
{
    const char* path = attr_get_str(attribs, "path", "");
    ab_connection_p connection = AB_CONNECTION_NULL;
    int rc = PLCTAG_STATUS_OK;
    int is_new = 0;
    int shared_connection = attr_get_int(attribs, "share_connection", 1); /* share the session by default. */

    pdebug(DEBUG_INFO, "Starting.");

    /* lock the session while this is happening because we do not
     * want a race condition where two tags try to create the same
     * connection at the same time.
     */

    critical_block(global_session_mut) {
        if(shared_connection) {
            connection = session_find_connection_by_path_unsafe(tag->session, path);
        } else {
            connection = AB_CONNECTION_NULL;
        }

        /* if we find one but it is in the process of disconnection, create a new one */
        if (connection == AB_CONNECTION_NULL) {
            connection = connection_create_unsafe(path, tag, shared_connection);
            is_new = 1;

            if(shared_connection) {
                pdebug(DEBUG_INFO, "Creating new connection.");
            } else {
                pdebug(DEBUG_INFO, "Creating new exclusive connection.");
            }
        } else {
            /* found a connection, nothing more to do. */
            pdebug(DEBUG_INFO, "connection_find_or_create() reusing existing connection.");
            rc = PLCTAG_STATUS_OK;
        }
    }

    if (connection == AB_CONNECTION_NULL) {
        pdebug(DEBUG_ERROR, "unable to create or find a connection!");
        rc = PLCTAG_ERR_BAD_GATEWAY;
        return rc;
    } else if(is_new) {
        /* do the ForwardOpen call to set up the connection */
        if((rc = connection_perform_forward_open(connection)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to perform ForwardOpen to set up connection with PLC!");
        }
    }

    tag->connection = connection;

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


int connection_acquire(ab_connection_p connection)
{
	if(!connection) {
		return PLCTAG_ERR_NULL_PTR;
	}
	
	pdebug(DEBUG_INFO,"Acquire connection.");
	
    return refcount_acquire(&connection->rc);
}

int connection_release(ab_connection_p connection)
{
	if(!connection) {
		return PLCTAG_ERR_NULL_PTR;
	}

	pdebug(DEBUG_INFO,"Release connection.");
	
    return refcount_release(&connection->rc);
}









/*
 * INTERNAL helper functions.
 */

/* not thread safe! */
ab_connection_p connection_create_unsafe(const char* path, ab_tag_p tag, int shared)
{
    ab_connection_p connection = (ab_connection_p)mem_alloc(sizeof(struct ab_connection_t));

    pdebug(DEBUG_INFO, "Starting.");

    if (!connection) {
        pdebug(DEBUG_ERROR, "Unable to allocate new connection!");
        return NULL;
    }

    connection->session = tag->session;
    connection->conn_seq_num = 1 /*(uint16_t)(intptr_t)(connection)*/;
    connection->orig_connection_id = ++(connection->session->conn_serial_number);
    connection->status = PLCTAG_STATUS_PENDING;
    connection->exclusive = !shared;

    /* connection is going to be referenced, so set refcount up. */
    connection->rc = refcount_init(1, connection, connection_destroy);

    /* copy the path for later */
    str_copy(&connection->path[0], path, MAX_CONN_PATH);

    /* copy path data from the tag */
    mem_copy(connection->conn_path, tag->conn_path, tag->conn_path_size);
    connection->conn_path_size = tag->conn_path_size;

    /*
     * Determine the right param for the connection.
     * This sets up the packet size, among other things.
     */
    switch(tag->protocol_type) {
        case AB_PROTOCOL_PLC:
        case AB_PROTOCOL_MLGX:
            connection->conn_params = AB_EIP_PLC5_PARAM;
            break;

        case AB_PROTOCOL_LGX:
        case AB_PROTOCOL_MLGX800:
            connection->conn_params = AB_EIP_LGX_PARAM;
            break;

        default:
            pdebug(DEBUG_WARN,"Unknown protocol/cpu type!");
            connection->status = PLCTAG_ERR_BAD_PARAM;
            break;
    }

    pdebug(DEBUG_DETAIL,"conn path size = %d", connection->conn_path_size);

    for(int j=0; j < connection->conn_path_size; j++) {
        pdebug(DEBUG_DETAIL,"conn_path[%d] = %x", j, connection->conn_path[j]);
    }

    /* add the connection to the session */
    /* FIXME - these could fail! */
    session_acquire(connection->session);
    session_add_connection_unsafe(connection->session, connection);

    pdebug(DEBUG_INFO, "Done.");

    return connection;
}



int connection_perform_forward_open(ab_connection_p connection)
{
    ab_request_p req;
    int64_t timeout_time;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = request_create(&req);

    do {
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN,"Unable to get new request.  rc=%d",rc);
            rc = 0;
            break;
        }

        /* send the ForwardOpen command to the PLC */
        if((rc = send_forward_open_req(connection, req)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN,"Unable to send ForwardOpen packet!");
            break;
        }

        /* wait for a response */
        timeout_time = time_ms() + CONNECTION_SETUP_TIMEOUT; 

        while (timeout_time > time_ms() && !req->resp_received) {
            sleep_ms(1);
        }

        /* timeout? */
        if(!req->resp_received) {
            pdebug(DEBUG_WARN,"Timed out waiting for ForwardOpen response!");
            rc = PLCTAG_ERR_TIMEOUT_ACK;
            break;
        }

        /* check for the ForwardOpen response. */
        if((rc = recv_forward_open_resp(connection, req)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN,"Unable to use ForwardOpen response!");
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }
    } while(0);

    connection->status = rc;

    if(req) {
		session_remove_request(connection->session,req);
        request_release(req);
        //request_destroy(&req);
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


int send_forward_open_req(ab_connection_p connection, ab_request_p req)
{
    eip_forward_open_request_t *fo;
    uint8_t *data;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO,"Starting");

    fo = (eip_forward_open_request_t*)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_forward_open_request_t);

    /* set up the path information. */
    mem_copy(data, connection->conn_path, connection->conn_path_size);
    data += connection->conn_path_size;

    /* fill in the static parts */

    /* encap header parts */
    fo->encap_command = h2le16(AB_EIP_READ_RR_DATA); /* 0x006F EIP Send RR Data command */
    fo->encap_length = h2le16(data - (uint8_t*)(&fo->interface_handle)); /* total length of packet except for encap header */
    fo->router_timeout = h2le16(1);                       /* one second is enough ? */

    /* CPF parts */
    fo->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    fo->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI); /* null address item type */
    fo->cpf_nai_item_length = h2le16(0);             /* no data, zero length */
    fo->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fo->cpf_udi_item_length = h2le16(data - (uint8_t*)(&fo->cm_service_code)); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fo->cm_service_code = AB_EIP_CMD_FORWARD_OPEN; /* 0x54 Forward Open Request */
    fo->cm_req_path_size = 2;                      /* size of path in 16-bit words */
    fo->cm_req_path[0] = 0x20;                     /* class */
    fo->cm_req_path[1] = 0x06;                     /* CM class */
    fo->cm_req_path[2] = 0x24;                     /* instance */
    fo->cm_req_path[3] = 0x01;                     /* instance 1 */

    /* Forward Open Params */
    fo->secs_per_tick = AB_EIP_SECS_PER_TICK;         /* seconds per tick, no used? */
    fo->timeout_ticks = AB_EIP_TIMEOUT_TICKS;         /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
    fo->orig_to_targ_conn_id = h2le32(0);             /* is this right?  Our connection id on the other machines? */
    fo->targ_to_orig_conn_id = h2le32(connection->orig_connection_id); /* connection id in the other direction. */
    /* this might need to be globally unique */
    fo->conn_serial_number = h2le16((uint16_t)(intptr_t)(connection)); /* our connection SEQUENCE number. */
    fo->orig_vendor_id = h2le16(AB_EIP_VENDOR_ID);               /* our unique :-) vendor ID */
    fo->orig_serial_number = h2le32(AB_EIP_VENDOR_SN);           /* our serial number. */
    fo->conn_timeout_multiplier = AB_EIP_TIMEOUT_MULTIPLIER;     /* timeout = mult * RPI */
    fo->orig_to_targ_rpi = h2le32(AB_EIP_RPI); /* us to target RPI - Request Packet Interval in microseconds */
    fo->orig_to_targ_conn_params = h2le16(connection->conn_params); /* packet size and some other things, based on protocol/cpu type */
    fo->targ_to_orig_rpi = h2le32(AB_EIP_RPI); /* target to us RPI - not really used for explicit messages? */
    fo->targ_to_orig_conn_params = h2le16(connection->conn_params); /* packet size and some other things, based on protocol/cpu type */
    fo->transport_class = AB_EIP_TRANSPORT_CLASS_T3; /* 0xA3, server transport, class 3, application trigger */
    fo->path_size = connection->conn_path_size/2; /* size in 16-bit words */

    /* set the size of the request */
    req->request_size = data - (req->data);

    /* mark it as ready to send */
    req->send_request = 1;

    /*
     * make sure the session serializes this with respect to other
     * control packets.  Apparently, the connection manager has no
     * buffers.
     */
    req->connected_request = 1;
    req->no_resend = 1; /* do not resend this, leads to problems.*/

    /* add the request to the session's list. */
    rc = session_add_request(connection->session, req);

    pdebug(DEBUG_INFO, "Done");

    return rc;
}


int recv_forward_open_resp(ab_connection_p connection, ab_request_p req)
{
    eip_forward_open_response_t *fo_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO,"Starting");

    fo_resp = (eip_forward_open_response_t*)(req->data);

    do {
        if(le2h16(fo_resp->encap_command) != AB_EIP_READ_RR_DATA) {
            pdebug(DEBUG_WARN,"Unexpected EIP packet type received: %d!",fo_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h16(fo_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"EIP command failed, response code: %d",fo_resp->encap_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(fo_resp->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"Forward Open command failed, response code: %d",fo_resp->general_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        connection->targ_connection_id = le2h32(fo_resp->orig_to_targ_conn_id);
        connection->orig_connection_id = le2h32(fo_resp->targ_to_orig_conn_id);
        connection->is_connected = 1;

        pdebug(DEBUG_INFO,"ForwardOpen succeeded with our connection ID %x and the PLC connection ID %x",connection->orig_connection_id, connection->targ_connection_id);

        pdebug(DEBUG_DETAIL,"Connection set up succeeded.");

        connection->status = PLCTAG_STATUS_OK;
        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_INFO,"Done.");

    return rc;
}






//~ int connection_add_tag_unsafe(ab_connection_p connection, ab_tag_p tag)
//~ {
    //~ pdebug(DEBUG_DETAIL, "Starting");

    //~ tag->next = connection->tags;
    //~ connection->tags = tag;

    //~ pdebug(DEBUG_DETAIL, "Done");

    //~ return PLCTAG_STATUS_OK;
//~ }


//~ int connection_add_tag(ab_connection_p connection, ab_tag_p tag)
//~ {
    //~ int rc = PLCTAG_STATUS_OK;

    //~ pdebug(DEBUG_INFO, "Starting.");

    //~ if(connection) {
        //~ critical_block(global_session_mut) {
            //~ rc = connection_add_tag_unsafe(connection, tag);
        //~ }
    //~ } else {
        //~ pdebug(DEBUG_WARN, "Connection ptr is null!");
        //~ rc = PLCTAG_ERR_NULL_PTR;
    //~ }

    //~ pdebug(DEBUG_INFO, "Done.");

    //~ return rc;
//~ }

//~ int connection_remove_tag_unsafe(ab_connection_p connection, ab_tag_p tag)
//~ {
    //~ ab_tag_p cur;
    //~ ab_tag_p prev;
    //~ int rc;

    //~ pdebug(DEBUG_INFO, "Starting.");

    //~ cur = connection->tags;
    //~ prev = NULL;

    //~ while (cur && cur != tag) {
        //~ prev = cur;
        //~ cur = cur->next;
    //~ }

    //~ if (cur == tag) {
        //~ if (prev) {
            //~ prev->next = cur->next;
        //~ } else {
            //~ connection->tags = cur->next;
        //~ }

        //~ tag->connection = NULL;

        //~ rc = PLCTAG_STATUS_OK;
    //~ } else {
        //~ rc = PLCTAG_ERR_NOT_FOUND;
    //~ }

    //~ if (connection_empty_unsafe(connection)) {
        //~ connection->disconnect_in_progress = 1;
    //~ } else {
        //~ pdebug(DEBUG_DETAIL, "connection not empty.  Not destroying.");
    //~ }

    //~ pdebug(DEBUG_INFO, "Done.");

    //~ return rc;
//~ }

//int connection_remove_tag(ab_connection_p connection, ab_tag_p tag)
//{
    //int rc = PLCTAG_STATUS_OK;

    //pdebug(DEBUG_INFO, "Starting.");

    //if(connection && connection->session) {
        //critical_block(global_session_mut) {
            //rc = connection_remove_tag_unsafe(connection, tag);
        //}

        ///* release the connection */
        //connection_release(connection);

        ///* don't block the mutex, check to see if we need to destroy the connection. */
        ///*
        //if(connection_is_empty(connection) && connection->disconnect_in_progress) {
            //pdebug(DEBUG_INFO,"Closing connection.");
            //connection_close(connection);

            //critical_block(global_session_mut) {
                //connection_destroy_unsafe(connection);
            //}
        //}
        //*/
    //} else {
        //pdebug(DEBUG_WARN, "Connection or session ptr is null!");
        //rc = PLCTAG_ERR_NULL_PTR;
    //}

    //pdebug(DEBUG_INFO, "Done.");

    //return rc;
//}


//~ int connection_empty_unsafe(ab_connection_p connection)
//~ {
    //~ if(!connection) {
        //~ return 1;
    //~ }

    //~ return (connection->tags == NULL);
//~ }


//~ int connection_is_empty(ab_connection_p connection)
//~ {
    //~ int rc = PLCTAG_STATUS_OK;

    //~ critical_block(global_session_mut) {
        //~ rc = connection_empty_unsafe(connection);
    //~ }

    //~ return rc;
//~ }

void connection_destroy(void *connection_arg)
{
    ab_connection_p connection = connection_arg;
	int really_destroy = 1;
	
    pdebug(DEBUG_INFO, "Starting.");

    if (!connection) {
        pdebug(DEBUG_WARN,"Connection destructor called with null pointer!");
        return;
    }

    /* do not destroy the connection if there are
     * connections still */

    /* removed due to refcount code
    if (connection->tags) {
        pdebug(DEBUG_WARN, "Attempt to destroy connection while open tags exist!");
        return 0;
    }
    */

	/*
	 * This needs to be done carefully.  We can have a race condition here.
	 * 
	 * If there is another thread that just looked up the connection and got
	 * a reference (and thus a ref count increment), then we have another
	 * reference and thus cannot delete this connection yet.
	 */
	critical_block(global_session_mut) {
		if(refcount_get_count(&connection->rc) > 0) {
			pdebug(DEBUG_WARN,"Some other thread took a reference to this connection before we could delete it.  Aborting deletion.");
			really_destroy = 0;
			break;
		}

		/* make sure the session does not reference the connection */
		session_remove_connection_unsafe(connection->session, connection);
		
		/* now no one can get a reference to this connection. */
	}
	
	if(really_destroy) {	
		/* clean up connection with the PLC, ignore return code, we can't do anything about it. */
		connection_close(connection);

		session_release(connection->session);

		/* do final clean up */
		mem_free(connection);
	}

    pdebug(DEBUG_INFO, "Done.");

    return;
}

/*
 * This should never be called directly.  It should only be called
 * as a result of the reference count hitting zero.
 */
/* not called due to refcount code
void connection_destroy(void conn_arg)
{
    ab_connection_p connection = conn_arg;

    critical_block(global_session_mut) {
        connection_destroy_unsafe(connection);
    }
}
*/

int connection_close(ab_connection_p connection)
{
    ab_request_p req;
    int64_t timeout_time = 0L;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = request_create(&req);

    do {
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN,"Unable to get new request.  rc=%d",rc);
            rc = 0;
            break;
        }

        /* send the ForwardClose command to the PLC */
        if((rc = send_forward_close_req(connection, req)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN,"Unable to send ForwardClose packet!");
            break;
        }

        /* wait for a response */
        timeout_time = time_ms() + CONNECTION_TEARDOWN_TIMEOUT; 

        while (timeout_time > time_ms() && !req->resp_received) {
            sleep_ms(1);
        }

        /* timeout? */
        if(!req->resp_received) {
            pdebug(DEBUG_WARN,"Timed out waiting for ForwardClose response!");
            rc = PLCTAG_ERR_TIMEOUT_ACK;
            break;
        }

        /* check for the ForwardClose response. */
        if((rc = recv_forward_close_resp(connection, req)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN,"Unable to use ForwardClose response!");
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

    } while(0);

    connection->status = rc;

    if(req) {
        session_remove_request(connection->session,req);
        request_release(req);
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


int send_forward_close_req(ab_connection_p connection, ab_request_p req)
{
    eip_forward_close_req_t *fo;
    uint8_t *data;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO,"Starting");

    fo = (eip_forward_close_req_t*)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_forward_close_req_t);

    /* set up the path information. */
    mem_copy(data, connection->conn_path, connection->conn_path_size);
    data += connection->conn_path_size;

    /* fill in the static parts */

    /* encap header parts */
    fo->encap_command = h2le16(AB_EIP_READ_RR_DATA); /* 0x006F EIP Send RR Data command */
    fo->encap_length = h2le16(data - (uint8_t*)(&fo->interface_handle)); /* total length of packet except for encap header */
    fo->router_timeout = h2le16(1);                       /* one second is enough ? */

    /* CPF parts */
    fo->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    fo->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI); /* null address item type */
    fo->cpf_nai_item_length = h2le16(0);             /* no data, zero length */
    fo->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fo->cpf_udi_item_length = h2le16(data - (uint8_t*)(&fo->cm_service_code)); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fo->cm_service_code = AB_EIP_CMD_FORWARD_CLOSE;/* 0x4E Forward Close Request */
    fo->cm_req_path_size = 2;                      /* size of path in 16-bit words */
    fo->cm_req_path[0] = 0x20;                     /* class */
    fo->cm_req_path[1] = 0x06;                     /* CM class */
    fo->cm_req_path[2] = 0x24;                     /* instance */
    fo->cm_req_path[3] = 0x01;                     /* instance 1 */

    /* Forward Open Params */
    fo->secs_per_tick = AB_EIP_SECS_PER_TICK;         /* seconds per tick, no used? */
    fo->timeout_ticks = AB_EIP_TIMEOUT_TICKS;         /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
    fo->conn_serial_number = h2le16((uint16_t)(intptr_t)(connection)); /* our connection SEQUENCE number. */
    fo->orig_vendor_id = h2le16(AB_EIP_VENDOR_ID);               /* our unique :-) vendor ID */
    fo->orig_serial_number = h2le32(AB_EIP_VENDOR_SN);           /* our serial number. */
    fo->path_size = connection->conn_path_size/2; /* size in 16-bit words */

    /* set the size of the request */
    req->request_size = data - (req->data);

    /* mark it as ready to send */
    req->send_request = 1;
    /*req->abort_after_send = 1;*/ /* don't return to us.*/

    /*
     * make sure the session serializes this with respect to other
     * control packets.  Apparently, the connection manager has no
     * buffers.
     */
    req->connected_request = 1;

    /* add the request to the session's list. */
    rc = session_add_request(connection->session, req);

    pdebug(DEBUG_INFO, "Done");

    return rc;
}


int recv_forward_close_resp(ab_connection_p connection, ab_request_p req)
{
    eip_forward_close_resp_t *fo_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO,"Starting");

    fo_resp = (eip_forward_close_resp_t*)(req->data);

    do {
        if(le2h16(fo_resp->encap_command) != AB_EIP_READ_RR_DATA) {
            pdebug(DEBUG_WARN,"Unexpected EIP packet type received: %d!",fo_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h16(fo_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"EIP command failed, response code: %d",fo_resp->encap_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(fo_resp->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"Forward Close command failed, response code: %d",fo_resp->general_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        pdebug(DEBUG_DETAIL,"Connection close succeeded.");

        connection->status = PLCTAG_STATUS_OK;
        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_INFO,"Done.");

    return rc;
}


//~ int mark_connection_for_request(ab_request_p request)
//~ {
    //~ int rc = PLCTAG_STATUS_OK;
    //~ int index = 0;

    //~ if(!request) {
        //~ return PLCTAG_ERR_NULL_PTR;
    //~ }

    //~ if(!request->connection) {
        //~ return PLCTAG_STATUS_OK;
    //~ }

    //~ /* FIXME DEBUG - remove! */
    //~ //return PLCTAG_STATUS_OK;

    //~ /* mark the connection as in use. */
    //~ for(index = 0; index < CONNECTION_MAX_IN_FLIGHT; index++) {
        //~ if(!request->connection->request_in_flight[index]) {
            //~ request->connection->request_in_flight[index] = 1;
            //~ request->connection->seq_in_flight[index] = request->connection->conn_seq_num;
            //~ pdebug(DEBUG_INFO,"Found empty connection slot at position %d",index);
            //~ return rc;
        //~ } else {
            //~ pdebug(DEBUG_INFO,"Slot %d already marked.",index);
        //~ }
    //~ }

    //~ return rc;
//~ }


//~ int clear_connection_for_request(ab_request_p request)
//~ {
    //~ int rc = PLCTAG_STATUS_OK;

    //~ if(request->connection) {
        //~ ab_connection_p connection = request->connection;
        //~ int index = 0;
        //~ int found = 0;

        //~ for(index = 0; index < CONNECTION_MAX_IN_FLIGHT; index++) {
            //~ if(connection->request_in_flight[index]) {
                //~ if(connection->seq_in_flight[index] == request->conn_seq) {
                    //~ pdebug(DEBUG_INFO, "Clearing connection in flight flag for packet sequence ID %d", request->conn_seq);
                    //~ connection->request_in_flight[index] = 0;
                    //~ found = 1;
                    //~ break;
                //~ }
            //~ }
        //~ }

        //~ if(!found) {
            //~ pdebug(DEBUG_INFO,"Packet with sequence ID %d not found in flight.", request->conn_seq);
        //~ }
    //~ }

    //~ return rc;
//~ }
