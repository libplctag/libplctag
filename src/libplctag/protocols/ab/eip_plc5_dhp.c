/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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


#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/ab/ab_common.h>
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/ab/eip_plc5_dhp.h>
#include <libplctag/protocols/ab/pccc.h>
#include <libplctag/protocols/ab/session.h>
#include <libplctag/protocols/ab/tag.h>
#include <utils/debug.h>


static int check_read_status(ab_tag_p tag);
static int check_write_status(ab_tag_p tag);


static int tag_read_start(ab_tag_p tag);
static int tag_status(ab_tag_p tag);
static int tag_tickler(ab_tag_p tag);
static int tag_write_start(ab_tag_p tag);
static int tag_write_bit_start(ab_tag_p tag);

struct tag_vtable_t eip_plc5_dhp_vtable = {(tag_vtable_func)ab_tag_abort_request, /* shared */
                                           (tag_vtable_func)tag_read_start, (tag_vtable_func)tag_status,
                                           (tag_vtable_func)tag_tickler, (tag_vtable_func)tag_write_start,
                                           (tag_vtable_func)NULL, /* wake_plc */

                                           /* data accessors */
                                           ab_get_int_attrib, ab_set_int_attrib,

                                           ab_get_byte_array_attrib};



START_PACK typedef struct {
    /* DH+ Routing */
    uint16_le dest_link;
    uint16_le dest_node;
    uint16_le src_link;
    uint16_le src_node;

    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
    uint8_t pccc_function;  /* FNC sub-function of command */
    uint16_le pccc_offset;  /* offset of requested in total request */
    uint16_le pccc_transfer_size; /* total number of words requested */
} END_PACK pccc_dhp_read_cmd_req;

START_PACK typedef struct {
    /* DH+ Routing */
    uint16_le dest_link;
    uint16_le dest_node;
    uint16_le src_link;
    uint16_le src_node;

    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
    uint8_t pccc_function;  /* FNC sub-function of command */
} END_PACK pccc_dhp_rmw_cmd_req;

START_PACK typedef struct {
    /* DH+ Routing */
    uint16_le dest_link;
    uint16_le dest_node;
    uint16_le src_link;
    uint16_le src_node;

    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
    uint8_t pccc_function;  /* FNC sub-function of command */
    uint16_le pccc_offset;  /* offset of requested in total request */
    uint16_le pccc_transfer_size; /* total number of words requested */
} END_PACK pccc_dhp_write_cmd_req;



START_PACK typedef struct {
    /* DH+ Routing */
    uint16_le dest_link;
    uint16_le dest_node;
    uint16_le src_link;
    uint16_le src_node;

    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
} END_PACK pccc_dhp_cmd_resp;


/*
 * tag_status
 *
 * PCCC/DH+-specific status.  This functions as a "tickler" routine
 * to check on the completion of async requests.
 */
int tag_status(ab_tag_p tag) {
    if(!tag->session) {
        /* this is not OK.  This is fatal! */
        return PLCTAG_ERR_CREATE;
    }

    if(tag->read_in_progress) { return PLCTAG_STATUS_PENDING; }

    if(tag->write_in_progress) { return PLCTAG_STATUS_PENDING; }

    return tag->status;
}


int tag_tickler(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    rc = check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->read_in_progress) {
        pdebug(DEBUG_SPEW, "Read in progress.");
        rc = check_read_status(tag);
        tag->status = (int8_t)rc;

        /* check to see if the read finished. */
        if(!tag->read_in_progress) {
            /* read done so create done. */
            if(tag->first_read) {
                tag->first_read = 0;
                tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)rc);
            }

            tag->read_complete = 1;
        }

        return rc;
    }

    if(tag->write_in_progress) {
        pdebug(DEBUG_SPEW, "Write in progress.");
        rc = check_write_status(tag);
        tag->status = (int8_t)rc;

        /* check to see if the write finished. */
        if(!tag->write_in_progress) { tag->write_complete = 1; }

        return rc;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return tag->status;
}


