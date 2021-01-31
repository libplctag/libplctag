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



#include <lib/libplctag.h>
#include <ab/ab_common.h>
#include <ab/pccc.h>
#include <ab/eip_plc5_dhp.h>
#include <ab/tag.h>
#include <ab/session.h>
#include <ab/defs.h>
#include <util/debug.h>


static int check_read_status(ab_tag_p tag);
static int check_write_status(ab_tag_p tag);



static int tag_read_start(ab_tag_p tag);
static int tag_status(ab_tag_p tag);
static int tag_tickler(ab_tag_p tag);
static int tag_write_start(ab_tag_p tag);

struct tag_vtable_t eip_plc5_dhp_vtable = {
    (tag_vtable_func)ab_tag_abort, /* shared */
    (tag_vtable_func)tag_read_start,
    (tag_vtable_func)tag_status,
    (tag_vtable_func)tag_tickler,
    (tag_vtable_func)tag_write_start,

    /* data accessors */
    ab_get_int_attrib,
    ab_set_int_attrib
};


START_PACK typedef struct {
    /* encap header */
    uint16_le encap_command;    /* ALWAYS 0x0070 Connected Send */
    uint16_le encap_length;   /* packet size in bytes less the header size, which is 24 bytes */
    uint32_le encap_session_handle;  /* from session set up */
    uint32_le encap_status;          /* always _sent_ as 0 */
    uint64_le encap_sender_context;  /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_le options;               /* 0, reserved for future use */

    /* Interface Handle etc. */
    uint32_le interface_handle;      /* ALWAYS 0 */
    uint16_le router_timeout;        /* in seconds, zero for Connected Sends! */

    /* Common Packet Format - CPF Connected */
    uint16_le cpf_item_count;        /* ALWAYS 2 */
    uint16_le cpf_cai_item_type;     /* ALWAYS 0x00A1 Connected Address Item */
    uint16_le cpf_cai_item_length;   /* ALWAYS 2 ? */
    uint32_le cpf_targ_conn_id;           /* the connection id from Forward Open */
    uint16_le cpf_cdi_item_type;     /* ALWAYS 0x00B1, Connected Data Item type */
    uint16_le cpf_cdi_item_length;   /* length in bytes of the rest of the packet */

    /* Connection sequence number */
    uint16_le cpf_conn_seq_num;      /* connection sequence ID, inc for each message */

    /* PLC5 DH+ Routing */
    uint16_le dest_link;
    uint16_le dest_node;
    uint16_le src_link;
    uint16_le src_node;

    /* PCCC Command */
    uint8_t pccc_command;           /* CMD read, write etc. */
    uint8_t pccc_status;            /* STS 0x00 in request */
    uint16_le pccc_seq_num;          /* TNSW transaction/sequence id */
    uint8_t pccc_function;          /* FNC sub-function of command */
    uint16_le pccc_transfer_offset;           /* offset of this request? */
    uint16_le pccc_transfer_size;    /* number of elements requested */
} END_PACK pccc_dhp_co_req;


START_PACK typedef struct {
    /* encap header */
    uint16_le encap_command;    /* ALWAYS 0x0070 Connected Send */
    uint16_le encap_length;   /* packet size in bytes less the header size, which is 24 bytes */
    uint32_le encap_session_handle;  /* from session set up */
    uint32_le encap_status;          /* always _sent_ as 0 */
    uint64_le encap_sender_context;  /* whatever we want to set this to, used for
                                     * identifying responses when more than one
                                     * are in flight at once.
                                     */
    uint32_le options;               /* 0, reserved for future use */

    /* Interface Handle etc. */
    uint32_le interface_handle;      /* ALWAYS 0 */
    uint16_le router_timeout;        /* in seconds, zero for Connected Sends! */

    /* Common Packet Format - CPF Connected */
    uint16_le cpf_item_count;        /* ALWAYS 2 */
    uint16_le cpf_cai_item_type;     /* ALWAYS 0x00A1 Connected Address Item */
    uint16_le cpf_cai_item_length;   /* ALWAYS 2 ? */
    uint32_le cpf_targ_conn_id;           /* the connection id from Forward Open */
    uint16_le cpf_cdi_item_type;     /* ALWAYS 0x00B1, Connected Data Item type */
    uint16_le cpf_cdi_item_length;   /* length in bytes of the rest of the packet */

    /* connection ID from request */
    uint16_le cpf_conn_seq_num;      /* connection sequence ID, inc for each message */

    /* PLC5 DH+ Routing */
    uint16_le dest_link;
    uint16_le dest_node;
    uint16_le src_link;
    uint16_le src_node;

    /* PCCC Command */
    uint8_t pccc_command;           /* CMD read, write etc. */
    uint8_t pccc_status;            /* STS 0x00 in request */
    uint16_le pccc_seq_num;         /* TNSW transaction/connection sequence number */
    //uint8_t pccc_data[ZLA_SIZE];    /* data for PCCC request. */
} END_PACK pccc_dhp_co_resp;


