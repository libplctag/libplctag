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

/*#ifdef __cplusplus
extern "C"
{
#endif
*/


#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/ab/ab_common.h>
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/ab/eip_lgx_pccc.h>
#include <libplctag/protocols/ab/error_codes.h>
#include <libplctag/protocols/ab/pccc.h>
#include <libplctag/protocols/ab/session.h>
#include <libplctag/protocols/ab/tag.h>
#include <utils/debug.h>


START_PACK typedef struct {
    /* PCCC Command Req Routing */
    uint8_t service_code;           /* ALWAYS 0x4B, Execute PCCC */
    uint8_t req_path_size;          /* ALWAYS 0x02, in 16-bit words */
    uint8_t req_path[4];            /* ALWAYS 0x20,0x67,0x24,0x01 for PCCC */
    uint8_t request_id_size;        /* ALWAYS 7 */
    uint16_le vendor_id;            /* Our CIP Vendor ID */
    uint32_le vendor_serial_number; /* Our CIP Vendor Serial Number */
    /* PCCC Command */
    uint8_t pccc_command;         /* CMD read, write etc. */
    uint8_t pccc_status;          /* STS 0x00 in request */
    uint16_le pccc_seq_num;       /* TNS transaction/sequence id */
    uint8_t pccc_function;        /* FNC sub-function of command */
    uint16_le pccc_offset;        /* offset of requested in total request */
    uint16_le pccc_transfer_size; /* total number of words requested */
} END_PACK embedded_pccc;


static int tag_read_start(ab_tag_p tag);
static int tag_status(ab_tag_p tag);
static int tag_tickler(ab_tag_p tag);
static int tag_write_start(ab_tag_p tag);

struct tag_vtable_t lgx_pccc_vtable = {(tag_vtable_func)ab_tag_abort_request, /* shared */
                                       (tag_vtable_func)tag_read_start, (tag_vtable_func)tag_status, (tag_vtable_func)tag_tickler,
                                       (tag_vtable_func)tag_write_start, (tag_vtable_func)NULL, /* wake_plc */

                                       /* data accessors */
                                       ab_get_int_attrib, ab_set_int_attrib,

                                       ab_get_byte_array_attrib};

static int check_read_status(ab_tag_p tag);
static int check_write_status(ab_tag_p tag);

/*
 * tag_status
 *
 * CIP/PCCC-specific status.
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
            /* read done */
            if(tag->first_read) {
                tag->first_read = 0;
                tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, PLCTAG_STATUS_OK);
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
 * Start a PCCC tag read (PLC5, SLC).
 */

