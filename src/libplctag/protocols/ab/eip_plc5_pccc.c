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

#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/ab/ab_common.h>
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/ab/eip_plc5_pccc.h>
#include <libplctag/protocols/ab/error_codes.h>
#include <libplctag/protocols/ab/pccc.h>
#include <libplctag/protocols/ab/session.h>
#include <libplctag/protocols/ab/tag.h>
#include <stdint.h>
#include <utils/debug.h>


/* PCCC */
static int tag_read_start(ab_tag_p tag);
static int tag_status(ab_tag_p tag);
static int tag_tickler(ab_tag_p tag);
static int tag_write_start(ab_tag_p tag);

struct tag_vtable_t plc5_vtable = {(tag_vtable_func)ab_tag_abort_request, /* shared */
                                   (tag_vtable_func)tag_read_start, (tag_vtable_func)tag_status, (tag_vtable_func)tag_tickler,
                                   (tag_vtable_func)tag_write_start, (tag_vtable_func)NULL, /* wake_plc */

                                   /* data accessors */
                                   ab_get_int_attrib, ab_set_int_attrib,

                                   ab_get_byte_array_attrib};


/* default string types used for PLC-5 PLCs. */
tag_byte_order_t plc5_tag_byte_order = {.is_allocated = 0,

                                        .int16_order = {0, 1},
                                        .int32_order = {0, 1, 2, 3},
                                        .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                        .float32_order = {2, 3, 0, 1}, /* yes, it is that weird. */
                                        .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                        .str_is_defined = 1,
                                        .str_is_counted = 1,
                                        .str_is_fixed_length = 1,
                                        .str_is_zero_terminated = 0,
                                        .str_is_byte_swapped = 1,

                                        .str_pad_to_multiple_bytes = 2,
                                        .str_count_word_bytes = 2,
                                        .str_max_capacity = 82,
                                        .str_total_length = 84,
                                        .str_pad_bytes = 0};


static int check_read_status(ab_tag_p tag);
static int check_write_status(ab_tag_p tag);

// START_PACK typedef struct {
//     /* encap header */
//     uint16_le encap_command;        /* ALWAYS 0x006f Unconnected Send*/
//     uint16_le encap_length;         /* packet size in bytes - 24 */
//     uint32_le encap_session_handle; /* from session set up */
//     uint32_le encap_status;         /* always _sent_ as 0 */
//     uint64_le encap_sender_context; /* whatever we want to set this to, used for
//                                      * identifying responses when more than one
//                                      * are in flight at once.
//                                      */
//     uint32_le encap_options;        /* 0, reserved for future use */

//     /* Interface Handle etc. */
//     uint32_le interface_handle; /* ALWAYS 0 */
//     uint16_le router_timeout;   /* in seconds, 5 or 10 seems to be good.*/

//     /* Common Packet Format - CPF Unconnected */
//     uint16_le cpf_item_count;      /* ALWAYS 2 */
//     uint16_le cpf_nai_item_type;   /* ALWAYS 0 */
//     uint16_le cpf_nai_item_length; /* ALWAYS 0 */
//     uint16_le cpf_udi_item_type;   /* ALWAYS 0x00B2 - Unconnected Data Item */
//     uint16_le cpf_udi_item_length; /* REQ: fill in with length of remaining data. */

//     /* PCCC Command Req Routing */
//     uint8_t service_code;           /* ALWAYS 0x4B, Execute PCCC */
//     uint8_t req_path_size;          /* ALWAYS 0x02, in 16-bit words */
//     uint8_t req_path[4];            /* ALWAYS 0x20,0x67,0x24,0x01 for PCCC */
//     uint8_t request_id_size;        /* ALWAYS 7 */
//     uint16_le vendor_id;            /* Our CIP Vendor ID */
//     uint32_le vendor_serial_number; /* Our CIP Vendor Serial Number */

//     /* PCCC Command */
//     uint8_t pccc_command;   /* CMD read, write etc. */
//     uint8_t pccc_status;    /* STS 0x00 in request */
//     uint16_le pccc_seq_num; /* TNS transaction/sequence id */
//     uint8_t pccc_function;  /* FNC sub-function of command */
//     // uint16_le pccc_transfer_offset;  /* offset of requested in total request */
//     // uint16_le pccc_transfer_size;    /* total number of elements requested */
// } END_PACK pccc_req;


START_PACK typedef struct {
    /* PCCC Command Req Routing */
    uint8_t service_code;           /* ALWAYS 0x4B, Execute PCCC */
    uint8_t req_path_size;          /* ALWAYS 0x02, in 16-bit words */
    uint8_t req_path[4];            /* ALWAYS 0x20,0x67,0x24,0x01 for PCCC */
    uint8_t request_id_size;        /* ALWAYS 7 */
    uint16_le vendor_id;            /* Our CIP Vendor ID */
    uint32_le vendor_serial_number; /* Our CIP Vendor Serial Number */
} END_PACK cip_pccc_req;

