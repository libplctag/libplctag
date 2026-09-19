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

#include <libplctag/protocols/cip/tag.h>

#include <inttypes.h>
#include <libplctag/protocols/cip/cip.h>
#include <libplctag/protocols/cip/conn.h>
#include <utils/rc.h>
#include <libplctag/protocols/cip/defs.h>
#include <libplctag/protocols/eip/defs.h>
#include <utils/byteorder.h>
#include <utils/mem.h>

#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <utils/debug.h>


/*
 * The vtable's status entry before a PLC-specific one replaces it.  Both modules
 * carried an identical copy, each of which logged tag->tag_id before testing tag
 * for null -- so a null tag would have crashed on the way to reporting itself.
 * The test comes first here.
 */
extern int cip_default_tag_status(plc_tag_p tag) {
    if(!tag) { return PLCTAG_ERR_NOT_FOUND; }

    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    return tag->status;
}


/*
 * A tag is pending while an operation is in flight, and fatally broken with no
 * connection.  The connection is passed in because its type is still module
 * specific; only its nullness matters here.
 */
extern int cip_tag_status(cip_tag_p tag, void *conn) {
    if(tag->read_in_progress) { return PLCTAG_STATUS_PENDING; }

    if(tag->write_in_progress) { return PLCTAG_STATUS_PENDING; }

    if(!conn) {
        /* this is not OK.  This is fatal! */
        return PLCTAG_ERR_CREATE;
    }

    return tag->status;
}


/*
 * Start a raw-tag write.  Raw tags carry a caller-supplied CIP payload, so the
 * only thing that varies between devices is which builder runs, and that arrives
 * as the two callbacks.
 */
extern int cip_raw_tag_write_start(cip_tag_p tag, cip_build_request_func build_connected,
                                   cip_build_request_func build_unconnected) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Starting");

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Raw tag found with a read in flight!");
        return PLCTAG_ERR_BAD_STATUS;
    }

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
        return PLCTAG_ERR_BUSY;
    }

    /* the write is now in flight */
    tag->write_in_progress = 1;

    rc = (tag->use_connected_msg) ? build_connected(tag) : build_unconnected(tag);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Unable to build write request!");
        tag->write_in_progress = 0;

        return rc;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_PENDING;
}


extern int cip_fill_tag_name(cip_tag_p tag, const char *name) {
    cip_tag_name_t ctx;
    int rc = PLCTAG_STATUS_OK;

    ctx.tag_id = tag->tag_id;
    ctx.elem_count = tag->elem_count;
    ctx.encoded_name = tag->encoded_name;
    ctx.encoded_name_size = 0;
    ctx.is_bit = 0;
    ctx.bit = 0;

    rc = cip_encode_tag_name(&ctx, name);

    if(rc == PLCTAG_STATUS_OK) {
        tag->encoded_name_size = ctx.encoded_name_size;
        tag->is_bit = (uint8_t)(ctx.is_bit ? 1 : 0);
        tag->bit = (uint8_t)ctx.bit;
    }

    return rc;
}


extern int cip_check_cpf_unconnected(cip_tag_p tag, cip_request_p request) {
    eip_cip_uc_resp *resp = (eip_cip_uc_resp *)(request->data);
    size_t data_item_start = 0;
    size_t data_item_length = 0;

    if(le2h16(resp->cpf_item_count) != 2) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Unconnected response has %u CPF items, expected 2!",
               le2h16(resp->cpf_item_count));
        return PLCTAG_ERR_BAD_DATA;
    }

    if(le2h16(resp->cpf_nai_item_type) != EIP_ITEM_NAI || le2h16(resp->cpf_udi_item_type) != EIP_ITEM_UDI) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id,
               "Unconnected response CPF item types are %04" PRIx16 "/%04" PRIx16 ", expected %04" PRIx16 "/%04" PRIx16 "!",
               le2h16(resp->cpf_nai_item_type), le2h16(resp->cpf_udi_item_type), EIP_ITEM_NAI, EIP_ITEM_UDI);
        return PLCTAG_ERR_BAD_DATA;
    }

    data_item_start = (size_t)((uint8_t *)(&resp->reply_service) - request->data);
    data_item_length = (size_t)le2h16(resp->cpf_udi_item_length);

    if(data_item_start + data_item_length != (size_t)request->request_size) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id,
               "Unconnected data item claims %" PRIu64 " bytes but the response is %d bytes with the item starting at %" PRIu64
               "!",
               (uint64_t)data_item_length, request->request_size, (uint64_t)data_item_start);
        return PLCTAG_ERR_BAD_DATA;
    }

    return PLCTAG_STATUS_OK;
}


