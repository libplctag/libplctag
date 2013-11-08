/***************************************************************************
 *   Copyright (C) 2012 by Process Control Engineers                       *
 *   Author Kyle Hayes  kylehayes@processcontrolengineers.com              *
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
  * 2012-02-23  KRH - Created file.                                        *
  *                                                                        *
  * 2012-06-15  KRH - Rename file and includes for code re-org to get      *
  *                   ready for DF1 implementation.  Refactor some common  *
  *                   parts into ab/util.c.                                *
  *                                                                        *
  * 2012-06-20  KRH - change plc_err() calls for new error API.            *
  *                                                                        *
  * 2012-12-19	KRH - Start refactoring for threaded API and attributes.   *
  *                                                                        *
  **************************************************************************/

/*#ifdef __cplusplus
extern "C"
{
#endif
*/



#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <platform.h>
#include <libplctag.h>
#include <libplctag_tag.h>
#include <ab/ab.h>
#include <ab/eip_data.h>
#include <ab/util.h>
#include <util/attr.h>


/* forward references */

/* API functions */

/* generic */
int ab_tag_abort(plc_tag tag);
int ab_tag_destroy(plc_tag p_tag);

/* PCCC */
int ab_tag_status_pccc(plc_tag p_tag);
int ab_tag_read_pccc_start(plc_tag p_tag);
int ab_tag_read_pccc_check(plc_tag p_tag);
int ab_tag_write_pccc_start(plc_tag p_tag);
int ab_tag_write_pccc_check(plc_tag p_tag);

/* PCCC with DH+ last hop */
int ab_tag_status_pccc_dhp(plc_tag p_tag);
int ab_tag_read_pccc_dhp_start(plc_tag p_tag);
int ab_tag_read_pccc_dhp_check(plc_tag p_tag);
int ab_tag_write_pccc_dhp_start(plc_tag p_tag);
int ab_tag_write_pccc_dhp_check(plc_tag p_tag);

/* CIP "native" */
int ab_tag_status_cip(plc_tag p_tag);
int ab_tag_read_cip_start(plc_tag p_tag);
int ab_tag_read_cip_check(plc_tag p_tag);
int ab_tag_write_cip_start(plc_tag p_tag);
int ab_tag_write_cip_check(plc_tag p_tag);


/* various helper functions */
int check_mutex();
ab_session_p ab_session_create(const char *host, int gw_port);
int ab_session_connect(ab_session_p session, const char *host);
int ab_session_empty(ab_session_p session);
int ab_session_register(ab_session_p session);
int ab_session_unregister(ab_session_p session);
int convert_path_to_ioi(ab_tag_p tag, const char *path);
int convert_tag_name(ab_tag_p tag, const char *name);
char *ab_decode_cip_status(int status);
int ab_check_cpu(ab_tag_p tag);
int find_or_create_session(ab_tag_p tag);
int check_tag_name(ab_tag_p tag);
tag_vtable_p set_tag_vtable(ab_tag_p tag);
void *request_handler_func(void *);
int request_create(ab_request_p *req);
int request_add(ab_session_p sess, ab_request_p req);
int request_remove(ab_session_p sess, ab_request_p req);
int request_destroy(ab_request_p *req);


int add_session_unsafe(ab_session_p n);
int add_session(ab_session_p s);
int remove_session_unsafe(ab_session_p n);
int remove_session(ab_session_p s);
ab_session_p find_session_by_host_unsafe(const char  *t);

int session_add_tag_unsafe(ab_session_p session, ab_tag_p tag);
int session_remove_tag_unsafe(ab_session_p session, ab_tag_p tag);

int ab_session_destroy(ab_session_p session);
int ab_session_destroy_unsafe(ab_session_p session);



/* global data */

/* session/tag handling */
ab_session_p sessions = NULL;
mutex_p tag_mutex = NULL;
lock_t tag_mutex_lock = LOCK_INIT; /* used for protecting access to set up the above mutex */

/* request/response handling thread */
thread_p request_handler_thread = NULL;

/* vtables for different kinds of tags */
struct tag_vtable_t default_vtable = { NULL, ab_tag_destroy, NULL, NULL };
struct tag_vtable_t cip_vtable = { ab_tag_abort, ab_tag_destroy, ab_tag_read_cip_start, ab_tag_status_cip, ab_tag_write_cip_start };
struct tag_vtable_t plc_vtable = { ab_tag_abort, ab_tag_destroy, ab_tag_read_pccc_start, ab_tag_status_pccc, ab_tag_write_pccc_start };
struct tag_vtable_t plc_dhp_vtable = { ab_tag_abort, ab_tag_destroy, ab_tag_read_pccc_dhp_start, ab_tag_status_pccc_dhp, ab_tag_write_pccc_dhp_start };



/*************************************************************************
 **************************** API Functions ******************************
 ************************************************************************/



plc_tag ab_tag_create(attr attribs)
{
    ab_tag_p tag = AB_TAG_NULL;
    int data_size=0;
    const char *path;
	int rc;

    pdebug("Starting.");


    /*
     * allocate memory for the new tag.  Do this first so that
     * we have a vehicle for returning status.
     */

    tag = (ab_tag_p)mem_alloc(sizeof(struct ab_tag_t));

    if(!tag) {
    	pdebug("Unable to allocate memory for AB EIP tag!");
    	return PLC_TAG_NULL;
    }

    /* store the attributes */
    tag->attributes = attribs;

    /* put in a temporary vtable so that deletes work */
    tag->p_tag.vtable = &default_vtable;

    /*
     * check the CPU type.
     *
     * This determines the protocol type.
     */
    if(ab_check_cpu(tag) != PLCTAG_STATUS_OK) {
        tag->p_tag.status = PLCTAG_ERR_BAD_DEVICE;
        return (plc_tag)tag;
    }

    /* AB PLCs are little endian. */
    tag->p_tag.endian = PLCTAG_DATA_LITTLE_ENDIAN;

    /* allocate memory for the data */
    data_size = attr_get_int(tag->attributes,"elem_count",1) * attr_get_int(tag->attributes,"elem_size",0);

    if(data_size == 0) {
    	/* failure! Need data_size! */
    	tag->p_tag.status = PLCTAG_ERR_BAD_PARAM;
    	return (plc_tag)tag;
    }

    tag->p_tag.size = data_size;
    tag->p_tag.data = mem_alloc(tag->p_tag.size);

    if(tag->p_tag.data == NULL) {
    	tag->p_tag.status = PLCTAG_ERR_NO_MEM;
    	return (plc_tag)tag;
    }

    /* get the connection path, punt if there is not one. */
    path = attr_get_str(tag->attributes,"path",NULL);

    if(path == NULL) {
    	tag->p_tag.status = PLCTAG_ERR_BAD_PARAM;
    	return (plc_tag)tag;
    }

    /* make sure the global mutex is set up */
    rc = check_mutex();
    if(rc != PLCTAG_STATUS_OK) {
    	tag->p_tag.status = rc;
    	return (plc_tag)tag;
    }

	/*
	 * now we start the part that might conflict with other threads.
	 *
	 * The rest of this is inside a locked block.
	 */
	pdebug("Locking mutex");
	critical_block(tag_mutex) {
		/* fake exceptions */
		do {
			/*
			 * Check the request handler thread.
			 */
			if(!request_handler_thread) {
				rc = thread_create(&request_handler_thread,request_handler_func, 32*1024, NULL);
				if(rc != PLCTAG_STATUS_OK) {
					pdebug("Unable to create request handler thread!");
					tag->p_tag.status = rc;
					break;
				}
			}

			/*
			 * Find or create a session.
			 */
			if(find_or_create_session(tag) != PLCTAG_STATUS_OK) {
				pdebug("Unable to create session!");
				tag->p_tag.status = PLCTAG_ERR_BAD_GATEWAY;
				break;
			}

		    /*
		     * parse the link path into the tag.  Note that it must
		     * pad the byte string to a multiple of 16-bit words. The function
		     * also adds the protocol/PLC specific routing information to the
		     * links specified.  This fills in fields in the connection about
		     * any DH+ special data.
		     */
		    if(convert_path_to_ioi(tag,path) != PLCTAG_STATUS_OK) {
		        pdebug("Unable to convert links strings to binary path!");
				tag->p_tag.status = PLCTAG_ERR_BAD_PARAM;
		        break;
		    }


			/*
			 * check the tag name, this is protocol specific.
			 */

			if(check_tag_name(tag) != PLCTAG_STATUS_OK) {
				pdebug("Bad tag name!");
				tag->p_tag.status = PLCTAG_ERR_BAD_PARAM;
				break;
			}

			/*
			 * set up tag vtable.  This is protocol specific
			 */
			tag->p_tag.vtable = set_tag_vtable(tag);

			if(!tag->p_tag.vtable) {
				pdebug("Unable to set tag vtable!");
				tag->p_tag.status = PLCTAG_ERR_BAD_PARAM;
				break;
			}

			/*
			 * add the tag to the session's list.
			 */
			if(session_add_tag_unsafe(tag->session,tag) != PLCTAG_STATUS_OK) {
				pdebug("unable to add new tag to connection!");

				tag->p_tag.status = PLCTAG_ERR_CREATE;
				break;
			}
		} while(0);
    }

    pdebug("Done.");

    return (plc_tag)tag;
}




int ab_tag_abort(plc_tag p_tag)
{
	ab_tag_p tag = (ab_tag_p)p_tag;
	int i;

	for(i = 0; i < tag->num_requests; i++) {
		if(tag->reqs[i]) {
			tag->reqs[i]->abort_request = 1;
			tag->reqs[i] = NULL;
		}
	}

	if(tag->reqs) {
		mem_free(tag->reqs);
		tag->reqs = NULL;
		tag->num_requests = 0;
	}

	tag->read_in_progress = 0;
	tag->write_in_progress = 0;

	return PLCTAG_STATUS_OK;
}






int ab_tag_destroy(plc_tag p_tag)
{
	int rc = PLCTAG_STATUS_OK;
    ab_tag_p tag = (ab_tag_p)p_tag;

    pdebug("Starting.");

    /* already destroyed? */
    if(!tag)
        return rc;

	/* stop any current actions. */
	plc_tag_abort(p_tag);

	/* remove it from the current connection */
	if(tag->session) {
		ab_session_p session = tag->session;

		/* fiddling with shared data here, synchronize */
		critical_block(tag_mutex) {
			pdebug("Removing tag");
			session_remove_tag_unsafe(session,tag);

			/* if the session is now empty, remove it */
			if(ab_session_empty(session)) {
				pdebug("Removing session");
				ab_session_destroy_unsafe(session);
			}
		}

		/* null out the pointer just in case. */
		tag->session = NULL;
	}

	/* remove any attributes saved. */
	if(tag->attributes) {
		attr_destroy(tag->attributes);
		tag->attributes = NULL;
	}

	if(tag->reqs) {
		mem_free(tag->reqs);
		tag->reqs = NULL;
	}

	if(tag->p_tag.data) {
		mem_free(tag->p_tag.data);
		tag->p_tag.data = NULL;
	}

	/* release memory */
	mem_free(tag);

	pdebug("done");

    return rc;
}




/*
 * set_tag_vtable
 *
 * Use various bits of information about the tag to determine
 * just what flavor of the protocol we will be using for this
 * tag.
 */
tag_vtable_p set_tag_vtable(ab_tag_p tag)
{
	switch(tag->protocol_type) {
		case AB_PROTOCOL_PLC:
			if(tag->use_dhp_direct) {
				if(!plc_dhp_vtable.abort) {
					plc_dhp_vtable.abort     = ab_tag_abort;
					plc_dhp_vtable.destroy   = ab_tag_destroy;
					plc_dhp_vtable.read      = ab_tag_read_pccc_dhp_start;
					plc_dhp_vtable.status    = ab_tag_status_pccc_dhp;
					plc_dhp_vtable.write     = ab_tag_write_pccc_dhp_start;
				}
				return &plc_dhp_vtable;
			} else {
				if(!plc_vtable.abort) {
					plc_vtable.abort     = ab_tag_abort;
					plc_vtable.destroy   = ab_tag_destroy;
					plc_vtable.read      = ab_tag_read_pccc_start;
					plc_vtable.status    = ab_tag_status_pccc;
					plc_vtable.write     = ab_tag_write_pccc_start;
				}
				return &plc_vtable;
			}
			break;

		case AB_PROTOCOL_MLGX:
			if(!plc_vtable.abort) {
				plc_vtable.abort     = ab_tag_abort;
				plc_vtable.destroy   = ab_tag_destroy;
				plc_vtable.read      = ab_tag_read_pccc_start;
				plc_vtable.status    = ab_tag_status_pccc;
				plc_vtable.write     = ab_tag_write_pccc_start;
			}
			return &plc_vtable;

			break;

		case AB_PROTOCOL_LGX:
			if(!cip_vtable.abort) {
				cip_vtable.abort     = ab_tag_abort;
				cip_vtable.destroy   = ab_tag_destroy;
				cip_vtable.read      = ab_tag_read_cip_start;
				cip_vtable.status    = ab_tag_status_cip;
				cip_vtable.write     = ab_tag_write_cip_start;
			}
			return &cip_vtable;

			break;

		default:
			return NULL;
			break;
	}

	return NULL;
}





/*
 * ab_tag_status_pccc
 *
 * CIP-specific status.  This functions as a "tickler" routine
 * to check on the completion of async requests.
 */
