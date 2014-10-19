/***************************************************************************
 *   Copyright (C) 2013 by Process Control Engineers                       *
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
#include <libplctag.h>
#include <libplctag_tag.h>
#include <ab/common.h>
#include <ab/pccc.h>
#include <ab/cip.h>
#include <ab/ab.h>
#include <ab/ab_defs.h>
#include <util/attr.h>




/*
 * Shared global data
 */

/* session/tag handling */
volatile ab_session_p sessions = NULL;
volatile mutex_p io_thread_mutex = NULL;
volatile lock_t tag_mutex_lock = LOCK_INIT; /* used for protecting access to set up the above mutex */

/* request/response handling thread */
volatile thread_p io_handler_thread = NULL;



/*
 * ab_tag_abort
 * 
 * This does the work of stopping any inflight requests.
 * This is not thread-safe.  It must be called from a function
 * that locks the tag's mutex or only from a single thread.
 */

int ab_tag_abort(ab_tag_p tag)
{
	int i;
	
	for(i = 0; i < tag->max_requests; i++) {
		if(tag->reqs[i]) {
			tag->reqs[i]->abort_request = 1;
			tag->reqs[i] = NULL;
		}
	}

	tag->read_in_progress = 0;
	tag->write_in_progress = 0;

	return PLCTAG_STATUS_OK;
}





/*
 * ab_tag_destroy
 * 
 * This is not completely thread-safe.  Two threads could hit this at
 * once.  It will safely remove outstanding requests, but that is about
 * it.  If two threads hit this at the same time, at least a double-free
 * will result.
 */


int ab_tag_destroy(ab_tag_p tag)
{
	int rc = PLCTAG_STATUS_OK;
    int debug = tag->debug;

    pdebug(debug,"Starting.");

    /* already destroyed? */
    if(!tag)
        return rc;

	/* 
	 * stop any current actions. Note that we
	 * want to use the thread-safe version here.  We
	 * do lock a mutex later, but a different one.
	 */
	plc_tag_abort((plc_tag)tag);


	/* this needs to be synchronized.  We are going to remove the tag completely. */
	critical_block(io_thread_mutex) {	
		/* remove it from the current connection */
		if(tag->session) {
			ab_session_p session = tag->session;

			pdebug(debug,"Removing tag");

			session_remove_tag_unsafe(tag, session);

			/* if the session is now empty, remove it */
			if(ab_session_empty(session)) {
				pdebug(debug,"Removing session");
				ab_session_destroy_unsafe(tag, session);
			}

			/* null out the pointer just in case. */
			tag->session = NULL;
		}

		if(tag->reqs) {
			mem_free(tag->reqs);
			tag->reqs = NULL;
		}

		if(tag->read_req_sizes) {
			mem_free(tag->read_req_sizes);
			tag->read_req_sizes = NULL;
		}

		if(tag->write_req_sizes) {
			mem_free(tag->write_req_sizes);
			tag->write_req_sizes = NULL;
		}

		if(tag->data) {
			mem_free(tag->data);
			tag->data = NULL;
		}

		/* release memory */
		mem_free(tag);
	}
	
	pdebug(debug,"done");

    return rc;
}









int check_cpu(ab_tag_p tag, attr attribs)
{
    const char *cpu_type = attr_get_str(attribs,"cpu","NONE");

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
        pdebug(tag->debug,"Unsupported device type: %s",cpu_type);

    	return PLCTAG_ERR_BAD_DEVICE;
    }

    return PLCTAG_STATUS_OK;
}