extern int cip_check_cpf_connected(cip_tag_p tag, cip_request_p request, uint32_t orig_connection_id,
                                   uint32_t targ_connection_id) {
    eip_cip_co_resp *resp = (eip_cip_co_resp *)(request->data);
    size_t data_item_start = 0;
    size_t data_item_length = 0;

    if(le2h16(resp->cpf_item_count) != 2) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Connected response has %u CPF items, expected 2!",
               le2h16(resp->cpf_item_count));
        return PLCTAG_ERR_BAD_DATA;
    }

    if(le2h16(resp->cpf_cai_item_type) != EIP_ITEM_CAI || le2h16(resp->cpf_cdi_item_type) != EIP_ITEM_CDI) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id,
               "Connected response CPF item types are %04" PRIx16 "/%04" PRIx16 ", expected %04" PRIx16 "/%04" PRIx16 "!",
               le2h16(resp->cpf_cai_item_type), le2h16(resp->cpf_cdi_item_type), EIP_ITEM_CAI, EIP_ITEM_CDI);
        return PLCTAG_ERR_BAD_DATA;
    }

    /*
     * Only meaningful once ForwardOpen has actually negotiated a connection.  Until then
     * orig_connection_id is just the local placeholder, we send connection ID zero on the
     * wire, and the target echoes zero back -- there is no connection identity to check.
     */
    if(targ_connection_id != 0 && le2h32(resp->cpf_orig_conn_id) != orig_connection_id) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id,
               "Connected response is for connection %" PRIx32 " but ours is %" PRIx32 "!", le2h32(resp->cpf_orig_conn_id),
               orig_connection_id);
        return PLCTAG_ERR_BAD_DATA;
    }

    /*
     * The connected data item covers the connection sequence number and everything after it.
     * Require it to match what we actually received rather than merely fit, otherwise the PLC
     * can shorten the item and leave the handlers reading bytes it never sent.
     */
    data_item_start = (size_t)((uint8_t *)(&resp->cpf_conn_seq_num) - request->data);
    data_item_length = (size_t)le2h16(resp->cpf_cdi_item_length);

    if(data_item_start + data_item_length != (size_t)request->request_size) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id,
               "Connected data item claims %" PRIu64 " bytes but the response is %d bytes with the item starting at %" PRIu64 "!",
               (uint64_t)data_item_length, request->request_size, (uint64_t)data_item_start);
        return PLCTAG_ERR_BAD_DATA;
    }

    return PLCTAG_STATUS_OK;
}

/*********************************************************************
 ** Request builders
 **
 ** AB's versions, shared with Omron.  Omron's copies were the same code with the
 ** fragmentation branches removed and, in the unconnected path, an overhead
 ** calculation that had drifted -- a path Omron never executed, because it forces
 ** connected messaging.
 **
 ** The connection arrives separately because its type is still module specific,
 ** and queuing or abandoning a request reaches module state, so those come through
 ** cip_build_io_t.
 *********************************************************************/

