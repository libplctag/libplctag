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

/*#ifdef __cplusplus
extern "C"
{
#endif
*/


#include <inttypes.h>
#include <stddef.h>

#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/ab/ab_common.h>
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/ab/eip_lgx_pccc.h>
#include <libplctag/protocols/cip/error_codes.h>
#include <libplctag/protocols/ab/pccc.h>
#include <libplctag/protocols/ab/session.h>
#include <libplctag/protocols/ab/tag.h>
#include <utils/debug.h>
#include <utils/macros.h>
#include <utils/mem.h>
#include <utils/rc.h>


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

struct tag_vtable_t lgx_pccc_vtable = {
    .abort = (tag_vtable_func)ab_tag_abort_request,
    .read = (tag_vtable_func)tag_read_start,
    .status = (tag_vtable_func)tag_status,
    .tickler = (tag_vtable_func)tag_tickler,
    .write = (tag_vtable_func)tag_write_start,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* data accessors */
    .get_int_attrib = ab_get_int_attrib,
    .set_int_attrib = ab_set_int_attrib,
    .get_byte_array_attrib = ab_get_byte_array_attrib,
};

static int check_read_status(ab_tag_p tag);
static int check_write_status(ab_tag_p tag);

/*
 * tag_status
 *
 * CIP/PCCC-specific status.
 */
int tag_status(ab_tag_p tag) {
    if(!tag->conn) {
        /* this is not OK.  This is fatal! */
        return PLCTAG_ERR_CREATE;
    }

    if(tag->read_in_progress) { return PLCTAG_STATUS_PENDING; }

    if(tag->write_in_progress) { return PLCTAG_STATUS_PENDING; }

    return tag->status;
}


int tag_tickler(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_SPEW, tag->tag_id, "Read in progress.");
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
        pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_SPEW, tag->tag_id, "Write in progress.");
        rc = check_write_status(tag);
        tag->status = (int8_t)rc;

        /* check to see if the write finished. */
        if(!tag->write_in_progress) { tag->write_complete = 1; }

        return rc;
    }

    pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_SPEW, tag->tag_id, "Done.");

    return tag->status;
}


/*
 * The read and write requests differ in six places and agree everywhere else:
 * the in-progress flag, the pre-write read, the overhead estimate, the PCCC
 * function code, and the bytes written after the encoded tag name.  Everything
 * between -- the busy check, the request allocation, the embedded PCCC header
 * and the whole Unconnected Send frame -- was written out twice.  One skeleton
 * builds the frame; this table says what is different about each direction.
 */
typedef struct {
    const char *name;

    /* the PCCC FNC code. */
    uint8_t pccc_function;

    /* true for the write direction, which sets tag->write_in_progress. */
    bool is_write;

    /*
     * Runs after the in-progress flag is set and before anything is built.
     * Returns PLCTAG_STATUS_OK to carry on building, or the status the caller
     * should get instead.  May be NULL.
     */
    int (*pre_request)(ab_tag_p tag);

    /*
     * How much room past the session's payload budget this direction needs.
     *
     * The two formulas do not agree about the connection path: the read ignores
     * it and counts a trailing uint16_le, the write counts 2 + conn_path_size and
     * counts no trailing pad.  Both also add sizeof(eip_cip_uc_req), which is
     * measured from byte 0 of req->data and so includes the EIP encapsulation
     * header.  Meanwhile session_get_available_cip_payload_space() takes the
     * unconnected branch here -- AB_PLC_LGX_PCCC always sets use_connected_msg
     * to 0, ab_common.c:311 -- and has already subtracted 2 + conn_path_size
     * itself, so at least one of the two counts the path twice.
     *
     * Each formula is carried over unchanged rather than reconciled.  Shrinking
     * a size estimate is the direction that lets a longer frame onto the wire,
     * and settling which byte each side counts from is the whole of the
     * response-size accounting work; see 2.40 in docs/deferred_fixes.md.  The
     * same mistake made in the other direction is what 1.9 records.
     */
    int (*request_overhead)(ab_tag_p tag);

    /* write everything that follows the encoded tag name. */
    void (*write_body)(ab_tag_p tag, uint8_t **data);
} lgx_pccc_variant_t;


static void set_in_progress(ab_tag_p tag, const lgx_pccc_variant_t *variant, bool in_progress) {
    if(variant->is_write) {
        tag->write_in_progress = (in_progress ? 1 : 0);
    } else {
        tag->read_in_progress = (in_progress ? 1 : 0);
    }
}


