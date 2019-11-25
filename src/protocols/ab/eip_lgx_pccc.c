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


#include <lib/libplctag.h>
#include <ab/ab_common.h>
#include <ab/pccc.h>
#include <ab/eip_lgx_pccc.h>
#include <ab/tag.h>
#include <ab/session.h>
#include <ab/defs.h>
#include <ab/error_codes.h>
#include <util/debug.h>


START_PACK typedef struct {
    /* PCCC Command Req Routing */
    uint8_t service_code;           /* ALWAYS 0x4B, Execute PCCC */
    uint8_t req_path_size;          /* ALWAYS 0x02, in 16-bit words */
    uint8_t req_path[4];            /* ALWAYS 0x20,0x67,0x24,0x01 for PCCC */
    uint8_t request_id_size;        /* ALWAYS 7 */
    uint16_le vendor_id;             /* Our CIP Vendor ID */
    uint32_le vendor_serial_number;  /* Our CIP Vendor Serial Number */
    /* PCCC Command */
    uint8_t pccc_command;           /* CMD read, write etc. */
    uint8_t pccc_status;            /* STS 0x00 in request */
    uint16_le pccc_seq_num;          /* TNS transaction/sequence id */
    uint8_t pccc_function;          /* FNC sub-function of command */
    uint16_le pccc_offset;           /* offset of requested in total request */
    uint16_le pccc_transfer_size;    /* total number of words requested */
} END_PACK embedded_pccc;


static int tag_read_start(ab_tag_p tag);
static int tag_status(ab_tag_p tag);
static int tag_tickler(ab_tag_p tag);
static int tag_write_start(ab_tag_p tag);

struct tag_vtable_t lgx_pccc_vtable = {
    (tag_vtable_func)ab_tag_abort, /* shared */
    (tag_vtable_func)tag_read_start,
    (tag_vtable_func)tag_status,
    (tag_vtable_func)tag_tickler,
    (tag_vtable_func)tag_write_start
};

static int check_read_status(ab_tag_p tag);
static int check_write_status(ab_tag_p tag);