extern int cip_calculate_write_data_per_packet(cip_tag_p tag, cip_conn_p conn) {
    int overhead = 0;
    int data_per_packet = 0;
    int available_payload = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Starting.");

    /* if we are here, then we have all the type data etc. */
    available_payload = cip_conn_get_available_payload_space(conn);

    if(tag->use_connected_msg) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Connected tag.");
        overhead = 1                             /* service request, one byte */
                   + tag->encoded_name_size      /* full encoded name */
                   + tag->encoded_type_info_size /* encoded type size */
                   + 2                           /* element count, 16-bit int */
                   + 4                           /* byte offset, 32-bit int */
                   + 8;                          /* MAGIC fudge factor */
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Unconnected tag.");
        overhead = 1                             /* CIP service Unconnected Send */
                   + 1                           /* path size */
                   + 4                           /* Connection Manager 20 06 24 1 */
                   + 1                           /* seconds per tick */
                   + 1                           /* timeout ticks */
                   + 2                           /* Embedded payload size */
                   + 1                           /* service request, one byte */
                   + tag->encoded_name_size      /* full encoded name */
                   + tag->encoded_type_info_size /* encoded type size */
                   + 2                           /* element count, 16-bit int */
                   + 4                           /* byte offset, 32-bit int */
                   + 8;                          /* MAGIC fudge factor */
    }

    /* make sure that overhead is an even number of bytes */
    if(overhead & 1) { overhead++; }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Write overhead is %d bytes.", overhead);

    data_per_packet = available_payload - overhead;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id,
           "Write packet available payload is %d, write overhead is %d, and write data per packet is %d.", available_payload,
           overhead, data_per_packet);

    if(data_per_packet <= 0) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id,
               "Unable to send request.  Packet overhead, %d bytes, is too large for available payload, %d bytes!", overhead,
               available_payload);
        return PLCTAG_ERR_TOO_LARGE;
    }

    int elements_per_packet = 0;
    int element_size = 0;

    /*
     * elem_size is derived from the PLC's response as tag->size / tag->elem_count, so a PLC
     * that returns fewer bytes than the tag has elements drives it to zero.  It is the
     * divisor below, so check it here.
     */
    if(tag->elem_size <= 0) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Tag element size of %d bytes is not usable!", tag->elem_size);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* if the tag size is less than 8 bytes, then use a multiple of the tag size.  Otherwise use
    8 bytes as the unit */
    if(tag->elem_size < 8) {
        elements_per_packet = data_per_packet / tag->elem_size;
        data_per_packet = elements_per_packet * tag->elem_size;
        element_size = tag->elem_size;
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Using tag size %d bytes for element size.", element_size);
    } else {
        /* round down to the nearest multiple of 8 bytes */
        elements_per_packet = data_per_packet / 8;
        data_per_packet = elements_per_packet * 8;
        element_size = 8;
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Using element size %d bytes.", element_size);
    }

    if(elements_per_packet < 1) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id,
               "Unable to send request.  Available payload, %d bytes, is too small to write at least %d bytes!",
               available_payload, element_size);
        return PLCTAG_ERR_TOO_LARGE;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Write data per packet is %d bytes.", data_per_packet);

    tag->write_data_per_packet = data_per_packet;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


extern int cip_build_write_bit_request_connected(cip_tag_p tag, cip_conn_p conn, const cip_build_io_t *io) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_req *cip = NULL;
    uint8_t *data = NULL;
    cip_request_p req = NULL;
    int i;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = cip_conn_create_request(conn, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    rc = cip_calculate_write_data_per_packet(tag, conn);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Unable to calculate valid write data per packet!.  rc=%s",
               plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < (tag->size * 2) + 2) { /* 2 masks plus a count word. */
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Insufficient space to write bit masks!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    cip = (eip_cip_co_req *)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_co_req);

    /*
     * set up the embedded CIP read packet
     * The format is:
     *
     * uint8_t cmd
     * LLA formatted name
     * uint16_t # size of a mask element
     * OR mask
     * AND mask
     */

    /*
     * set up the CIP Read-Modify-Write request type.
     */
    *data = CIP_CMD_RMW;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* write an INT of the mask size. */
    *data = (uint8_t)(tag->elem_size & 0xFF);
    data++;
    *data = (uint8_t)((tag->elem_size >> 8) & 0xFF);
    data++;

    /* write the OR mask */
    for(i = 0; i < tag->elem_size; i++) {
        if((tag->bit / 8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            /* if the bit is set, then we want to mask it on. */
            if(tag->data[tag->bit / 8] & mask) {
                *data = mask;
            } else {
                *data = (uint8_t)0;
            }

            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "adding OR mask byte %d: %x", i, *data);

            data++;
        } else {
            /* this is not the data we care about. */
            *data = (uint8_t)0;

            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "adding OR mask byte %d: %x", i, *data);

            data++;
        }
    }

    /* write the AND mask */
    for(i = 0; i < tag->elem_size; i++) {
        if((tag->bit / 8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            /* if the bit is set, then we want to _not_ mask it off. */
            if(tag->data[tag->bit / 8] & mask) {
                *data = (uint8_t)0xFF;
            } else {
                *data = (uint8_t)(~mask);
            }

            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "adding AND mask byte %d: %x", i, *data);

            data++;
        } else {
            /* this is not the data we care about. */
            *data = (uint8_t)0xFF;

            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "adding AND mask byte %d: %x", i, *data);

            data++;
        }
    }

    /* let the rest of the system know that the write is complete after this. */
    tag->offset = tag->size;

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(EIP_ITEM_CAI); /* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);             /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(EIP_ITEM_CDI); /* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&cip->cpf_conn_seq_num))); /* REQ: fill in with length of remaining data. */

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));

    /* allow packing if the tag allows it. */
    req->allow_packing = tag->allow_packing;

    /* add the request to the session's list. */
    rc = io->add_request(conn, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        /* session_add_request() takes its own reference; ours is still outstanding. */
        req = rc_dec(req);
        io->abort_request(tag);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}