/*
 * tag_read_start
 *
 * Start a PCCC tag read (PLC5).
 */

int tag_read_start(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));
    ab_request_p req = NULL;
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    int cip_payload_space = session_get_available_cip_payload_space(tag->session);

    pdebug(DEBUG_INFO, "Starting");

    /* pseudo-exception block for error handling */
    do {
        /* check for busy */
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_WARN, "Read or write operation already in flight!");
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        tag->read_in_progress = 1;

        /* calculate response overhead and available payload space */
        int response_overhead = sizeof(pccc_dhp_cmd_resp);
        int response_payload_space = cip_payload_space - response_overhead;

        if(response_payload_space < 0) {
            pdebug(DEBUG_WARN, "PLC5 read response overhead (%d bytes) exceeds session payload space (%d bytes).",
                   response_overhead, cip_payload_space);
            tag->read_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        if(response_payload_space < tag->size) {
            pdebug(DEBUG_WARN,
                   "Tag size (%d bytes) exceeds available response data space (%d bytes). PLC5 DH+ PCCC does not support fragmentation.",
                   tag->size, response_payload_space);
            tag->read_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* calculate request overhead */
        int request_overhead =   (int)sizeof(pccc_dhp_read_cmd_req)
                               + tag->encoded_name_size
                               + 1;

        pdebug(DEBUG_INFO, "PLC5 request overhead: PCCC read command header size %zu, encoded_name=%d, data_size=1, total=%d bytes",
               sizeof(pccc_dhp_read_cmd_req), tag->encoded_name_size, request_overhead);

        int request_payload_space = cip_payload_space - request_overhead;

        if(request_payload_space < 0) {
            pdebug(
                DEBUG_DETAIL,
                "PLC5 read request overhead (%d bytes) exceeds session payload space (%d bytes). Tag name too long or PCCC packet limit exceeded.",
                request_overhead, cip_payload_space);
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* create the request */
        rc = session_create_request(tag->session, tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to get new request.  rc=%d", rc);
            break;
        }

        pdebug(DEBUG_INFO, "Request created. Request capacity: %d bytes", req->request_capacity);

        if(request_overhead > req->request_capacity) {
            pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC request overhead (%d bytes) exceeds request capacity (%d bytes).", request_overhead,
                   req->request_capacity);
            rc_dec(req);
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* point the struct pointers to the buffer */
        eip_cpf_co_header *cip_req = (eip_cpf_co_header *)(req->data);
        pccc_dhp_read_cmd_req *pccc_cmd = (pccc_dhp_read_cmd_req *)(cip_req + 1);
        embed_start = (uint8_t *)(pccc_cmd + 1);

        /* fill in DH+ fields */
        pccc_cmd->dest_link = h2le16(0);
        pccc_cmd->dest_node = h2le16(tag->session->dhp_dest);
        pccc_cmd->src_link = h2le16(0);
        pccc_cmd->src_node = h2le16(0);

        /* fill in PCCC command fields */
        pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        pccc_cmd->pccc_status = 0;
        pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
        pccc_cmd->pccc_function = AB_EIP_PLC5_RANGE_READ_FUNC;
        pccc_cmd->pccc_offset = h2le16(0);
        pccc_cmd->pccc_transfer_size = h2le16((uint16_t)(tag->size / 2));

        /* point data pointer just past the fixed data fields */
        data = embed_start;

        /* copy encoded tag name into the request */
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        /* add data size byte */
        *data = (uint8_t)(tag->size);
        data++;

        /* debug: request full data length */
        ptrdiff_t calculated_request_size = (ptrdiff_t)(data - req->data);
        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC request full data length: %td bytes.", calculated_request_size);

        /* debug: CIP data length */
        ptrdiff_t cip_request_size = (ptrdiff_t)(data - embed_start);
        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC request CIP data length: %td bytes.", cip_request_size);

        /* fill in Common Packet Format fields */
        cip_req->cpf_item_count = h2le16(2);
        cip_req->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);
        cip_req->cpf_cai_item_length = h2le16(4);
        cip_req->cpf_targ_conn_id = h2le32(tag->session->targ_connection_id);
        cip_req->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);
        cip_req->cpf_conn_seq_num = h2le16(conn_seq_id);
        cip_req->cpf_cdi_item_length = h2le16((uint16_t)((size_t)cip_request_size + sizeof(cip_req->cpf_conn_seq_num)));

        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC request CPF CDI item length: %u bytes.", le2h16(cip_req->cpf_cdi_item_length));

        cip_req->router_timeout = h2le16(1);
        cip_req->encap_command = h2le16(AB_EIP_CONNECTED_SEND);

        /* set request size */
        req->request_size = (int)calculated_request_size;
        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC request size set to %d bytes.", req->request_size);

        /* debug: dump request data */
        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC request data:");
        pdebug_dump_bytes(DEBUG_DETAIL, req->data, (int)calculated_request_size);

        /* add request to session */
        rc = session_add_request(tag->session, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to add request to session! rc=%d", rc);
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* set tag->req if successful, else clean up */
    if(rc == PLCTAG_STATUS_OK) {
        critical_block(tag->api_mutex) {
            if(tag->req) {
                pdebug(DEBUG_WARN, "Request already set! This should not happen!");
                rc = PLCTAG_ERR_BAD_DATA;
            } else {
                pdebug(DEBUG_INFO, "Setting request for tag %d", tag->tag_id);
                tag->req = req;
                rc = PLCTAG_STATUS_PENDING;
            }
        }
    } else {
        pdebug(DEBUG_WARN, "Failed to generate new read request rc=%s", plc_tag_decode_error(rc));
        tag->read_in_progress = 0;
        req = rc_dec(req);
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");
    return rc;
}



/*
 * check_read_status
 *
 * NOTE that we can have only one outstanding request because PCCC
 * does not support fragments.
 */
static int check_read_status(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting");

    /* get the header pointers */
    eip_cpf_co_header *eip_cpf = (eip_cpf_co_header *)(tag->req->data);
    pccc_dhp_cmd_resp *pccc_cmd = (pccc_dhp_cmd_resp *)(eip_cpf + 1);

    uint8_t *data = (uint8_t *)(pccc_cmd + 1);
    uint8_t *data_end = tag->req->data + tag->req->request_size;

    /* fake exceptions */
    do {
        if(pccc_cmd->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d - %s", pccc_cmd->pccc_status,
                   pccc_decode_error(&pccc_cmd->pccc_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /* did we get the right amount of data? */
        if((data_end - data) != tag->size) {
            if((int)(data_end - data) > tag->size) {
                pdebug(DEBUG_WARN, "Too much data received!  Expected %d bytes but got %d bytes!", tag->size,
                       (int)(data_end - data));
                rc = PLCTAG_ERR_TOO_LARGE;
            } else {
                pdebug(DEBUG_WARN, "Too little data received!  Expected %d bytes but got %d bytes!", tag->size,
                       (int)(data_end - data));
                rc = PLCTAG_ERR_TOO_SMALL;
            }
            break;
        }

        /* copy data into the tag. */
        mem_copy(tag->data, data, (int)(data_end - data));

        tag->read_in_progress = 0;
        tag->read_complete = 1;

        rc = PLCTAG_STATUS_OK;
    } while(0);

    ab_tag_abort_request(tag);

    pdebug(DEBUG_SPEW, "Done with status %s.", plc_tag_decode_error(rc));

    return rc;
}


int tag_write_start(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    size_t overhead, data_per_packet;

    pdebug(DEBUG_INFO, "Starting.");

    if(tag->is_bit) {
        return tag_write_bit_start(tag);
    }

    do {
        /* check for busy */
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_WARN, "Read (%d) or write (%d) operation already in flight!", tag->read_in_progress, tag->write_in_progress);
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        tag->write_in_progress = 1;

        /* How much overhead? */
        overhead =   sizeof(eip_cpf_co_header)
                   + sizeof(pccc_dhp_write_cmd_req)
                   + (size_t)(unsigned int)tag->encoded_name_size
                   + 1;

        int session_payload_space = session_get_available_cip_payload_space(tag->session);

        if(session_payload_space <= 0) {
            pdebug(DEBUG_WARN, "Unable to get valid payload space from session. Available payload: %d bytes", session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        data_per_packet = (size_t)session_payload_space - overhead;

        if(data_per_packet <= 0) {
            pdebug(DEBUG_WARN, "Unable to send request.  Packet overhead, %d bytes, is too large for available payload, %d bytes!",
                   overhead, session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        if(data_per_packet < (size_t)tag->size) {
            pdebug(DEBUG_WARN, "Tag size is %d, write overhead is %d, and write data per packet is %zu.", tag->size, overhead,
                   data_per_packet);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* get a request buffer */
        rc = session_create_request(tag->session, tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to get new request.  rc=%d", rc);
            tag->write_in_progress = 0;
            break;
        }

        /* point the struct pointers to the buffer */
        eip_cpf_co_header *cip_req = (eip_cpf_co_header *)(req->data);
        pccc_dhp_write_cmd_req *pccc_cmd = (pccc_dhp_write_cmd_req *)(cip_req + 1);
        embed_start = (uint8_t *)(pccc_cmd + 1);

        /* fill in DH+ fields */
        pccc_cmd->dest_link = h2le16(0);
        pccc_cmd->dest_node = h2le16(tag->session->dhp_dest);
        pccc_cmd->src_link = h2le16(0);
        pccc_cmd->src_node = h2le16(0);

        /* fill in PCCC command fields */
        pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        pccc_cmd->pccc_status = 0;
        pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
        pccc_cmd->pccc_function = AB_EIP_PLC5_RANGE_WRITE_FUNC;
        pccc_cmd->pccc_offset = h2le16(0);
        pccc_cmd->pccc_transfer_size = h2le16((uint16_t)(tag->size / 2));

        /* point data pointer just past the fixed data fields */
        data = (uint8_t *)(pccc_cmd + 1);

        /* copy encoded tag name into the request */
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        /* now copy the data to write */
        mem_copy(data, tag->data, tag->size);
        data += tag->size;

        /* debug: request full data length */
        ptrdiff_t calculated_request_size = (ptrdiff_t)(data - req->data);
        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC write request full data length: %td bytes.", calculated_request_size);

        /* debug: dump request data */
        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC write request data:");
        pdebug_dump_bytes(DEBUG_DETAIL, req->data, (int)calculated_request_size);

        /* debug: CIP data length */
        ptrdiff_t cip_request_size = (ptrdiff_t)(data - embed_start);
        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC write request CIP data length: %td bytes.", cip_request_size);

        /* fill in Common Packet Format fields */
        cip_req->cpf_item_count = h2le16(2);
        cip_req->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);
        cip_req->cpf_cai_item_length = h2le16(4);
        cip_req->cpf_targ_conn_id = h2le32(tag->session->targ_connection_id);
        cip_req->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);
        cip_req->cpf_conn_seq_num = h2le16(conn_seq_id);
        cip_req->cpf_cdi_item_length = h2le16((uint16_t)((size_t)cip_request_size + sizeof(cip_req->cpf_conn_seq_num)));

        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC request CPF CDI item length: %u bytes.", le2h16(cip_req->cpf_cdi_item_length));

        cip_req->router_timeout = h2le16(1);
        cip_req->encap_command = h2le16(AB_EIP_CONNECTED_SEND);

        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC write request CPF UDI item length: %u bytes.", le2h16(cip_req->cpf_cdi_item_length));

        cip_req->router_timeout = h2le16(1);
        cip_req->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND);

        /* set request size */
        req->request_size = (int)calculated_request_size;
        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC write request size set to %d bytes.", req->request_size);

        /* add request to session */
        rc = session_add_request(tag->session, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to add write request to session! rc=%d", rc);
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* set tag->req if successful, else clean up */
    if(rc == PLCTAG_STATUS_OK) {
        critical_block(tag->api_mutex) {
            if(tag->req) {
                pdebug(DEBUG_WARN, "Request already set! This should not happen!");
                rc = PLCTAG_ERR_BAD_DATA;
            } else {
                pdebug(DEBUG_INFO, "Setting write request for tag %d", tag->tag_id);
                tag->req = req;
                rc = PLCTAG_STATUS_PENDING;
            }
        }
    } else {
        pdebug(DEBUG_WARN, "Failed to generate new write request rc=%s", plc_tag_decode_error(rc));
        req = rc_dec(req);
        tag->write_in_progress = 0;
        tag->write_complete = 1;
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");
    return rc;
}

int tag_write_bit_start(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    size_t overhead = 0;
    int data_per_packet = 0;

    pdebug(DEBUG_INFO, "Starting.");

    do {
        /* check for busy */
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_WARN, "Read (%d) or write (%d) operation already in flight!", tag->read_in_progress, tag->write_in_progress);
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        tag->write_in_progress = 1;

        /* How much overhead? */
        overhead =   sizeof(eip_cpf_co_header)
                   + sizeof(pccc_dhp_rmw_cmd_req)
                   + (size_t)tag->encoded_name_size
                   + (size_t)(tag->elem_size * 2); /* AND/OR masks */

        int session_payload_space = session_get_available_cip_payload_space(tag->session);

        if(session_payload_space <= 0) {
            pdebug(DEBUG_WARN, "Unable to get valid payload space from session. Available payload: %d bytes", session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        data_per_packet = session_payload_space - (int)overhead;

        if(data_per_packet < 0) {
            pdebug(DEBUG_WARN, "Unable to send request.  Packet overhead, %zu bytes, is too large for available payload, %d bytes!",
                   overhead, session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* check if we can fit the AND/OR masks in the packet */
        if(data_per_packet < (int)(tag->elem_size * 2)) {
            pdebug(DEBUG_WARN, "Tag elem_size is %d, write overhead is %zu, and write data per packet is %zu.", tag->elem_size, overhead,
                   data_per_packet);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* get a request buffer */
        rc = session_create_request(tag->session, tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to get new request.  rc=%d", rc);
            tag->write_in_progress = 0;
            break;
        }

        /* stack the struct pointers as in tag_read_start */
        eip_cpf_co_header *cip_req = (eip_cpf_co_header *)(req->data);
        pccc_dhp_rmw_cmd_req *pccc_cmd = (pccc_dhp_rmw_cmd_req *)(cip_req + 1);
        embed_start = (uint8_t *)(pccc_cmd + 1);
        data = embed_start;

        /* copy encoded tag name into the request */
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        /* AND/reset mask */
        for(int i = 0; i < tag->elem_size; i++) {
            if((tag->bit / 8) == i) {
                uint8_t mask = (uint8_t)(1 << (tag->bit % 8));
                if(tag->data[i] & mask) {
                    *data = (uint8_t)0xFF;
                } else {
                    *data = (uint8_t)~mask;
                }
                pdebug(DEBUG_DETAIL, "adding reset mask byte %d: %x", i, *data);
                data++;
            } else {
                *data = (uint8_t)0xFF;
                pdebug(DEBUG_DETAIL, "adding reset mask byte %d: %x", i, *data);
                data++;
            }
        }
        /* OR/set mask */
        for(int i = 0; i < tag->elem_size; i++) {
            if((tag->bit / 8) == i) {
                *data = tag->data[i] & (uint8_t)(1 << (tag->bit % 8));
                pdebug(DEBUG_DETAIL, "adding set mask byte %d: %x", i, *data);
                data++;
            } else {
                *data = (uint8_t)0x00;
                pdebug(DEBUG_DETAIL, "adding set mask byte %d: %x", i, *data);
                data++;
            }
        }

        /* debug: request full data length */
        ptrdiff_t calculated_request_size = (ptrdiff_t)(data - req->data);
        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC bit write request full data length: %td bytes.", calculated_request_size);

        /* debug: dump request data */
        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC bit write request data:");
        pdebug_dump_bytes(DEBUG_DETAIL, req->data, (int)calculated_request_size);

        /* debug: CIP data length */
        ptrdiff_t cip_request_size = (ptrdiff_t)(data - embed_start);
        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC bit write request CIP data length: %td bytes.", cip_request_size);

        /* fill in DH+ fields */
        pccc_cmd->dest_link = h2le16(0);
        pccc_cmd->dest_node = h2le16(tag->session->dhp_dest);
        pccc_cmd->src_link = h2le16(0);
        pccc_cmd->src_node = h2le16(0);

        /* fill in PCCC command fields */
        pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        pccc_cmd->pccc_status = 0;
        pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
        pccc_cmd->pccc_function = AB_EIP_PLC5_RMW_FUNC;

        /* fill in Common Packet Format fields */
        cip_req->cpf_item_count = h2le16(2);
        cip_req->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);
        cip_req->cpf_cai_item_length = h2le16(4);
        cip_req->cpf_targ_conn_id = h2le32(tag->session->targ_connection_id);
        cip_req->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);
        cip_req->cpf_conn_seq_num = h2le16(conn_seq_id);
        cip_req->cpf_cdi_item_length = h2le16((uint16_t)((size_t)cip_request_size + sizeof(cip_req->cpf_conn_seq_num)));

        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC bit write request CPF CDI item length: %u bytes.", le2h16(cip_req->cpf_cdi_item_length));

        cip_req->router_timeout = h2le16(1);
        cip_req->encap_command = h2le16(AB_EIP_CONNECTED_SEND);

        /* set request size */
        req->request_size = (int)calculated_request_size;
        pdebug(DEBUG_DETAIL, "PLC5 DH+ PCCC bit write request size set to %d bytes.", req->request_size);

        /* add request to session */
        rc = session_add_request(tag->session, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to add bit write request to session! rc=%d", rc);
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* set tag->req if successful, else clean up */
    if(rc == PLCTAG_STATUS_OK) {
        critical_block(tag->api_mutex) {
            if(tag->req) {
                pdebug(DEBUG_WARN, "Request already set! This should not happen!");
                rc = PLCTAG_ERR_BAD_DATA;
            } else {
                pdebug(DEBUG_INFO, "Setting bit write request for tag %d", tag->tag_id);
                tag->req = req;
                rc = PLCTAG_STATUS_PENDING;
            }
        }
    } else {
        pdebug(DEBUG_WARN, "Failed to generate new bit write request rc=%s", plc_tag_decode_error(rc));
        req = rc_dec(req);
        tag->write_in_progress = 0;
        tag->write_complete = 1;
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");
    return rc;
}


/*
 * check_write_status
 *
 * Fragments are not supported.
 */
static int check_write_status(ab_tag_p tag) {
    pccc_resp *pccc;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    /* the request reference is valid. */

    pccc = (pccc_resp *)(tag->req->data);

    /* fake exception */
    do {
        if(pccc->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d", pccc->general_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d - %s", pccc->pccc_status,
                   pccc_decode_error(&pccc->pccc_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        tag->write_in_progress = 0;
        tag->write_complete = 1;

        rc = PLCTAG_STATUS_OK;
    } while(0);

    ab_tag_abort_request(tag);

    pdebug(DEBUG_SPEW, "Done with status %s.", plc_tag_decode_error(rc));

    /* Success! */
    return rc;
}