/*
 * tag_status
 *
 * CIP/PCCC-specific status.
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

    return tag->status;
}




int tag_tickler(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    if(tag->read_in_progress) {
        pdebug(DEBUG_SPEW, "Read in progress.");
        rc = check_read_status(tag);
        tag->status = rc;
        return rc;
    }

    if(tag->write_in_progress) {
        pdebug(DEBUG_SPEW, "Write in progress.");
        rc = check_write_status(tag);
        tag->status = rc;
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

int tag_read_start(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));;
    int overhead;
    int data_per_packet;
    eip_cip_uc_req *lgx_pccc;
    embedded_pccc *embed_pccc;
    uint8_t *data;
    uint8_t *embed_start;

    pdebug(DEBUG_INFO,"Starting");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_WARN, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    tag->read_in_progress = 1;

    /* calculate based on the response. */
    overhead =   1      /* reply code */
                 +1      /* reserved */
                 +1      /* general status */
                 +1      /* status size */
                 +1      /* request ID size */
                 +2      /* vendor ID */
                 +4      /* vendor serial number */
                 +1      /* PCCC command */
                 +1      /* PCCC status */
                 +2      /* PCCC sequence number */
                 +1      /* type byte */
                 +2      /* maximum extended type. */
                 +2      /* maximum extended size. */
                 +1      /* secondary type byte if type was array. */
                 +2      /* maximum extended type. */
                 +2;     /* maximum extended size. */

    /* FIXME - this is not correct for this kind of transaction */

    data_per_packet = session_get_max_payload(tag->session) - overhead;

    if(data_per_packet <= 0) {
        tag->read_in_progress = 0;
        pdebug(DEBUG_WARN,"Unable to send request.  Packet overhead, %d bytes, is too large for packet, %d bytes!", overhead, session_get_max_payload(tag->session));
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(data_per_packet < tag->size) {
        tag->read_in_progress = 0;
        pdebug(DEBUG_DETAIL,"Tag size is %d, write overhead is %d, and write data per packet is %d.", tag->size, overhead, data_per_packet);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);

    if(rc != PLCTAG_STATUS_OK) {
        tag->read_in_progress = 0;
        pdebug(DEBUG_WARN,"Unable to get new request.  rc=%d",rc);
        return rc;
    }

    /* point the struct pointers to the buffer*/
    lgx_pccc = (eip_cip_uc_req *)(req->data);
    embed_pccc = (embedded_pccc *)(lgx_pccc + 1);

    /* set up the embedded PCCC packet */
    embed_start = (uint8_t *)(embed_pccc);

    /* Command Routing */
    embed_pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE;  /* ALWAYS 0x4B, Execute PCCC */
    embed_pccc->req_path_size = 2;   /* ALWAYS 2, size in words of path, next field */
    embed_pccc->req_path[0] = 0x20;  /* class */
    embed_pccc->req_path[1] = 0x67;  /* PCCC Execute */
    embed_pccc->req_path[2] = 0x24;  /* instance */
    embed_pccc->req_path[3] = 0x01;  /* instance 1 */

    /* Vendor info */
    embed_pccc->request_id_size = 7;  /* ALWAYS 7 */
    embed_pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);             /* Our CIP Vendor */
    embed_pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN);      /* our unique serial number */

    /* fill in the PCCC command */
    embed_pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
    embed_pccc->pccc_status = 0;  /* STS 0 in request */
    embed_pccc->pccc_seq_num = h2le16(conn_seq_id); /* FIXME - get sequence ID from session? */
    embed_pccc->pccc_function = AB_EIP_PCCCLGX_TYPED_READ_FUNC;
    embed_pccc->pccc_offset = h2le16((uint16_t)0);
    embed_pccc->pccc_transfer_size = h2le16((uint16_t)tag->elem_count); /* This is the offset items */

    /* point to the end of the struct */
    data = (uint8_t *)(embed_pccc + 1);

    /* copy encoded tag name into the request */
    mem_copy(data,tag->encoded_name,tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* FIXME - This is the total items */
    *((uint16_le *)data) = h2le16((uint16_t)tag->elem_count); /* elements */
    data += sizeof(uint16_le);

    /* if this is not an multiple of 16-bit chunks, pad it out */
    if((data - embed_start) & 0x01) {
        *data = 0;
        data++;
    }

    /*
     * after the embedded packet, we need to tell the message router
     * how to get to the target device.
     */

    /* encap fields */
    lgx_pccc->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND);    /* set up for unconnected sending */

    /* router timeout */
    lgx_pccc->router_timeout = h2le16(1);                 /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    lgx_pccc->cpf_item_count        = h2le16(2);                /* ALWAYS 2 */
    lgx_pccc->cpf_nai_item_type     = h2le16(AB_EIP_ITEM_NAI);  /* ALWAYS 0 */
    lgx_pccc->cpf_nai_item_length   = h2le16(0);                /* ALWAYS 0 */
    lgx_pccc->cpf_udi_item_type     = h2le16(AB_EIP_ITEM_UDI);  /* ALWAYS 0x00B2 - Unconnected Data Item */

    lgx_pccc->cm_service_code       = AB_EIP_CMD_UNCONNECTED_SEND;     /* unconnected send */
    lgx_pccc->cm_req_path_size      = 0x02;     /* path size in words */
    lgx_pccc->cm_req_path[0]        = 0x20;     /* class */
    lgx_pccc->cm_req_path[1]        = 0x06;     /* Connection Manager */
    lgx_pccc->cm_req_path[2]        = 0x24;     /* instance */
    lgx_pccc->cm_req_path[3]        = 0x01;     /* instance 1 */

    /* Unconnected send needs timeout information */
    lgx_pccc->secs_per_tick = AB_EIP_SECS_PER_TICK; /* seconds per tick */
    lgx_pccc->timeout_ticks = AB_EIP_TIMEOUT_TICKS; /* timeout = src_secs_per_tick * src_timeout_ticks */

    /* how big is the embedded packet? */
    lgx_pccc->uc_cmd_length = h2le16((uint16_t)(data - embed_start));

    /* copy the path */
    if(tag->session->conn_path_size > 0) {
        *data = (tag->session->conn_path_size) / 2; /* in 16-bit words */
        data++;
        *data = 0; /* reserved/pad */
        data++;
        mem_copy(data, tag->session->conn_path, tag->session->conn_path_size);
        data += tag->session->conn_path_size;
    } else {
        pdebug(DEBUG_DETAIL, "connection path is of length %d!", tag->session->conn_path_size);
    }

    /* how big is the unconnected data item? */
    lgx_pccc->cpf_udi_item_length   = h2le16((uint16_t)(data - (uint8_t *)(&lgx_pccc->cm_service_code)));

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));

    /* mark it as ready to send */
    //req->send_request = 1;
    req->allow_packing = tag->allow_packing;

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
        tag->req = rc_dec(req);
        tag->read_in_progress = 0;

        return rc;
    }

    /* save the request for later */
    tag->req = req;
    req = NULL;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}