extern int cip_build_write_bit_request_unconnected(cip_tag_p tag, cip_conn_p conn, const cip_build_io_t *io) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_uc_req *cip = NULL;
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    uint8_t *embed_end = NULL;
    cip_request_p req = NULL;
    int i = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = cip_conn_create_request(conn, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    rc = cip_calculate_write_data_per_packet(tag, conn);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Unable to calculate valid write data per packet!.  rc=%s",
               plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < (tag->size * 2) + 2) { /* 2 masks plus a count word. */
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Insufficient space to write bit masks!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    cip = (eip_cip_uc_req *)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_uc_req);

    embed_start = data;

    /*
     * set up the embedded CIP read packet
     * The format is:
     *
     * uint8_t cmd
     * LLA formatted name
     * uint16_t # size of a mask element
     * OR mask
     * AND mask
     */

    /*
     * set up the CIP Read-Modify-Write request type.
     */
    *data = CIP_CMD_RMW;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* write an INT of the mask size. */
    *data = (uint8_t)(tag->elem_size & 0xFF);
    data++;
    *data = (uint8_t)((tag->elem_size >> 8) & 0xFF);
    data++;

    /* write the OR mask */
    for(i = 0; i < tag->elem_size; i++) {
        if((tag->bit / 8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            /* if the bit is set, then we want to mask it on. */
            if(tag->data[tag->bit / 8] & mask) {
                *data = mask;
            } else {
                *data = (uint8_t)0;
            }

            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "adding OR mask byte %d: %x", i, *data);

            data++;
        } else {
            /* this is not the data we care about. */
            *data = (uint8_t)0;

            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "adding OR mask byte %d: %x", i, *data);

            data++;
        }
    }

    /* write the AND mask */
    for(i = 0; i < tag->elem_size; i++) {
        if((tag->bit / 8) == i) {
            uint8_t mask = (uint8_t)(1 << (tag->bit % 8));

            /* if the bit is set, then we want to _not_ mask it off. */
            if(tag->data[tag->bit / 8] & mask) {
                *data = (uint8_t)0xFF;
            } else {
                *data = (uint8_t)(~mask);
            }

            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "adding AND mask byte %d: %x", i, *data);

            data++;
        } else {
            /* this is not the data we care about. */
            *data = (uint8_t)0xFF;

            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "adding AND mask byte %d: %x", i, *data);

            data++;
        }
    }

    /* let the rest of the system know that the write is complete after this. */
    tag->offset = tag->size;

    /* now we go back and fill in the fields of the static part */
    /* mark the end of the embedded packet */
    embed_end = data;

    /*
     * after the embedded packet, we need to tell the message router
     * how to get to the target device.
     */

    /* Now copy in the routing information for the embedded message */
    *data = (conn->conn_path_size) / 2; /* in 16-bit words */
    data++;
    *data = 0;
    data++;
    mem_copy(data, conn->conn_path, conn->conn_path_size);
    data += conn->conn_path_size;

    /* now fill in the rest of the structure. */

    /* encap fields */
    cip->encap_command = h2le16(EIP_UNCONNECTED_SEND); /* ALWAYS 0x006F Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_nai_item_type = h2le16(EIP_ITEM_NAI); /* ALWAYS 0 */
    cip->cpf_nai_item_length = h2le16(0);             /* ALWAYS 0 */
    cip->cpf_udi_item_type = h2le16(EIP_ITEM_UDI); /* ALWAYS 0x00B2 - Unconnected Data Item */
    cip->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&(cip->cm_service_code)))); /* REQ: fill in with length of remaining data. */

    /* CM Service Request - Connection Manager */
    cip->cm_service_code = CIP_CMD_UNCONNECTED_SEND; /* 0x52 Unconnected Send */
    cip->cm_req_path_size = 2;                          /* 2, size in 16-bit words of path, next field */
    cip->cm_req_path[0] = 0x20;                         /* class */
    cip->cm_req_path[1] = 0x06;                         /* Connection Manager */
    cip->cm_req_path[2] = 0x24;                         /* instance */
    cip->cm_req_path[3] = 0x01;                         /* instance 1 */

    /* Unconnected send needs timeout information */
    cip->secs_per_tick = CIP_SECS_PER_TICK; /* seconds per tick */
    cip->timeout_ticks = CIP_TIMEOUT_TICKS; /* timeout = srd_secs_per_tick * src_timeout_ticks */

    /* size of embedded packet */
    cip->uc_cmd_length = h2le16((uint16_t)(embed_end - embed_start));

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));

    /* allow packing if the tag allows it. */
    req->allow_packing = tag->allow_packing;

    /* add the request to the session's list. */
    rc = io->add_request(conn, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        /* session_add_request() takes its own reference; ours is still outstanding. */
        req = rc_dec(req);
        io->abort_request(tag);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}