int ab_tag_status_pccc(plc_tag p_tag)
{
	ab_tag_p tag = (ab_tag_p)p_tag;

	if(tag->read_in_progress) {
		int rc = ab_tag_read_pccc_check(p_tag);

		p_tag->status = rc;

		return rc;
	}

	if(tag->write_in_progress) {
		int rc = ab_tag_write_pccc_check(p_tag);

		p_tag->status = rc;

		return rc;
	}

	/*
	 * If the session is not completely set up,
	 * mark this tag as pending.
	 */
	if(tag->session) {
		if(!tag->session->is_connected) {
			p_tag->status = PLCTAG_STATUS_PENDING;
		} else {
			p_tag->status = PLCTAG_STATUS_OK;
		}
	}

	return p_tag->status;
}





/* FIXME -- this needs to be converted to unconnected messaging */

int ab_tag_read_pccc_start(plc_tag p_tag)
{
    ab_tag_p tag;
    eip_pccc_req *pccc;
    uint8_t *data;
    int elem_count = 0;
    int rc;
    uint16_t conn_seq_id = 0;
    ab_request_p req;

    pdebug("Starting");

    tag = (ab_tag_p)p_tag;

    /* get element count */
    elem_count = attr_get_int(tag->attributes,"elem_count",1);

    /* get a request buffer */
    rc = request_create(&req);

    if(rc != PLCTAG_STATUS_OK) {
    	pdebug("Unable to get new request.  rc=%d",rc);
    	p_tag->status = rc;
    	return rc;
    }

    /* point the struct pointers to the buffer*/
    pccc = (eip_pccc_req*)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_pccc_req);

    /* copy laa into the request */
    mem_copy(data,tag->encoded_name,tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* we need the count twice? */
    *((uint16_t*)data) = h2le16(elem_count); /* FIXME - bytes or INTs? */
    data += sizeof(uint16_t);

    /* encap fields */
    pccc->encap_command = h2le16(AB_EIP_CONNECTED_SEND);    /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    pccc->router_timeout = h2le16(1);                 /* one second timeout, enough? */

    /* Common Packet Format fields */
    pccc->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    pccc->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    pccc->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4 ? */
    //pccc->cpf_targ_conn_id = tag->connection->targ_connection_id;
    pccc->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    pccc->cpf_cdi_item_length = h2le16(data - (uint8_t*)(&(pccc->cpf_conn_seq_num)));/* REQ: fill in with length of remaining data. */

    /* connection sequence id */
    //tag->connection->conn_seq_num++;
    //pccc->cpf_conn_seq_num = h2le16(tag->connection->conn_seq_num);

    /* Command Routing */
    pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE;  /* ALWAYS 0x4B, Execute PCCC */
    pccc->req_path_size = 2;   /* ALWAYS 2, size in words of path, next field */
    pccc->req_path[0] = 0x20;  /* class */
    pccc->req_path[1] = 0x67;  /* PCCC Execute */
    pccc->req_path[2] = 0x24;  /* instance */
    pccc->req_path[3] = 0x01;  /* instance 1 */

    /* PCCC ID */
    pccc->request_id_size = 7;  /* ALWAYS 7 */
    pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);             /* Our CIP Vendor */
    pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN);      /* our unique serial number */

    /* PCCC Command */
    //conn_seq_id = (uint16_t)session_get_new_seq_id(tag->session); /* FIXME - this does not work on big endian machines! */
    pccc->pccc_command = AB_PCCC_TYPED_CMD;
    pccc->pccc_status = 0;  /* STS 0 in request */
    pccc->pccc_seq_num = h2le16(conn_seq_id); /* fill in  later */
    pccc->pccc_function = AB_PCCC_TYPED_READ_FUNC;
    pccc->pccc_transfer_size = h2le16(elem_count); /* This is not in the docs, but it is in the data. */
    											   /* FIXME - bytes or INTs? */

    /* get ready to add the request to the queue for this session */
    req->request_size = data - (req->data);
    req->send_request = 1;

    /* add the request to the session's list. */
    rc = request_add(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
    	pdebug("Unable to lock add request to session! rc=%d",rc);
    	request_destroy(&req);
    	p_tag->status = rc;
    	return rc;
    }

    /* save the request for later */

    /* FIXME
     *
     * This must be rewritten to use multiple request buffers.
     */

    //tag->req = req;
    tag->read_in_progress = 1;

    tag->p_tag.status = PLCTAG_STATUS_PENDING;

    return PLCTAG_STATUS_PENDING;
}



/* FIXME
 *
 * This must be rewritten to use multiple request buffers and unconnected explicit messages.
 */


int ab_tag_read_pccc_check(plc_tag p_tag)
{
    ab_tag_p tag;
    eip_pccc_resp *pccc_resp;
    uint8_t *data;
    uint8_t *data_end;
    int pccc_res_type;
    int pccc_res_length;
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;

    pdebug("Starting");

    tag = (ab_tag_p)p_tag;

    /* is there an outstanding request? */
    //if(!tag->req) {
    	//tag->read_in_progress = 0;
    	//tag->p_tag.status = PLCTAG_ERR_NULL_PTR;
    	//return PLCTAG_ERR_NULL_PTR;
    //}

    //if(!tag->req->resp_received) {
    //	tag->p_tag.status = PLCTAG_STATUS_PENDING;
    //	return PLCTAG_STATUS_PENDING;
    //}

    /* FIXME, just here to get compilation */
    req = tag->reqs[0];

    /* fake exceptions */
    do {
    	pccc_resp = (eip_pccc_resp*)(req->data);

		data_end = (req->data + pccc_resp->encap_length + sizeof(eip_encap_t));

		if(le2h16(pccc_resp->encap_command) != AB_EIP_CONNECTED_SEND) {
			pdebug("Unexpected EIP packet type received: %d!",pccc_resp->encap_command);
			rc = PLCTAG_ERR_BAD_DATA;
			break;
		}

		if(le2h16(pccc_resp->encap_status) != AB_EIP_OK) {
			pdebug("EIP command failed, response code: %d",pccc_resp->encap_status);
			rc = PLCTAG_ERR_REMOTE_ERR;
			break;
		}

		if(pccc_resp->general_status != AB_EIP_OK) {
			pdebug("PCCC command failed, response code: %d",pccc_resp->general_status);
			rc = PLCTAG_ERR_REMOTE_ERR;
			break;
		}

		if(pccc_resp->pccc_status != AB_EIP_OK) {
			/*pdebug(PLC_LOG_ERR,PLC_ERR_READ, "PCCC command failed, response code: %d",pccc_resp->pccc_status);*/
			decode_pccc_error(pccc_resp->pccc_data[0]);
			rc = PLCTAG_ERR_REMOTE_ERR;
			break;
		}

		/* point to the start of the data */
		data = pccc_resp->pccc_data;

		if(!(data = ab_decode_pccc_dt_byte(data,data_end - data, &pccc_res_type,&pccc_res_length))) {
			pdebug("Unable to decode PCCC response data type and data size!");
			rc = PLCTAG_ERR_BAD_DATA;
			break;
		}

		/* this gives us the overall type of the response and the number of bytes remaining in it.
		 * If the type is an array, then we need to decode another one of these words
		 * to get the type of each element and the size of each element.  We will
		 * need to adjust the size if we care.
		 */

		if(pccc_res_type == AB_PCCC_DATA_ARRAY) {
			if(!(data = ab_decode_pccc_dt_byte(data,data_end - data, &pccc_res_type,&pccc_res_length))) {
				pdebug("Unable to decode PCCC response array element data type and data size!");
				rc = PLCTAG_ERR_BAD_DATA;
				break;
			}
		}

		/* copy data into the tag. */
		if((data_end - data) > p_tag->size) {
			rc = PLCTAG_ERR_TOO_LONG;
			break;
		}

		mem_copy(p_tag->data,data,data_end - data);

		rc = PLCTAG_STATUS_OK;
    } while(0);

    /* get rid of the request now */
    if(req) {
     	request_destroy(&req);
        req = NULL;
    }

    /* the read is done. */
    tag->read_in_progress = 0;

    p_tag->status = rc;

    pdebug("Done.");

    return rc;
}




/* FIXME -- this needs to be converted to unconnected messaging and multiple requests. */

int ab_tag_write_pccc_start(plc_tag p_tag)
{
	int rc = PLCTAG_STATUS_OK;
    ab_tag_p tag;
    eip_pccc_req *pccc;
    uint8_t *data;
    uint8_t element_def[16];
    int element_def_size;
    uint8_t array_def[16];
    int array_def_size;
    int pccc_data_type;
    int elem_size = 0;
    int elem_count = 0;
    uint16_t conn_seq_id = 0;
    ab_request_p req = NULL;

    pdebug("Starting.");

    tag = (ab_tag_p)p_tag;

    /* get a request buffer */
    rc = request_create(&req);

    if(rc != PLCTAG_STATUS_OK) {
    	pdebug("Unable to get new request.  rc=%d",rc);
    	p_tag->status = rc;
    	return rc;
    }

    pccc = (eip_pccc_req*)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_pccc_req);

    /* copy laa into the request */
    mem_copy(data,tag->encoded_name,tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* What type and size do we have? */
    elem_size = attr_get_int(tag->attributes,"elem_size",0);
    elem_count = attr_get_int(tag->attributes,"elem_count",1);

    if(elem_size != 2 && elem_size != 4) {
        pdebug("Unsupported data type size: %d",elem_size);
    	request_destroy(&req);
    	tag->p_tag.status = PLCTAG_ERR_NOT_ALLOWED;
        return PLCTAG_ERR_NOT_ALLOWED;
    }

    if(elem_size == 4)
        pccc_data_type = AB_PCCC_DATA_REAL;
    else
        pccc_data_type = AB_PCCC_DATA_INT;

    /* generate the data type/data size fields, first the element part so that
     * we can get the size for the array part.
     */
    if(!(element_def_size = ab_encode_pccc_dt_byte(element_def,sizeof(element_def),pccc_data_type,elem_size))) {
        pdebug("Unable to encode PCCC request array element data type and size fields!");
    	request_destroy(&req);
    	tag->p_tag.status = PLCTAG_ERR_ENCODE;
        return PLCTAG_ERR_ENCODE;
    }

    if(!(array_def_size = ab_encode_pccc_dt_byte(array_def,sizeof(array_def),AB_PCCC_DATA_ARRAY,element_def_size + elem_size*elem_count))) {
        pdebug("Unable to encode PCCC request data type and size fields!");
    	request_destroy(&req);
        tag->p_tag.status = PLCTAG_ERR_ENCODE;
        return PLCTAG_ERR_ENCODE;
    }

    /* copy the array data first. */
    mem_copy(data,array_def,array_def_size);
    data += array_def_size;

    /* copy the element data */
    mem_copy(data,element_def,element_def_size);
    data += element_def_size;

    /* now copy the data to write */
    mem_copy(data,tag->p_tag.data,tag->p_tag.size);
    data += tag->p_tag.size;

    /* now fill in the rest of the structure. */

    /* encap fields */
    pccc->encap_command = h2le16(AB_EIP_CONNECTED_SEND);    /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    pccc->router_timeout = h2le16(1);                 /* one second timeout, enough? */

    /* Common Packet Format fields */
    pccc->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    pccc->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    pccc->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4 ? */
    //pccc->cpf_targ_conn_id = tag->connection->targ_connection_id;
    pccc->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    pccc->cpf_cdi_item_length = h2le16(data - (uint8_t*)(&(pccc->cpf_conn_seq_num)));/* REQ: fill in with length of remaining data. */

    /* connection sequence id */
    //conn_seq_id = connection_get_new_seq_id(tag->connection);
    pccc->cpf_conn_seq_num = h2le16(conn_seq_id);

    /* Command Routing */
    pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE;  /* ALWAYS 0x4B, Execute PCCC */
    pccc->req_path_size = 2;   /* ALWAYS 2, size in words of path, next field */
    pccc->req_path[0] = 0x20;  /* class */
    pccc->req_path[1] = 0x67;  /* PCCC Execute */
    pccc->req_path[2] = 0x24;  /* instance */
    pccc->req_path[3] = 0x01;  /* instance 1 */

    /* PCCC ID */
    pccc->request_id_size = 7;  /* ALWAYS 7 */
    pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);             /* Our CIP Vendor */
    pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN);      /* our unique serial number */

    /* PCCC Command */
    pccc->pccc_command = AB_PCCC_TYPED_CMD;
    pccc->pccc_status = 0;  /* STS 0 in request */
    //pccc->pccc_seq_num = h2le16(tag->connection->conn_seq_num);
    pccc->pccc_function = AB_PCCC_TYPED_WRITE_FUNC;
    /* FIXME - what should be the count here?  It is bytes, 16-bit
     * words or something else?
     *
     * Seems to be the number of elements??
     */
    pccc->pccc_transfer_size = h2le16(elem_count); /* This is not in the docs, but it is in the data. */


    /* get ready to add the request to the queue for this session */
    req->request_size = data - (req->data);
    req->send_request = 1;
    //req->conn_id = tag->connection->orig_connection_id;
    req->conn_seq = conn_seq_id;

    /* add the request to the session's list. */
    rc = request_add(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
    	pdebug("Unable to lock add request to session! rc=%d",rc);
    	request_destroy(&req);
    	p_tag->status = rc;
    	return rc;
    }

    /* save the request for later */
    tag->reqs[0] = req;

    /* the write is now pending */
    tag->write_in_progress = 1;
    tag->p_tag.status = PLCTAG_STATUS_PENDING;

    return PLCTAG_STATUS_PENDING;
}