/*
 * check_read_status
 *
 * NOTE that we can have only one outstanding request because PCCC
 * does not support fragments.
 */


static int check_read_status(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;

    pdebug(DEBUG_SPEW,"Starting");

    /* check for request in flight. */
    if (!tag->req) {
        tag->read_in_progress = 0;
        tag->offset = 0;

        pdebug(DEBUG_WARN,"Read in progress, but no request in flight!");

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

            pdebug(DEBUG_WARN,"Session reported failure of request: %s.", plc_tag_decode_error(rc));

            tag->read_in_progress = 0;
            tag->offset = 0;

            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        if(rc_is_error(rc)) {
            /* the request is dead, from session side. */
            tag->req = rc_dec(tag->req);
        }

        return rc;
    }

    /* the request is ours exclusively. */

    req = tag->req;

    /* fake exceptions */
    do {
        pccc_resp *pccc;
        uint8_t *data;
        uint8_t *data_end;
        uint8_t *type_start;
        uint8_t *type_end;
        int pccc_res_type;
        int pccc_res_length;

        pccc = (pccc_resp *)(req->data);

        /* point to the start of the data */
        data = (uint8_t *)pccc + sizeof(*pccc);

        data_end = (req->data + le2h16(pccc->encap_length) + sizeof(eip_encap));

        if(le2h16(pccc->encap_command) != AB_EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_WARN,"Unexpected EIP packet type received: %d!",pccc->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(pccc->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"EIP command failed, response code: %d",le2h32(pccc->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"PCCC command failed, response code: (%d) %s", pccc->general_status, decode_cip_error_long((uint8_t *)&(pccc->general_status)));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d - %s", *data, pccc_decode_error(*data));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        type_start = data;

        if(!(data = pccc_decode_dt_byte(data, (int)(data_end - data), &pccc_res_type,&pccc_res_length))) {
            pdebug(DEBUG_WARN,"Unable to decode PCCC response data type and data size!");
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        /* this gives us the overall type of the response and the number of bytes remaining in it.
         * If the type is an array, then we need to decode another one of these words
         * to get the type of each element and the size of each element.  We will
         * need to adjust the size if we care.
         */

        if(pccc_res_type == AB_PCCC_DATA_ARRAY) {
            if(!(data = pccc_decode_dt_byte(data, (int)(data_end - data), &pccc_res_type,&pccc_res_length))) {
                pdebug(DEBUG_WARN,"Unable to decode PCCC response array element data type and data size!");
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
        if(!tag->pre_write_read) {
            mem_copy(tag->data, data, (int)(data_end - data));
        }

        /* copy type data into tag. */
        tag->encoded_type_info_size = (int)(type_end - type_start);
        mem_copy(tag->encoded_type_info, type_start, tag->encoded_type_info_size);

        /* done! */
        tag->first_read = 0;

        /* have the IO thread take care of the request buffers */
        ab_tag_abort(tag);

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request */
    if(tag->req) {
        tag->req = rc_dec(req);
    }

    tag->read_in_progress = 0;

    /* if this is a pre-read for a write, then pass off the the write routine */
    if (rc == PLCTAG_STATUS_OK && tag->pre_write_read) {
        pdebug(DEBUG_DETAIL, "Restarting write call now.");

        tag->pre_write_read = 0;
        rc = tag_write_start(tag);
    }

    pdebug(DEBUG_SPEW,"Done.");

    return rc;
}





/* FIXME  convert to unconnected messages. */

int tag_write_start(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_uc_req *lgx_pccc;
    embedded_pccc *embed_pccc;
    uint8_t *data;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));;
    ab_request_p req = NULL;
    uint8_t *embed_start;
    int overhead, data_per_packet;

    pdebug(DEBUG_INFO,"Starting.");

    if(tag->read_in_progress || tag->write_in_progress) {
        pdebug(DEBUG_WARN, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    tag->write_in_progress = 1;

    if (tag->first_read) {
        pdebug(DEBUG_DETAIL, "No read has completed yet, doing pre-read to get type information.");

        tag->pre_write_read = 1;
        tag->write_in_progress = 0;

        return tag_read_start(tag);
    }

    /* overhead comes from the request in this case */
    overhead =   1  /* CIP PCCC command */
                 +2  /* path size for PCCC command */
                 +4  /* path to PCCC command object */
                 +1  /* request ID size */
                 +2  /* vendor ID */
                 +4  /* vendor serial number */
                 +1  /* PCCC command */
                 +1  /* PCCC status */
                 +2  /* PCCC sequence number */
                 +1  /* PCCC function */
                 +2  /* request offset */
                 +2  /* request total transfer size in elements. */
                 + (tag->encoded_name_size)
                 +2; /* actual request size in elements */

    data_per_packet = session_get_max_payload(tag->session) - overhead;

    if(data_per_packet <= 0) {
        tag->write_in_progress = 0;
        pdebug(DEBUG_WARN,"Unable to send request.  Packet overhead, %d bytes, is too large for packet, %d bytes!", overhead, session_get_max_payload(tag->session));
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(data_per_packet < tag->size) {
        tag->write_in_progress = 0;
        pdebug(DEBUG_DETAIL,"Tag size is %d, write overhead is %d, and write data per packet is %d.", session_get_max_payload(tag->session), overhead, data_per_packet);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* get a request buffer */
    rc = session_create_request(tag->session, tag->tag_id, &req);

    if(rc != PLCTAG_STATUS_OK) {
        tag->write_in_progress = 0;
        pdebug(DEBUG_WARN,"Unable to get new request.  rc=%d",rc);
        return rc;
    }

    /* point the struct pointers to the buffer*/
    lgx_pccc = (eip_cip_uc_req *)(req->data);
    embed_pccc = (embedded_pccc *)(lgx_pccc + 1);

    /* set up the embedded PCCC packet */
    embed_start = (uint8_t *)(embed_pccc);

    /* Command Routing */
    embed_pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE;  /* ALWAYS 0x4B, Execute PCCC */
    embed_pccc->req_path_size = 2;   /* ALWAYS 2, size in words of path, next field */
    embed_pccc->req_path[0] = 0x20;  /* class */
    embed_pccc->req_path[1] = 0x67;  /* PCCC Execute */
    embed_pccc->req_path[2] = 0x24;  /* instance */
    embed_pccc->req_path[3] = 0x01;  /* instance 1 */

    /* Vendor info */
    embed_pccc->request_id_size = 7;  /* ALWAYS 7 */
    embed_pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);             /* Our CIP Vendor */
    embed_pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN);      /* our unique serial number */

    /* fill in the PCCC command */
    embed_pccc->pccc_command = AB_EIP_PCCC_TYPED_CMD;
    embed_pccc->pccc_status = 0;  /* STS 0 in request */
    embed_pccc->pccc_seq_num = h2le16(conn_seq_id); /* FIXME - get sequence ID from session? */
    embed_pccc->pccc_function = AB_EIP_PCCCLGX_TYPED_WRITE_FUNC;
    embed_pccc->pccc_offset = h2le16(0);
    embed_pccc->pccc_transfer_size = h2le16((uint16_t)tag->elem_count); /* This is the offset items */

    /* point to the end of the struct */
    data = (uint8_t *)(embed_pccc + 1);

    /* copy encoded name  into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* copy the type info from the read. */
    mem_copy(data, tag->encoded_type_info, tag->encoded_type_info_size);
    data += tag->encoded_type_info_size;

    /* now copy the data to write */
    mem_copy(data,tag->data,tag->size);
    data += tag->size;

    /* if this is not an multiple of 16-bit chunks, pad it out */
    if((data - embed_start) & 0x01) {
        *data = 0;
        data++;
    }

    /*
     * after the embedded packet, we need to tell the message router
     * how to get to the target device.
     */

    /* encap fields */
    lgx_pccc->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND);    /* set up for unconnected sending */

    /* router timeout */
    lgx_pccc->router_timeout = h2le16(1);                 /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    lgx_pccc->cpf_item_count        = h2le16(2);                /* ALWAYS 2 */
    lgx_pccc->cpf_nai_item_type     = h2le16(AB_EIP_ITEM_NAI);  /* ALWAYS 0 */
    lgx_pccc->cpf_nai_item_length   = h2le16(0);                /* ALWAYS 0 */
    lgx_pccc->cpf_udi_item_type     = h2le16(AB_EIP_ITEM_UDI);  /* ALWAYS 0x00B2 - Unconnected Data Item */

    lgx_pccc->cm_service_code       = AB_EIP_CMD_UNCONNECTED_SEND;     /* unconnected send */
    lgx_pccc->cm_req_path_size      = 0x02;     /* path size in words */
    lgx_pccc->cm_req_path[0]        = 0x20;     /* class */
    lgx_pccc->cm_req_path[1]        = 0x06;     /* Connection Manager */
    lgx_pccc->cm_req_path[2]        = 0x24;     /* instance */
    lgx_pccc->cm_req_path[3]        = 0x01;     /* instance 1 */

    /* Unconnected send needs timeout information */
    lgx_pccc->secs_per_tick = AB_EIP_SECS_PER_TICK; /* seconds per tick */
    lgx_pccc->timeout_ticks = AB_EIP_TIMEOUT_TICKS; /* timeout = src_secs_per_tick * src_timeout_ticks */

    /* how big is the embedded packet? */
    lgx_pccc->uc_cmd_length = h2le16((uint16_t)(data - embed_start));

    /* copy the path */
    if(tag->session->conn_path_size > 0) {
        *data = (tag->session->conn_path_size) / 2; /* in 16-bit words */
        data++;
        *data = 0; /* reserved/pad */
        data++;
        mem_copy(data, tag->session->conn_path, tag->session->conn_path_size);
        data += tag->session->conn_path_size;
    }

    /* how big is the unconnected data item? */
    lgx_pccc->cpf_udi_item_length   = h2le16((uint16_t)(data - (uint8_t *)(&lgx_pccc->cm_service_code)));

    /* get ready to add the request to the queue for this session */
    req->request_size = (int)(data - (req->data));

    /* add the request to the session's list. */
    rc = session_add_request(tag->session, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
        tag->write_in_progress = 0;
        tag->req = rc_dec(req);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}



/*
 * check_write_status
 *
 * Fragments are not supported.
 */
static int check_write_status(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req;

    pdebug(DEBUG_SPEW,"Starting.");

    /* is there an outstanding request? */
    if (!tag->req) {
        tag->write_in_progress = 0;
        tag->offset = 0;

        pdebug(DEBUG_WARN,"Write in progress, but no request in flight!");

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

            pdebug(DEBUG_WARN,"Session reported failure of request: %s.", plc_tag_decode_error(rc));

            tag->write_in_progress = 0;
            tag->offset = 0;

            break;
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        if(rc_is_error(rc)) {
            /* the request is dead, from session side. */
            tag->req = rc_dec(tag->req);
        }

        return rc;
    }

    /* the request is ours exclusively. */

    req = tag->req;

    /* fake exception */
    do {
        pccc_resp *pccc;
        uint8_t *data;

        pccc = (pccc_resp *)(req->data);

        /* point to the start of the data */
        data = (uint8_t *)pccc + sizeof(*pccc);

        /* check the response status */
        if( le2h16(pccc->encap_command) != AB_EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_WARN,"EIP unexpected response packet type: %d!",pccc->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(pccc->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"EIP command failed, response code: %d",le2h32(pccc->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN,"PCCC command failed, response code: %d",pccc->general_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d - %s",pccc->pccc_status, pccc_decode_error(*data));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* clean up the request */
    tag->req = rc_dec(req);

    tag->write_in_progress = 0;

    pdebug(DEBUG_SPEW,"Done.");

    /* Success! */
    return rc;
}