static int read_request_overhead(ab_tag_p tag) {
    return (int)sizeof(eip_cip_uc_req) + (int)sizeof(embedded_pccc) + tag->encoded_name_size + (int)sizeof(uint16_le);
}


static void read_write_body(ab_tag_p tag, uint8_t **data) {
    *((uint16_le *)(*data)) = h2le16((uint16_t)tag->elem_count);
    *data += sizeof(uint16_le);
}


/*
 * Make sure there is a type descriptor to send.
 *
 * A typed write carries one ahead of the data.  There are three ways to have one, in
 * descending order of confidence:
 *
 *   1. The descriptor a read reply carried.  It is the PLC's own answer for this exact
 *      file, so it is preferred whenever there is one -- which, since AB_PLC_LGX_PCCC
 *      reads at creation again, is every ordinary write.
 *   2. One built from the data file type.  The tag name already says everything needed:
 *      N is an integer file of two-byte elements, F a float file of four.  No round trip.
 *   3. A read issued now, with the write restarted when it lands.
 *
 * Route 2 exists because route 3 was silently unreachable for years: ab_common.c cleared
 * first_read for this PLC type, which was the only thing gating the pre-write read, so a
 * write with no read behind it sent a descriptor of zero bytes and the PLC refused the
 * command.  See 1.10 in docs/deferred_fixes.md.
 *
 * Route 2 is deliberately the fallback rather than the default.  pccc_encode_type_info()
 * agrees with a ControlLogix on what the descriptor means but not byte for byte -- it puts
 * a small array size in the nybble where the PLC escaped it into a following byte -- and
 * whether a PLC accepts the shorter form is untested.  Keeping the read's answer first
 * means that question only ever arises when there is no answer to keep.
 */
static int write_pre_request(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    if(tag->encoded_type_info_size > 0) { return PLCTAG_STATUS_OK; }

    rc = pccc_encode_type_info(tag->encoded_type_info, (int)sizeof(tag->encoded_type_info), tag->file_type,
                               tag->elem_size, tag->elem_count);
    if(rc > 0) {
        tag->encoded_type_info_size = rc;

        pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_DETAIL, tag->tag_id,
               "No read has completed yet; built a %d byte type descriptor from the data file type.", rc);

        return PLCTAG_STATUS_OK;
    }

    pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_DETAIL, tag->tag_id,
           "No read has completed yet and no descriptor can be built for this file type, doing pre-read.");

    tag->pre_write_read = 1;
    tag->write_in_progress = 0;

    return tag_read_start(tag);
}


static int write_request_overhead(ab_tag_p tag) {
    return (int)sizeof(eip_cip_uc_req) + (int)sizeof(embedded_pccc) + tag->encoded_name_size + tag->encoded_type_info_size
           + (int)((tag->conn->conn_path_size > 0) ? (2 + tag->conn->conn_path_size) : 0);
}


static void write_write_body(ab_tag_p tag, uint8_t **data) {
    mem_copy(*data, tag->encoded_type_info, tag->encoded_type_info_size);
    *data += tag->encoded_type_info_size;
    mem_copy(*data, tag->data, tag->size);
    *data += tag->size;
}


static const lgx_pccc_variant_t lgx_pccc_read_variant = {
    .name = "Logix-mapped PCCC read",
    .pccc_function = AB_EIP_PCCCLGX_TYPED_READ_FUNC,
    .is_write = false,
    .pre_request = NULL,
    .request_overhead = read_request_overhead,
    .write_body = read_write_body,
};


static const lgx_pccc_variant_t lgx_pccc_write_variant = {
    .name = "Logix-mapped PCCC write",
    .pccc_function = AB_EIP_PCCCLGX_TYPED_WRITE_FUNC,
    .is_write = true,
    .pre_request = write_pre_request,
    .request_overhead = write_request_overhead,
    .write_body = write_write_body,
};


/*
 * Build and send one Logix-mapped PCCC request.  The frame is an Unconnected
 * Send carrying an embedded Execute PCCC service, with the routing path, if
 * any, appended after the embedded command.
 */