int check_tag_name(ab_tag_p tag, const char *name)
{
	int debug = tag->debug;

	if(str_cmp(name,"NONE") == 0) {
		return PLCTAG_ERR_BAD_PARAM;
	}

    /* attempt to parse the tag name */
    switch(tag->protocol_type) {
        case AB_PROTOCOL_PLC:
        case AB_PROTOCOL_MLGX:
            if(!pccc_encode_tag_name(tag->encoded_name,&(tag->encoded_name_size),name,MAX_TAG_NAME)) {
                pdebug(debug,"parse of PCCC-style tag name %s failed!",name);

                return PLCTAG_ERR_BAD_PARAM;
            }

            break;
        case AB_PROTOCOL_LGX:
            if(!cip_encode_tag_name(tag,name)) {
                pdebug(debug,"parse of CIP-style tag name %s failed!",name);

                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        default:
            /* how would we get here? */
            pdebug(debug,"unsupported protocol %d",tag->protocol_type);

            return PLCTAG_ERR_BAD_PARAM;

            break;
    }

    return PLCTAG_STATUS_OK;
}







int send_eip_request(ab_request_p req)
{
	int rc;

	/*pdebug(debug,"Starting.");*/

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
		pdebug_dump_bytes(req->debug, req->data,req->request_size);

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
					/*pdebug(debug,"Error reading socket! rc=%d",rc);*/
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
		session->resp_seq_id = ((eip_encap_t*)(session->recv_data))->encap_sender_context;
		session->has_response = 1;

		/*
		if(session->resp_seq_id == 0) {
			pdebug(debug,"Got zero response ID");
		}
		*/
	}

	return rc;
}










/*
 * check_mutex
 *
 * check to see if the global mutex is set up.  If not, do an atomic
 * lock and set it up.
 */