/* FIXME -- this needs to be converted to unconnected messaging and multiple requests. */
int ab_tag_write_pccc_check(plc_tag p_tag)
{
    ab_tag_p tag;
    eip_pccc_resp *pccc_resp;
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;

    pdebug("Starting.");

    tag = (ab_tag_p)p_tag;

    req = tag->reqs[0];

    /* is there an outstanding request? */
    if(!req) {
    	tag->write_in_progress = 0;
    	tag->p_tag.status = PLCTAG_ERR_NULL_PTR;
    	return PLCTAG_ERR_NULL_PTR;
    }

    if(!req->resp_received) {
    	tag->p_tag.status = PLCTAG_STATUS_PENDING;
    	return PLCTAG_STATUS_PENDING;
    }

	/*
	 * we have a response. Remove the request/response from
	 * the list for the session.
	 */
	rc = request_remove(tag->session, req);

	if(rc != PLCTAG_STATUS_OK) {
		pdebug("Unable to remove the request from the list! rc=%d",rc);

		/* since we could not remove it, maybe the thread can. */
		plc_tag_abort(p_tag);

		/* not our request any more */
		req = NULL;

		tag->p_tag.status = rc;

		return rc;
	}

    /* fake exception */
    do {
		pccc_resp = (eip_pccc_resp*)(req->data);

		/* check the response status */
		if( le2h16(pccc_resp->encap_command) != AB_EIP_CONNECTED_SEND) {
			pdebug("EIP unexpected response packet type: %d!",pccc_resp->encap_command);
			rc = PLCTAG_ERR_BAD_DATA;
			break;
		}

		if(le2h16(pccc_resp->encap_status) != AB_EIP_OK) {
			pdebug("EIP command failed, response code: %d",pccc_resp->encap_status);
			rc = PLCTAG_ERR_REMOTE_ERR;
			break;
		}

		if(pccc_resp->general_status != AB_EIP_OK) {
			pdebug("PCCC command failed, response code: %d",pccc_resp->general_status);
			rc = PLCTAG_ERR_REMOTE_ERR;
			break;
		}

		if(pccc_resp->pccc_status != AB_EIP_OK) {
			/*pdebug(PLC_LOG_ERR,PLC_ERR_READ, "PCCC command failed, response code: %d",pccc_resp->pccc_status);*/
			decode_pccc_error(pccc_resp->pccc_data[0]);
			rc = PLCTAG_ERR_REMOTE_ERR;
			break;
		}

		tag->p_tag.status = PLCTAG_STATUS_OK;
		rc = PLCTAG_STATUS_OK;
    } while(0);

    tag->write_in_progress = 0;
    tag->p_tag.status = rc;

    if(req) {
    	request_destroy(&(req));
    	req = NULL;
    }

    pdebug("Done.");

    /* Success! */
    return rc;
}









/*
 * PCCC DH+ Routines
 */


/*
 * ab_tag_status_pccc_dhp
 *
 * PCCC/DH+-specific status.  This functions as a "tickler" routine
 * to check on the completion of async requests.
 */
int ab_tag_status_pccc_dhp(plc_tag p_tag)
{
	ab_tag_p tag = (ab_tag_p)p_tag;

	if(tag->read_in_progress) {
		int rc = ab_tag_read_pccc_check(p_tag);

		p_tag->status = rc;

		return rc;
	}

	if(tag->write_in_progress) {
		int rc = ab_tag_write_pccc_check(p_tag);

		p_tag->status = rc;

		return rc;
	}

	/*
	 * if the session we are using is not yet connected,
	 * then return PENDING and let the tag that started
	 * the connection finish it.
	 *
	 * FIXME - if the tag that started the connection
	 * fails to connect or is aborted/destroyed, then the
	 * connection will never be created.
	 */
	if(tag->session) {
		if(!tag->session->is_connected) {
			p_tag->status = PLCTAG_STATUS_PENDING;
		} else {
			p_tag->status = PLCTAG_STATUS_OK;
		}
	}

	return p_tag->status;
}





/* FIXME -- this needs to be converted to unconnected messaging */
int ab_tag_read_pccc_dhp_start(plc_tag p_tag)
{
    ab_tag_p tag;
    eip_pccc_dhp_req *pccc;
    uint8_t *data;
    int elem_count = 0;
    int rc = PLCTAG_STATUS_OK;
    uint16_t conn_seq_id = 0;
    ab_request_p req;

    pdebug("Starting");

    tag = (ab_tag_p)p_tag;

    /* get element count */
    elem_count = attr_get_int(tag->attributes,"elem_count",1);

    /* get a request buffer */
    rc = request_create(&req);

    if(rc != PLCTAG_STATUS_OK) {
    	pdebug("Unable to get new request.  rc=%d",rc);
    	p_tag->status = rc;
    	return rc;
    }

    pccc = (eip_pccc_dhp_req*)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_pccc_req);

    /* copy laa into the request */
    mem_copy(data,tag->encoded_name,tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* we need the count twice? */
    *((uint16_t*)data) = h2le16(elem_count); /* FIXME - bytes or INTs? */
    data += sizeof(uint16_t);

    /* encap fields */
    pccc->encap_command = h2le16(AB_EIP_CONNECTED_SEND);    /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    pccc->router_timeout = h2le16(1);                 /* one second timeout, enough? */

    /* Common Packet Format fields */
    pccc->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    pccc->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    pccc->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4 ? */
    //pccc->cpf_targ_conn_id = tag->connection->targ_connection_id;
    pccc->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    pccc->cpf_cdi_item_length = h2le16(data - (uint8_t*)(&(pccc->dest_link)));/* REQ: fill in with length of remaining data. */

    /* connection sequence id */
    //conn_seq_id = connection_get_new_seq_id(tag->connection);

    /*
     * FIXME
     *
     * Is this really missing?
     */
    /* pccc->cpf_conn_seq_num = h2le16(tag->connection->conn_seq_num);*/

    /* DH+ Routing */
    pccc->dest_link = 0;
    pccc->dest_node = tag->dhp_dest;
    pccc->src_link = 0;
    pccc->src_node = tag->dhp_src;

    /* PCCC Command */
    pccc->pccc_command = AB_PCCC_TYPED_CMD;
    pccc->pccc_status = 0;  /* STS 0 in request */
    pccc->pccc_seq_num = h2le16(conn_seq_id);
    pccc->pccc_function = AB_PCCC_TYPED_READ_FUNC;
    pccc->pccc_transfer_size = h2le16(elem_count); /* This is not in the docs, but it is in the data. */
                                                        /* FIXME - bytes or INTs? */

    /* get ready to add the request to the queue for this session */
    req->request_size = data - (req->data);
    req->send_request = 1;
    //req->conn_id = tag->connection->orig_connection_id;
    req->conn_seq = conn_seq_id;

    /* add the request to the session's list. */
    rc = request_add(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
    	pdebug("Unable to lock add request to session! rc=%d",rc);
    	request_destroy(&req);
    	p_tag->status = rc;
    	return rc;
    }

    /* save the request for later */
    tag->reqs[0] = req;
    tag->read_in_progress = 1;

    /* the read is now pending */
    tag->p_tag.status = PLCTAG_STATUS_PENDING;

    pdebug("Done.");

    return PLCTAG_STATUS_PENDING;
}




/* FIXME -- this needs to be converted to unconnected messaging. */
int ab_tag_read_pccc_dhp_check(plc_tag p_tag)
{
    ab_tag_p tag;
    eip_pccc_dhp_resp *pccc_resp;
    uint8_t *data;
    uint8_t *data_end;
    int pccc_res_type;
    int pccc_res_length;
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;

    pdebug("Starting");

    tag = (ab_tag_p)p_tag;

    req = tag->reqs[0];

    /* is there an outstanding request? */
     if(!req) {
 	tag->read_in_progress = 0;
     	tag->p_tag.status = PLCTAG_ERR_NULL_PTR;
     	return PLCTAG_ERR_NULL_PTR;
     }

     if(!req->resp_received) {
     	tag->p_tag.status = PLCTAG_STATUS_PENDING;
     	return PLCTAG_STATUS_PENDING;
     }


 	/*
 	 * we have a response. Remove the request/response from
 	 * the list for the session.
 	 */
 	rc = request_remove(tag->session, req);

 	if(rc != PLCTAG_STATUS_OK) {
 		pdebug("Unable to remove the request from the list! rc=%d",rc);

 		/* since we could not remove it, maybe the thread can. */
 		plc_tag_abort(p_tag);

 		/* not our request any more */
 		req = NULL;

 		tag->p_tag.status = rc;

 		return rc;
 	}


     /* fake exception */
     do {
		pccc_resp = (eip_pccc_dhp_resp*)(req->data);

		data_end = (req->data + pccc_resp->encap_length + sizeof(eip_encap_t));

		if( le2h16(pccc_resp->encap_command) != AB_EIP_CONNECTED_SEND) {
			pdebug("Unexpected EIP packet type received: %d!",pccc_resp->encap_command);
			rc = PLCTAG_ERR_BAD_DATA;
			break;
		}

		if(le2h16(pccc_resp->encap_status) != AB_EIP_OK) {
			pdebug("EIP command failed, response code: %d",pccc_resp->encap_status);
			rc = PLCTAG_ERR_REMOTE_ERR;
			break;
		}

		if(pccc_resp->pccc_status != AB_EIP_OK) {
			/*pdebug(PLC_LOG_ERR,PLC_ERR_READ, "PCCC command failed, response code: %d",pccc_resp->pccc_status);*/
			decode_pccc_error(pccc_resp->pccc_data[0]);
			rc = PLCTAG_ERR_REMOTE_ERR;
			break;
		}

		/* point to the start of the data */
		data = pccc_resp->pccc_data;

		if(!(data = ab_decode_pccc_dt_byte(data,data_end - data, &pccc_res_type,&pccc_res_length))) {
			pdebug("Unable to decode PCCC response data type and data size!");
			rc = PLCTAG_ERR_BAD_DATA;
			break;
		}

		/* this gives us the overall type of the response and the number of bytes remaining in it.
		 * If the type is an array, then we need to decode another one of these words
		 * to get the type of each element and the size of each element.  We will
		 * need to adjust the size if we care.
		 */

		if(pccc_res_type == AB_PCCC_DATA_ARRAY) {
			if(!(data = ab_decode_pccc_dt_byte(data,data_end - data, &pccc_res_type,&pccc_res_length))) {
				pdebug("Unable to decode PCCC response array element data type and data size!");
				rc = PLCTAG_ERR_BAD_DATA;
				break;
			}
		}

		/* copy data into the tag. */
		if((data_end - data) > p_tag->size) {
			rc = PLCTAG_ERR_TOO_LONG;
			break;
		}

		/* all OK, copy the data. */
		mem_copy(p_tag->data,data,data_end - data);

		rc = PLCTAG_STATUS_OK;
     } while(0);

     if(req) {
     	request_destroy(&(req));
        req = NULL;
     }

     tag->read_in_progress = 0;

     tag->p_tag.status = rc;

     pdebug("Done.");

     return rc;
}