extern int cip_build_write_request_connected(cip_tag_p tag, cip_conn_p conn, const cip_build_io_t *io, int byte_offset) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_req *cip = NULL;
    uint8_t *data = NULL;
    cip_request_p req = NULL;
    int multiple_requests = 0;
    int write_size = 0;
    int str_pad_to_multiple_bytes = 1;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Starting.");

    if(tag->is_bit) { return cip_build_write_bit_request_connected(tag, conn, io); }

    /* get a request buffer */
    rc = cip_conn_create_request(conn, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    rc = cip_calculate_write_data_per_packet(tag, conn);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Unable to calculate valid write data per packet!.  rc=%s",
               plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < tag->size) { multiple_requests = 1; }

    /*
     * A write too large for one packet needs a fragmentation mechanism.  AB has the
     * CIP fragmented write service; a PLC with no mechanism cannot do this at all.
     * See cip_plc_config_t.supports_fragmented_operations.
     */
    if(multiple_requests && !conn->plc_config.supports_fragmented_operations) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Tag is too large for an unfragmented request on this PLC!");
        return PLCTAG_ERR_TOO_LARGE;
    }

    cip = (eip_cip_co_req *)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_co_req);

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

    /*
     * set up the CIP Read request type.
     * Different if more than one request.
     *
     * This handles a bug where attempting fragmented requests
     * does not appear to work with a single boolean.
     */
    *data = (multiple_requests) ? CIP_CMD_WRITE_FRAG : CIP_CMD_WRITE;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* copy encoded type info */
    if(tag->encoded_type_info_size) {
        mem_copy(data, tag->encoded_type_info, tag->encoded_type_info_size);
        data += tag->encoded_type_info_size;
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Data type unsupported!");
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* copy the item count, little endian */
    *((uint16_le *)data) = h2le16((uint16_t)(tag->elem_count));
    data += sizeof(uint16_le);

    if(multiple_requests) {
        /* put in the byte offset */
        *((uint32_le *)data) = h2le32((uint32_t)(byte_offset));
        data += sizeof(uint32_le);
    }

    /* how much data to write? */
    write_size = tag->size - tag->offset;

    if(write_size > tag->write_data_per_packet) { write_size = tag->write_data_per_packet; }

    /* now copy the data to write */
    mem_copy(data, tag->data + tag->offset, write_size);
    data += write_size;
    tag->offset += write_size;

    /* need to pad data to multiple of either 1, 2 or 4 bytes */
    /* for some PLCs (OmronNJ), padding causes issues when writing counted strings as it creates a mismatch between
        the length of the string and the count integer, therefor this padding can be disabled using the str_pad_16_bits attribute
     */
    str_pad_to_multiple_bytes = (int)tag->byte_order->str_pad_to_multiple_bytes;
    if((str_pad_to_multiple_bytes == 2 || str_pad_to_multiple_bytes == 4) && write_size != 0) {
        if(write_size % str_pad_to_multiple_bytes != 0) {
            int pad_size = str_pad_to_multiple_bytes - (write_size % str_pad_to_multiple_bytes);
            for(int i = 0; i < pad_size; i++) {
                *data = 0;
                data++;
            }
        }
    }

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(EIP_ITEM_CAI); /* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);             /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(EIP_ITEM_CDI); /* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&cip->cpf_conn_seq_num))); /* REQ: fill in with length of remaining data. */

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));

    /* allow packing if the tag allows it. */
    req->allow_packing = tag->allow_packing;

    /* add the request to the session's list. */
    rc = io->add_request(conn, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        /* session_add_request() takes its own reference; ours is still outstanding. */
        req = rc_dec(req);
        io->abort_request(tag);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}


