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
 * 2012-02-23  KRH - Created file.                                        *
 *                                                                        *
 * 2012-06-15  KRH - Rename file and includes for code re-org to get      *
 *                   ready for DF1 implementation.  Refactor some common  *
 *                   parts into ab/util.c.                                *
 *                                                                        *
 * 2012-06-20  KRH - change plc_err() calls for new error API.            *
 *                                                                        *
 * 2012-12-19   KRH - Start refactoring for threaded API and attributes.   *
 *                                                                        *
 **************************************************************************/

#include <ctype.h>
#include <platform.h>
#include <lib/libplctag.h>
#include <lib/libplctag_tag.h>
#include <ab/eip.h>
#include <ab/ab_common.h>
#include <ab/cip.h>
#include <ab/tag.h>
#include <ab/session.h>
#include <ab/eip_cip.h>
#include <util/attr.h>
#include <util/debug.h>


int allocate_request_slot(ab_tag_p tag);
int allocate_read_request_slot(ab_tag_p tag);
int allocate_write_request_slot(ab_tag_p tag);
int build_read_request(ab_tag_p tag, int slot, int byte_offset);
int build_write_request(ab_tag_p tag, int slot, int byte_offset);
static int check_read_status(ab_tag_p tag);
static int check_write_status(ab_tag_p tag);
int calculate_write_sizes(ab_tag_p tag);

/*************************************************************************
 **************************** API Functions ******************************
 ************************************************************************/

/*
 * eip_cip_tag_status
 *
 * CIP-specific status.  This functions as a "tickler" routine
 * to check on the completion of async requests.
 */