/* FIXME -- this needs to be converted to unconnected messaging */
int ab_tag_write_pccc_dhp_start(plc_tag p_tag)
{
    ab_tag_p tag;
    eip_pccc_dhp_req *pccc;
    uint8_t *data;
    uint8_t element_def[16];
    int element_def_size;
    uint8_t array_def[16];
    int array_def_size;
    int pccc_data_type;
    int elem_size;
    int elem_count;
    int rc = PLCTAG_STATUS_OK;
    uint16_t conn_seq_id = 0;
    ab_request_p req;

    pdebug("Starting.");

    tag = (ab_tag_p)p_tag;

    /* get a request buffer */
    rc = request_create(&req);

    if(rc != PLCTAG_STATUS_OK) {
    	pdebug("Unable to get new request.  rc=%d",rc);
    	p_tag->status = rc;
    	return rc;
    }

    pccc = (eip_pccc_dhp_req*)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_pccc_req);

    /* copy laa into the request */
    mem_copy(data,tag->encoded_name,tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* What type and size do we have? */
	elem_size = attr_get_int(tag->attributes,"elem_size",0);
	elem_count = attr_get_int(tag->attributes,"elem_count",1);

	if(elem_size != 2 && elem_size != 4) {
		pdebug("Unsupported data type size: %d",elem_size);
    	request_destroy(&req);
		tag->p_tag.status = PLCTAG_ERR_NOT_ALLOWED;
		return PLCTAG_ERR_NOT_ALLOWED;
	}

	if(elem_size == 4)
		pccc_data_type = AB_PCCC_DATA_REAL;
	else
		pccc_data_type = AB_PCCC_DATA_INT;

    /* generate the data type/data size fields, first the element part so that
     * we can get the size for the array part.
     */

    if(!(element_def_size = ab_encode_pccc_dt_byte(element_def,sizeof(element_def),pccc_data_type,elem_size))) {
        pdebug("Unable to encode PCCC request array element data type and size fields!");
        request_destroy(&req);
        tag->p_tag.status = PLCTAG_ERR_ENCODE;
        return PLCTAG_ERR_ENCODE;
    }

    if(!(array_def_size = ab_encode_pccc_dt_byte(array_def,sizeof(array_def),AB_PCCC_DATA_ARRAY,element_def_size + elem_size*elem_count))) {
        pdebug("Unable to encode PCCC request data type and size fields!");
        request_destroy(&req);
        tag->p_tag.status = PLCTAG_ERR_ENCODE;
        return PLCTAG_ERR_ENCODE;
    }

    /* copy the array data first. */
    mem_copy(data,array_def,array_def_size);
    data += array_def_size;

    /* copy the element data */
    mem_copy(data,element_def,element_def_size);
    data += element_def_size;

    /* now copy the data to write */
    mem_copy(data,tag->p_tag.data, elem_size * elem_count);
    data += elem_size * elem_count;


    /* now fill in the rest of the structure. */

    /* encap fields */
    pccc->encap_command = h2le16(AB_EIP_CONNECTED_SEND);    /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    pccc->router_timeout = h2le16(1);                 /* one second timeout, enough? */

    /* Common Packet Format fields */
    pccc->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    pccc->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    pccc->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4 ? */
    //pccc->cpf_targ_conn_id = tag->connection->targ_connection_id;
    pccc->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    pccc->cpf_cdi_item_length = h2le16(data - (uint8_t*)(&(pccc->dest_link)));/* REQ: fill in with length of remaining data. */

    /* DH+ Routing */
    pccc->dest_link = 0;
    pccc->dest_node = tag->dhp_dest;
    pccc->src_link = 0;
    pccc->src_node = tag->dhp_src;

    /* PCCC Command */
    pccc->pccc_command = AB_PCCC_TYPED_CMD;
    pccc->pccc_status = 0;  /* STS 0 in request */

    //conn_seq_id = connection_get_new_seq_id(tag->connection);
    pccc->pccc_seq_num = h2le16(conn_seq_id);
    pccc->pccc_function = AB_PCCC_TYPED_WRITE_FUNC;
    /* FIXME - what should be the count here?  It is bytes, 16-bit
     * words or something else??
     *
     * Seems to be the number of elements?
     */
    pccc->pccc_transfer_size = h2le16(elem_count); /* This is not in the docs, but it is in the data. */


    /* get ready to add the request to the queue for this session */
    req->request_size = data - (req->data);
    req->send_request = 1;
    //req->conn_id = tag->connection->orig_connection_id;
    //req->conn_seq = connection_get_new_seq_id(tag->connection);

    /* add the request to the session's list. */
    rc = request_add(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
    	pdebug("Unable to lock add request to session! rc=%d",rc);
    	request_destroy(&req);
    	p_tag->status = rc;
    	return rc;
    }

    /* save the request for later */
    tag->reqs[0] = req;

    /* the write is now pending */
    tag->write_in_progress = 1;
    tag->p_tag.status = PLCTAG_STATUS_PENDING;

    pdebug("Done.");

    return PLCTAG_STATUS_PENDING;
}



/* FIXME -- this needs to be converted to unconnected messaging. */
int ab_tag_write_pccc_dhp_check(plc_tag p_tag)
{
    ab_tag_p tag;
    eip_pccc_dhp_resp *pccc_resp;
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;

    pdebug("Starting.");

    tag = (ab_tag_p)p_tag;

    req = tag->reqs[0];

    /* is there an outstanding request? */
     if(!req) {
    	 tag->write_in_progress = 0;
     	tag->p_tag.status = PLCTAG_ERR_NULL_PTR;
     	return PLCTAG_ERR_NULL_PTR;
     }

     if(!req->resp_received) {
     	tag->p_tag.status = PLCTAG_STATUS_PENDING;
     	return PLCTAG_STATUS_PENDING;
     }

 	/*
 	 * we have a response. Remove the request/response from
 	 * the list for the session.
 	 */
 	rc = request_remove(tag->session, req);

 	if(rc != PLCTAG_STATUS_OK) {
 		pdebug("Unable to remove the request from the list! rc=%d",rc);

 		/* since we could not remove it, maybe the thread can. */
 		plc_tag_abort(p_tag);

 		/* not our request any more */
 		req = NULL;

 		tag->p_tag.status = rc;

 		return rc;
 	}


 	/* fake exception */
 	do {
		pccc_resp = (eip_pccc_dhp_resp*)(req->data);

		/* check the response status */
		if( le2h16(pccc_resp->encap_command) != AB_EIP_CONNECTED_SEND) {
			pdebug("EIP unexpected response packet type: %d!",pccc_resp->encap_command);
			rc = PLCTAG_ERR_BAD_DATA;
			break;
		}

		if(le2h16(pccc_resp->encap_status) != AB_EIP_OK) {
			pdebug("EIP command failed, response code: %d",pccc_resp->encap_status);
			rc = PLCTAG_ERR_REMOTE_ERR;
			break;
		}

		if(pccc_resp->pccc_status != AB_EIP_OK) {
			/*pdebug("PCCC command failed, response code: %d",pccc_resp->pccc_status);*/
			decode_pccc_error(pccc_resp->pccc_data[0]);
			rc = PLCTAG_ERR_REMOTE_ERR;
			break;
		}

		/*if(pccc_resp->general_status != AB_EIP_OK) {
			pdebug("PCCC command failed, response code: %d",pccc_resp->general_status);
			return 0;
		}*/

		/* everything OK */
		rc = PLCTAG_STATUS_OK;
    } while(0);

    tag->write_in_progress = 0;
    tag->p_tag.status = rc;

    if(req) {
    	request_destroy(&(req));
     	req = NULL;
    }

    pdebug("Done.");

    return rc;
}





/*
 * CIP Routines
 */



/*
 * ab_tag_status_cip
 *
 * CIP-specific status.  This functions as a "tickler" routine
 * to check on the completion of async requests.
 */
int ab_tag_status_cip(plc_tag p_tag)
{
	ab_tag_p tag = (ab_tag_p)p_tag;

	if(tag->read_in_progress) {
		int rc = ab_tag_read_cip_check(p_tag);

		p_tag->status = rc;

		return rc;
	}

	if(tag->write_in_progress) {
		int rc = ab_tag_write_cip_check(p_tag);

		p_tag->status = rc;

		return rc;
	}

	/*
	 * if the session we are using is not yet connected,
	 * then return PENDING and let the tag that started
	 * the connection finish it.
	 *
	 * FIXME - if the tag that started the connection
	 * fails to connect or is aborted/destroyed, then the
	 * connection will never be created.
	 */
	if(tag->session) {
		if(!tag->session->is_connected) {
			p_tag->status = PLCTAG_STATUS_PENDING;
		} else {
			p_tag->status = PLCTAG_STATUS_OK;
		}
	}

	return p_tag->status;
}



int ab_tag_read_cip_start(plc_tag p_tag)
{
	int rc = PLCTAG_STATUS_OK;
    ab_tag_p tag;
    eip_cip_uc_req *cip;
    uint8_t *data;
    uint8_t *embed_start, *embed_end;
    int elem_count;
    ab_request_p req = NULL;
    int i;
    int byte_offset = 0;
    int overhead;
    int data_per_packet;
    int num_reqs;

    pdebug("Starting");

    /* cast the tag to the right type. */
    tag = (ab_tag_p)p_tag;


	/* how much overhead per packet? This is for the response packet, not the request  */
	overhead = sizeof(eip_cip_uc_resp) 		/* base packet size */
			   + 4;							/* abbreviated type (could be 2 bytes) */

	data_per_packet = MAX_EIP_PACKET_SIZE - overhead;

	/* we want a multiple of 4 bytes */
	data_per_packet &= 0xFFFFFFFC;

	if(data_per_packet <= 0) {
		pdebug("Unable to send request.  Packet overhead, %d bytes, is too large for packet, %d bytes!", overhead, MAX_EIP_PACKET_SIZE);
		p_tag->status = PLCTAG_ERR_TOO_LONG;
		return PLCTAG_ERR_TOO_LONG;
	}

	num_reqs = (p_tag->size + (data_per_packet-1)) / data_per_packet;

	pdebug("We need %d requests of up to %d bytes each.", num_reqs, data_per_packet);

	tag->reqs = (ab_request_p*)mem_alloc(num_reqs * sizeof(ab_request_p));

	if(!tag->reqs) {
		pdebug("Unable to get memory for request array!");
		p_tag->status = PLCTAG_ERR_NO_MEM;
		return rc;
	}

	/* store this for later */
	tag->num_requests = num_reqs;
	tag->frag_size = data_per_packet;

	/* loop over the requests */
	for(i = 0; i < tag->num_requests; i++) {
		/* get a request buffer */
		rc = request_create(&req);

		if(rc != PLCTAG_STATUS_OK) {
			pdebug("Unable to get new request.  rc=%d",rc);
			p_tag->status = rc;
			return rc;
		}

		/* point the request struct at the buffer */
		cip = (eip_cip_uc_req*)(req->data);

		/* point to the end of the struct */
		data = (req->data) + sizeof(eip_cip_uc_req);


		/*
		 * set up the embedded CIP read packet
		 * The format is:
		 *
		 * uint8_t cmd
		 * LLA formatted name
		 * uint16_t # of elements to read
		 */

		embed_start = data;

		/* set up the CIP Read request */
		*data = AB_EIP_CMD_CIP_READ_FRAG;
		data++;

		/* copy the tag name into the request */
		mem_copy(data,tag->encoded_name,tag->encoded_name_size);
		data += tag->encoded_name_size;

		/* add the count of elements to read. */
	    elem_count = attr_get_int(tag->attributes,"elem_count",1);
		*((uint16_t*)data) = h2le16(elem_count);
		data += sizeof(uint16_t);

		/* add the byte offset for this request */
		*((uint32_t*)data) = h2le32(byte_offset);
		data += sizeof(uint32_t);
		byte_offset += tag->frag_size;

		/* mark the end of the embedded packet */
		embed_end = data;

		/*
		 * after the embedded packet, we need to tell the message router
		 * how to get to the target device.
		 */

		/* Now copy in the routing information for the embedded message */
		/*
		 * routing information.  Format:
		 *
		 * uint8_t path_size
		 * uint8_t reserved (zero)
		 * uint8_t[...] path (padded to even number of entries)
		 */
		*data = (tag->conn_path_size)/2; /* in 16-bit words */
		data++;
		*data = 0;
		data++;
		mem_copy(data,tag->conn_path, tag->conn_path_size);
		data += tag->conn_path_size;

		/* now we go back and fill in the fields of the static part */

		/* encap fields */
		cip->encap_command = h2le16(AB_EIP_READ_RR_DATA);    /* ALWAYS 0x0070 Unconnected Send*/

		/* router timeout */
		cip->router_timeout = h2le16(1);                 /* one second timeout, enough? */

		/* Common Packet Format fields for unconnected send. */
		cip->cpf_item_count 		= h2le16(2);				/* ALWAYS 2 */
		cip->cpf_nai_item_type 		= h2le16(AB_EIP_ITEM_NAI);  /* ALWAYS 0 */
		cip->cpf_nai_item_length 	= h2le16(0);   				/* ALWAYS 0 */
		cip->cpf_udi_item_type		= h2le16(AB_EIP_ITEM_UDI);  /* ALWAYS 0x00B2 - Unconnected Data Item */
		cip->cpf_udi_item_length	= h2le16(data - (uint8_t*)(&(cip->cm_service_code)));  /* REQ: fill in with length of remaining data. */

		/* CM Service Request - Connection Manager */
		cip->cm_service_code = AB_EIP_CMD_UNCONNECTED_SEND;        /* 0x52 Unconnected Send */
		cip->cm_req_path_size = 2;   /* 2, size in 16-bit words of path, next field */
		cip->cm_req_path[0] = 0x20;  /* class */
		cip->cm_req_path[1] = 0x06;  /* Connection Manager */
		cip->cm_req_path[2] = 0x24;  /* instance */
		cip->cm_req_path[3] = 0x01;  /* instance 1 */

		/* Unconnected send needs timeout information */
		cip->secs_per_tick = AB_EIP_SECS_PER_TICK;	/* seconds per tick */
		cip->timeout_ticks = AB_EIP_TIMEOUT_TICKS;  /* timeout = srd_secs_per_tick * src_timeout_ticks */

		/* size of embedded packet */
		cip->uc_cmd_length = h2le16(embed_end - embed_start);

		/* set the size of the request */
		req->request_size = data - (req->data);

		/* mark it as ready to send */
		req->send_request = 1;

		/* add the request to the session's list. */
		rc = request_add(tag->session, req);

		if(rc != PLCTAG_STATUS_OK) {
			pdebug("Unable to add request to session! rc=%d",rc);
			request_destroy(&req);
			p_tag->status = rc;
			return rc;
		}

		/* save the request for later */
		tag->reqs[i] = req;
		req = NULL;
	}

	/* mark the tag read in progress */
    tag->read_in_progress = 1;

    /* the read is now pending */
    tag->p_tag.status = PLCTAG_STATUS_PENDING;

    pdebug("Done.");

    return PLCTAG_STATUS_PENDING;
}






