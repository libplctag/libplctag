/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
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

/*
 * PCCC mapped onto a Logix controller.
 *
 * A ControlLogix answers the Execute PCCC service with the typed read and typed write
 * functions, 0x68 and 0x67, against a symbolic tag name rather than a PLC/5 or SLC logical
 * address.  Everything around those two commands -- the request prefix, the tickler, the
 * status call, attaching the request to the tag -- is shared with pccc.c; the two commands
 * and the typed reply they produce are all that is local.
 *
 * A typed read carries its own data-type descriptor, which a range read does not.  That is
 * why check_read_status() here decodes the descriptor and saves it as tag->encoded_type_info,
 * and why a typed write has to send that descriptor back.  The create read supplies it, so
 * every write has it.
 */

#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/ab/ab_common.h>
#include <libplctag/modules/ab/eip_lgx_pccc.h>
#include <libplctag/modules/ab/pccc.h>
#include <libplctag/modules/cip/error_codes.h>
#include <libplctag/modules/cip/tag.h>
#include <stddef.h>
#include <utils/byteorder.h>
#include <utils/debug.h>


static int tag_read_start(cip_tag_p tag);
static int tag_tickler(cip_tag_p tag);
static int tag_write_start(cip_tag_p tag);
static int check_read_status(cip_tag_p tag);
static int check_write_status(cip_tag_p tag);

struct tag_vtable_t lgx_pccc_vtable = {
    .abort = (tag_vtable_func)cip_data_tag_abort,
    .read = (tag_vtable_func)tag_read_start,
    .status = (tag_vtable_func)pccc_tag_status,
    .tickler = (tag_vtable_func)tag_tickler,
    .write = (tag_vtable_func)tag_write_start,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* data accessors */
    .attribs = ab_attribs,
};


int tag_tickler(cip_tag_p tag) { return pccc_tickle(tag, check_read_status, check_write_status); }


/*
 * tag_read_start
 *
 * Start a PCCC typed read against a Logix controller.
 */
int tag_read_start(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    cip_request_p req = NULL;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));
    uint8_t *data = NULL;
    size_t overhead = 0;

    /* remember the TNS so pccc_check_response_header() can match the reply to this request. */
    tag->req_pccc_seq_num = conn_seq_id;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Starting.");

    if(tag->create_read_status < PLCTAG_STATUS_OK) { return tag->create_read_status; }

    do {
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
            return PLCTAG_ERR_BUSY;
        }

        tag->read_in_progress = 1;

        /* the whole tag has to come back in one reply: PCCC has no fragmentation. */
        rc = pccc_check_payload_space(tag, sizeof(cip_pccc_full_resp), (size_t)(unsigned int)tag->size);
        if(rc != PLCTAG_STATUS_OK) { break; }

        /*
         * The request is the Execute PCCC prefix, the typed read command, the symbolic name,
         * the element count, and a pad byte if the result is an odd length.
         */
        overhead = sizeof(cip_pccc_req) + sizeof(plc5_pccc_read_cmd_req) + (size_t)(unsigned int)tag->encoded_name_size
                   + sizeof(uint16_le) + 1;

        rc = pccc_check_payload_space(tag, overhead, 0);
        if(rc != PLCTAG_STATUS_OK) { break; }

        rc = session_create_request(tag->session, tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Unable to get new request, %s!", plc_tag_decode_error(rc));
            break;
        }

        if(overhead > (size_t)(unsigned int)req->request_capacity) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                   "PCCC request overhead (%zu bytes) exceeds request capacity (%d bytes).", overhead, req->request_capacity);
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        data = pccc_write_prefix(tag, req, false);

        plc5_pccc_read_cmd_req *pccc_cmd = (plc5_pccc_read_cmd_req *)data;

        pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        pccc_cmd->pccc_status = 0;
        pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
        pccc_cmd->pccc_function = AB_EIP_PCCCLGX_TYPED_READ_FUNC;
        pccc_cmd->pccc_offset = h2le16(0);
        pccc_cmd->pccc_transfer_size = h2le16((uint16_t)tag->elem_count);

        data = (uint8_t *)(pccc_cmd + 1);

        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        *((uint16_le *)data) = h2le16((uint16_t)tag->elem_count);
        data += sizeof(uint16_le);

        /* the PCCC command must end on an even boundary. */
        if((data - req->data) & 0x01) {
            *data = 0;
            data++;
        }

        /* hand the finished CIP message to the connection, which adds the routed framing. */
        rc = cip_submit_payload(tag->session, req, (int)(data - req->data), tag->allow_packing);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Unable to submit the request, %s!",
                   plc_tag_decode_error(rc));
            req = NULL; /* cip_submit_payload() released it. */
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    rc = pccc_finish_request(tag, req, rc, false);

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Done.");

    return rc;
}