/*
 * tag_status
 *
 * PCCC/DH+-specific status.  This functions as a "tickler" routine
 * to check on the completion of async requests.
 */
int tag_status(ab_tag_p tag)
{
    if (!tag->session) {
        /* this is not OK.  This is fatal! */
        return PLCTAG_ERR_CREATE;
    }

    if(tag->read_in_progress) {
        return PLCTAG_STATUS_PENDING;
    }

    if(tag->write_in_progress) {
        return PLCTAG_STATUS_PENDING;
    }

    return GET_STATUS(tag->status);
}




int tag_tickler(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    if(tag->read_in_progress) {
        pdebug(DEBUG_SPEW, "Read in progress.");
        rc = check_read_status(tag);
        SET_STATUS(tag->status, rc);

        /* check to see if the read finished. */
        if(!tag->read_in_progress) {
            tag->read_complete = 1;
        }

        return rc;
    }

    if(tag->write_in_progress) {
        pdebug(DEBUG_SPEW, "Write in progress.");
        rc = check_write_status(tag);
        SET_STATUS(tag->status, rc);

        /* check to see if the write finished. */
        if(!tag->write_in_progress) {
            tag->write_complete = 1;
        }

        return rc;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return GET_STATUS(tag->status);
}


/*
 * tag_read_start
 *
 * This does not support multiple request fragments.
 */
int tag_read_start(ab_tag_p tag)
{
    pccc_dhp_co_req *pccc;
    uint8_t *data = NULL;
    int data_per_packet = 0;
    int overhead = 0;
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;

    pdebug(DEBUG_INFO, "Starting");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_WARN, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    tag->read_in_progress = 1;

    /* What is the overhead in the _response_ */
    overhead =   1  /* pccc command */
                 +1  /* pccc status */
                 +2;  /* pccc sequence num */

    data_per_packet = session_get_max_payload(tag->session) - overhead;

    if(data_per_packet <= 0) {
        tag->read_in_progress = 0;
        pdebug(DEBUG_WARN, "Unable to send request.  Packet overhead, %d bytes, is too large for packet, %d bytes!", overhead, session_get_max_payload(tag->session));
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(data_per_packet < tag->size) {
        tag->read_in_progress = 0;
        pdebug(DEBUG_DETAIL, "Unable to send request: Tag size is %d, write overhead is %d, and write data per packet is %d!", tag->size, overhead, data_per_packet);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        tag->read_in_progress = 0;
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    pccc = (pccc_dhp_co_req *)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(pccc_dhp_co_req);

    /* copy encoded tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* amount of data to get this time */
    *data = (uint8_t)(tag->size); /* bytes for this transfer */
    data++;

    /* encap fields */
    pccc->encap_command = h2le16(AB_EIP_CONNECTED_SEND);    /* ALWAYS 0x006F Unconnected Send*/

    /* router timeout */
    pccc->router_timeout = h2le16(1);                 /* one second timeout, enough? */

    /* Common Packet Format fields */
    pccc->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    pccc->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    pccc->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4 ? */
    pccc->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    pccc->cpf_cdi_item_length = h2le16((uint16_t)(data - (uint8_t *)(&(pccc->cpf_conn_seq_num)))); /* REQ: fill in with length of remaining data. */

    /* DH+ Routing */
    pccc->dest_link = h2le16(0);
    pccc->dest_node = h2le16(tag->session->dhp_dest);
    pccc->src_link = h2le16(0);
    pccc->src_node = h2le16(0) /*h2le16(tag->dhp_src)*/;

    /* PCCC Command */
    pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
    pccc->pccc_status = 0;  /* STS 0 in request */
    pccc->pccc_seq_num = /*h2le16(conn_seq_id)*/ h2le16((uint16_t)(intptr_t)(tag->session));
    pccc->pccc_function = AB_EIP_PLC5_RANGE_READ_FUNC;
    pccc->pccc_transfer_offset = h2le16((uint16_t)0);
    pccc->pccc_transfer_size = h2le16((uint16_t)((tag->size)/2));  /* size in 2-byte words */

    /* get ready to add the request to the queue for this session */
    req->request_size = (int)(data - (req->data));

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        tag->read_in_progress = 0;
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
        req->abort_request = 1;
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;
//    tag->status = PLCTAG_STATUS_PENDING;

    /* the read is now pending */
    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}



int tag_write_start(ab_tag_p tag)
{
    pccc_dhp_co_req *pccc;
    uint8_t *data;
//    uint8_t element_def[16];
//    int element_def_size;
//    uint8_t array_def[16];
//    int array_def_size;
//    int pccc_data_type;
    int data_per_packet = 0;
    int overhead = 0;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));;
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;

    pdebug(DEBUG_INFO, "Starting");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_WARN, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    tag->write_in_progress = 1;

    /* how many packets will we need? How much overhead? */
    overhead = 2        /* size of sequence num */
               +8        /* DH+ routing */
               +1        /* DF1 command */
               +1        /* status */
               +2        /* PCCC packet sequence number */
               +1        /* PCCC function */
               +2        /* request offset */
               +2        /* tag size in elements */
               +(tag->encoded_name_size)
               +2;       /* this request size in elements */

    data_per_packet = session_get_max_payload(tag->session) - overhead;

    if(data_per_packet <= 0) {
        tag->write_in_progress = 0;
        pdebug(DEBUG_WARN, "Unable to send request.  Packet overhead, %d bytes, is too large for packet, %d bytes!", overhead, session_get_max_payload(tag->session));
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(data_per_packet < tag->size) {
        tag->write_in_progress = 0;
        pdebug(DEBUG_WARN, "PCCC requests cannot be fragmented.  Too much data requested.");
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);

    if(rc != PLCTAG_STATUS_OK) {
        tag->write_in_progress = 0;
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    pccc = (pccc_dhp_co_req *)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(pccc_dhp_co_req);

    /* copy laa into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* now copy the data to write */
    mem_copy(data, tag->data, tag->size);
    data += tag->size;

    /* now fill in the rest of the structure. */

    /* encap fields */
    pccc->encap_command = h2le16(AB_EIP_CONNECTED_SEND);    /* ALWAYS 0x006F Unconnected Send*/

    /* router timeout */
    pccc->router_timeout = h2le16(1);                 /* one second timeout, enough? */

    /* Common Packet Format fields */
    pccc->cpf_item_count = h2le16(2);                 /* ALWAYS 2 */
    pccc->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);/* ALWAYS 0x00A1 connected address item */
    pccc->cpf_cai_item_length = h2le16(4);            /* ALWAYS 4 ? */
    pccc->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);/* ALWAYS 0x00B1 - connected Data Item */
    pccc->cpf_cdi_item_length = h2le16((uint16_t)(data - (uint8_t *)(&(pccc->cpf_conn_seq_num)))); /* REQ: fill in with length of remaining data. */

    /* DH+ Routing */
    pccc->dest_link = h2le16(0);
    pccc->dest_node = h2le16(tag->session->dhp_dest);
    pccc->src_link = h2le16(0);
    pccc->src_node = h2le16(0) /*h2le16(tag->dhp_src)*/;

    /* PCCC Command */
    pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
    pccc->pccc_status = 0;  /* STS 0 in request */
    pccc->pccc_seq_num = h2le16(conn_seq_id); /* FIXME - get sequence ID from session? */
    pccc->pccc_function = AB_EIP_PLC5_RANGE_WRITE_FUNC;
    pccc->pccc_transfer_offset = h2le16((uint16_t)0);
    pccc->pccc_transfer_size = h2le16((uint16_t)((tag->size)/2));  /* size in 2-byte words */

    /* get ready to add the request to the queue for this session */
    req->request_size = (int)(data - (req->data));

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        tag->write_in_progress = 0;
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
        req->abort_request = 1;
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;
//    tag->status = PLCTAG_STATUS_PENDING;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


/*
 * check_read_status
 *
 * PCCC does not support request fragments.
 */
static int check_read_status(ab_tag_p tag)
{
    pccc_dhp_co_resp *resp;
    uint8_t *data;
    uint8_t *data_end;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting");

    /* is there a request in flight? */
    if (!tag->req) {
        tag->read_in_progress = 0;
        tag->offset = 0;

        pdebug(DEBUG_WARN, "Read in progress, but no request in flight!");

        return PLCTAG_ERR_READ;
    }

    /* request can be used by two threads at once. */
    spin_block(&tag->req->lock) {
        if(!tag->req->resp_received) {
            rc = PLCTAG_STATUS_PENDING;
            break;
        }

        /* check to see if it was an abort on the session side. */
        if(tag->req->status != PLCTAG_STATUS_OK) {
            rc = tag->req->status;
            tag->req->abort_request = 1;

            pdebug(DEBUG_WARN, "Session reported failure of request: %s.", plc_tag_decode_error(rc));

            tag->read_in_progress = 0;
            tag->offset = 0;

            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        if(rc_is_error(rc)) {
            /* the request is dead, from session side. */
            tag->read_in_progress = 0;
            tag->offset = 0;
            tag->req = rc_dec(tag->req);
        }

        return rc;
    }

    /* the request is ours exclusively. */

    resp = (pccc_dhp_co_resp *)(tag->req->data);

    /* point to the start of the data */
    data = (uint8_t *)resp + sizeof(*resp);

    /* point to the end of the data */
    data_end = (tag->req->data + le2h16(resp->encap_length) + sizeof(eip_encap));

    /* fake exception */
    do {
        if(le2h16(resp->encap_command) != AB_EIP_CONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(resp->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(resp->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d - %s", resp->pccc_status, pccc_decode_error(&resp->pccc_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /* did we get the right amount of data? */
        if((data_end - data) != tag->size) {
            if((int)(data_end - data) > tag->size) {
                pdebug(DEBUG_WARN, "Too much data received!  Expected %d bytes but got %d bytes!", tag->size, (int)(data_end - data));
                rc = PLCTAG_ERR_TOO_LARGE;
            } else {
                pdebug(DEBUG_WARN, "Too little data received!  Expected %d bytes but got %d bytes!", tag->size, (int)(data_end - data));
                rc = PLCTAG_ERR_TOO_SMALL;
            }
            break;
        }

        /* copy data into the tag. */
        mem_copy(tag->data, data, (int)(data_end - data));

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request */
    tag->req->abort_request = 1;
    tag->req = rc_dec(tag->req);

    tag->read_in_progress = 0;

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


static int check_write_status(ab_tag_p tag)
{
    pccc_dhp_co_resp *pccc_resp;
//    uint8_t *data = NULL;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    /* is there an outstanding request? */
    if (!tag->req) {
        tag->write_in_progress = 0;
        tag->offset = 0;

        pdebug(DEBUG_WARN, "Write in progress, but no request in flight!");

        return PLCTAG_ERR_WRITE;
    }

    /* request can be used by two threads at once. */
    spin_block(&tag->req->lock) {
        if(!tag->req->resp_received) {
            rc = PLCTAG_STATUS_PENDING;
            break;
        }

        /* check to see if it was an abort on the session side. */
        if(tag->req->status != PLCTAG_STATUS_OK) {
            rc = tag->req->status;
            tag->req->abort_request = 1;

            pdebug(DEBUG_WARN, "Session reported failure of request: %s.", plc_tag_decode_error(rc));

            tag->write_in_progress = 0;
            tag->offset = 0;

            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        if(rc_is_error(rc)) {
            /* the request is dead, from session side. */
            tag->write_in_progress = 0;
            tag->offset = 0;

            tag->req = rc_dec(tag->req);
        }

        return rc;
    }

    /* the request is ours exclusively. */

    pccc_resp = (pccc_dhp_co_resp *)(tag->req->data);

    /* point data just past the header */
//    data = (uint8_t *)pccc_resp + sizeof(*pccc_resp);

    /* fake exception */
    do {
        /* check the response status */
        if( le2h16(pccc_resp->encap_command) != AB_EIP_CONNECTED_SEND) {
            pdebug(DEBUG_WARN, "EIP unexpected response packet type: %d!", pccc_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(pccc_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(pccc_resp->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc_resp->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d - %s", pccc_resp->pccc_status, pccc_decode_error(&pccc_resp->pccc_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /* everything OK */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up any outstanding requests. */
    tag->req->abort_request = 1;
    tag->req = rc_dec(tag->req);
    tag->write_in_progress = 0;

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}