int ab_tag_read_cip_check(plc_tag p_tag)
{
	int rc = PLCTAG_STATUS_OK;
	ab_tag_p tag = (ab_tag_p)p_tag;
	eip_cip_uc_resp *cip_resp;
    uint8_t *data;
    uint8_t *data_end;
    int i;
    ab_request_p req;
    int byte_offset = 0;

    /* is there an outstanding request? */
    if(!tag->reqs) {
    	tag->read_in_progress = 0;
    	tag->p_tag.status = PLCTAG_ERR_NULL_PTR;
    	return PLCTAG_ERR_NULL_PTR;
    }

    for(i = 0; i < tag->num_requests; i++) {
		if(tag->reqs[i] && !tag->reqs[i]->resp_received) {
			tag->p_tag.status = PLCTAG_STATUS_PENDING;
			return PLCTAG_STATUS_PENDING;
		}
    }



    /*
     * process each request.  If there is more than one request, then
     * we need to make sure that we copy the data into the right part
     * of the tag's data buffer.
     */
    for(i = 0; i < tag->num_requests; i++) {
    	req = tag->reqs[i];

    	if(!req) {
    		rc = PLCTAG_ERR_NULL_PTR;
    		break;
    	}

		/* point to the data */
		cip_resp = (eip_cip_uc_resp*)(req->data);

		data_end = (req->data + cip_resp->encap_length + sizeof(eip_encap_t));

		if(le2h16(cip_resp->encap_command) != AB_EIP_READ_RR_DATA) {
			pdebug("Unexpected EIP packet type received: %d!",cip_resp->encap_command);
			rc = PLCTAG_ERR_BAD_DATA;
			break;
		}

		if(le2h16(cip_resp->encap_status) != AB_EIP_OK) {
			pdebug("EIP command failed, response code: %d",cip_resp->encap_status);
			rc = PLCTAG_ERR_REMOTE_ERR;
			break;
		}

		if(cip_resp->reply_service != (AB_EIP_CMD_CIP_READ_FRAG | AB_EIP_CMD_CIP_OK)) {
			pdebug("CIP response reply service unexpected: %d",cip_resp->reply_service);
			rc = PLCTAG_ERR_BAD_DATA;
			break;
		}

		if(cip_resp->status != AB_CIP_STATUS_OK && cip_resp->status != AB_CIP_STATUS_FRAG) {
			pdebug("CIP read failed with status: %d",cip_resp->status);
			pdebug(ab_decode_cip_status(cip_resp->status));
			rc = PLCTAG_ERR_REMOTE_ERR;
			break;
		}

		/* point to the start of the data */
		data = (req->data)+sizeof(eip_cip_uc_resp);

		/* the first byte of the response is a type byte. */
		pdebug("type byte = %d (%x)",(int)*data,(int)*data);

		/*
		 * AB has a relatively complicated scheme for data typing.  The type is
		 * required when writing.  Most of the types are basic types and occupy
		 * a known amount of space.  Aggregate types like structs and arrays
		 * occupy a variable amount of space.  In addition, structs and arrays
		 * can be in two forms: full and abbreviated.  Full form for structs includes
		 * all data types (in full) for fields of the struct.  Abbreviated form for
		 * structs includes a two byte CRC calculated across the full form.  For arrays,
		 * full form includes index limits and base data type.  Abbreviated arrays
		 * drop the limits and encode any structs as abbreviate structs.  At least
		 * we think this is what is happening.
		 *
		 * Luckily, we do not actually care what these bytes mean, we just need
		 * to copy them and skip past them for the actual data.
		 */

		/* check for a simple/base type */
		if((*data) >= AB_CIP_DATA_BIT && (*data) <= AB_CIP_DATA_STRINGI) {
			/* copy the type info for later. */
			if(tag->encoded_type_info_size == 0) {
				tag->encoded_type_info_size = 2;
				mem_copy(tag->encoded_type_info,data,tag->encoded_type_info_size);
			}

			/* skip the type byte and zero length byte */
			data += 2;
		} else if(     (*data) == AB_CIP_DATA_ABREV_STRUCT
					|| (*data) == AB_CIP_DATA_ABREV_ARRAY
					|| (*data) == AB_CIP_DATA_FULL_STRUCT
					|| (*data) == AB_CIP_DATA_FULL_ARRAY)
		{
			/* this is an aggregate type of some sort, the type info is variable length */
			int type_length = *(data+1) + 2; /*
												   * MAGIC
												   * add 2 to get the total length including
												   * the type byte and the length byte.
												   */

			/* check for extra long types */
			if(type_length > MAX_TAG_TYPE_INFO) {
				pdebug("Read data type info is too long (%d)!", type_length);
				rc = PLCTAG_ERR_TOO_LONG;
				break;
			}

			/* copy the type info for later. */
			if(tag->encoded_type_info_size == 0) {
				tag->encoded_type_info_size = type_length;
				mem_copy(tag->encoded_type_info,data,tag->encoded_type_info_size);
			}

			data += type_length;
		} else {
			pdebug("Unsupported data type returned, type byte=%d",*data);
			rc = PLCTAG_ERR_UNSUPPORTED;
			break;
		}

		/* copy data into the tag. */
		if((byte_offset + (data_end - data)) > p_tag->size) {
			pdebug("Read data is too long (%d bytes) to fit in tag data buffer (%d bytes)!",byte_offset + (int)(data_end-data),p_tag->size);
			rc = PLCTAG_ERR_TOO_LONG;
			break;
		}

		pdebug("Got %d bytes of data", (data_end - data));

		mem_copy(p_tag->data  + byte_offset, data, (data_end - data));

		/* update byte_offset */
		byte_offset += tag->frag_size;

		/* set the return code */
		rc = PLCTAG_STATUS_OK;
    } /* end of for(i = 0; i < tag->num_requests; i++) */


    /*
     * Now remove the requests from the session's request list.
     */

    for(i = 0; i < tag->num_requests; i++) {
    	int tmp_rc;

    	req = tag->reqs[i];

    	tmp_rc = request_remove(tag->session, req);

		if(tmp_rc != PLCTAG_STATUS_OK) {
			pdebug("Unable to remove the request from the list! rc=%d",rc);

			/* since we could not remove it, maybe the thread can. */
			req->abort_request = 1;

			rc = tmp_rc;
		} else {
			/* free up the request resources */
			request_destroy(&req);
		}

		/* mark it as freed */
		tag->reqs[i] = NULL;
    }

    /* free the request array */
    mem_free(tag->reqs);
    tag->reqs = NULL;
    tag->num_requests = 0;

    tag->read_in_progress = 0;

    tag->p_tag.status = rc;

    pdebug("Done.");

    return rc;
}





int ab_tag_write_cip_start(plc_tag p_tag)
{
	int rc = PLCTAG_STATUS_OK;
    ab_tag_p tag;
    eip_cip_uc_req *cip;
    uint8_t *data;
    uint8_t *embed_start, *embed_end;
    int elem_size;
    int elem_count;
    ab_request_p req = NULL;
    int i;
    int encoded_type_size = 0;
    int byte_offset = 0;
    int overhead;
    int data_per_packet;
    int num_reqs;

    pdebug("Starting");

    /* cast the tag to the right type. */
    tag = (ab_tag_p)p_tag;

	/* What data size do we have? */
	elem_size = attr_get_int(tag->attributes,"elem_size",0);
	elem_count = attr_get_int(tag->attributes,"elem_count",1);

	/* how much overhead per packet? */
	if(tag->encoded_type_info_size) {
		encoded_type_size = tag->encoded_type_info_size;
		pdebug("got existing encoded type size of %d", encoded_type_size);
	} else {
		/*
		 * FIXME
		 *
		 * All this code should be removed.
		 */
		switch(elem_size) {
			case 1:
				encoded_type_size = 2;
				break;
			case 2:
				encoded_type_size = 2;
				break;
			case 4: /* FIXME - does this work for floats?  AB_CIP_DATA_REAL */
				encoded_type_size = 2;
				break;
			default:
				/* FIXME - handle structures! */
				pdebug("Unsupported data element size: %d!",elem_size);
				tag->p_tag.status = PLCTAG_ERR_UNSUPPORTED;
				return PLCTAG_ERR_UNSUPPORTED;
				break;
		}
	}

	overhead = sizeof(eip_cip_uc_req) 			/* base packet size */
			   + 1								/* service request, one byte */
			   + tag->encoded_name_size			/* full encoded name */
			   + encoded_type_size				/* encoded type size */
			   + tag->conn_path_size+2			/* encoded device path size plus two bytes for length and padding */
			   + 2								/* element count, 16-bit int */
			   + 4;								/* byte offset, 32-bit int */

	data_per_packet = MAX_EIP_PACKET_SIZE - overhead;

	/* we want a multiple of 4 bytes */
	data_per_packet &= 0xFFFFFFFC;

	if(data_per_packet <= 0) {
		pdebug("Unable to send request.  Packet overhead, %d bytes, is too large for packet, %d bytes!", overhead, MAX_EIP_PACKET_SIZE);
		p_tag->status = PLCTAG_ERR_TOO_LONG;
		return PLCTAG_ERR_TOO_LONG;
	}

	num_reqs = (p_tag->size + (data_per_packet-1)) / data_per_packet;

	pdebug("We need %d requests of up to %d bytes each.", num_reqs, data_per_packet);

	tag->reqs = (ab_request_p*)mem_alloc(num_reqs * sizeof(ab_request_p));

	if(!tag->reqs) {
		pdebug("Unable to get memory for request array!");
		p_tag->status = PLCTAG_ERR_NO_MEM;
		return PLCTAG_ERR_NO_MEM;
	}

	/* store this for later */
	tag->num_requests = num_reqs;
	tag->frag_size = data_per_packet;

	/* loop over the requests */
	for(i = 0; i < tag->num_requests; i++) {
		/* get a request buffer */
		rc = request_create(&req);

		if(rc != PLCTAG_STATUS_OK) {
			pdebug("Unable to get new request.  rc=%d",rc);
			p_tag->status = rc;
			return rc;
		}

		cip = (eip_cip_uc_req*)(req->data);

		/* point to the end of the struct */
		data = (req->data) + sizeof(eip_cip_uc_req);

		/*
		 * set up the embedded CIP read packet
		 * The format is:
		 *
		 * uint8_t cmd
		 * LLA formatted name
		 * data type to write
		 * uint16_t # of elements to write
		 * data to write
		 */

		embed_start = data;

		/* set up the CIP Read request */
		if(num_reqs == 1) {
			*data = AB_EIP_CMD_CIP_WRITE;
		} else {
			*data = AB_EIP_CMD_CIP_WRITE_FRAG;
		}

		data++;

		/* copy the tag name into the request */
		mem_copy(data,tag->encoded_name,tag->encoded_name_size);
		data += tag->encoded_name_size;

		/*
		 * If we have encoded type info, copy it.
		 */
		if(tag->encoded_type_info_size) {
			mem_copy(data,tag->encoded_type_info,tag->encoded_type_info_size);
			data += tag->encoded_type_info_size;
		} else {
			/*
			 * FIXME
			 *
			 * All this code should be removed.
			 */
			switch(elem_size) {
				case 1:
					*data = AB_CIP_DATA_SINT; data++;
					*data = 0; data++; /* padding */
					break;
				case 2:
					*data = AB_CIP_DATA_INT; data++;
					*data = 0; data++; /* padding */
					break;
				case 4: /* FIXME - does this work for floats?  AB_CIP_DATA_REAL */
					*data = AB_CIP_DATA_DINT; data++;
					*data = 0; data++; /* padding */
					break;
				default:
					/* FIXME - handle structures! */
					pdebug("Unsupported data element size: %d!",elem_size);
					tag->p_tag.status = PLCTAG_ERR_UNSUPPORTED;
					return PLCTAG_ERR_UNSUPPORTED;
					break;
			}
		}

		/* copy the item count, little endian */
		*((uint16_t*)data) = h2le16(elem_count);
		data += 2;

		if(num_reqs > 1) {
			/* put in the byte offset */
			*((uint32_t*)data) = h2le32(byte_offset);
			data += 4;
		}

		/* now copy the data to write */
		if(tag->frag_size < (p_tag->size - byte_offset)) {
			mem_copy(data, tag->p_tag.data + byte_offset, tag->frag_size);
			data += tag->frag_size;

			/* no need for padding here as frag_size is a multiple of 4. */
		} else {
			mem_copy(data, tag->p_tag.data + byte_offset, p_tag->size - byte_offset);
			data += p_tag->size - byte_offset;

			/* need to pad data to multiple of 16-bits */
			if((p_tag->size - byte_offset) & 0x01) {
				*data = 0;
				data++;
			}
		}


		/* mark the end of the embedded packet */
		embed_end = data;

		byte_offset += tag->frag_size;


		/*
		 * after the embedded packet, we need to tell the message router
		 * how to get to the target device.
		 */

		/* Now copy in the routing information for the embedded message */
		*data = (tag->conn_path_size)/2; /* in 16-bit words? */
		data++;
		*data = 0;
		data++;
		mem_copy(data,tag->conn_path, tag->conn_path_size);
		data += tag->conn_path_size;

		/* now fill in the rest of the structure. */

		/* encap fields */
		cip->encap_command = h2le16(AB_EIP_READ_RR_DATA);    /* ALWAYS 0x006F Unconnected Send*/

		/* router timeout */
		cip->router_timeout = h2le16(1);                 /* one second timeout, enough? */

		/* Common Packet Format fields for unconnected send. */
		cip->cpf_item_count 		= h2le16(2);				/* ALWAYS 2 */
		cip->cpf_nai_item_type 		= h2le16(AB_EIP_ITEM_NAI);  /* ALWAYS 0 */
		cip->cpf_nai_item_length 	= h2le16(0);   				/* ALWAYS 0 */
		cip->cpf_udi_item_type		= h2le16(AB_EIP_ITEM_UDI);  /* ALWAYS 0x00B2 - Unconnected Data Item */
		cip->cpf_udi_item_length	= h2le16(data - (uint8_t*)(&(cip->cm_service_code)));  /* REQ: fill in with length of remaining data. */

		/* CM Service Request - Connection Manager */
		cip->cm_service_code = AB_EIP_CMD_UNCONNECTED_SEND;        /* 0x52 Unconnected Send */
		cip->cm_req_path_size = 2;   /* 2, size in 16-bit words of path, next field */
		cip->cm_req_path[0] = 0x20;  /* class */
		cip->cm_req_path[1] = 0x06;  /* Connection Manager */
		cip->cm_req_path[2] = 0x24;  /* instance */
		cip->cm_req_path[3] = 0x01;  /* instance 1 */

		/* Unconnected send needs timeout information */
		cip->secs_per_tick = AB_EIP_SECS_PER_TICK;	/* seconds per tick */
		cip->timeout_ticks = AB_EIP_TIMEOUT_TICKS;  /* timeout = srd_secs_per_tick * src_timeout_ticks */

		/* size of embedded packet */
		cip->uc_cmd_length = h2le16(embed_end - embed_start);

		/* set the size of the request */
		req->request_size = data - (req->data);

		/* mark it as ready to send */
		req->send_request = 1;

		/* add the request to the session's list. */
		rc = request_add(tag->session, req);

		if(rc != PLCTAG_STATUS_OK) {
			pdebug("Unable to lock add request to session! rc=%d",rc);
			plc_tag_abort(p_tag);
			request_destroy(&req);
			p_tag->status = rc;
			return rc;
		}

		/* save the request for later */
		tag->reqs[i] = req;
	} /* end for(i = 0; i < tag->num_reqs; i++) ... */

    /* the write is now pending */
    tag->write_in_progress = 1;
    tag->p_tag.status = PLCTAG_STATUS_PENDING;

    pdebug("Done.");

    return PLCTAG_STATUS_PENDING;
}