static int lgx_pccc_request_start(ab_tag_p tag, const lgx_pccc_variant_t *variant) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->conn));
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    int session_payload_space = session_get_available_cip_payload_space(tag->conn);

    /* remember the TNS so pccc_check_response_header() can match the reply to this request. */
    tag->req_pccc_seq_num = conn_seq_id;

    pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_INFO, tag->tag_id, "Starting %s.", variant->name);

    do {
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        set_in_progress(tag, variant, true);

        if(variant->pre_request) {
            rc = variant->pre_request(tag);
            if(rc != PLCTAG_STATUS_OK) { break; }
        }

        int request_overhead = variant->request_overhead(tag);
        int request_payload_space = session_payload_space - request_overhead;

        if(request_payload_space < 0 || request_payload_space < tag->size) {
            pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_WARN, tag->tag_id,
                   "Request overhead (%d bytes) exceeds session payload space (%d bytes) or tag size too large.",
                   request_overhead, session_payload_space);
            set_in_progress(tag, variant, false);
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        rc = session_create_request(tag->conn, tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            set_in_progress(tag, variant, false);
            pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_WARN, tag->tag_id, "Unable to get new request. rc=%d", rc);
            break;
        }

        eip_cip_uc_req *lgx_pccc = (eip_cip_uc_req *)(req->data);
        embedded_pccc *embed_pccc = (embedded_pccc *)(lgx_pccc + 1);
        embed_start = (uint8_t *)(embed_pccc);

        embed_pccc->service_code = CIP_CMD_PCCC_EXECUTE;
        embed_pccc->req_path_size = 2;
        embed_pccc->req_path[0] = 0x20;
        embed_pccc->req_path[1] = 0x67;
        embed_pccc->req_path[2] = 0x24;
        embed_pccc->req_path[3] = 0x01;
        embed_pccc->request_id_size = 7;
        embed_pccc->vendor_id = h2le16(CIP_VENDOR_ID);
        embed_pccc->vendor_serial_number = h2le32(CIP_VENDOR_SN);
        embed_pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        embed_pccc->pccc_status = 0;
        embed_pccc->pccc_seq_num = h2le16(conn_seq_id);
        embed_pccc->pccc_function = variant->pccc_function;
        embed_pccc->pccc_offset = h2le16(0);
        embed_pccc->pccc_transfer_size = h2le16((uint16_t)tag->elem_count);

        data = (uint8_t *)(embed_pccc + 1);
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        variant->write_body(tag, &data);

        if((data - embed_start) & 0x01) {
            *data = 0;
            data++;
        }

        lgx_pccc->encap_command = h2le16(EIP_UNCONNECTED_SEND);
        lgx_pccc->router_timeout = h2le16(1);
        lgx_pccc->cpf_item_count = h2le16(2);
        lgx_pccc->cpf_nai_item_type = h2le16(EIP_ITEM_NAI);
        lgx_pccc->cpf_nai_item_length = h2le16(0);
        lgx_pccc->cpf_udi_item_type = h2le16(EIP_ITEM_UDI);
        lgx_pccc->cm_service_code = CIP_CMD_UNCONNECTED_SEND;
        lgx_pccc->cm_req_path_size = 0x02;
        lgx_pccc->cm_req_path[0] = 0x20;
        lgx_pccc->cm_req_path[1] = 0x06;
        lgx_pccc->cm_req_path[2] = 0x24;
        lgx_pccc->cm_req_path[3] = 0x01;
        lgx_pccc->secs_per_tick = CIP_SECS_PER_TICK;
        lgx_pccc->timeout_ticks = CIP_TIMEOUT_TICKS;
        lgx_pccc->uc_cmd_length = h2le16((uint16_t)(data - embed_start));
        if(tag->conn->conn_path_size > 0) {
            *data = (tag->conn->conn_path_size) / 2;
            data++;
            *data = 0;
            data++;
            mem_copy(data, tag->conn->conn_path, tag->conn->conn_path_size);
            data += tag->conn->conn_path_size;
        }
        lgx_pccc->cpf_udi_item_length = h2le16((uint16_t)(data - (uint8_t *)(&lgx_pccc->cm_service_code)));
        req->request_size = (int)(data - (req->data));
        req->allow_packing = tag->allow_packing;
        rc = session_add_request(tag->conn, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
            req = rc_dec(req);
            ab_tag_abort_request(tag);
            break;
        }
        tag->req = req;
        rc = PLCTAG_STATUS_PENDING;
    } while(0);

    if(rc != PLCTAG_STATUS_PENDING) { set_in_progress(tag, variant, false); }

    pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_INFO, tag->tag_id, "Done.");
    return rc;
}


