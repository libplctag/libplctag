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

#include <lib/libplctag.h>
#include <platform.h>
#include <ab/eip.h>
#include <ab/session.h>
#include <ab/connection.h>
#include <ab/request.h>
#include <util/debug.h>


/* this must be called with the session mutex held */
int send_eip_request_unsafe(ab_request_p req)
{
    int rc;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* if we have not already started, then start the send */
    if (!req->send_in_progress) {
        eip_encap_t* encap = (eip_encap_t*)(req->data);
        int payload_size = req->request_size - sizeof(eip_encap_t);

        /* set up the session sequence ID for this transaction */
        if(encap->encap_command == h2le16(AB_EIP_READ_RR_DATA)) {
			/* get new ID */
			req->session->session_seq_id++;
			
            req->session_seq_id = req->session->session_seq_id;
            encap->encap_sender_context = req->session->session_seq_id; /* link up the request seq ID and the packet seq ID */
            
            /* mark the session as being used if this is a serialized packet */
            //~ mark_session_for_request(req);

            pdebug(DEBUG_INFO,"Sending unconnected packet with session sequence ID %llx",req->session->session_seq_id);
        } else {
            eip_cip_co_req *conn_req = (eip_cip_co_req*)(req->data);
            
            /* set up the connection information */
            conn_req->cpf_targ_conn_id = h2le32(req->connection->targ_connection_id);
            req->conn_id = req->connection->orig_connection_id;

            req->connection->conn_seq_num++;
            conn_req->cpf_conn_seq_num = h2le16(req->connection->conn_seq_num);
            req->conn_seq = req->connection->conn_seq_num;  
            
            /* mark the connection as being used. */
			//~ mark_connection_for_request(req);
			
            pdebug(DEBUG_INFO,"Sending connected packet with connection ID %x and sequence ID %u(%x)",req->conn_id, req->conn_seq, req->conn_seq);
        }

        /* set up the rest of the request */
        req->current_offset = 0; /* nothing written yet */

        /* fill in the header fields. */
        encap->encap_length = h2le16(payload_size);
        encap->encap_session_handle = req->session->session_handle;
        encap->encap_status = h2le32(0);
        encap->encap_options = h2le32(0);

        /* display the data */
        pdebug(DEBUG_INFO,"Sending packet of size %d",req->request_size);
        pdebug_dump_bytes(DEBUG_INFO, req->data, req->request_size);

        req->send_in_progress = 1;
    }

    /* send the packet */
    rc = socket_write(req->session->sock, req->data + req->current_offset, req->request_size - req->current_offset);

    if (rc >= 0) {
        req->current_offset += rc;

        /* are we done? */
        if (req->current_offset >= req->request_size) {
            req->send_request = 0;
            req->send_in_progress = 0;
            req->current_offset = 0;
            
            req->time_sent = time_ms();
            req->send_count++;

            /* set this request up for a receive action */
            if(req->abort_after_send) {
                req->abort_request = 1; /* for one shots */
            } else {
                req->recv_in_progress = 1;
            }
        }

        rc = PLCTAG_STATUS_OK;
    } else {
        /* oops, error of some sort. */
        req->status = rc;
        req->send_request = 0;
        req->send_in_progress = 0;
        req->recv_in_progress = 0;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}

/*
 * recv_eip_response
 *
 * Look at the passed session and read any data we can
 * to fill in a packet.  If we already have a full packet,
 * punt.
 */
int recv_eip_response_unsafe(ab_session_p session)
{
    uint32_t data_needed = 0;
    int rc = PLCTAG_STATUS_OK;
    
    /* skip the rest if we already have a packet waiting in the session buffer. */
    if(session->has_response) {
		return PLCTAG_STATUS_OK;
	}

    /*pdebug(DEBUG_DETAIL,"Starting.");*/

    /*
     * Determine the amount of data to get.  At a minimum, we
     * need to get an encap header.  This will determine
     * whether we need to get more data or not.
     */
    data_needed = (session->recv_offset < sizeof(eip_encap_t)) ? 
											sizeof(eip_encap_t) : 
											sizeof(eip_encap_t) + le2h16(((eip_encap_t*)(session->recv_data))->encap_length);

    if (session->recv_offset < data_needed) {
        /* read everything we can */
        do {
            rc = socket_read(session->sock, session->recv_data + session->recv_offset,
                             data_needed - session->recv_offset);

            /*pdebug(DEBUG_DETAIL,"socket_read rc=%d",rc);*/

            if (rc < 0) {
                if (rc != PLCTAG_ERR_NO_DATA) {
                    /* error! */
                    pdebug(DEBUG_WARN,"Error reading socket! rc=%d",rc);
                    return rc;
                }
            } else {
                session->recv_offset += rc;

                /*pdebug_dump_bytes(session->debug, session->recv_data, session->recv_offset);*/

                /* recalculate the amount of data needed if we have just completed the read of an encap header */
                if (session->recv_offset >= sizeof(eip_encap_t)) {
                    data_needed = sizeof(eip_encap_t) + le2h16(((eip_encap_t*)(session->recv_data))->encap_length);
                }
            }
        } while (rc > 0 && session->recv_offset < data_needed);
    }

    /* did we get all the data? */
    if (session->recv_offset >= data_needed) {
        session->resp_seq_id = ((eip_encap_t*)(session->recv_data))->encap_sender_context;
        session->has_response = 1;

        rc = PLCTAG_STATUS_OK;

        pdebug(DEBUG_DETAIL, "request received all needed data.");

        if(((eip_encap_t*)(session->recv_data))->encap_command == h2le16(AB_EIP_READ_RR_DATA)) {
            eip_encap_t *encap = (eip_encap_t*)(session->recv_data);
            pdebug(DEBUG_INFO,"Received unconnected packet with session sequence ID %llx",encap->encap_sender_context);
        } else {
            eip_cip_co_resp *resp = (eip_cip_co_resp*)(session->recv_data);
            pdebug(DEBUG_INFO,"Received connected packet with connection ID %x and sequence ID %u(%x)",le2h32(resp->cpf_orig_conn_id), le2h16(resp->cpf_conn_seq_num), le2h16(resp->cpf_conn_seq_num));
        }
    }

    return rc;
}