extern int cip_build_write_request_unconnected(cip_tag_p tag, cip_conn_p conn, const cip_build_io_t *io, int byte_offset) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_uc_req *cip = NULL;
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    uint8_t *embed_end = NULL;
    cip_request_p req = NULL;
    int multiple_requests = 0;
    int write_size = 0;
    int str_pad_to_multiple_bytes = 1;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Starting.");

    if(tag->is_bit) { return cip_build_write_bit_request_unconnected(tag, conn, io); }

    /* get a request buffer */
    rc = cip_conn_create_request(conn, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    rc = cip_calculate_write_data_per_packet(tag, conn);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Unable to calculate valid write data per packet!.  rc=%s",
               plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->write_data_per_packet < tag->size) { multiple_requests = 1; }

    /*
     * A write too large for one packet needs a fragmentation mechanism.  AB has the
     * CIP fragmented write service; a PLC with no mechanism cannot do this at all.
     * See cip_plc_config_t.supports_fragmented_operations.
     */
    if(multiple_requests && !conn->plc_config.supports_fragmented_operations) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Tag is too large for an unfragmented request on this PLC!");
        return PLCTAG_ERR_TOO_LARGE;
    }

    cip = (eip_cip_uc_req *)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_uc_req);

    embed_start = data;

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

    /*
     * set up the CIP Read request type.
     * Different if more than one request.
     *
     * This handles a bug where attempting fragmented requests
     * does not appear to work with a single boolean.
     */
    *data = (multiple_requests) ? CIP_CMD_WRITE_FRAG : CIP_CMD_WRITE;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* copy encoded type info */
    if(tag->encoded_type_info_size) {
        mem_copy(data, tag->encoded_type_info, tag->encoded_type_info_size);
        data += tag->encoded_type_info_size;
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Data type unsupported!");
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* copy the item count, little endian */
    *((uint16_le *)data) = h2le16((uint16_t)(tag->elem_count));
    data += sizeof(uint16_le);

    if(multiple_requests) {
        /* put in the byte offset */
        *((uint32_le *)data) = h2le32((uint32_t)byte_offset);
        data += sizeof(uint32_le);
    }

    /* how much data to write? */
    write_size = tag->size - tag->offset;

    if(write_size > tag->write_data_per_packet) { write_size = tag->write_data_per_packet; }

    /* now copy the data to write */
    mem_copy(data, tag->data + tag->offset, write_size);
    data += write_size;
    tag->offset += write_size;

    /* need to pad data to multiple of either 1, 2 or 4 bytes */
    /* for some PLCs (OmronNJ), padding causes issues when writing counted strings as it creates a mismatch between
        the length of the string and the count integer, therefor this padding can be disabled using the str_pad_16_bits attribute
     */
    str_pad_to_multiple_bytes = (int)tag->byte_order->str_pad_to_multiple_bytes;
    if((str_pad_to_multiple_bytes == 2 || str_pad_to_multiple_bytes == 4) && write_size != 0) {
        if(write_size % str_pad_to_multiple_bytes != 0) {
            int pad_size = str_pad_to_multiple_bytes - (write_size % str_pad_to_multiple_bytes);
            for(int i = 0; i < pad_size; i++) {
                *data = 0;
                data++;
            }
        }
    }


    /* now we go back and fill in the fields of the static part */
    /* mark the end of the embedded packet */
    embed_end = data;

    /*
     * after the embedded packet, we need to tell the message router
     * how to get to the target device.
     */

    /* Now copy in the routing information for the embedded message */
    *data = (conn->conn_path_size) / 2; /* in 16-bit words */
    data++;
    *data = 0;
    data++;
    mem_copy(data, conn->conn_path, conn->conn_path_size);
    data += conn->conn_path_size;

    /* now fill in the rest of the structure. */

    /* encap fields */
    cip->encap_command = h2le16(EIP_UNCONNECTED_SEND); /* ALWAYS 0x006F Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_nai_item_type = h2le16(EIP_ITEM_NAI); /* ALWAYS 0 */
    cip->cpf_nai_item_length = h2le16(0);             /* ALWAYS 0 */
    cip->cpf_udi_item_type = h2le16(EIP_ITEM_UDI); /* ALWAYS 0x00B2 - Unconnected Data Item */
    cip->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&(cip->cm_service_code)))); /* REQ: fill in with length of remaining data. */

    /* CM Service Request - Connection Manager */
    cip->cm_service_code = CIP_CMD_UNCONNECTED_SEND; /* 0x52 Unconnected Send */
    cip->cm_req_path_size = 2;                          /* 2, size in 16-bit words of path, next field */
    cip->cm_req_path[0] = 0x20;                         /* class */
    cip->cm_req_path[1] = 0x06;                         /* Connection Manager */
    cip->cm_req_path[2] = 0x24;                         /* instance */
    cip->cm_req_path[3] = 0x01;                         /* instance 1 */

    /* Unconnected send needs timeout information */
    cip->secs_per_tick = CIP_SECS_PER_TICK; /* seconds per tick */
    cip->timeout_ticks = CIP_TIMEOUT_TICKS; /* timeout = srd_secs_per_tick * src_timeout_ticks */

    /* size of embedded packet */
    cip->uc_cmd_length = h2le16((uint16_t)(embed_end - embed_start));

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));

    /* allow packing if the tag allows it. */
    req->allow_packing = tag->allow_packing;

    /* add the request to the session's list. */
    rc = io->add_request(conn, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        /* session_add_request() takes its own reference; ours is still outstanding. */
        req = rc_dec(req);
        io->abort_request(tag);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}