int ab_tag_write_cip_check(plc_tag p_tag)
{
    ab_tag_p tag;
    eip_cip_uc_resp *cip_resp;
    int rc = PLCTAG_STATUS_OK;
    int i;
    ab_request_p req;

    tag = (ab_tag_p)p_tag;

    /* is there an outstanding request? */
    if(!tag->reqs) {
    	tag->write_in_progress = 0;
    	tag->p_tag.status = PLCTAG_ERR_NULL_PTR;
    	return PLCTAG_ERR_NULL_PTR;
    }

    for(i = 0; i < tag->num_requests; i++) {
		if(tag->reqs[i] && !tag->reqs[i]->resp_received) {
			tag->p_tag.status = PLCTAG_STATUS_PENDING;
			return PLCTAG_STATUS_PENDING;
		}
    }



    /*
     * process each request.  If there is more than one request, then
     * we need to make sure that we copy the data into the right part
     * of the tag's data buffer.
     */
    for(i = 0; i < tag->num_requests; i++) {
    	int reply_service;

    	req = tag->reqs[i];

    	if(!req) {
    		rc = PLCTAG_ERR_NULL_PTR;
    		break;
    	}

		/* point to the data */
		cip_resp = (eip_cip_uc_resp*)(req->data);

		if(le2h16(cip_resp->encap_command) != AB_EIP_READ_RR_DATA) {
			pdebug("Unexpected EIP packet type received: %d!",cip_resp->encap_command);
			rc = PLCTAG_ERR_BAD_DATA;
			break;
		}

		if(le2h16(cip_resp->encap_status) != AB_EIP_OK) {
			pdebug("EIP command failed, response code: %d",cip_resp->encap_status);
			rc = PLCTAG_ERR_REMOTE_ERR;
			break;
		}

    	/* if we have fragmented the request, we need to look for a different return code */
		reply_service = ((tag->num_requests > 1) ? (AB_EIP_CMD_CIP_WRITE_FRAG | AB_EIP_CMD_CIP_OK) : (AB_EIP_CMD_CIP_WRITE | AB_EIP_CMD_CIP_OK));

		if(cip_resp->reply_service != reply_service) {
			pdebug("CIP response reply service unexpected: %d",cip_resp->reply_service);
			rc = PLCTAG_ERR_BAD_DATA;
			break;
		}

		if(cip_resp->status != AB_CIP_STATUS_OK && cip_resp->status != AB_CIP_STATUS_FRAG) {
			pdebug("CIP read failed with status: %d",cip_resp->status);
			pdebug(ab_decode_cip_status(cip_resp->status));
			rc = PLCTAG_ERR_REMOTE_ERR;
			break;
		}
    }

    /*
     * Now remove the requests from the session's request list.
     */
    for(i = 0; i < tag->num_requests; i++) {
    	int tmp_rc;

    	req = tag->reqs[i];

    	tmp_rc = request_remove(tag->session, req);

		if(tmp_rc != PLCTAG_STATUS_OK) {
			pdebug("Unable to remove the request from the list! rc=%d",rc);

			/* since we could not remove it, maybe the thread can. */
			req->abort_request = 1;

			rc = tmp_rc;
		} else {
			/* free up the request resources */
			request_destroy(&req);
		}

		/* mark it as freed */
		tag->reqs[i] = NULL;
    }

    /* free the request array */
    mem_free(tag->reqs);
    tag->reqs = NULL;
    tag->num_requests = 0;

    tag->write_in_progress = 0;
    tag->p_tag.status = rc;

    pdebug("Done.");

    return rc;
}






/**************************************************************************
 ************************** Helper Functions ******************************
 *************************************************************************/



/*
 * check_mutex
 *
 * check to see if the global mutex is set up.  If not, do an atomic
 * lock and set it up.
 */
int check_mutex()
{
	int rc = PLCTAG_STATUS_OK;

	/* loop until we get the lock flag */
	while(!lock_acquire(&tag_mutex_lock)) {
		sleep_ms(1);
	}

	/* first see if the mutex is there. */
	if(!tag_mutex) {
		rc = mutex_create(&tag_mutex);
		if(rc != PLCTAG_STATUS_OK) {
			pdebug("Unable to create global tag mutex!");
		}
	}

	/* we hold the lock, so clear it.*/
	lock_release(&tag_mutex_lock);

	return rc;
}


/*
 * session_get_new_connection_id
 *
 * A wrapper function to get a new connection ID from a session.
 */

/*uint32_t session_get_new_connection_id_unsafe(ab_session_p session)
{
	return session->connection_id++;
}*/



/*
 * session_get_new_connection_id
 *
 * A thread-safe function to get a new connection ID.
 */

/*uint32_t session_get_new_connection_id(ab_session_p session)
{
	uint32_t res = 0;

	critical_block(tag_mutex) {
		res = session_get_new_connection_id_unsafe(session);
	}

	return res;
}*/


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
	uint16_t res;

	critical_block(tag_mutex) {
		res = session_get_new_seq_id_unsafe(sess);
	}

	return res;
}




/*
 * request_create
 *
 * This does not do much for now other than allocate memory.  In the future
 * it may be desired to keep a pool of request buffers instead.  This shim
 * is here so that such a change can be done without major code changes
 * elsewhere.
 */
int request_create(ab_request_p *req)
{
	int rc = PLCTAG_STATUS_OK;
	ab_request_p res;

	res = (ab_request_p)mem_alloc(sizeof(struct ab_request_t));

	if(!res) {
		*req = NULL;
		rc = PLCTAG_ERR_NO_MEM;
	} else {
		*req = res;
	}

	return rc;
}


/*
 * request_add_unsafe
 *
 * You must hold the mutex before calling this!
 */
int request_add_unsafe(ab_session_p sess, ab_request_p req)
{
	int rc = PLCTAG_STATUS_OK;
	ab_request_p cur,prev;

	/* make sure the request points to the session */
	req->session = sess;

	/* we add the request to the end of the list. */
	cur = sess->requests;
	prev = NULL;

	while(cur) {
		prev = cur;
		cur = cur->next;
	}

	if(!prev) {
		sess->requests = req;
	} else {
		prev->next = req;
	}

	return rc;
}




/*
 * request_add
 *
 * This is a thread-safe version of the above routine.
 */
int request_add(ab_session_p sess, ab_request_p req)
{
	int rc = PLCTAG_STATUS_OK;

	critical_block(tag_mutex) {
		rc = request_add_unsafe(sess,req);
	}

	return rc;
}



/*
 * request_remove_unsafe
 *
 * You must hold the mutex before calling this!
 */
int request_remove_unsafe(ab_session_p sess, ab_request_p req)
{
	int rc = PLCTAG_STATUS_OK;
	ab_request_p cur,prev;

	/* find the request and remove it from the list. */
	cur = sess->requests;
	prev = NULL;

	while(cur && cur != req) {
		prev = cur;
		cur = cur->next;
	}

	if(cur == req) {
		if(!prev) {
			sess->requests = cur->next;
		} else {
			prev->next = cur->next;
		}
	} /* else not found */

	req->next = NULL;

	return rc;
}





/*
 * request_remove
 *
 * This is a thread-safe version of the above routine.
 */
int request_remove(ab_session_p sess, ab_request_p req)
{
	int rc = PLCTAG_STATUS_OK;

	critical_block(tag_mutex) {
		rc = request_remove_unsafe(sess,req);
	}

	return rc;
}





/*
 * request_destroy
 *
 * The request must be removed from any lists before this!
 */
int request_destroy(ab_request_p *req)
{
	if(req && *req) {
		mem_free(*req);
		*req = NULL;
	}

	return PLCTAG_STATUS_OK;
}




int ab_check_cpu(ab_tag_p tag)
{
    const char *cpu_type = attr_get_str(tag->attributes,"cpu","NONE");

    if(    !str_cmp_i(cpu_type,"plc")
        || !str_cmp_i(cpu_type,"plc5")
        || !str_cmp_i(cpu_type,"slc")
        || !str_cmp_i(cpu_type,"slc500")) { tag->protocol_type = AB_PROTOCOL_PLC; }
    else if(!str_cmp_i(cpu_type,"micrologix")
        || !str_cmp_i(cpu_type,"mlgx")) { tag->protocol_type = AB_PROTOCOL_MLGX; }
    else if(!str_cmp_i(cpu_type,"compactlogix")
        || !str_cmp_i(cpu_type,"clgx")
        || !str_cmp_i(cpu_type,"lgx")
        || !str_cmp_i(cpu_type,"controllogix")
        || !str_cmp_i(cpu_type,"contrologix")
        || !str_cmp_i(cpu_type,"flexlogix")
        || !str_cmp_i(cpu_type,"flgx")) { tag->protocol_type = AB_PROTOCOL_LGX; }
    else {
        pdebug("Unsupported device type: %s",cpu_type);

    	return PLCTAG_ERR_BAD_DEVICE;
    }

    return PLCTAG_STATUS_OK;
}






int find_or_create_session(ab_tag_p tag)
{
    const char *session_gw = attr_get_str(tag->attributes,"gateway","");
    int session_gw_port = attr_get_int(tag->attributes,"gateway_port",AB_EIP_DEFAULT_PORT);
    ab_session_p session;
    int shared_session = attr_get_int(tag->attributes,"share_session",1); /* share the session by default. */

    /* if we are to share sessions, then look for an existing one. */
    if(shared_session) {
    	session = find_session_by_host_unsafe(session_gw);
    } else {
    	/* no sharing, create a new one */
    	session = AB_SESSION_NULL;
    }

    if(session == AB_SESSION_NULL) {
        session = ab_session_create(session_gw, session_gw_port);
    } else {
        pdebug("find_or_create_session() reusing existing session.");
    }

    if(session == AB_SESSION_NULL) {
        pdebug("unable to create or find a session!");

    	return PLCTAG_ERR_BAD_GATEWAY;
    }

    tag->session = session;

    return PLCTAG_STATUS_OK;
}






