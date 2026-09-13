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
               "Unconnected data item claims %zu bytes but the response is %d bytes with the item starting at %zu!",
               data_item_length, request->request_size, data_item_start);
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
               "Connected data item claims %zu bytes but the response is %d bytes with the item starting at %zu!",
               data_item_length, request->request_size, data_item_start);
        return PLCTAG_ERR_BAD_DATA;
    }

    return PLCTAG_STATUS_OK;
}