int eip_cip_tag_status(ab_tag_p tag)
{
    if (tag->read_in_progress) {
        int rc = check_read_status(tag);

        tag->status = rc;

        return rc;
    }

    if (tag->write_in_progress) {
        int rc = check_write_status(tag);

        tag->status = rc;

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
    if (tag->session) {
        if (!tag->session->is_connected) {
            tag->status = PLCTAG_STATUS_PENDING;
        } else {
            tag->status = PLCTAG_STATUS_OK;
        }
    }

    return tag->status;
}

/*
 * eip_cip_tag_read_start
 *
 * This function must be called only from within one thread, or while
 * the tag's mutex is locked.
 *
 * The function starts the process of getting tag data from the PLC.
 */

int eip_cip_tag_read_start(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    int i;
    int byte_offset = 0;

    pdebug(DEBUG_INFO, "Starting");

    /* is this the first read? */
    if (tag->first_read) {
        /*
         * On a new tag, the first time we read, we go through and
         * request the maximum possible (up to the size of the tag)
         * each time.  We record what we actually get back in the
         * tag->read_req_sizes array.  The next time we read, we
         * use that array to make the new requests.
         */

        rc = allocate_read_request_slot(tag);

        if (rc != PLCTAG_STATUS_OK) {
            tag->status = rc;
            return rc;
        }

        /*
         * The PLC may not send back as much data as we would like.
         * So, we attempt to determine what the size will be by
         * single-stepping through the requests the first time.
         * This will be slow, but subsequent reads will be pipelined.
         */

        /* determine the byte offset this time. */
        byte_offset = 0;

        /* scan and add the byte offsets */
        for (i = 0; i < tag->num_read_requests && tag->reqs[i]; i++) {
            byte_offset += tag->read_req_sizes[i];
        }

        pdebug(DEBUG_DETAIL, "First read tag->num_read_requests=%d, byte_offset=%d.", tag->num_read_requests, byte_offset);

        /* i is the index of the first new request */
        rc = build_read_request(tag, i, byte_offset);

        if (rc != PLCTAG_STATUS_OK) {
            tag->status = rc;
            return rc;
        }

    } else {
        /* this is not the first read, so just set up all the requests at once. */
        byte_offset = 0;

        for (i = 0; i < tag->num_read_requests; i++) {
            rc = build_read_request(tag, i, byte_offset);

            if (rc != PLCTAG_STATUS_OK) {
                tag->status = rc;
                return rc;
            }

            byte_offset += tag->read_req_sizes[i];
        }
    }

    /* mark the tag read in progress */
    tag->read_in_progress = 1;

    /* the read is now pending */
    tag->status = PLCTAG_STATUS_PENDING;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}

/*
 * eip_cip_tag_write_start
 *
 * This must be called from one thread alone, or while the tag mutex is
 * locked.
 *
 * The routine starts the process of writing to a tag.
 */

int eip_cip_tag_write_start(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    int i;
    int byte_offset = 0;

    pdebug(DEBUG_INFO, "Starting");

    /*
     * if the tag has not been read yet, read it.
     *
     * This gets the type data and sets up the request
     * buffers.
     */

    if (tag->first_read) {
        pdebug(DEBUG_DETAIL, "No read has completed yet, doing pre-read to get type information.");

        tag->pre_write_read = 1;

        return eip_cip_tag_read_start(tag);
    }

    /*
     * calculate the number and size of the write requests
     * if we have not already done so.
     */
    if (!tag->num_write_requests) {
        rc = calculate_write_sizes(tag);
    }

    if (rc != PLCTAG_STATUS_OK) {
        tag->status = rc;
        return rc;
    }

    /* set up all the requests at once. */
    byte_offset = 0;

    for (i = 0; i < tag->num_write_requests; i++) {
        rc = build_write_request(tag, i, byte_offset);

        if (rc != PLCTAG_STATUS_OK) {
            tag->status = rc;
            return rc;
        }

        byte_offset += tag->write_req_sizes[i];
    }

    /* the write is now pending */
    tag->write_in_progress = 1;
    tag->status = PLCTAG_STATUS_PENDING;

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}

/*
 * allocate_request_slot
 *
 * Increase the number of available request slots.
 */
int allocate_request_slot(ab_tag_p tag)
{
    int* old_sizes;
    ab_request_p* old_reqs;
    int i;
    int old_max = tag->max_requests;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* bump up the number of allowed requests */
    tag->max_requests += DEFAULT_MAX_REQUESTS;

    pdebug(DEBUG_DETAIL, "setting max_requests = %d", tag->max_requests);

    /* (re)allocate the read size array */
    old_sizes = tag->read_req_sizes;
    tag->read_req_sizes = (int*)mem_alloc(tag->max_requests * sizeof(int));

    if (!tag->read_req_sizes) {
        mem_free(old_sizes);
        tag->status = PLCTAG_ERR_NO_MEM;
        return tag->status;
    }

    /* copy the size data */
    if (old_sizes) {
        for (i = 0; i < old_max; i++) {
            tag->read_req_sizes[i] = old_sizes[i];
        }

        mem_free(old_sizes);
    }

    /* (re)allocate the write size array */
    old_sizes = tag->write_req_sizes;
    tag->write_req_sizes = (int*)mem_alloc(tag->max_requests * sizeof(int));

    if (!tag->write_req_sizes) {
        mem_free(old_sizes);
        tag->status = PLCTAG_ERR_NO_MEM;
        return tag->status;
    }

    /* copy the size data */
    if (old_sizes) {
        for (i = 0; i < old_max; i++) {
            tag->write_req_sizes[i] = old_sizes[i];
        }

        mem_free(old_sizes);
    }

    /* do the same for the request array */
    old_reqs = tag->reqs;
    tag->reqs = (ab_request_p*)mem_alloc(tag->max_requests * sizeof(ab_request_p));

    if (!tag->reqs) {
        tag->status = PLCTAG_ERR_NO_MEM;
        return tag->status;
    }

    /* copy the request data, there shouldn't be anything here I think... */
    if (old_reqs) {
        for (i = 0; i < old_max; i++) {
            tag->reqs[i] = old_reqs[i];
        }

        mem_free(old_reqs);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}

int allocate_read_request_slot(ab_tag_p tag)
{
    /* increase the number of available request slots */
    tag->num_read_requests++;

    if (tag->num_read_requests > tag->max_requests) {
        return allocate_request_slot(tag);
    }

    return PLCTAG_STATUS_OK;
}

int allocate_write_request_slot(ab_tag_p tag)
{
    /* increase the number of available request slots */
    tag->num_write_requests++;

    if (tag->num_write_requests > tag->max_requests) {
        return allocate_request_slot(tag);
    }

    return PLCTAG_STATUS_OK;
}

int build_read_request(ab_tag_p tag, int slot, int byte_offset)
{
    eip_cip_uc_req* cip;
    uint8_t* data;
    uint8_t* embed_start, *embed_end;
    ab_request_p req = NULL;
    int rc;

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = request_create(&req);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        tag->status = rc;
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
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* add the count of elements to read. */
    *((uint16_t*)data) = h2le16(tag->elem_count);
    data += sizeof(uint16_t);

    /* add the byte offset for this request */
    *((uint32_t*)data) = h2le32(byte_offset);
    data += sizeof(uint32_t);

    /* mark the end of the embedded packet */
    embed_end = data;

    /* Now copy in the routing information for the embedded message */
    /*
     * routing information.  Format:
     *
     * uint8_t path_size in 16-bit words
     * uint8_t reserved/pad (zero)
     * uint8_t[...] path (padded to even number of bytes)
     */
    if(tag->conn_path_size > 0) {
        *data = (tag->conn_path_size) / 2; /* in 16-bit words */
        data++;
        *data = 0; /* reserved/pad */
        data++;
        mem_copy(data, tag->conn_path, tag->conn_path_size);
        data += tag->conn_path_size;
    }

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(AB_EIP_READ_RR_DATA); /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI); /* ALWAYS 0 */
    cip->cpf_nai_item_length = h2le16(0);             /* ALWAYS 0 */
    cip->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI); /* ALWAYS 0x00B2 - Unconnected Data Item */
    cip->cpf_udi_item_length = h2le16(data - (uint8_t*)(&cip->cm_service_code)); /* REQ: fill in with length of remaining data. */

    /* CM Service Request - Connection Manager */
    cip->cm_service_code = AB_EIP_CMD_UNCONNECTED_SEND; /* 0x52 Unconnected Send */
    cip->cm_req_path_size = 2;                          /* 2, size in 16-bit words of path, next field */
    cip->cm_req_path[0] = 0x20;                         /* class */
    cip->cm_req_path[1] = 0x06;                         /* Connection Manager */
    cip->cm_req_path[2] = 0x24;                         /* instance */
    cip->cm_req_path[3] = 0x01;                         /* instance 1 */

    /* Unconnected send needs timeout information */
    cip->secs_per_tick = AB_EIP_SECS_PER_TICK; /* seconds per tick */
    cip->timeout_ticks = AB_EIP_TIMEOUT_TICKS; /* timeout = src_secs_per_tick * src_timeout_ticks */

    /* size of embedded packet */
    cip->uc_cmd_length = h2le16(embed_end - embed_start);

    /* set the size of the request */
    req->request_size = data - (req->data);

    /* mark it as ready to send */
    req->send_request = 1;

    /* add the request to the session's list. */
    rc = request_add(tag->session, req);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to add request to session! rc=%d", rc);
        request_destroy(&req);
        tag->status = rc;
        return rc;
    }

    /* save the request for later */
    tag->reqs[slot] = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}