extern int cip_build_read_request_connected(cip_tag_p tag, cip_conn_p conn, const cip_build_io_t *io, int byte_offset) {
    eip_cip_co_req *cip = NULL;
    uint8_t *data = NULL;
    cip_request_p req = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t read_cmd = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = cip_conn_create_request(conn, tag->tag_id, &req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    /* point the request struct at the buffer */
    cip = (eip_cip_co_req *)(req->data);

    /* point to the end of the struct */
    data = (req->data) + sizeof(eip_cip_co_req);

    /*
     * set up the embedded CIP read packet
     * The format is:
     *
     * uint8_t cmd
     * LLA formatted name
     * uint16_t # of elements to read
     */

    /*
     * A read too large for one packet needs the fragmented service and a byte offset.
     * A PLC with no fragmentation mechanism gets the plain read and will fail on a
     * tag it cannot return in one response.
     */
    read_cmd = conn->plc_config.supports_fragmented_operations ? CIP_CMD_READ_FRAG : CIP_CMD_READ;

    *data = read_cmd;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* add the count of elements to read. */
    *((uint16_le *)data) = h2le16((uint16_t)(tag->elem_count));
    data += sizeof(uint16_le);

    if(read_cmd == CIP_CMD_READ_FRAG) {
        /* add the byte offset for this request */
        *((uint32_le *)data) = h2le32((uint32_t)byte_offset);
        data += sizeof(uint32_le);
    }

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(EIP_CONNECTED_SEND); /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_cai_item_type = h2le16(EIP_ITEM_CAI); /* ALWAYS 0x00A1 connected address item */
    cip->cpf_cai_item_length = h2le16(4);             /* ALWAYS 4, size of connection ID*/
    cip->cpf_cdi_item_type = h2le16(EIP_ITEM_CDI); /* ALWAYS 0x00B1 - connected Data Item */
    cip->cpf_cdi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&cip->cpf_conn_seq_num))); /* REQ: fill in with length of remaining data. */

    /* Check if the payload size exceeds available space before setting request_size */
    int packet_payload_size = (int)(data - (uint8_t *)(&cip->cpf_conn_seq_num));
    int available_payload = cip_conn_get_available_payload_space(conn);

    if(packet_payload_size > available_payload) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Request payload (%d bytes) exceeds available space (%d bytes)!",
               packet_payload_size, available_payload);
        io->abort_request(tag);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));

    req->allow_packing = tag->allow_packing;

    /*
     * What the packing logic needs to know about this response: how big it was last
     * time, whether this is the first read so the size is still unknown, and whether
     * an overflowing packed response could be recovered from.
     */
    req->response_size = tag->size;
    req->first_read = tag->first_read;
    req->supports_fragmented_operations = conn->plc_config.supports_fragmented_operations;

    /* add the request to the connection's list. */
    rc = io->add_request(conn, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        /* session_add_request() takes its own reference; ours is still outstanding. */
        req = rc_dec(req);
        io->abort_request(tag);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}