START_PACK typedef struct {
    /* CIP Reply */
    uint8_t reply_code;     /* 0xCB Execute PCCC Reply */
    uint8_t reserved;       /* 0x00 in reply */
    uint8_t general_status; /* 0x00 for success */
    uint8_t status_size;    /* number of 16-bit words of extra status, 0 if success */

    /* PCCC matching info */
    uint8_t request_id_size;        /* ALWAYS 7 */
    uint16_le vendor_id;            /* Our CIP Vendor ID */
    uint32_le vendor_serial_number; /* Our CIP Vendor Serial Number */
} END_PACK cip_pccc_resp;


START_PACK typedef struct {
    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
    uint8_t pccc_function;  /* FNC sub-function of command */
} END_PACK pccc_cmd_req;


START_PACK typedef struct {
    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
    uint8_t pccc_function;  /* FNC sub-function of command */
    uint16_le pccc_offset;  /* offset of requested in total request */
    uint16_le pccc_transfer_size; /* total number of words requested */
} END_PACK pccc_read_cmd_req;

START_PACK typedef struct {
    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
    uint8_t pccc_function;  /* FNC sub-function of command */
    uint16_le pccc_offset;  /* offset of requested in total request */
    uint16_le pccc_transfer_size; /* total number of words requested */
} END_PACK pccc_write_cmd_req;



START_PACK typedef struct {
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
} END_PACK pccc_cmd_resp;