/*
 * tag_read_start
 *
 * Start a PCCC tag read (PLC5, SLC).
 */
int tag_read_start(ab_tag_p tag) { return lgx_pccc_request_start(tag, &lgx_pccc_read_variant); }


/*
 * check_read_status
 *
 * NOTE that we can have only one outstanding request because PCCC
 * does not support fragments.
 */


static int check_read_status(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_SPEW, tag->tag_id, "Starting");

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

        rc = pccc_check_response_header(tag, false);
        if(rc != PLCTAG_STATUS_OK) { break; }

        pccc = (pccc_resp *)(tag->req->data);

        /* point to the start of the data */
        data = (uint8_t *)pccc + sizeof(*pccc);

        data_end = tag->req->data + tag->req->request_size;

        if(le2h16(pccc->encap_command) != EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_WARN, tag->tag_id, "Unexpected EIP packet type received: %" PRIu16 "!",
                   le2h16(pccc->encap_command));
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(pccc->encap_status) != EIP_OK) {
            pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_WARN, tag->tag_id, "EIP command failed, response code: %d",
                   le2h32(pccc->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->general_status != EIP_OK) {
            pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_WARN, tag->tag_id, "PCCC command failed, response code: (%d) %s",
                   pccc->general_status,
                   decode_cip_error_long((uint8_t *)&(pccc->general_status),
                                         cip_error_data_size((uint8_t *)&(pccc->general_status), data_end)));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->pccc_status != EIP_OK) {
            pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_WARN, tag->tag_id, "PCCC command failed, response code: %d - %s",
                   pccc->pccc_status, pccc_decode_error(&pccc->pccc_status, cip_error_data_size(&pccc->pccc_status, data_end)));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        type_start = data;

        if(!(data = pccc_decode_dt_byte(data, (int)(data_end - data), &pccc_res_type, &pccc_res_length))) {
            pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_WARN, tag->tag_id,
                   "Unable to decode PCCC response data type and data size!");
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
                pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_WARN, tag->tag_id,
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

        /*
         * skip this if this is a pre-read.
         * Otherwise this will overwrite the values
         * the user has set, possibly.
         */
        if(!tag->pre_write_read) { mem_copy(tag->data, data, (int)(data_end - data)); }

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

        // /* have the IO thread take care of the request buffers */
        // ab_tag_abort_request(tag);

        rc = PLCTAG_STATUS_OK;
    } while(0);

    ab_tag_abort_request(tag);

    /* if this is a pre-read for a write, then pass off the the write routine */
    if(rc == PLCTAG_STATUS_OK && tag->pre_write_read) {
        pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_DETAIL, tag->tag_id, "Restarting write call now.");

        tag->pre_write_read = 0;
        rc = tag_write_start(tag);
    }

    pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}


/*
 * tag_write_start
 *
 * Start a PCCC tag write (PLC5, SLC).  A tag that has not been read yet has no
 * encoded type information to send, so the first write turns into a read.
 */
int tag_write_start(ab_tag_p tag) { return lgx_pccc_request_start(tag, &lgx_pccc_write_variant); }


/*
 * check_write_status
 *
 * Fragments are not supported.
 */
static int check_write_status(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_SPEW, tag->tag_id, "Starting");

    do {
        pccc_resp *pccc = NULL;

        rc = pccc_check_response_header(tag, false);
        if(rc != PLCTAG_STATUS_OK) { break; }

        pccc = (pccc_resp *)(tag->req->data);

        uint8_t *data_end = tag->req->data + tag->req->request_size;

        if(pccc->general_status != EIP_OK) {
            pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_WARN, tag->tag_id, "PCCC command failed, response code: %d",
                   pccc->general_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->pccc_status != EIP_OK) {
            pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_WARN, tag->tag_id, "PCCC command failed, response code: %d - %s",
                   pccc->pccc_status, pccc_decode_error(&pccc->pccc_status, cip_error_data_size(&pccc->pccc_status, data_end)));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    ab_tag_abort_request(tag);

    pdebug(DEBUG_MODULE_AB_EIP_LGX_PCCC, DEBUG_SPEW, tag->tag_id, "Done.");

    return rc;
}