/*
 * check_read_status
 *
 * NOTE that we can have only one outstanding request because PCCC
 * does not support fragments.
 */
static int check_read_status(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_SPEW, tag->tag_id, "Starting.");

    /* fake exceptions */
    do {
        cip_pccc_full_resp *pccc = NULL;
        uint8_t *data = NULL;
        uint8_t *data_end = NULL;
        uint8_t *type_start = NULL;
        uint8_t *type_end = NULL;
        int pccc_res_type = 0;
        int pccc_res_length = 0;

        rc = pccc_check_response_header(tag, false);
        if(rc != PLCTAG_STATUS_OK) { break; }

        pccc = (cip_pccc_full_resp *)(tag->req->data);

        /* point to the start of the data */
        data = (uint8_t *)pccc + sizeof(*pccc);

        data_end = tag->req->data + tag->req->request_size;

        if(pccc->general_status != CIP_EIP_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "PCCC command failed, response code: (%d) %s",
                   pccc->general_status,
                   decode_cip_error_long((uint8_t *)&(pccc->general_status),
                                         cip_error_data_size((uint8_t *)&(pccc->general_status), data_end)));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->pccc_status != CIP_EIP_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "PCCC command failed, response code: %d - %s",
                   pccc->pccc_status, pccc_decode_error(&pccc->pccc_status, cip_error_data_size(&pccc->pccc_status, data_end)));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        type_start = data;

        if(!(data = pccc_decode_dt_byte(data, (int)(data_end - data), &pccc_res_type, &pccc_res_length))) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Unable to decode PCCC response data type and data size!");
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        /*
         * this gives us the overall type of the response and the number of bytes remaining in it.
         * If the type is an array, then we need to decode another one of these words
         * to get the type of each element and the size of each element.  We will
         * need to adjust the size if we care.
         */

        if(pccc_res_type == AB_PCCC_DATA_ARRAY) {
            if(!(data = pccc_decode_dt_byte(data, (int)(data_end - data), &pccc_res_type, &pccc_res_length))) {
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                       "Unable to decode PCCC response array element data type and data size!");
                rc = PLCTAG_ERR_BAD_DATA;
                break;
            }
        }

        type_end = data;

        /* copy data into the tag. */
        if((intptr_t)data > (intptr_t)data_end || (data_end - data) > (ptrdiff_t)tag->size) {
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        mem_copy(tag->data, data, (int)(data_end - data));

        /* copy type data into tag. */
        if((intptr_t)type_start > (intptr_t)type_end) {
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        tag->encoded_type_info_size = (int)(type_end - type_start);

        if(tag->encoded_type_info_size > (int)sizeof(tag->encoded_type_info)) {
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        mem_copy(tag->encoded_type_info, type_start, tag->encoded_type_info_size);

        rc = PLCTAG_STATUS_OK;
    } while(0);

    cip_tag_abort_request(tag);

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


/*
 * tag_write_start
 *
 * Build and submit the PCCC typed write of the whole buffer.
 *
 * This family has no masked write.  A ControlLogix's PCCC emulation serves only the typed
 * pair 0x68/0x67 -- the PLC/5 read-modify-write function 0x26 is not mapped -- and the
 * logical address it takes has no bit field.  So a bit write sends the whole word, and the
 * other fifteen bits are whatever the tag last read.  The library does not read first: the
 * app owns the buffer and must refresh it before a bit write if the word can change.
 */
int tag_write_start(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    cip_request_p req = NULL;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));
    uint8_t *data = NULL;
    size_t overhead = 0;

    /* remember the TNS so pccc_check_response_header() can match the reply to this request. */
    tag->req_pccc_seq_num = conn_seq_id;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Starting.");

    if(tag->create_read_status < PLCTAG_STATUS_OK) { return tag->create_read_status; }

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    do {
        tag->write_in_progress = 1;

        /*
         * The request is the Execute PCCC prefix, the typed write command, the symbolic name,
         * the type descriptor, the data, and a pad byte if the result is an odd length.
         */
        overhead = sizeof(cip_pccc_req) + sizeof(plc5_pccc_write_cmd_req) + (size_t)(unsigned int)tag->encoded_name_size
                   + (size_t)(unsigned int)tag->encoded_type_info_size + 1;

        /* the whole tag has to go out in one request: PCCC has no fragmentation. */
        rc = pccc_check_payload_space(tag, overhead, (size_t)(unsigned int)tag->size);
        if(rc != PLCTAG_STATUS_OK) { break; }

        rc = session_create_request(tag->session, tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Unable to get new request, %s!", plc_tag_decode_error(rc));
            break;
        }

        /*
         * Nothing above has bounded the data against the request buffer, and tag->size comes
         * from the attribute string, so check before copying into it.
         */
        if(overhead + (size_t)(unsigned int)tag->size > (size_t)(unsigned int)req->request_capacity) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                   "PCCC request of %zu bytes exceeds request capacity (%d bytes).",
                   overhead + (size_t)(unsigned int)tag->size, req->request_capacity);
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        data = pccc_write_prefix(tag, req, false);

        plc5_pccc_write_cmd_req *pccc_cmd = (plc5_pccc_write_cmd_req *)data;

        pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        pccc_cmd->pccc_status = 0;
        pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
        pccc_cmd->pccc_function = AB_EIP_PCCCLGX_TYPED_WRITE_FUNC;
        pccc_cmd->pccc_offset = h2le16(0);
        pccc_cmd->pccc_transfer_size = h2le16((uint16_t)tag->elem_count);

        data = (uint8_t *)(pccc_cmd + 1);

        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        mem_copy(data, tag->encoded_type_info, tag->encoded_type_info_size);
        data += tag->encoded_type_info_size;

        mem_copy(data, tag->data, tag->size);
        data += tag->size;

        /* the PCCC command must end on an even boundary. */
        if((data - req->data) & 0x01) {
            *data = 0;
            data++;
        }

        /* hand the finished CIP message to the connection, which adds the routed framing. */
        rc = cip_submit_payload(tag->session, req, (int)(data - req->data), tag->allow_packing);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Unable to submit the request, %s!",
                   plc_tag_decode_error(rc));
            req = NULL; /* cip_submit_payload() released it. */
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    rc = pccc_finish_request(tag, req, rc, true);

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Done.");

    return rc;
}


/*
 * check_write_status
 *
 * Fragments are not supported.
 */
static int check_write_status(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_SPEW, tag->tag_id, "Starting.");

    do {
        cip_pccc_full_resp *pccc = NULL;
        uint8_t *data_end = NULL;

        rc = pccc_check_response_header(tag, false);
        if(rc != PLCTAG_STATUS_OK) { break; }

        pccc = (cip_pccc_full_resp *)(tag->req->data);
        data_end = tag->req->data + tag->req->request_size;

        if(pccc->general_status != CIP_EIP_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "PCCC command failed, response code: (%d) %s",
                   pccc->general_status,
                   decode_cip_error_long((uint8_t *)&(pccc->general_status),
                                         cip_error_data_size((uint8_t *)&(pccc->general_status), data_end)));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->pccc_status != CIP_EIP_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "PCCC command failed, response code: %d - %s",
                   pccc->pccc_status, pccc_decode_error(&pccc->pccc_status, cip_error_data_size(&pccc->pccc_status, data_end)));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    cip_tag_abort_request(tag);

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}