/*
 * tag_status
 *
 * get the tag status.
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
            /* first read done */
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
        int response_overhead = sizeof(cip_pccc_resp) + sizeof(pccc_cmd_resp);
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
                   "Tag size (%d bytes) exceeds available response data space (%d bytes). PLC5 PCCC does not support fragmentation.",
                   tag->size, response_payload_space);
            tag->read_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* calculate request overhead */
        int request_overhead =   (int)sizeof(cip_pccc_req)
                               + (int)sizeof(pccc_read_cmd_req)
                               + tag->encoded_name_size
                               + 1;

        pdebug(DEBUG_INFO, "PLC5 request overhead: CIP/PCCC header size %zu, PCCC read command header size %zu, encoded_name=%d, data_size=1, total=%d bytes",
               sizeof(cip_pccc_req), sizeof(pccc_read_cmd_req), tag->encoded_name_size, request_overhead);

        int request_payload_space = cip_payload_space - request_overhead;

        if(request_payload_space < 0) {
            pdebug(
                DEBUG_WARN,
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
            pdebug(DEBUG_WARN, "PLC5 PCCC request overhead (%d bytes) exceeds request capacity (%d bytes).", request_overhead,
                   req->request_capacity);
            rc_dec(req);
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* point the struct pointers to the buffer */
        eip_cpf_uc_header *cip_req = (eip_cpf_uc_header *)(req->data);
        cip_pccc_req *cip_pccc = (cip_pccc_req *)(cip_req + 1);
        pccc_read_cmd_req *pccc_cmd = (pccc_read_cmd_req *)(cip_pccc + 1);
        embed_start = (uint8_t *)(&cip_pccc->service_code);

        /* fill in CIP/PCCC header fields */
        cip_pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE;
        cip_pccc->req_path_size = 2;
        cip_pccc->req_path[0] = 0x20;
        cip_pccc->req_path[1] = 0x67;
        cip_pccc->req_path[2] = 0x24;
        cip_pccc->req_path[3] = 0x01;

        cip_pccc->request_id_size = 7;
        cip_pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);
        cip_pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN);

        /* fill in PCCC command fields */
        pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        pccc_cmd->pccc_status = 0;
        pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
        pccc_cmd->pccc_function = AB_EIP_PLC5_RANGE_READ_FUNC;
        pccc_cmd->pccc_offset = h2le16(0);
        pccc_cmd->pccc_transfer_size = h2le16((uint16_t)(tag->size / 2));

        /* point data pointer just past the fixed data fields */
        data = (uint8_t *)(pccc_cmd) + sizeof(*pccc_cmd);

        /* copy encoded tag name into the request */
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        /* add data size byte */
        *data = (uint8_t)(tag->size);
        data++;

        /* debug: request full data length */
        ptrdiff_t calculated_request_size = (ptrdiff_t)(data - req->data);
        pdebug(DEBUG_WARN, "PLC5 PCCC request full data length: %td bytes.", calculated_request_size);

        /* debug: CIP data length */
        ptrdiff_t cip_request_size = (ptrdiff_t)(data - embed_start);
        pdebug(DEBUG_WARN, "PLC5 PCCC request CIP data length: %td bytes.", cip_request_size);

        /* fill in Common Packet Format fields */
        cip_req->cpf_item_count = h2le16(2);
        cip_req->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI);
        cip_req->cpf_nai_item_length = h2le16(0);
        cip_req->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI);
        cip_req->cpf_udi_item_length = h2le16((uint16_t)cip_request_size);

        pdebug(DEBUG_WARN, "PLC5 PCCC request CPF UDI item length: %u bytes.", le2h16(cip_req->cpf_udi_item_length));

        cip_req->router_timeout = h2le16(1);
        cip_req->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND);

        /* set request size */
        req->request_size = (int)calculated_request_size;
        pdebug(DEBUG_WARN, "PLC5 PCCC request size set to %d bytes.", req->request_size);

        /* debug: dump request data */
        pdebug(DEBUG_WARN, "PLC5 PCCC request data:");
        pdebug_dump_bytes(DEBUG_WARN, req->data, (int)calculated_request_size);

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
    eip_cpf_uc_header *eip_cpf = (eip_cpf_uc_header *)(tag->req->data);
    cip_pccc_resp *cip_pccc = (cip_pccc_resp *)(eip_cpf + 1);
    pccc_cmd_resp *pccc_cmd = (pccc_cmd_resp *)(cip_pccc + 1);

    uint8_t *data = (uint8_t *)(pccc_cmd + 1);
    uint8_t *data_end = tag->req->data + tag->req->request_size;

    /* fake exceptions */
    do {
        if(cip_pccc->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: (%d) %s", cip_pccc->general_status,
                   decode_cip_error_long((uint8_t *)&(cip_pccc->general_status)));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

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

    do {
        /* check for busy */
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_WARN, "Read (%d) or write (%d) operation already in flight!", tag->read_in_progress, tag->write_in_progress);
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        tag->write_in_progress = 1;

        /* How much overhead? */
        overhead =   sizeof(cip_pccc_req)
                   + sizeof(pccc_write_cmd_req)
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
        eip_cpf_uc_header *cip_req = (eip_cpf_uc_header *)(req->data);
        cip_pccc_req *cip_pccc = (cip_pccc_req *)(cip_req + 1);
        pccc_write_cmd_req *pccc_cmd = (pccc_write_cmd_req *)(cip_pccc + 1);
        embed_start = (uint8_t *)(&cip_pccc->service_code);

        /* fill in CIP/PCCC header fields */
        cip_pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE;
        cip_pccc->req_path_size = 2;
        cip_pccc->req_path[0] = 0x20;
        cip_pccc->req_path[1] = 0x67;
        cip_pccc->req_path[2] = 0x24;
        cip_pccc->req_path[3] = 0x01;

        cip_pccc->request_id_size = (uint8_t)(sizeof(cip_pccc->request_id_size) + sizeof(cip_pccc->vendor_id) + sizeof(cip_pccc->vendor_serial_number));
        cip_pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);
        cip_pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN);

        /* fill in PCCC command fields */
        pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        pccc_cmd->pccc_status = 0;
        pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
        pccc_cmd->pccc_function = (tag->is_bit ? AB_EIP_PLC5_RMW_FUNC : AB_EIP_PLC5_RANGE_WRITE_FUNC);
        pccc_cmd->pccc_offset = h2le16(0);
        pccc_cmd->pccc_transfer_size = h2le16((uint16_t)(tag->size / 2));

        /* point data pointer just past the fixed data fields */
        data = (uint8_t *)(pccc_cmd + 1);

        /* copy encoded tag name into the request */
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        /* now copy the data to write */
        if(!tag->is_bit) {
            mem_copy(data, tag->data, tag->size);
            data += tag->size;
        } else {
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
        }

        /* debug: request full data length */
        ptrdiff_t calculated_request_size = (ptrdiff_t)(data - req->data);
        pdebug(DEBUG_WARN, "PLC5 PCCC write request full data length: %td bytes.", calculated_request_size);

        /* debug: dump request data */
        pdebug(DEBUG_WARN, "PLC5 PCCC write request data:");
        pdebug_dump_bytes(DEBUG_WARN, req->data, (int)calculated_request_size);

        /* debug: CIP data length */
        ptrdiff_t cip_request_size = (ptrdiff_t)(data - embed_start);
        pdebug(DEBUG_WARN, "PLC5 PCCC write request CIP data length: %td bytes.", cip_request_size);

        /* fill in Common Packet Format fields */
        cip_req->cpf_item_count = h2le16(2);
        cip_req->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI);
        cip_req->cpf_nai_item_length = h2le16(0);
        cip_req->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI);
        cip_req->cpf_udi_item_length = h2le16((uint16_t)cip_request_size);

        pdebug(DEBUG_WARN, "PLC5 PCCC write request CPF UDI item length: %u bytes.", le2h16(cip_req->cpf_udi_item_length));

        cip_req->router_timeout = h2le16(1);
        cip_req->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND);

        /* set request size */
        req->request_size = (int)calculated_request_size;
        pdebug(DEBUG_WARN, "PLC5 PCCC write request size set to %d bytes.", req->request_size);

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

    pdebug(DEBUG_WARN, "Done with status %s.", plc_tag_decode_error(rc));

    /* Success! */
    return rc;
}