extern int cip_build_read_request_unconnected(cip_tag_p tag, cip_conn_p conn, const cip_build_io_t *io, int byte_offset) {
    eip_cip_uc_req *cip;
    uint8_t *data;
    uint8_t *embed_start, *embed_end;
    cip_request_p req = NULL;
    int rc = PLCTAG_STATUS_OK;
    uint8_t read_cmd = 0;
    uint16_le tmp_uint16_le;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Starting.");

    /* get a request buffer */
    rc = cip_conn_create_request(conn, tag->tag_id, &req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Unable to get new request.  rc=%d", rc);
        return rc;
    }

    /* point the request struct at the buffer */
    cip = (eip_cip_uc_req *)(req->data);

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
    /*
     * A read too large for one packet needs the fragmented service and a byte offset.
     * A PLC with no fragmentation mechanism gets the plain read and will fail on a
     * tag it cannot return in one response.
     */
    read_cmd = conn->plc_config.supports_fragmented_operations ? CIP_CMD_READ_FRAG : CIP_CMD_READ;

    *data = read_cmd;
    data++;

    /* copy the tag name into the request */
    mem_copy(data, tag->encoded_name, tag->encoded_name_size);
    data += tag->encoded_name_size;

    /* add the count of elements to read. */
    tmp_uint16_le = h2le16((uint16_t)(tag->elem_count));
    mem_copy(data, &tmp_uint16_le, (int)(unsigned int)sizeof(tmp_uint16_le));
    data += sizeof(tmp_uint16_le);

    /* add the byte offset for this request */
    if(read_cmd == CIP_CMD_READ_FRAG) {
        /* FIXME - this may not work on some processors. */
        *((uint32_le *)data) = h2le32((uint32_t)byte_offset);
        data += sizeof(uint32_le);
    }

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
    if(conn->conn_path_size > 0) {
        *data = (conn->conn_path_size) / 2; /* in 16-bit words */
        data++;
        *data = 0; /* reserved/pad */
        data++;
        mem_copy(data, conn->conn_path, conn->conn_path_size);
        data += conn->conn_path_size;
    }

    /* now we go back and fill in the fields of the static part */

    /* encap fields */
    cip->encap_command = h2le16(EIP_UNCONNECTED_SEND); /* ALWAYS 0x0070 Unconnected Send*/

    /* router timeout */
    cip->router_timeout = h2le16(1); /* one second timeout, enough? */

    /* Common Packet Format fields for unconnected send. */
    cip->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    cip->cpf_nai_item_type = h2le16(EIP_ITEM_NAI); /* ALWAYS 0 */
    cip->cpf_nai_item_length = h2le16(0);             /* ALWAYS 0 */
    cip->cpf_udi_item_type = h2le16(EIP_ITEM_UDI); /* ALWAYS 0x00B2 - Unconnected Data Item */
    cip->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&cip->cm_service_code))); /* REQ: fill in with length of remaining data. */

    /* CM Service Request - Connection Manager */
    cip->cm_service_code = CIP_CMD_UNCONNECTED_SEND; /* 0x52 Unconnected Send */
    cip->cm_req_path_size = 2;                          /* 2, size in 16-bit words of path, next field */
    cip->cm_req_path[0] = 0x20;                         /* class */
    cip->cm_req_path[1] = 0x06;                         /* Connection Manager */
    cip->cm_req_path[2] = 0x24;                         /* instance */
    cip->cm_req_path[3] = 0x01;                         /* instance 1 */

    /* Unconnected send needs timeout information */
    cip->secs_per_tick = CIP_SECS_PER_TICK; /* seconds per tick */
    cip->timeout_ticks = CIP_TIMEOUT_TICKS; /* timeout = src_secs_per_tick * src_timeout_ticks */

    /* size of embedded packet */
    cip->uc_cmd_length = h2le16((uint16_t)(embed_end - embed_start));

    /* Check if the payload size exceeds available space before setting request_size */
    int packet_payload_size = (int)(embed_end - embed_start);
    int available_payload = cip_conn_get_available_payload_space(conn);

    if(packet_payload_size > available_payload) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Request payload (%d bytes) exceeds available space (%d bytes)!",
               packet_payload_size, available_payload);
        io->abort_request(tag);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* set the size of the request */
    req->request_size = (int)(data - (req->data));

    /* allow packing if the tag allows it. */
    req->allow_packing = tag->allow_packing;

    /*
     * What the packing logic needs to know about this response: how big it was last
     * time, whether this is the first read so the size is still unknown, and whether
     * an overflowing packed response could be recovered from.
     */
    req->response_size = tag->size;
    req->first_read = tag->first_read;
    req->supports_fragmented_operations = conn->plc_config.supports_fragmented_operations;

    /* add the request to the connection's list. */
    rc = io->add_request(conn, req);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, tag->tag_id, "Unable to add request to session! rc=%d", rc);
        /* session_add_request() takes its own reference; ours is still outstanding. */
        req = rc_dec(req);
        io->abort_request(tag);
        return rc;
    }

    /* save the request for later */
    tag->req = req;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Done");

    return PLCTAG_STATUS_OK;
}