int build_write_request(ab_tag_p tag, int slot, int byte_offset)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_uc_req* cip;
    uint8_t* data;
    uint8_t* embed_start, *embed_end;
    ab_request_p req = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    /* get a request buffer */
    rc = request_create(&req);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to get new request.  rc=%d", rc);
        tag->status = rc;
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

    /*
     * set up the CIP Read request type.
     * Different if more than one request.
     *
     * This handles a bug where attempting fragmented requests
     * does not appear to work with a single boolean.
     */
    *data = (tag->num_write_requests > 1) ? AB_EIP_CMD_CIP_WRITE_FRAG : AB_EIP_CMD_CIP_WRITE;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* copy encoded type info */
    if (tag->encoded_type_info_size) {
        mem_copy(data, tag->encoded_type_info, tag->encoded_type_info_size);
        data += tag->encoded_type_info_size;
    } else {
        tag->status = PLCTAG_ERR_UNSUPPORTED;
        return tag->status;
    }

    /* copy the item count, little endian */
    *((uint16_t*)data) = h2le16(tag->elem_count);
    data += 2;

    if (tag->num_write_requests > 1) {
        /* put in the byte offset */
        *((uint32_t*)data) = h2le32(byte_offset);
        data += 4;
    }

    /* now copy the data to write */
    mem_copy(data, tag->data + byte_offset, tag->write_req_sizes[slot]);
    data += tag->write_req_sizes[slot];

    /* need to pad data to multiple of 16-bits */
    if (tag->write_req_sizes[slot] & 0x01) {
        *data = 0;
        data++;
    }

    /* mark the end of the embedded packet */
    embed_end = data;

    /*
     * after the embedded packet, we need to tell the message router
     * how to get to the target device.
     */

    /* Now copy in the routing information for the embedded message */
    *data = (tag->conn_path_size) / 2; /* in 16-bit words */
    data++;
    *data = 0;
    data++;
    mem_copy(data, tag->conn_path, tag->conn_path_size);
    data += tag->conn_path_size;

    /* now fill in the rest of the structure. */

    /* encap fields */
    cip->encap_command = h2le16(AB_EIP_READ_RR_DATA); /* ALWAYS 0x006F Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI); /* ALWAYS 0 */
    cip->cpf_nai_item_length = h2le16(0);             /* ALWAYS 0 */
    cip->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI); /* ALWAYS 0x00B2 - Unconnected Data Item */
    cip->cpf_udi_item_length = h2le16(data - (uint8_t*)(&(cip->cm_service_code))); /* REQ: fill in with length of remaining data. */

    /* CM Service Request - Connection Manager */
    cip->cm_service_code = AB_EIP_CMD_UNCONNECTED_SEND; /* 0x52 Unconnected Send */
    cip->cm_req_path_size = 2;                          /* 2, size in 16-bit words of path, next field */
    cip->cm_req_path[0] = 0x20;                         /* class */
    cip->cm_req_path[1] = 0x06;                         /* Connection Manager */
    cip->cm_req_path[2] = 0x24;                         /* instance */
    cip->cm_req_path[3] = 0x01;                         /* instance 1 */

    /* Unconnected send needs timeout information */
    cip->secs_per_tick = AB_EIP_SECS_PER_TICK; /* seconds per tick */
    cip->timeout_ticks = AB_EIP_TIMEOUT_TICKS; /* timeout = srd_secs_per_tick * src_timeout_ticks */

    /* size of embedded packet */
    cip->uc_cmd_length = h2le16(embed_end - embed_start);

    /* set the size of the request */
    req->request_size = data - (req->data);

    /* mark it as ready to send */
    req->send_request = 1;

    /* add the request to the session's list. */
    rc = request_add(tag->session, req);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to lock add request to session! rc=%d", rc);
        plc_tag_abort((plc_tag)tag);
        request_destroy(&req);
        tag->status = rc;
        return rc;
    }

    /* save the request for later */
    tag->reqs[slot] = req;

    pdebug(DEBUG_INFO, "Done");

    return PLCTAG_STATUS_OK;
}