int check_tag_name(ab_tag_p tag)
{
	const char *name = attr_get_str(tag->attributes,"name","NONE");

	if(str_cmp(name,"NONE") == 0) {
		return PLCTAG_ERR_BAD_PARAM;
	}

    /* attempt to parse the tag name */
    switch(tag->protocol_type) {
        case AB_PROTOCOL_PLC:
        case AB_PROTOCOL_MLGX:
            if(!ab_make_laa(tag->encoded_name,&(tag->encoded_name_size),name,MAX_TAG_NAME)) {
                pdebug("parse of PCCC-style tag name %s failed!",name);

                return PLCTAG_ERR_BAD_PARAM;
            }

            break;
        case AB_PROTOCOL_LGX:
            if(!convert_tag_name(tag,name)) {
                pdebug("parse of CIP-style tag name %s failed!",name);

                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        default:
            /* how would we get here? */
            pdebug("unsupported protocol %d",tag->protocol_type);

            return PLCTAG_ERR_BAD_PARAM;

            break;
    }

    return PLCTAG_STATUS_OK;
}







int send_eip_request(ab_request_p req)
{
	int rc;

	pdebug("Starting.");

	/* if we have not already started, then start the send */
	if(!req->send_in_progress) {
		eip_encap_t *encap;
		int payload_size = req->request_size - sizeof(eip_encap_t);

		/* set up the session sequence ID for this transaction */
		req->session->session_seq_id++;
		req->session_seq_id = req->session->session_seq_id;

		/* set up the rest of the request */
		req->current_offset = 0; /* nothing written yet */

		encap = (eip_encap_t*)(req->data);

		/* fill in the header fields. */
		encap->encap_length              = h2le16(payload_size);
		encap->encap_session_handle      = req->session->session_handle;
		encap->encap_status              = h2le32(0);
		encap->encap_sender_context 	 = req->session_seq_id; /* link up the request seq ID and the packet seq ID */
		encap->encap_options             = h2le32(0);

		/* display the data */
		pdebug_dump_bytes(req->data,req->request_size);

		req->send_in_progress = 1;
	}

	/* send the packet */
	rc = socket_write(req->session->sock,req->data + req->current_offset,req->request_size - req->current_offset);

	if(rc >= 0) {
		req->current_offset += rc;

		/* are we done? */
		if(req->current_offset >= req->request_size) {
			req->send_request = 0;
			req->send_in_progress = 0;
			req->current_offset = 0;

			/* set this request up for a receive action */
			req->recv_in_progress = 1;
		}

		rc = PLCTAG_STATUS_OK;
	} else {
		/* oops, error of some sort. */
		req->status = rc;
		req->send_request = 0;
		req->send_in_progress = 0;
		req->recv_in_progress = 0;
	}

	return rc;
}



/*
 * recv_eip_response
 *
 * Look at the passed session and read any data we can
 * to fill in a packet.  If we already have a full packet,
 * punt.
 */
int recv_eip_response(ab_session_p session)
{
	int data_needed = 0;
	int rc = PLCTAG_STATUS_OK;

	/*
	 * Determine the amount of data to get.  At a minimum, we
	 * need to get an encap header.  This will determine
	 * whether we need to get more data or not.
	 */
	if(session->recv_offset < sizeof(eip_encap_t)) {
		data_needed = sizeof(eip_encap_t);
	} else {
		data_needed = sizeof(eip_encap_t) + ((eip_encap_t*)(session->recv_data))->encap_length;
	}

	if(session->recv_offset < data_needed) {
		/* read everything we can */
		do {
			rc = socket_read(session->sock, session->recv_data + session->recv_offset, data_needed - session->recv_offset);

			if(rc < 0) {
				if(rc != PLCTAG_ERR_NO_DATA) {
					/* error! */
					pdebug("Error reading socket! rc=%d",rc);
					return rc;
				}
			} else {
				session->recv_offset += rc;

				/* recalculate the amount of data needed if we have just completed the read of an encap header */
				if(session->recv_offset >= sizeof(eip_encap_t)) {
					data_needed = sizeof(eip_encap_t) + ((eip_encap_t*)(session->recv_data))->encap_length;
				}
			}
		} while(rc > 0 && session->recv_offset < data_needed);
	}


	/* did we get all the data? */
	if(session->recv_offset >= data_needed) {
		pdebug("got full packet of size %d",session->recv_offset);
		pdebug_dump_bytes(session->recv_data,session->recv_offset);

		session->resp_seq_id = ((eip_encap_t*)(session->recv_data))->encap_sender_context;
		session->has_response = 1;

		if(session->resp_seq_id == 0) {
			pdebug("Got zero response ID");
		}
	}

	return rc;
}









int session_check_incoming_data(ab_session_p session)
{
	int rc = PLCTAG_STATUS_OK;

	/*
	 * if there is no current received sequence ID, then
	 * see if we can get some data.
	 */
	if(!session->has_response) {
		rc = recv_eip_response(session);

		/* NO_DATA just means that there was nothing to read yet. */
		if(rc == PLCTAG_ERR_NO_DATA || rc >= 0) {
			rc = PLCTAG_STATUS_OK;
		}

		if(rc != PLCTAG_STATUS_PENDING && rc != PLCTAG_STATUS_OK) {
			/* error! */
			/* FIXME */
		}
	}

	/*
	 * we may have read in enough data above to finish off a
	 * response packet. If so, process it.
	 */
	if(session->has_response){
		/* we got a response, so decrement the number of messages in flight counter */
		/*session->num_reqs_in_flight--;
		pdebug("num_reqs_in_flight=%d",session->num_reqs_in_flight);*/

		/* find the request for which there is a response pending. */
		ab_request_p tmp = session->requests;

		while(tmp) {
			eip_encap_t *encap = (eip_encap_t *)(session->recv_data);

			/*
			 * if this is a connected send response, we can look at the
			 * connection sequence ID and connection ID to see if this
			 * response is for the request.
			 *
			 * FIXME - it appears that PCCC/DH+ requests do not have the cpf_conn_seq_num
			 * field.  Use the PCCC sequence in those cases??  How do we tell?
			 */
			if(encap->encap_command == AB_EIP_CONNECTED_SEND) {
				eip_cip_resp *resp = (eip_cip_resp *)(session->recv_data);

				if(resp->cpf_orig_conn_id == tmp->conn_id && resp->cpf_conn_seq_num == tmp->conn_seq) {
					break;
				}
			} else {
				/*
				 * the only place we use this is during a Forward Open/Close.
				 * Check the status on it too.
				 */
				if(encap->encap_sender_context != 0 && encap->encap_sender_context == tmp->session_seq_id) {
					break;
				}
			}
			tmp = tmp->next;
		}

		if(tmp) {
			/* copy the data from the session's buffer */
			mem_copy(tmp->data, session->recv_data, session->recv_offset);

			tmp->resp_received = 1;
			tmp->send_in_progress = 0;
			tmp->send_request = 0;
			tmp->request_size = session->recv_offset;
		} else {
			pdebug("Response for unknown request.");
		}

		/*
		 * if we did not find a request, it may have already been aborted, so
		 * just clean up.
		 */

		/* reset the session's buffer */
		mem_set(session->recv_data, 0, MAX_REQ_RESP_SIZE);
		session->recv_offset = 0;
		session->resp_seq_id = 0;
		session->has_response = 0;
	}

	return rc;
}



int request_check_outgoing_data(ab_session_p session, ab_request_p req)
{
	int rc = PLCTAG_STATUS_OK;

	/*
	 * Check to see if we can send something.
	 */

	if(!session->current_request && req->send_request /*&& session->num_reqs_in_flight < MAX_REQS_IN_FLIGHT*/) {
		/* nothing being sent and this request is outstanding */
		session->current_request = req;

		/*session->num_reqs_in_flight++;
		pdebug("num_reqs_in_flight=%d",session->num_reqs_in_flight);*/
	}

	/* if we are already sending this request, check its status */
	if(session->current_request == req) {
		/* is the request done? */
		if(req->send_request) {
			/* not done, try sending more */
			send_eip_request(req);
			/* FIXME - handle return code! */
		} else {
			/*
			 * done in some manner, remove it from the session to let
			 * another request get sent.
			 */
			session->current_request = NULL;
		}
	}

	return rc;
}




void *request_handler_func(void *not_used)
{
	int rc;
	ab_session_p cur_sess;

	while(1) {
		/* we need the mutex */
		if(tag_mutex == NULL) {
			pdebug("tag_mutex is NULL!");
			break;
		}

		//pdebug("Locking mutex");

		critical_block(tag_mutex) {
			/*
			 * loop over the sessions.  For each session, see if we can read some
			 * data.  If we can, read it in and try to update a request.  If the
			 * session has outstanding requests that need to be sent, try to send
			 * them.
			 */

			cur_sess = sessions;

			while(cur_sess) {
				ab_request_p cur_req;
				ab_request_p prev_req;

				/* check for incoming data. */
				rc = session_check_incoming_data(cur_sess);

				if(rc != PLCTAG_STATUS_OK) {
					pdebug("Error when checking for incoming session data! %d",rc);
					/* FIXME - do something useful with this error */
				}

				/* loop over the requests in the session */
				cur_req = cur_sess->requests;
				prev_req = NULL;

				while(cur_req) {
					/* check for abort before anything else. */
					if(cur_req->abort_request) {
						ab_request_p tmp;

						/*
						 * is this in the process of being sent?
						 * if so, abort the abort because otherwise we would send
						 * a partial packet and cause all kinds of problems.
						 * FIXME
						 */
						if(cur_sess->current_request != cur_req) {
							if(prev_req) {
								prev_req->next = cur_req->next;
							} else {
								/* cur is head of list */
								cur_sess->requests = cur_req->next;
							}

							tmp = cur_req;
							cur_req = cur_req->next;

							/* free the the request */
							request_destroy(&tmp);

							continue;
						}
					}

					rc = request_check_outgoing_data(cur_sess, cur_req);

					/* move to the next request */
					prev_req = cur_req;
					cur_req = cur_req->next;
				}

				/*  move to the next session */
				cur_sess = cur_sess->next;
			}
		} /* end synchronized block */

		/* give up the CPU */
		sleep_ms(1);
	}

	thread_stop();

	return NULL;
}



int add_session_unsafe(ab_session_p n)
{
    if(!n)
        return PLCTAG_ERR_NULL_PTR;

    n->prev = NULL;
	n->next = sessions;

    if(sessions) {
        sessions->prev = n;
    }

    sessions = n;

    return PLCTAG_STATUS_OK;
}


int add_session(ab_session_p s)
{
	int rc;

	pdebug("Locking mutex");
	critical_block(tag_mutex) {
		rc = add_session_unsafe(s);
	}

	return rc;
}




int remove_session_unsafe(ab_session_p n)
{
	ab_session_p tmp;

    if(!n || !sessions)
    	return 0;

    tmp = sessions;

    while(tmp && tmp != n)
    	tmp = tmp->next;

    if(!tmp || tmp != n) {
    	return PLCTAG_ERR_NOT_FOUND;
    }

    if(n->next) {
    	n->next->prev = n->prev;
    }

    if(n->prev) {
    	n->prev->next = n->next;
    } else {
    	sessions = n->next;
    }

    n->next = NULL;
    n->prev = NULL;

    return PLCTAG_STATUS_OK;
}







int remove_session(ab_session_p s)
{
	int rc;

	pdebug("Locking mutex");
	critical_block(tag_mutex) {
		rc = remove_session_unsafe(s);
	}
	pdebug("Mutex released");

	return rc;
}





ab_session_p find_session_by_host_unsafe(const char  *t)
{
	ab_session_p tmp;
	int count = 0;

	tmp = sessions;

	while(tmp && str_cmp_i(tmp->host,t)) {
		tmp=tmp->next;
		count++;
	}

	pdebug("found %d sessions",count);

    if(!tmp) {
    	return (ab_session_p)NULL;
    }

    return tmp;
}


/* not threadsafe */
int session_add_tag_unsafe(ab_session_p session, ab_tag_p tag)
{
	tag->next = session->tags;
	session->tags = tag;

	return PLCTAG_STATUS_OK;
}


/* not threadsafe */
int session_remove_tag_unsafe(ab_session_p session, ab_tag_p tag)
{
	ab_tag_p tmp, prev;
	int count = 0;

	tmp = session->tags;
	prev = NULL;

	while(tmp && tmp != tag) {
		prev = tmp;
		tmp = tmp->next;
		count++;
	}

	pdebug("found %d tags",count);

	if(tmp) {
		if(!prev) {
			session->tags = tmp->next;
		} else {
			prev->next = tmp->next;
		}
	}

	return PLCTAG_STATUS_OK;
}



ab_session_p ab_session_create(const char *host, int gw_port)
{
    ab_session_p session = AB_SESSION_NULL;

    pdebug("Starting");

    session = (ab_session_p)mem_alloc(sizeof(struct ab_session_t));

    if(!session) {
        pdebug("alloc failed errno: %d",errno);

        return AB_SESSION_NULL;
    }

    str_copy(session->host,host,MAX_SESSION_HOST);

    /* we must connect to the gateway and register */
    if(!ab_session_connect(session,host)) {
        mem_free(session);
        pdebug("session connect failed!");
        return AB_SESSION_NULL;
    }

    if(!ab_session_register(session)) {
        ab_session_destroy_unsafe(session);
        pdebug("session registration failed!");
        return AB_SESSION_NULL;
    }

    /*
     * We assume that we are running in a threaded environment,
     * so every session will have a different address.
     *
     * FIXME - is this needed?
     */
    session->session_seq_id = 0;

    /* this is called while the mutex is held */
    add_session_unsafe(session);

    pdebug("Done.");

    return session;
}





/*
 * ab_session_connect()
 *
 * Connect to the host/port passed via TCP.  Set all the relevant fields in
 * the passed session object.
 */

int ab_session_connect(ab_session_p session, const char *host)
{
	int rc;

    pdebug("Starting.");

    /* Open a socket for communication with the gateway. */
    rc = socket_create(&(session->sock));

    if(rc) {
    	pdebug("Unable to create socket for session!");
    	return 0;
    }

    rc = socket_connect_tcp(session->sock, host, AB_EIP_DEFAULT_PORT);
	
    if(rc != PLCTAG_STATUS_OK) {
    	pdebug("Unable to connect socket for session!");
    	return 0;
    }

    /* everything is OK.  We have a TCP stream open to a gateway. */
    session->is_connected = 1;

    pdebug("Done.");
	
    return 1;
}



int ab_session_destroy_unsafe(ab_session_p session)
{
    pdebug("Starting.");

    if(!session)
        return 1;

    /* do not destroy the session if there are
     * connections still */
    if(session->tags) {
        pdebug("Attempt to destroy session while open tags exist!");
        return 0;
    }

    if(ab_session_unregister(session))
        return 0;

    /* close the socket. */
    if(session->is_connected) {
		socket_close(session->sock);
		socket_destroy(&(session->sock));
        session->is_connected = 0;
    }

    remove_session_unsafe(session);

    mem_free(session);

    pdebug("Done.");

    return 1;
}

int ab_session_destroy(ab_session_p session) {
	int rc;

	critical_block(tag_mutex) {
		rc = ab_session_destroy_unsafe(session);
	}

	return rc;
}



int ab_session_empty(ab_session_p session)
{
    return (session->tags == NULL);
}





int ab_session_register(ab_session_p session)
{
    eip_session_reg_req *req;
    eip_encap_t *resp;
    int rc;
    int data_size = 0;
    uint64_t timeout_time;

    pdebug("Starting.");

    /*
     * clear the session data.
     *
     * We use the receiving buffer because we do not have a request and nothing can
     * be coming in (we hope) on the socket yet.
     */
    mem_set(session->recv_data, 0, sizeof(eip_session_reg_req));

    req = (eip_session_reg_req *)(session->recv_data);

    /* fill in the fields of the request */
    req->encap_command 			= h2le16(AB_EIP_REGISTER_SESSION);
    req->encap_length           = h2le16(sizeof(eip_session_reg_req) - sizeof(eip_encap_t));
	req->encap_session_handle   = session->session_handle;
	req->encap_status           = h2le32(0);
	req->encap_sender_context   = (uint64_t)0;
	req->encap_options          = h2le32(0);

    req->eip_version   			= h2le16(AB_EIP_VERSION);
    req->option_flags  			= 0;

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

    pdebug("sending data:");
    pdebug_dump_bytes(session->recv_data, data_size);

    while(timeout_time > time_ms() && session->recv_offset < data_size) {
    	rc = socket_write(session->sock, session->recv_data + session->recv_offset, data_size - session->recv_offset);

    	if(rc < 0) {
			pdebug("Unable to send session registration packet! rc=%d",rc);
		    session->recv_offset = 0;
			return rc;
		}

    	session->recv_offset += rc;

    	/* don't hog the CPU */
    	if(session->recv_offset < data_size) {
    		sleep_ms(1);
    	}
    }

    if(session->recv_offset != data_size) {
        session->recv_offset = 0;;
    	return PLCTAG_ERR_TIMEOUT;
    }

    /* get the response from the gateway */

    /* ready the input buffer */
    session->recv_offset = 0;
    mem_set(session->recv_data, 0, MAX_REQ_RESP_SIZE);

    timeout_time = time_ms() + 5000; /* MAGIC */
    while(timeout_time > time_ms()) {
    	if(session->recv_offset < sizeof(eip_encap_t)) {
    		data_size = sizeof(eip_encap_t);
    	} else {
    		data_size = sizeof(eip_encap_t) + ((eip_encap_t*)(session->recv_data))->encap_length;
    	}

    	if(session->recv_offset < data_size) {
    		rc = socket_read(session->sock, session->recv_data + session->recv_offset, data_size - session->recv_offset);

    		if(rc < 0) {
    			if(rc != PLCTAG_ERR_NO_DATA) {
					/* error! */
					pdebug("Error reading socket! rc=%d",rc);
					return rc;
    			}
    		} else {
    			session->recv_offset += rc;

    			/* recalculate the amount of data needed if we have just completed the read of an encap header */
    			if(session->recv_offset >= sizeof(eip_encap_t)) {
    				data_size = sizeof(eip_encap_t) + ((eip_encap_t*)(session->recv_data))->encap_length;
    			}
    		}
    	}

    	/* did we get all the data? */
    	if(session->recv_offset == data_size) {
    		break;
    	} else {
    		/* do not hog the CPU */
    		sleep_ms(1);
    	}
    }


    if(session->recv_offset != data_size) {
        session->recv_offset = 0;
    	return PLCTAG_ERR_TIMEOUT;
    }

    /* set the offset back to zero for the next packet */
    session->recv_offset = 0;


    pdebug("received response:");
    pdebug_dump_bytes(session->recv_data, data_size);


    /* encap header is at the start of the buffer */
    resp = (eip_encap_t *)(session->recv_data);

    /* check the response status */
    if(le2h16(resp->encap_command) != AB_EIP_REGISTER_SESSION) {
        pdebug("EIP unexpected response packet type: %d!",resp->encap_command);
        return PLCTAG_ERR_BAD_DATA;
    }

    if(le2h16(resp->encap_status) != AB_EIP_OK) {
        pdebug("EIP command failed, response code: %d",resp->encap_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* after all that, save the session handle, we will
     * use it in future packets.
     */
    session->session_handle = resp->encap_session_handle; /* opaque to us */

    pdebug("Done.");

    return 1;
}




int ab_session_unregister(ab_session_p session)
{
    pdebug("Starting.");

    if(session->sock) {
    	socket_close(session->sock);
    	socket_destroy(&(session->sock));
    	session->sock = NULL;
    	session->is_connected = 0;
    }

    pdebug("Done.");

    return PLCTAG_STATUS_OK;
}








/*
 * convert_path_to_ioi()
 *
 * This function takes a path string of comma separated components that are numbers or
 * colon-separated triples that designate a DHP connection.  It converts the path
 * into a path segment in the passed ag.
 *
 * If the protocol type is for a PLC5 series and the last hop in the path is
 * DH+, then we need to set up a different message routing path.
 */

int convert_path_to_ioi(ab_tag_p tag, const char *path)
{
    int ioi_size=0;
    int link_index=0;
    int last_is_dhp=0;
    int has_dhp=0;
    char dhp_channel;
    int src_addr=0, dest_addr=0;
    int tmp=0;
    char **links=NULL;
    char *link=NULL;
    uint8_t *data = tag->conn_path;

    /* split the path */
    links = str_split(path,",");
    if(links == NULL) {
    	return PLCTAG_ERR_BAD_PARAM;
    }

    /* work along each string. */
    link = links[link_index];
    while(link && ioi_size < (MAX_CONN_PATH-2)) {   /* MAGIC -2 to allow for padding */
        if(sscanf(link,"%c:%d:%d",&dhp_channel,&src_addr,&dest_addr) == 3) {
            /* DHP link */
            switch(dhp_channel) {
                case 'a':
                case 'A':
                case '2':
                    dhp_channel = 1;
                    break;
                case 'b':
                case 'B':
                case '3':
                    dhp_channel = 2;
                    break;
                default:
                    /* unknown port! */
                	if(links) mem_free(links);
                    return PLCTAG_ERR_BAD_PARAM;
                    break;
            }

            last_is_dhp = 1;
            has_dhp = 1;
        } else {
            last_is_dhp = 0;

            if(str_to_int(link, &tmp) != 0) {
            	if(links) mem_free(links);
                return PLCTAG_ERR_BAD_PARAM;
            }

            *data = tmp;

            /*printf("convert_links() link(%d)=%s (%d)\n",i,*links,tmp);*/

            data++;
            ioi_size++;
        }
        /* FIXME - handle case where IP address is in path */

        link_index++;
        link = links[link_index];
    }

    /* we do not need the split string anymore. */
    if(links) {
    	mem_free(links);
    	links = NULL;
    }

    /*
     * zero out the last byte if we need to.
     * This pads out the path to a multiple of 16-bit
     * words.
     */
    if(ioi_size & 0x01) {
        *data = 0;
        ioi_size++;
    }

    /* set the connection path size */
    tag->conn_path_size = ioi_size;

   /* Add to the path based on the protocol type and
     * whether the last part is DH+.  Only some combinations of
     * DH+ and PLC type work.
     */
    if(last_is_dhp && tag->protocol_type == AB_PROTOCOL_PLC) {
        /* We have to make the difference from the more
         * generic case.
         */
        tag->routing_path[0] = 0x20; /* class */
        tag->routing_path[1] = 0xA6; /* DH+ */
        tag->routing_path[2] = 0x24; /* instance */
        tag->routing_path[3] = dhp_channel;  /* 1 = Channel A, 2 = Channel B */
        tag->routing_path[4] = 0x2C; /* ? */
        tag->routing_path[5] = 0x01; /* ? */
        tag->routing_path_size = 6;

        tag->dhp_src  = src_addr;
        tag->dhp_dest = dest_addr;
        tag->use_dhp_direct = 1;
    } else if(!has_dhp){
        /* we can do a generic path to the router
         * object in the PLC.
         */
        tag->routing_path[0] = 0x20;
        tag->routing_path[1] = 0x02; /* router ? */
        tag->routing_path[2] = 0x24;
        tag->routing_path[3] = 0x01;
        tag->routing_path_size = 4;

        tag->dhp_src  = 0;
        tag->dhp_dest = 0;
        tag->use_dhp_direct = 0;
    } else {
        /* we had the special DH+ format and it was
         * either not last or not a PLC5/SLC.  That
         * is an error.
         */

        tag->dhp_src  = 0;
        tag->dhp_dest = 0;
        tag->use_dhp_direct = 0;

        return PLCTAG_ERR_BAD_PARAM;
    }

    return PLCTAG_STATUS_OK;
}










#ifdef START
#undef START
#endif
#define START 1

#ifdef ARRAY
#undef ARRAY
#endif
#define ARRAY 2

#ifdef DOT
#undef DOT
#endif
#define DOT 3

#ifdef NAME
#undef NAME
#endif
#define NAME 4

/*
 * convert_tag_name()
 *
 * This takes a LGX-style tag name like foo[14].blah and
 * turns it into an IOI path/string.
 */

int convert_tag_name(ab_tag_p tag,const char *name)
{
    uint8_t *data = tag->encoded_name;
    const char *p = name;
    uint8_t *word_count = NULL;
    uint8_t *dp = NULL;
    uint8_t *name_len;
    int state;

    /* reserve room for word count for IOI string. */
    word_count = data;
    dp = data + 1;

    state = START;

    while(*p) {
        switch(state) {
            case START:
                /* must start with an alpha character. */
                if(isalpha(*p) || *p == '_') {
                    state = NAME;
                } else if(*p == '.') {
                    state = DOT;
                } else if(*p == '[') {
                    state = ARRAY;
                } else {
                    return 0;
                }

                break;

            case NAME:
                *dp = 0x91; /* start of ASCII name */
                dp++;
                name_len = dp;
                *name_len = 0;
                dp++;

                while(isalnum(*p) || *p == '_') {
                    *dp = *p;
                    dp++;
                    p++;
                    (*name_len)++;
                }

                /* must pad the name to a multiple of two bytes */
                if(*name_len & 0x01) {
                    *dp = 0;
                    dp++;
                }

                state = START;

                break;

            case ARRAY:
                /* move the pointer past the [ character */
                p++;

                do {
                    uint32_t val;
                    char *np = NULL;
                    val = (uint32_t)strtol(p,&np,0);

                    if(np == p) {
                        /* we must have a number */
                        return 0;
                    }

                    p = np;

                    if(val > 0xFFFF) {
                        *dp = 0x2A; dp++;  /* 4-byte value */
                        *dp = 0; dp++;     /* padding */

                        /* copy the value in little-endian order */
                        *dp = val & 0xFF; dp++;
                        *dp = (val >> 8) & 0xFF; dp++;
                        *dp = (val >> 16) & 0xFF; dp++;
                        *dp = (val >> 24) & 0xFF; dp++;
                    } else if(val > 0xFF) {
                        *dp = 0x29; dp++;  /* 2-byte value */
                        *dp = 0; dp++;     /* padding */

                        /* copy the value in little-endian order */
                        *dp = val & 0xFF; dp++;
                        *dp = (val >> 8) & 0xFF; dp++;
                    } else {
                        *dp = 0x28; dp++;  /* 1-byte value */
                        *dp = val; dp++;     /* value */
                    }

                    /* each up whitespace */
                    while(isspace(*p)) p++;
                } while(*p == ',');

                if(*p != ']')
                    return 0;

                p++;

                state = START;

                break;

            case DOT:
                p++;
                state = START;
                break;

            default:
                /* this should never happen */
                return 0;

                break;
        }
    }

    /* word_count is in units of 16-bit integers, do not
     * count the word_count value itself.
     */
    *word_count = ((dp - data)-1)/2;

    /* store the size of the whole result */
    tag->encoded_name_size = dp - data;

    return 1;
}






char *ab_decode_cip_status(int status)
{
    switch(status) {
        case 0x04:
            return "Bad or indecipherable IOI!";
            break;

        case 0x05:
            return "Unknown tag or item!";
            break;

        case 0x06:
            return "Response too large, partial data transfered!";
            break;

        case 0x0A:
            return "Error processing attributes!";
            break;

        case 0x13:
            return "Insufficient data/params to process request!";
            break;

        case 0x1C:
            return "Insufficient attributes to process request!";
            break;

        case 0x26:
            return "IOI word length does not match IOI length processed!";
            break;

        case 0xFF:
        	/* extended status */

        default:
            return "Unknown error status.";
            break;
    }

    return "Unknown error status.";
}



/*#ifdef __cplusplus
}
#endif
*/