int check_mutex(int debug)
{
	int rc = PLCTAG_STATUS_OK;

	/* loop until we get the lock flag */
	while(!lock_acquire((lock_t *)&tag_mutex_lock)) {
		sleep_ms(1);
	}

	/*
	 * FIXME - this is still a race condition on some processors.
	 * Replace with a CAS loop.
	 */

	/* first see if the mutex is there. */
	if(!io_thread_mutex) {
		rc = mutex_create((mutex_p *)&io_thread_mutex);
		if(rc != PLCTAG_STATUS_OK) {
			pdebug(debug,"Unable to create global tag mutex!");
		}
	}

	/* we hold the lock, so clear it.*/
	lock_release((lock_t *)&tag_mutex_lock);

	return rc;
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
	uint16_t res;

	critical_block(io_thread_mutex) {
		res = (uint16_t)session_get_new_seq_id_unsafe(sess);
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

	critical_block(io_thread_mutex) {
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

	critical_block(io_thread_mutex) {
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







int find_or_create_session(ab_tag_p tag, attr attribs)
{
	int debug = tag->debug;
    const char *session_gw = attr_get_str(attribs,"gateway","");
    int session_gw_port = attr_get_int(attribs,"gateway_port",AB_EIP_DEFAULT_PORT);
    ab_session_p session;
    int shared_session = attr_get_int(attribs,"share_session",1); /* share the session by default. */

    /* if we are to share sessions, then look for an existing one. */
    if(shared_session) {
    	session = find_session_by_host_unsafe(tag, session_gw);
    } else {
    	/* no sharing, create a new one */
    	session = AB_SESSION_NULL;
    }

    if(session == AB_SESSION_NULL) {
        session = ab_session_create(tag, session_gw, session_gw_port);
    } else {
        pdebug(debug,"find_or_create_session() reusing existing session.");
    }

    if(session == AB_SESSION_NULL) {
        pdebug(debug,"unable to create or find a session!");

    	return PLCTAG_ERR_BAD_GATEWAY;
    }

    tag->session = session;

    return PLCTAG_STATUS_OK;
}









int add_session_unsafe(ab_tag_p tag,  ab_session_p n)
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


int add_session(ab_tag_p tag,  ab_session_p s)
{
	int rc;

	pdebug(tag->debug,"Locking mutex");
	critical_block(io_thread_mutex) {
		rc = add_session_unsafe(tag, s);
	}

	return rc;
}




int remove_session_unsafe(ab_tag_p tag, ab_session_p n)
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







int remove_session(ab_tag_p tag,  ab_session_p s)
{
	int rc;

	pdebug(tag->debug,"Locking mutex");
	critical_block(io_thread_mutex) {
		rc = remove_session_unsafe(tag, s);
	}
	pdebug(tag->debug,"Mutex released");

	return rc;
}





ab_session_p find_session_by_host_unsafe(ab_tag_p tag, const char  *t)
{
	ab_session_p tmp;
	int count = 0;

	tmp = sessions;

	while(tmp && str_cmp_i(tmp->host,t)) {
		tmp=tmp->next;
		count++;
	}

	pdebug(tag->debug,"found %d sessions",count);

    if(!tmp) {
    	return (ab_session_p)NULL;
    }

    return tmp;
}


/* not threadsafe */
int session_add_tag_unsafe(ab_tag_p tag, ab_session_p session)
{
	tag->next = session->tags;
	session->tags = tag;

	return PLCTAG_STATUS_OK;
}


/* not threadsafe */
int session_remove_tag_unsafe(ab_tag_p tag, ab_session_p session)
{
	ab_tag_p tmp, prev;
	int count = 0;
	int debug = tag->debug;

	tmp = session->tags;
	prev = NULL;

	while(tmp && tmp != tag) {
		prev = tmp;
		tmp = tmp->next;
		count++;
	}

	pdebug(debug,"found %d tags",count);

	if(tmp) {
		if(!prev) {
			session->tags = tmp->next;
		} else {
			prev->next = tmp->next;
		}
	}

	return PLCTAG_STATUS_OK;
}



ab_session_p ab_session_create(ab_tag_p tag, const char *host, int gw_port)
{
    ab_session_p session = AB_SESSION_NULL;
    int debug = tag->debug;

    pdebug(debug,"Starting");

    session = (ab_session_p)mem_alloc(sizeof(struct ab_session_t));

    if(!session) {
        pdebug(debug,"alloc failed errno: %d",errno);

        return AB_SESSION_NULL;
    }

    str_copy(session->host,host,MAX_SESSION_HOST);

    /* we must connect to the gateway and register */
    if(!ab_session_connect(tag, session,host)) {
        mem_free(session);
        pdebug(debug,"session connect failed!");
        return AB_SESSION_NULL;
    }

    if(!ab_session_register(tag, session)) {
        ab_session_destroy_unsafe(tag, session);
        pdebug(debug,"session registration failed!");
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
    add_session_unsafe(tag, session);

    pdebug(debug,"Done.");

    return session;
}





/*
 * ab_session_connect()
 *
 * Connect to the host/port passed via TCP.  Set all the relevant fields in
 * the passed session object.
 */

int ab_session_connect(ab_tag_p tag, ab_session_p session, const char *host)
{
	int rc;
	int debug = tag->debug;

    pdebug(debug,"Starting.");

    /* Open a socket for communication with the gateway. */
    rc = socket_create(&(session->sock));

    if(rc) {
    	pdebug(debug,"Unable to create socket for session!");
    	return 0;
    }

    rc = socket_connect_tcp(session->sock, host, AB_EIP_DEFAULT_PORT);

    if(rc != PLCTAG_STATUS_OK) {
    	pdebug(debug,"Unable to connect socket for session!");
    	return 0;
    }

    /* everything is OK.  We have a TCP stream open to a gateway. */
    session->is_connected = 1;

    pdebug(debug,"Done.");

    return 1;
}



int ab_session_destroy_unsafe(ab_tag_p tag, ab_session_p session)
{
	int debug = tag->debug;

    pdebug(debug,"Starting.");

    if(!session)
        return 1;

    /* do not destroy the session if there are
     * connections still */
    if(session->tags) {
        pdebug(debug,"Attempt to destroy session while open tags exist!");
        return 0;
    }

    if(ab_session_unregister(tag, session))
        return 0;

    /* close the socket. */
    if(session->is_connected) {
		socket_close(session->sock);
		socket_destroy(&(session->sock));
        session->is_connected = 0;
    }

    remove_session_unsafe(tag, session);

    mem_free(session);

    pdebug(debug,"Done.");

    return 1;
}



int ab_session_destroy(ab_tag_p tag, ab_session_p session)
{
	int rc;

	critical_block(io_thread_mutex) {
		rc = ab_session_destroy_unsafe(tag, session);
	}

	return rc;
}



int ab_session_empty(ab_session_p session)
{
    return (session->tags == NULL);
}





int ab_session_register(ab_tag_p tag, ab_session_p session)
{
	int debug = tag->debug;
    eip_session_reg_req *req;
    eip_encap_t *resp;
    int rc;
    int data_size = 0;
    uint64_t timeout_time;

    pdebug(debug,"Starting.");

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

    pdebug(debug,"sending data:");
    pdebug_dump_bytes(debug,session->recv_data, data_size);

    while(timeout_time > time_ms() && session->recv_offset < data_size) {
    	rc = socket_write(session->sock, session->recv_data + session->recv_offset, data_size - session->recv_offset);

    	if(rc < 0) {
			pdebug(debug,"Unable to send session registration packet! rc=%d",rc);
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
					pdebug(debug,"Error reading socket! rc=%d",rc);
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


    pdebug(debug,"received response:");
    pdebug_dump_bytes(debug,session->recv_data, data_size);


    /* encap header is at the start of the buffer */
    resp = (eip_encap_t *)(session->recv_data);

    /* check the response status */
    if(le2h16(resp->encap_command) != AB_EIP_REGISTER_SESSION) {
        pdebug(debug,"EIP unexpected response packet type: %d!",resp->encap_command);
        return PLCTAG_ERR_BAD_DATA;
    }

    if(le2h16(resp->encap_status) != AB_EIP_OK) {
        pdebug(debug,"EIP command failed, response code: %d",resp->encap_status);
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /* after all that, save the session handle, we will
     * use it in future packets.
     */
    session->session_handle = resp->encap_session_handle; /* opaque to us */

    pdebug(debug,"Done.");

    return 1;
}




int ab_session_unregister(ab_tag_p tag, ab_session_p session)
{
    if(session->sock) {
    	socket_close(session->sock);
    	socket_destroy(&(session->sock));
    	session->sock = NULL;
    	session->is_connected = 0;
    }

    return PLCTAG_STATUS_OK;
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
		pdebug(debug,"num_reqs_in_flight=%d",session->num_reqs_in_flight);*/

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
				eip_cip_resp_old *resp = (eip_cip_resp_old *)(session->recv_data);

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
			pdebug(tmp->debug,"got full packet of size %d",session->recv_offset);
			pdebug_dump_bytes(tmp->debug, session->recv_data,session->recv_offset);

			/* copy the data from the session's buffer */
			mem_copy(tmp->data, session->recv_data, session->recv_offset);

			tmp->resp_received = 1;
			tmp->send_in_progress = 0;
			tmp->send_request = 0;
			tmp->request_size = session->recv_offset;
		} /*else {
			pdebug(debug,"Response for unknown request.");
		}*/

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
		pdebug(debug,"num_reqs_in_flight=%d",session->num_reqs_in_flight);*/
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


#ifdef WIN32
DWORD __stdcall request_handler_func(LPVOID not_used)
#else
void *request_handler_func(void *not_used)
#endif
{
	int rc;
	ab_session_p cur_sess;
	int debug = 1;

	while(1) {
		/* we need the mutex */
		if(io_thread_mutex == NULL) {
			pdebug(debug,"tag_mutex is NULL!");
			break;
		}

		//pdebug(debug,"Locking mutex");

		critical_block(io_thread_mutex) {
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
					pdebug(debug,"Error when checking for incoming session data! %d",rc);
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