/*
 * check_read_status
 *
 * This routine checks for any outstanding requests and copies in data
 * that has arrived.  At the end of the request, it will clean up the request
 * buffers.  This is not thread-safe!  It should be called with the tag mutex
 * locked!
 */

static int check_read_status(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    eip_cip_uc_resp* cip_resp;
    uint8_t* data;
    uint8_t* data_end;
    int i;
    ab_request_p req;
    int byte_offset = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* is there an outstanding request? */
    if (!tag->reqs) {
        tag->read_in_progress = 0;
        tag->status = PLCTAG_ERR_NULL_PTR;
        return PLCTAG_ERR_NULL_PTR;
    }

    for (i = 0; i < tag->num_read_requests; i++) {
        if (tag->reqs[i] && !tag->reqs[i]->resp_received) {
            tag->status = PLCTAG_STATUS_PENDING;
            return PLCTAG_STATUS_PENDING;
        }
    }

    /*
     * process each request.  If there is more than one request, then
     * we need to make sure that we copy the data into the right part
     * of the tag's data buffer.
     */
    for (i = 0; i < tag->num_read_requests; i++) {
        req = tag->reqs[i];

        if (!req) {
            rc = PLCTAG_ERR_NULL_PTR;
            break;
        }

        /* skip if already processed */
        if (req->processed) {
            byte_offset += tag->read_req_sizes[i];
            continue;
        }

        req->processed = 1;

        pdebug(DEBUG_DETAIL, "processing request %d", i);

        /* point to the data */
        cip_resp = (eip_cip_uc_resp*)(req->data);

        /* point to the start of the data */
        data = (req->data) + sizeof(eip_cip_uc_resp);

        /* point the end of the data */
        data_end = (req->data + cip_resp->encap_length + sizeof(eip_encap_t));

        /* check the status */
        if (le2h16(cip_resp->encap_command) != AB_EIP_READ_RR_DATA) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", cip_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (le2h16(cip_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", cip_resp->encap_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /*
         * FIXME
         *
         * It probably should not be necessary to check for both as setting the type to anything other
         * than fragmented is error-prone.
         */

        if (cip_resp->reply_service != (AB_EIP_CMD_CIP_READ_FRAG | AB_EIP_CMD_CIP_OK)
            && cip_resp->reply_service != (AB_EIP_CMD_CIP_READ | AB_EIP_CMD_CIP_OK) ) {
            pdebug(DEBUG_WARN, "CIP response reply service unexpected: %d", cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (cip_resp->status != AB_CIP_STATUS_OK && cip_resp->status != AB_CIP_STATUS_FRAG) {
            pdebug(DEBUG_WARN, "CIP read failed with status: %d", cip_resp->status);
            pdebug(DEBUG_WARN, cip_decode_status(cip_resp->status));

            switch (cip_resp->status) {
                case 0x04: /* FIXME - should be defined constants */
                case 0x05:
                case 0x13:
                case 0x1C:
                    rc = PLCTAG_ERR_BAD_PARAM;
                    break;

                default:
                    rc = PLCTAG_ERR_REMOTE_ERR;
                    break;
            }

            break;
        }

        /* the first byte of the response is a type byte. */
        pdebug(DEBUG_DETAIL, "type byte = %d (%x)", (int)*data, (int)*data);

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
        if ((*data) >= AB_CIP_DATA_BIT && (*data) <= AB_CIP_DATA_STRINGI) {
            /* copy the type info for later. */
            if (tag->encoded_type_info_size == 0) {
                tag->encoded_type_info_size = 2;
                mem_copy(tag->encoded_type_info, data, tag->encoded_type_info_size);
            }

            /* skip the type byte and zero length byte */
            data += 2;
        } else if ((*data) == AB_CIP_DATA_ABREV_STRUCT || (*data) == AB_CIP_DATA_ABREV_ARRAY ||
                   (*data) == AB_CIP_DATA_FULL_STRUCT || (*data) == AB_CIP_DATA_FULL_ARRAY) {
            /* this is an aggregate type of some sort, the type info is variable length */
            int type_length =
                *(data + 1) + 2; /*
                                                                       * MAGIC
                                                                       * add 2 to get the total length including
                                                                       * the type byte and the length byte.
                                                                       */

            /* check for extra long types */
            if (type_length > MAX_TAG_TYPE_INFO) {
                pdebug(DEBUG_WARN, "Read data type info is too long (%d)!", type_length);
                rc = PLCTAG_ERR_TOO_LONG;
                break;
            }

            /* copy the type info for later. */
            if (tag->encoded_type_info_size == 0) {
                tag->encoded_type_info_size = type_length;
                mem_copy(tag->encoded_type_info, data, tag->encoded_type_info_size);
            }

            data += type_length;
        } else {
            pdebug(DEBUG_WARN, "Unsupported data type returned, type byte=%d", *data);
            rc = PLCTAG_ERR_UNSUPPORTED;
            break;
        }

        /* copy data into the tag. */
        if ((byte_offset + (data_end - data)) > tag->size) {
            pdebug(DEBUG_WARN,
                   "Read data is too long (%d bytes) to fit in tag data buffer (%d bytes)!",
                   byte_offset + (int)(data_end - data),
                   tag->size);
            rc = PLCTAG_ERR_TOO_LONG;
            break;
        }

        pdebug(DEBUG_DETAIL, "Got %d bytes of data", (data_end - data));

        /*
         * copy the data, but only if this is not
         * a pre-read for a subsequent write!  We do not
         * want to overwrite the data the upstream has
         * put into the tag's data buffer.
         */
        if (!tag->pre_write_read) {
            mem_copy(tag->data + byte_offset, data, (data_end - data));
        }

        /* save the size of the response for next time */
        tag->read_req_sizes[i] = (data_end - data);

        /*
         * did we get any data back? a zero-length response is
         * an error here.
         */

        if ((data_end - data) == 0) {
            rc = PLCTAG_ERR_NO_DATA;
            break;
        } else {
            /* bump the byte offset */
            byte_offset += (data_end - data);

            /* set the return code */
            rc = PLCTAG_STATUS_OK;
        }
    } /* end of for(i = 0; i < tag->num_requests; i++) */

    /* are we actually done? */
    if (rc == PLCTAG_STATUS_OK) {
        if (byte_offset < tag->size) {
            /* no, not yet */
            if (tag->first_read) {
                /* call read start again to get the next piece */
                pdebug(DEBUG_DETAIL, "calling ab_rag_read_cip_start_unsafe() to get the next chunk.");
                rc = eip_cip_tag_read_start(tag);
            } else {
                pdebug(DEBUG_WARN, "Insufficient data read for tag!");
                ab_tag_abort(tag);
                rc = PLCTAG_ERR_READ;
            }
        } else {
            /* done! */
            tag->first_read = 0;

            tag->read_in_progress = 0;

            /* have the IO thread take care of the request buffers */
            ab_tag_abort(tag);

            /* Now remove the requests from the session's request list. */

            /* FIXME - this functionality has been removed to simplify
             * this case.  All deallocation of requests is done by the
             * IO thread now.  The abort call above handles this.
             */
            // for(i = 0; i < tag->num_read_requests; i++) {
            // int tmp_rc;

            // req = tag->reqs[i];

            // tmp_rc = request_remove(tag->session, req);

            // if(tmp_rc != PLCTAG_STATUS_OK) {
            //  pdebug(DEBUG_WARN,"Unable to remove the request from the list! rc=%d",rc);
            //
            //  /* since we could not remove it, maybe the thread can. */
            //  req->abort_request = 1;
            //
            //  rc = tmp_rc;
            //} else {
            //  /* free up the request resources */
            //  request_destroy(&req);
            //}

            ///* mark it as freed */
            // tag->reqs[i] = NULL;
            //}

            /* if this is a pre-read for a write, then pass off the the write routine */
            if (tag->pre_write_read) {
                pdebug(DEBUG_DETAIL, "Restarting write call now.");

                tag->pre_write_read = 0;
                rc = eip_cip_tag_write_start(tag);
            }
        }
    } else {
        /* error ! */
        pdebug(DEBUG_WARN, "Error received!");
    }

    tag->status = rc;

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}

/*
 * check_write_status
 *
 * This routine must be called with the tag mutex locked.  It checks the current
 * status of a write operation.  If the write is done, it triggers the clean up.
 */

static int check_write_status(ab_tag_p tag)
{
    eip_cip_uc_resp* cip_resp;
    int rc = PLCTAG_STATUS_OK;
    int i;
    ab_request_p req;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* is there an outstanding request? */
    if (!tag->reqs) {
        tag->write_in_progress = 0;
        tag->status = PLCTAG_ERR_NULL_PTR;

        pdebug(DEBUG_INFO, "No outstanding request!");

        return PLCTAG_ERR_NULL_PTR;
    }

    for (i = 0; i < tag->num_write_requests; i++) {
        if (tag->reqs[i] && !tag->reqs[i]->resp_received) {
            tag->status = PLCTAG_STATUS_PENDING;

            pdebug(DEBUG_DETAIL, "Request still in progress.");

            return PLCTAG_STATUS_PENDING;
        }
    }

    /*
     * process each request.  If there is more than one request, then
     * we need to make sure that we copy the data into the right part
     * of the tag's data buffer.
     */
    for (i = 0; i < tag->num_write_requests; i++) {
        int reply_service;

        req = tag->reqs[i];

        if (!req) {
            rc = PLCTAG_ERR_NULL_PTR;
            break;
        }

        /* point to the data */
        cip_resp = (eip_cip_uc_resp*)(req->data);

        if (le2h16(cip_resp->encap_command) != AB_EIP_READ_RR_DATA) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", cip_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (le2h16(cip_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", cip_resp->encap_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /* if we have fragmented the request, we need to look for a different return code */
        reply_service = ((tag->num_write_requests > 1) ? (AB_EIP_CMD_CIP_WRITE_FRAG | AB_EIP_CMD_CIP_OK) :
                         (AB_EIP_CMD_CIP_WRITE | AB_EIP_CMD_CIP_OK));

        if (cip_resp->reply_service != reply_service) {
            pdebug(DEBUG_WARN, "CIP response reply service unexpected: %d", cip_resp->reply_service);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if (cip_resp->status != AB_CIP_STATUS_OK && cip_resp->status != AB_CIP_STATUS_FRAG) {
            pdebug(DEBUG_WARN, "CIP read failed with status: %d", cip_resp->status);
            pdebug(DEBUG_WARN, cip_decode_status(cip_resp->status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }
    }

    /*
     * Now remove the requests from the session's request list.
         *
         * FIXME - this has been removed in favor of making the
         * IO thread do the clean up.
     */
    // for(i = 0; i < tag->num_write_requests; i++) {
    //  int tmp_rc;

    //  req = tag->reqs[i];

    //  tmp_rc = request_remove(tag->session, req);

    //  if(tmp_rc != PLCTAG_STATUS_OK) {
    //      pdebug(DEBUG_WARN,"Unable to remove the request from the list! rc=%d",rc);

    //      /* since we could not remove it, maybe the thread can. */
    //      req->abort_request = 1;

    //      rc = tmp_rc;
    //  } else {
    //      /* free up the request resources */
    //      request_destroy(&req);
    //  }

    //  /* mark it as freed */
    //  tag->reqs[i] = NULL;
    //}

    /* this triggers the clean up */
    ab_tag_abort(tag);

    tag->write_in_progress = 0;
    tag->status = rc;

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}

int calculate_write_sizes(ab_tag_p tag)
{
    int overhead;
    int data_per_packet;
    int num_reqs;
    int rc = PLCTAG_STATUS_OK;
    int i;
    int byte_offset;

    pdebug(DEBUG_DETAIL, "Starting.");

    if (tag->num_write_requests > 0) {
        pdebug(DEBUG_DETAIL, "Early termination, write sizes already calculated.");
        return rc;
    }

    /* if we are here, then we have all the type data etc. */
    overhead = sizeof(eip_cip_uc_req)        /* base packet size */
               + 1                           /* service request, one byte */
               + tag->encoded_name_size      /* full encoded name */
               + tag->encoded_type_info_size /* encoded type size */
               + tag->conn_path_size + 2     /* encoded device path size plus two bytes for length and padding */
               + 2                           /* element count, 16-bit int */
               + 4                           /* byte offset, 32-bit int */
               + 8;                          /* MAGIC fudge factor */

    data_per_packet = MAX_EIP_PACKET_SIZE - overhead;

    /* we want a multiple of 4 bytes */
    data_per_packet &= 0xFFFFFFFC;

    if (data_per_packet <= 0) {
        pdebug(DEBUG_WARN,
               "Unable to send request.  Packet overhead, %d bytes, is too large for packet, %d bytes!",
               overhead,
               MAX_EIP_PACKET_SIZE);
        tag->status = PLCTAG_ERR_TOO_LONG;
        return PLCTAG_ERR_TOO_LONG;
    }

    num_reqs = (tag->size + (data_per_packet - 1)) / data_per_packet;

    pdebug(DEBUG_DETAIL, "We need %d requests.", num_reqs);

    byte_offset = 0;

    for (i = 0; i < num_reqs && rc == PLCTAG_STATUS_OK; i++) {
        /* allocate a new slot */
        rc = allocate_write_request_slot(tag);

        if (rc == PLCTAG_STATUS_OK) {
            /* how much data are we going to write in this packet? */
            if ((tag->size - byte_offset) > data_per_packet) {
                tag->write_req_sizes[i] = data_per_packet;
            } else {
                tag->write_req_sizes[i] = (tag->size - byte_offset);
            }

            pdebug(DEBUG_DETAIL, "Request %d is of size %d.", i, tag->write_req_sizes[i]);

            /* update the byte offset for the next packet */
            byte_offset += tag->write_req_sizes[i];
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}

/*#ifdef __cplusplus
}
#endif
*/