int tag_read_start(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    int session_payload_space = session_get_available_cip_payload_space(tag->session);

    pdebug(DEBUG_INFO, "Starting");

    do {
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_WARN, "Read or write operation already in flight!");
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        tag->read_in_progress = 1;

        int request_overhead = (int)sizeof(eip_cip_uc_req)
                             + (int)sizeof(embedded_pccc)
                             + tag->encoded_name_size
                             + (int)sizeof(uint16_le);

        int request_payload_space = session_payload_space - request_overhead;

        if(request_payload_space < 0 || request_payload_space < tag->size) {
            pdebug(DEBUG_WARN, "Request overhead (%d bytes) exceeds session payload space (%d bytes) or tag size too large.", request_overhead, session_payload_space);
            tag->read_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        rc = session_create_request(tag->session, tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            tag->read_in_progress = 0;
            pdebug(DEBUG_WARN, "Unable to get new request. rc=%d", rc);
            break;
        }

        eip_cip_uc_req *lgx_pccc = (eip_cip_uc_req *)(req->data);
        embedded_pccc *embed_pccc = (embedded_pccc *)(lgx_pccc + 1);
        embed_start = (uint8_t *)(embed_pccc);

        embed_pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE;
        embed_pccc->req_path_size = 2;
        embed_pccc->req_path[0] = 0x20;
        embed_pccc->req_path[1] = 0x67;
        embed_pccc->req_path[2] = 0x24;
        embed_pccc->req_path[3] = 0x01;
        embed_pccc->request_id_size = 7;
        embed_pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);
        embed_pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN);
        embed_pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        embed_pccc->pccc_status = 0;
        embed_pccc->pccc_seq_num = h2le16(conn_seq_id);
        embed_pccc->pccc_function = AB_EIP_PCCCLGX_TYPED_READ_FUNC;
        embed_pccc->pccc_offset = h2le16(0);
        embed_pccc->pccc_transfer_size = h2le16((uint16_t)tag->elem_count);

        data = (uint8_t *)(embed_pccc + 1);
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;
        *((uint16_le *)data) = h2le16((uint16_t)tag->elem_count);
        data += sizeof(uint16_le);
        if((data - embed_start) & 0x01) {
            *data = 0;
            data++;
        }

        lgx_pccc->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND);
        lgx_pccc->router_timeout = h2le16(1);
        lgx_pccc->cpf_item_count = h2le16(2);
        lgx_pccc->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI);
        lgx_pccc->cpf_nai_item_length = h2le16(0);
        lgx_pccc->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI);
        lgx_pccc->cm_service_code = AB_EIP_CMD_UNCONNECTED_SEND;
        lgx_pccc->cm_req_path_size = 0x02;
        lgx_pccc->cm_req_path[0] = 0x20;
        lgx_pccc->cm_req_path[1] = 0x06;
        lgx_pccc->cm_req_path[2] = 0x24;
        lgx_pccc->cm_req_path[3] = 0x01;
        lgx_pccc->secs_per_tick = AB_EIP_SECS_PER_TICK;
        lgx_pccc->timeout_ticks = AB_EIP_TIMEOUT_TICKS;
        lgx_pccc->uc_cmd_length = h2le16((uint16_t)(data - embed_start));
        if(tag->session->conn_path_size > 0) {
            *data = (tag->session->conn_path_size) / 2;
            data++;
            *data = 0;
            data++;
            mem_copy(data, tag->session->conn_path, tag->session->conn_path_size);
            data += tag->session->conn_path_size;
        }
        lgx_pccc->cpf_udi_item_length = h2le16((uint16_t)(data - (uint8_t *)(&lgx_pccc->cm_service_code)));
        req->request_size = (int)(data - (req->data));
        req->allow_packing = tag->allow_packing;
        rc = session_add_request(tag->session, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
            req = rc_dec(req);
            ab_tag_abort_request(tag);
            break;
        }
        tag->req = req;
        rc = PLCTAG_STATUS_PENDING;
    } while(0);

    if(rc != PLCTAG_STATUS_PENDING) {
        tag->read_in_progress = 0;
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

    /* the request reference is valid. */

    /* fake exceptions */
    do {
        pccc_resp *pccc;
        uint8_t *data;
        uint8_t *data_end;
        uint8_t *type_start;
        uint8_t *type_end;
        int pccc_res_type;
        int pccc_res_length;

        pccc = (pccc_resp *)(tag->req->data);

        /* point to the start of the data */
        data = (uint8_t *)pccc + sizeof(*pccc);

        data_end = (tag->req->data + le2h16(pccc->encap_length) + sizeof(eip_encap));

        if(le2h16(pccc->encap_command) != AB_EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", pccc->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(pccc->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(pccc->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: (%d) %s", pccc->general_status,
                   decode_cip_error_long((uint8_t *)&(pccc->general_status)));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d - %s", pccc->pccc_status,
                   pccc_decode_error(&pccc->pccc_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        type_start = data;

        if(!(data = pccc_decode_dt_byte(data, (int)(data_end - data), &pccc_res_type, &pccc_res_length))) {
            pdebug(DEBUG_WARN, "Unable to decode PCCC response data type and data size!");
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        /* this gives us the overall type of the response and the number of bytes remaining in it.
         * If the type is an array, then we need to decode another one of these words
         * to get the type of each element and the size of each element.  We will
         * need to adjust the size if we care.
         */

        if(pccc_res_type == AB_PCCC_DATA_ARRAY) {
            if(!(data = pccc_decode_dt_byte(data, (int)(data_end - data), &pccc_res_type, &pccc_res_length))) {
                pdebug(DEBUG_WARN, "Unable to decode PCCC response array element data type and data size!");
                rc = PLCTAG_ERR_BAD_DATA;
                break;
            }
        }

        type_end = data;

        /* copy data into the tag. */
        if((data_end - data) > tag->size) {
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /*
         * skip this if this is a pre-read.
         * Otherwise this will overwrite the values
         * the user has set, possibly.
         */
        if(!tag->pre_write_read) { mem_copy(tag->data, data, (int)(data_end - data)); }

        /* copy type data into tag. */
        tag->encoded_type_info_size = (int)(type_end - type_start);
        mem_copy(tag->encoded_type_info, type_start, tag->encoded_type_info_size);

        // /* have the IO thread take care of the request buffers */
        // ab_tag_abort_request(tag);

        rc = PLCTAG_STATUS_OK;
    } while(0);

    ab_tag_abort_request(tag);

    /* if this is a pre-read for a write, then pass off the the write routine */
    if(rc == PLCTAG_STATUS_OK && tag->pre_write_read) {
        pdebug(DEBUG_DETAIL, "Restarting write call now.");

        tag->pre_write_read = 0;
        rc = tag_write_start(tag);
    }

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


int tag_write_start(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    int session_payload_space = session_get_available_cip_payload_space(tag->session);

    pdebug(DEBUG_INFO, "Starting");

    do {
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_WARN, "Read or write operation already in flight!");
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        tag->write_in_progress = 1;

        if(tag->first_read) {
            pdebug(DEBUG_DETAIL, "No read has completed yet, doing pre-read to get type information.");
            tag->pre_write_read = 1;
            tag->write_in_progress = 0;
            rc = tag_read_start(tag);
            break;
        }

        int request_overhead = (int)sizeof(eip_cip_uc_req)
                             + (int)sizeof(embedded_pccc)
                             + tag->encoded_name_size
                             + tag->encoded_type_info_size
                             + (int)((tag->session->conn_path_size > 0) ? (2 + tag->session->conn_path_size) : 0);

        int request_payload_space = session_payload_space - request_overhead;

        if(request_payload_space < 0 || request_payload_space < tag->size) {
            pdebug(DEBUG_WARN, "Request overhead (%d bytes) exceeds session payload space (%d bytes) or tag size too large.", request_overhead, session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        rc = session_create_request(tag->session, tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            tag->write_in_progress = 0;
            pdebug(DEBUG_WARN, "Unable to get new request. rc=%d", rc);
            break;
        }

        eip_cip_uc_req *lgx_pccc = (eip_cip_uc_req *)(req->data);
        embedded_pccc *embed_pccc = (embedded_pccc *)(lgx_pccc + 1);
        embed_start = (uint8_t *)(embed_pccc);

        embed_pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE;
        embed_pccc->req_path_size = 2;
        embed_pccc->req_path[0] = 0x20;
        embed_pccc->req_path[1] = 0x67;
        embed_pccc->req_path[2] = 0x24;
        embed_pccc->req_path[3] = 0x01;
        embed_pccc->request_id_size = 7;
        embed_pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);
        embed_pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN);
        embed_pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        embed_pccc->pccc_status = 0;
        embed_pccc->pccc_seq_num = h2le16(conn_seq_id);
        embed_pccc->pccc_function = AB_EIP_PCCCLGX_TYPED_WRITE_FUNC;
        embed_pccc->pccc_offset = h2le16(0);
        embed_pccc->pccc_transfer_size = h2le16((uint16_t)tag->elem_count);

        data = (uint8_t *)(embed_pccc + 1);
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;
        mem_copy(data, tag->encoded_type_info, tag->encoded_type_info_size);
        data += tag->encoded_type_info_size;
        mem_copy(data, tag->data, tag->size);
        data += tag->size;
        if((data - embed_start) & 0x01) {
            *data = 0;
            data++;
        }

        lgx_pccc->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND);
        lgx_pccc->router_timeout = h2le16(1);
        lgx_pccc->cpf_item_count = h2le16(2);
        lgx_pccc->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI);
        lgx_pccc->cpf_nai_item_length = h2le16(0);
        lgx_pccc->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI);
        lgx_pccc->cm_service_code = AB_EIP_CMD_UNCONNECTED_SEND;
        lgx_pccc->cm_req_path_size = 0x02;
        lgx_pccc->cm_req_path[0] = 0x20;
        lgx_pccc->cm_req_path[1] = 0x06;
        lgx_pccc->cm_req_path[2] = 0x24;
        lgx_pccc->cm_req_path[3] = 0x01;
        lgx_pccc->secs_per_tick = AB_EIP_SECS_PER_TICK;
        lgx_pccc->timeout_ticks = AB_EIP_TIMEOUT_TICKS;
        lgx_pccc->uc_cmd_length = h2le16((uint16_t)(data - embed_start));
        if(tag->session->conn_path_size > 0) {
            *data = (tag->session->conn_path_size) / 2;
            data++;
            *data = 0;
            data++;
            mem_copy(data, tag->session->conn_path, tag->session->conn_path_size);
            data += tag->session->conn_path_size;
        }
        lgx_pccc->cpf_udi_item_length = h2le16((uint16_t)(data - (uint8_t *)(&lgx_pccc->cm_service_code)));
        req->request_size = (int)(data - (req->data));
        req->allow_packing = tag->allow_packing;
        rc = session_add_request(tag->session, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
            req = rc_dec(req);
            ab_tag_abort_request(tag);
            break;
        }
        tag->req = req;
        rc = PLCTAG_STATUS_PENDING;
    } while(0);

    if(rc != PLCTAG_STATUS_PENDING) {
        tag->write_in_progress = 0;
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
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting");

    do {
        pccc_resp *pccc = (pccc_resp *)(tag->req->data);

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

        rc = PLCTAG_STATUS_OK;
    } while(0);

    ab_tag_abort_request(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}
