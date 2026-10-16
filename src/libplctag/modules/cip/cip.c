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
 * Connection Manager packet encoding and decoding.
 *
 * Nothing here touches a socket.  An encoder fills conn->data and sets conn->data_size; a
 * decoder reads what conn->data holds and updates the connection fields the reply carries.
 * conn.c owns the sending, the receiving and the state machine that sequences them.
 */

#include <libplctag/api/libplctag.h>
#include <libplctag/modules/cip/cip.h>
#include <libplctag/modules/cip/cip_tag_create.h>
#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/cip/error_codes.h>
#include <libplctag/modules/cip/services.h>
#include <libplctag/modules/cip/wire.h>
#include <inttypes.h>
#include <platform.h>
#include <stddef.h>
#include <utils/debug.h>


void cip_encode_route_timeout(int timeout_ms, uint8_t *secs_per_tick, uint8_t *timeout_ticks) {
    uint8_t tick_exponent = 0;

    if(timeout_ms < 1) { timeout_ms = 1; }

    /* 2^tick_exponent milliseconds per tick, at most 255 ticks. */
    while(tick_exponent < 15 && ((int64_t)1 << tick_exponent) * 255 < (int64_t)timeout_ms) { tick_exponent++; }

    *secs_per_tick = tick_exponent;
    *timeout_ticks = (uint8_t)(((int64_t)timeout_ms + ((int64_t)1 << tick_exponent) - 1) >> tick_exponent);

    if(*timeout_ticks == 0) { *timeout_ticks = 1; }
}


int cip_encode_forward_open_ex(cip_conn_p conn) {
    eip_forward_open_request_ex_t *fo = NULL;
    uint8_t *data;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    mem_set(conn->data, 0, (int)(sizeof(*fo) + conn->conn_path_size));

    fo = (eip_forward_open_request_ex_t *)(conn->data);

    /* point to the end of the struct */
    data = (conn->data) + sizeof(*fo);

    /* set up the path information. */
    mem_copy(data, conn->conn_path, conn->conn_path_size);
    data += conn->conn_path_size;

    /* fill in the static parts */

    /* encap header parts */
    fo->encap_command = h2le16(CIP_EIP_UNCONNECTED_SEND); /* 0x006F EIP Send RR Data command */
    fo->encap_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->interface_handle))); /* total length of packet except for encap header */
    fo->encap_session_handle = h2le32(conn->session_handle);
    fo->encap_sender_context = h2le64(++conn->session_seq_id);
    fo->router_timeout = h2le16(1); /* one second is enough ? */

    /* CPF parts */
    fo->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    fo->cpf_nai_item_type = h2le16(CIP_EIP_ITEM_NAI); /* null address item type */
    fo->cpf_nai_item_length = h2le16(0);             /* no data, zero length */
    fo->cpf_udi_item_type = h2le16(CIP_EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fo->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->cm_service_code))); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fo->cm_service_code = CIP_EIP_CMD_FORWARD_OPEN_EX; /* 0x54 Forward Open Request or 0x5B for Forward Open Extended */
    fo->cm_req_path_size = 2;                         /* size of path in 16-bit words */
    fo->cm_req_path[0] = 0x20;                        /* class */
    fo->cm_req_path[1] = 0x06;                        /* CM class */
    fo->cm_req_path[2] = 0x24;                        /* instance */
    fo->cm_req_path[3] = 0x01;                        /* instance 1 */

    /* Forward Open Params */
    cip_encode_route_timeout(CIP_EIP_CONN_TIMEOUT_MS, &fo->secs_per_tick, &fo->timeout_ticks);
    fo->orig_to_targ_conn_id = h2le32(0);     /* is this right?  Our connection id on the other machines? */
    fo->targ_to_orig_conn_id = h2le32(conn->orig_connection_id); /* Our connection id in the other direction. */
    /* this might need to be globally unique */
    conn->conn_serial_number = next_conn_serial_number(conn->conn_serial_number);
    fo->conn_serial_number = h2le16(conn->conn_serial_number); /* our connection ID/serial number. */
    fo->orig_vendor_id = h2le16(CIP_EIP_VENDOR_ID);                /* our unique :-) vendor ID */
    fo->orig_serial_number = h2le32(CIP_EIP_VENDOR_SN);            /* our serial number. */
    fo->conn_timeout_multiplier = CIP_EIP_TIMEOUT_MULTIPLIER;      /* timeout = mult * RPI */
    fo->orig_to_targ_rpi = h2le32(CIP_EIP_RPI);                    /* us to target RPI - Request Packet Interval in microseconds */
    fo->orig_to_targ_conn_params_ex = h2le32(
        CIP_EIP_CONN_PARAM_EX | conn->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    fo->targ_to_orig_rpi = h2le32(CIP_EIP_RPI);              /* target to us RPI - not really used for explicit messages? */
    fo->targ_to_orig_conn_params_ex = h2le32(
        CIP_EIP_CONN_PARAM_EX | conn->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    fo->transport_class = CIP_EIP_TRANSPORT_CLASS_T3;        /* 0xA3, server transport, class 3, application trigger */
    fo->path_size = conn->conn_path_size / 2;            /* size in 16-bit words */

    /* set the size of the request */
    conn->data_size = (uint32_t)(data - (conn->data));

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done");

    return PLCTAG_STATUS_OK;
}


int cip_encode_forward_open_old(cip_conn_p conn) {
    eip_forward_open_request_t *fo = NULL;
    uint8_t *data;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    mem_set(conn->data, 0, (int)(sizeof(*fo) + conn->conn_path_size));

    fo = (eip_forward_open_request_t *)(conn->data);

    /* point to the end of the struct */
    data = (conn->data) + sizeof(eip_forward_open_request_t);

    /* set up the path information. */
    mem_copy(data, conn->conn_path, conn->conn_path_size);
    data += conn->conn_path_size;

    /* fill in the static parts */

    /* encap header parts */
    fo->encap_command = h2le16(CIP_EIP_UNCONNECTED_SEND); /* 0x006F EIP Send RR Data command */
    fo->encap_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->interface_handle))); /* total length of packet except for encap header */
    fo->encap_session_handle = h2le32(conn->session_handle);
    fo->encap_sender_context = h2le64(++conn->session_seq_id);
    fo->router_timeout = h2le16(1); /* one second is enough ? */

    /* CPF parts */
    fo->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    fo->cpf_nai_item_type = h2le16(CIP_EIP_ITEM_NAI); /* null address item type */
    fo->cpf_nai_item_length = h2le16(0);             /* no data, zero length */
    fo->cpf_udi_item_type = h2le16(CIP_EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fo->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->cm_service_code))); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fo->cm_service_code = CIP_EIP_CMD_FORWARD_OPEN; /* 0x54 Forward Open Request or 0x5B for Forward Open Extended */
    fo->cm_req_path_size = 2;                      /* size of path in 16-bit words */
    fo->cm_req_path[0] = 0x20;                     /* class */
    fo->cm_req_path[1] = 0x06;                     /* CM class */
    fo->cm_req_path[2] = 0x24;                     /* instance */
    fo->cm_req_path[3] = 0x01;                     /* instance 1 */

    /* Forward Open Params */
    cip_encode_route_timeout(CIP_EIP_CONN_TIMEOUT_MS, &fo->secs_per_tick, &fo->timeout_ticks);
    fo->orig_to_targ_conn_id = h2le32(0);     /* is this right?  Our connection id on the other machines? */
    fo->targ_to_orig_conn_id = h2le32(conn->orig_connection_id); /* Our connection id in the other direction. */
    /* this might need to be globally unique */
    conn->conn_serial_number = next_conn_serial_number(conn->conn_serial_number);
    fo->conn_serial_number = h2le16(conn->conn_serial_number); /* our connection SEQUENCE number. */
    fo->orig_vendor_id = h2le16(CIP_EIP_VENDOR_ID);                /* our unique :-) vendor ID */
    fo->orig_serial_number = h2le32(CIP_EIP_VENDOR_SN);            /* our serial number. */
    fo->conn_timeout_multiplier = CIP_EIP_TIMEOUT_MULTIPLIER;      /* timeout = mult * RPI */

    fo->orig_to_targ_rpi = h2le32(CIP_EIP_RPI); /* us to target RPI - Request Packet Interval in microseconds */

    /* screwy logic if this is a DH+ route! */
    if(conn->is_dhp) {
        fo->orig_to_targ_conn_params = h2le16(CIP_EIP_PLC5_PARAM);
    } else {
        fo->orig_to_targ_conn_params = h2le16(
            CIP_EIP_CONN_PARAM | conn->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    }

    fo->targ_to_orig_rpi = h2le32(CIP_EIP_RPI); /* target to us RPI - not really used for explicit messages? */

    /* screwy logic if this is a DH+ route! */
    if(conn->is_dhp) {
        fo->targ_to_orig_conn_params = h2le16(CIP_EIP_PLC5_PARAM);
    } else {
        fo->targ_to_orig_conn_params = h2le16(
            CIP_EIP_CONN_PARAM | conn->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    }

    fo->transport_class = CIP_EIP_TRANSPORT_CLASS_T3; /* 0xA3, server transport, class 3, application trigger */
    fo->path_size = conn->conn_path_size / 2;     /* size in 16-bit words */

    /* set the size of the request */
    conn->data_size = (uint32_t)(data - (conn->data));

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done");

    return PLCTAG_STATUS_OK;
}


int cip_encode_forward_close(cip_conn_p conn) {
    eip_forward_close_req_t *fc;
    uint8_t *data;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    fc = (eip_forward_close_req_t *)(conn->data);

    /* point to the end of the struct */
    data = (conn->data) + sizeof(*fc);

    /* set up the path information. */
    mem_copy(data, conn->conn_path, conn->conn_path_size);
    data += conn->conn_path_size;

    /* FIXME DEBUG */
    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Forward Close connection path:");
    pdebug_dump_bytes(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, conn->conn_path, conn->conn_path_size);

    /* fill in the static parts */

    /* encap header parts */
    fc->encap_command = h2le16(CIP_EIP_UNCONNECTED_SEND); /* 0x006F EIP Send RR Data command */
    fc->encap_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fc->interface_handle))); /* total length of packet except for encap header */
    fc->encap_sender_context = h2le64(++conn->session_seq_id);
    fc->router_timeout = h2le16(1); /* one second is enough ? */

    /* CPF parts */
    fc->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    fc->cpf_nai_item_type = h2le16(CIP_EIP_ITEM_NAI); /* null address item type */
    fc->cpf_nai_item_length = h2le16(0);             /* no data, zero length */
    fc->cpf_udi_item_type = h2le16(CIP_EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fc->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fc->cm_service_code))); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fc->cm_service_code = CIP_EIP_CMD_FORWARD_CLOSE; /* 0x4E Forward Close Request */
    fc->cm_req_path_size = 2;                       /* size of path in 16-bit words */
    fc->cm_req_path[0] = 0x20;                      /* class */
    fc->cm_req_path[1] = 0x06;                      /* CM class */
    fc->cm_req_path[2] = 0x24;                      /* instance */
    fc->cm_req_path[3] = 0x01;                      /* instance 1 */

    /* Forward Open Params */
    cip_encode_route_timeout(CIP_EIP_CONN_TIMEOUT_MS, &fc->secs_per_tick, &fc->timeout_ticks);
    fc->conn_serial_number = h2le16(conn->conn_serial_number); /* our connection SEQUENCE number. */
    fc->orig_vendor_id = h2le16(CIP_EIP_VENDOR_ID);                /* our unique :-) vendor ID */
    fc->orig_serial_number = h2le32(CIP_EIP_VENDOR_SN);            /* our serial number. */
    fc->path_size = conn->conn_path_size / 2;                  /* size in 16-bit words */
    fc->reserved = (uint8_t)0;                                    /* padding for the path. */

    /* set the size of the request */
    conn->data_size = (uint32_t)(data - (conn->data));

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done");

    return PLCTAG_STATUS_OK;
}


int cip_decode_forward_open_response(cip_conn_p conn) {
    eip_forward_open_response_t *fo_resp;
    int rc = PLCTAG_STATUS_OK;

    conn->fo_ext_status = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    fo_resp = (eip_forward_open_response_t *)(conn->data);

    do {
        /*
         * recv_eip_response() only guarantees that we got an EIP header.  We are about to
         * read the CIP reply status, so require everything up to and including status_size.
         * An error reply legitimately stops there -- it carries extended status instead of
         * the connection IDs -- so do not demand the whole struct here.  The buffer is not
         * cleared between packets, so a short response would otherwise be read as stale
         * data from the previous one.
         */
        if((size_t)conn->data_size < offsetof(eip_forward_open_response_t, orig_to_targ_conn_id)) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                   "Forward Open response of %u bytes is too short to hold the CIP reply status at %d bytes!", conn->data_size,
                   (int)offsetof(eip_forward_open_response_t, orig_to_targ_conn_id));
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        if(le2h16(fo_resp->encap_command) != CIP_EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unexpected EIP packet type received: %d!", fo_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(fo_resp->encap_status) != CIP_EIP_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "EIP command failed, response code: %d", fo_resp->encap_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(fo_resp->general_status != CIP_EIP_OK) {
            uint8_t *status = &fo_resp->general_status;
            size_t status_size = cip_error_data_size(status, conn->data + conn->data_size);
            uint16_t extended_status = cip_extended_status_word(status, status_size, 0);

            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Forward Open failed: %s (0x%02x/0x%04x) -- %s",
                   decode_cip_error_short(status, status_size), fo_resp->general_status, extended_status,
                   decode_cip_error_long(status, status_size));

            rc = decode_cip_error_code(status, status_size);

            /* the error code alone cannot tell a missing route from a link that is down. */
            conn->fo_ext_status = extended_status;

            /*
             * A size rejection may name a size the PLC does support.  Only newer firmware
             * sends it, so zero here means "the PLC did not say" and the caller falls back
             * to its own list of sizes to try.  It is recorded and not acted on here: what
             * to do with it is the negotiation's decision, not the decoder's.
             */
            if(rc == PLCTAG_ERR_TOO_LARGE) {
                conn->fo_offered_size = cip_extended_status_word(status, status_size, 1);

                if(conn->fo_offered_size > 0) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "PLC reports that it supports a connection size of %u bytes.",
                           conn->fo_offered_size);
                }
            }

            break;
        }

        /* a success reply must carry the connection IDs and the rest of the fixed fields. */
        if((size_t)conn->data_size < sizeof(*fo_resp)) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                   "Successful Forward Open response of %u bytes is too short to hold the connection data of %d bytes!",
                   conn->data_size, (int)sizeof(*fo_resp));
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        /* success! */
        conn->targ_connection_id = le2h32(fo_resp->orig_to_targ_conn_id);
        conn->orig_connection_id = le2h32(fo_resp->targ_to_orig_conn_id);

        critical_block(conn->session_mutex) { conn->max_payload_size = conn->max_payload_guess; }

        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0,
               "ForwardOpen succeeded with our connection ID %x and the PLC connection ID %x with packet size %u.",
               conn->orig_connection_id, conn->targ_connection_id, conn->max_payload_size);

        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return rc;
}

int cip_decode_forward_close_response(cip_conn_p conn) {
    eip_forward_close_resp_t *fo_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    fo_resp = (eip_forward_close_resp_t *)(conn->data);

    do {
        /*
         * As in the Forward Open case, we only know we got an EIP header so far.  We read
         * no further than general_status here, so the CIP reply status prefix is enough.
         */
        if((size_t)conn->data_size < offsetof(eip_forward_close_resp_t, conn_serial_number)) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                   "Forward Close response of %u bytes is too short to hold the CIP reply status at %d bytes!",
                   conn->data_size, (int)offsetof(eip_forward_close_resp_t, conn_serial_number));
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        if(le2h16(fo_resp->encap_command) != CIP_EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unexpected EIP packet type received: %d!", fo_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(fo_resp->encap_status) != CIP_EIP_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "EIP command failed, response code: %d", fo_resp->encap_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(fo_resp->general_status != CIP_EIP_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Forward Close command failed, response code: %d",
                   fo_resp->general_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Connection close succeeded.");

        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return rc;
}


/*
 * ============================================================================
 *  Per-request framing
 *
 *  Everything above is the Connection Manager's own packets.  What follows wraps and
 *  unwraps an ordinary request: the EIP encapsulation and CPF framing that go around a
 *  CIP message, the bundling of several messages into one Multiple Service Packet, and
 *  the reverse on the way back.  Still no socket -- conn.c hands these a buffer.
 * ============================================================================
 */


/* bytes of EIP and CPF framing the transport puts ahead of the CIP message. */
static int cip_framing_size(cip_conn_p conn, bool unrouted) {
    if(unrouted) { return (int)sizeof(eip_cpf_uc_header); }

    return conn->use_connected_msg ? (int)sizeof(eip_cip_co_req) : (int)sizeof(eip_cip_uc_req);
}


/*
 * Lay the EIP encapsulation and CPF framing into the connection buffer around a
 * CIP message that already sits cip_framing_size() bytes in, and append the route
 * path for unconnected messaging.  The session handle and the sequence numbers are
 * left zeroed: prepare_request() fills those in afterwards, exactly as it does for
 * a packet a builder framed itself.
 */
static int write_request_framing(cip_conn_p conn, int payload_size, bool unrouted) {
    int offset = cip_framing_size(conn, unrouted);
    uint8_t *data = conn->data + offset + payload_size;

    /*
     * Clear the framing first.  The connection buffer is reused packet to packet, so
     * every field this function and prepare_request() do not write would otherwise keep
     * whatever the previous packet left there -- interface_handle, which must be zero,
     * picks up the session registration's protocol version, and a ControlLogix rejects
     * the packet at the encapsulation layer for it.  The framed builders never saw this
     * because they filled a freshly allocated, zeroed request buffer and pack_requests()
     * copied the whole thing over the stale bytes.
     */
    mem_set(conn->data, 0, offset);

    if(unrouted) {
        /*
         * Addressed to the device at the gateway, so the CPF carries the CIP message directly:
         * no Connection Manager wrapper to route it onward and no path for it to follow.
         */
        eip_cpf_uc_header *cip = (eip_cpf_uc_header *)(conn->data);

        cip->encap_command = h2le16(CIP_EIP_UNCONNECTED_SEND);
        cip->router_timeout = h2le16(1);

        cip->cpf_item_count = h2le16(2);
        cip->cpf_nai_item_type = h2le16(CIP_EIP_ITEM_NAI);
        cip->cpf_nai_item_length = h2le16(0);
        cip->cpf_udi_item_type = h2le16(CIP_EIP_ITEM_UDI);
        cip->cpf_udi_item_length = h2le16((uint16_t)payload_size);
    } else if(conn->use_connected_msg) {
        eip_cip_co_req *cip = (eip_cip_co_req *)(conn->data);

        cip->encap_command = h2le16(CIP_EIP_CONNECTED_SEND);

        /*
         * Zero on a connected send.  The field is the timeout a routing device applies while
         * it forwards a request, and a connected send is not forwarded -- it rides a
         * connection whose own timeout already bounds it.  This was 1, which hardware
         * tolerates because nothing consumes the field on this path.
         */
        cip->router_timeout = h2le16(0);

        cip->cpf_item_count = h2le16(2);
        cip->cpf_cai_item_type = h2le16(CIP_EIP_ITEM_CAI);
        cip->cpf_cai_item_length = h2le16(4);
        cip->cpf_cdi_item_type = h2le16(CIP_EIP_ITEM_CDI);

        /* the connected data item covers the sequence number as well as the CIP message. */
        cip->cpf_cdi_item_length = h2le16((uint16_t)(payload_size + (int)sizeof(cip->cpf_conn_seq_num)));
    } else {
        eip_cip_uc_req *cip = (eip_cip_uc_req *)(conn->data);

        /* CIP pads an odd-length embedded message to a word; the length field below does not count the pad. */
        if(payload_size & 1) {
            *data = 0;
            data++;
        }

        /* the route path to the target follows the embedded CIP message. */
        if(conn->conn_path_size > 0) {
            *data = (uint8_t)(conn->conn_path_size / 2); /* in 16-bit words */
            data++;
            *data = 0; /* reserved/pad */
            data++;
            mem_copy(data, conn->conn_path, (int)conn->conn_path_size);
            data += conn->conn_path_size;
        }

        cip->encap_command = h2le16(CIP_EIP_UNCONNECTED_SEND);
        cip->router_timeout = h2le16(1);

        cip->cpf_item_count = h2le16(2);
        cip->cpf_nai_item_type = h2le16(CIP_EIP_ITEM_NAI);
        cip->cpf_nai_item_length = h2le16(0);
        cip->cpf_udi_item_type = h2le16(CIP_EIP_ITEM_UDI);
        cip->cpf_udi_item_length = h2le16((uint16_t)(data - (uint8_t *)(&cip->cm_service_code)));

        cip->cm_service_code = CIP_EIP_CMD_UNCONNECTED_SEND;
        cip->cm_req_path_size = 2; /* in 16-bit words */
        cip->cm_req_path[0] = 0x20; /* class */
        cip->cm_req_path[1] = 0x06; /* Connection Manager */
        cip->cm_req_path[2] = 0x24; /* instance */
        cip->cm_req_path[3] = 0x01; /* instance 1 */

        /* the route timeout should match how long this end is actually willing to wait. */
        cip_encode_route_timeout(CIP_EIP_CONN_TIMEOUT_MS, &cip->secs_per_tick, &cip->timeout_ticks);

        cip->uc_cmd_length = h2le16((uint16_t)payload_size);
    }

    conn->data_size = (uint32_t)(data - conn->data);

    return PLCTAG_STATUS_OK;
}


/*
 * Assemble the exchange into the connection buffer.  The encoders wrote each request's CIP
 * message into conn->staging, so a single request is one copy and a bundle is the Multiple
 * Service Packet header followed by the messages back to back.
 *
 * Nothing here reads the slots' tags: one may have been aborted since the exchange was built,
 * and its bytes go out anyway.  The reply to it is dropped.
 */
int pack_exchange(cip_conn_p conn) {
    int num_requests = (int)conn->slot_count;
    bool unrouted = conn->exchange_unrouted;
    int offset = cip_framing_size(conn, unrouted);
    uint8_t *payload = conn->data + offset;
    int payload_size = 0;
    /* the route path, its size and reserved bytes, and the pad an odd-length message may need. */
    int route_path_size = (conn->use_connected_msg || unrouted) ? 0 : ((int)conn->conn_path_size + 3);
    size_t room = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting with %d request(s).", num_requests);

    /* everything the framing does not use is what the messages have to fit in. */
    if((size_t)(offset + route_path_size) > (size_t)conn->data_capacity) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
               "Framing of %d bytes does not fit the connection buffer of %u bytes!", offset + route_path_size,
               conn->data_capacity);
        return PLCTAG_ERR_TOO_LARGE;
    }

    room = (size_t)conn->data_capacity - (size_t)offset - (size_t)route_path_size;

    if(num_requests == 1) {
        if((size_t)conn->slots[0].size > room) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                   "CIP message of %d bytes does not fit the %zu bytes left in the connection buffer!",
                   (int)conn->slots[0].size, room);
            return PLCTAG_ERR_TOO_LARGE;
        }

        mem_copy(payload, conn->staging + conn->slots[0].offset, (int)conn->slots[0].size);
        payload_size = (int)conn->slots[0].size;
    } else {
        cip_multi_req_header *multi = (cip_multi_req_header *)payload;
        size_t header_size = sizeof(cip_multi_req_header) + (sizeof(uint16_le) * (size_t)num_requests);
        int current_offset = (int)(sizeof(uint16_le) + (sizeof(uint16_le) * (size_t)num_requests));
        uint8_t *next = payload + header_size;

        if(header_size > room) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                   "Bundled request header does not fit the connection buffer!");
            return PLCTAG_ERR_TOO_LARGE;
        }

        multi->service_code = CIP_EIP_CMD_CIP_MULTI;
        multi->req_path_size = 0x02; /* length of path in words */
        multi->req_path[0] = 0x20;   /* Class */
        multi->req_path[1] = 0x02;   /* CM */
        multi->req_path[2] = 0x24;   /* Instance */
        multi->req_path[3] = 0x01;   /* #1 */
        multi->request_count = h2le16((uint16_t)num_requests);

        for(int i = 0; i < num_requests; i++) {
            size_t used = (size_t)(next - payload);

            if(conn->slots[i].size < 0 || (size_t)conn->slots[i].size > room - used) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                       "Bundled requests do not fit the connection buffer of %u bytes!", conn->data_capacity);
                return PLCTAG_ERR_TOO_LARGE;
            }

            multi->request_offsets[i] = h2le16((uint16_t)current_offset);

            mem_copy(next, conn->staging + conn->slots[i].offset, (int)conn->slots[i].size);

            next += conn->slots[i].size;
            current_offset += (int)conn->slots[i].size;
        }

        payload_size = (int)(next - payload);
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "CIP message is %d bytes.", payload_size);

    return write_request_framing(conn, payload_size, unrouted);
}




/*
 * Check the CPF layer of a received packet.  The framed path leaves this to the
 * tag modules, because they are the ones that can still see the framing; once the
 * transport strips it, the transport has to be the one that validates it.
 */
static int validate_response_cpf(cip_conn_p conn) {
    if(le2h16(((eip_encap *)(conn->data))->encap_command) == CIP_EIP_CONNECTED_SEND) {
        eip_cip_co_resp *resp = (eip_cip_co_resp *)(conn->data);
        size_t data_item_start = 0;
        size_t data_item_length = 0;

        if((size_t)conn->data_size < sizeof(eip_cip_co_resp)) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Connected response of %u bytes is too short for a CIP response!",
                   conn->data_size);
            return PLCTAG_ERR_TOO_SMALL;
        }

        if(le2h16(resp->cpf_item_count) != 2) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Connected response has %u CPF items, expected 2!",
                   le2h16(resp->cpf_item_count));
            return PLCTAG_ERR_BAD_DATA;
        }

        if(le2h16(resp->cpf_cai_item_type) != CIP_EIP_ITEM_CAI || le2h16(resp->cpf_cdi_item_type) != CIP_EIP_ITEM_CDI) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                   "Connected response CPF item types are %04" PRIx16 "/%04" PRIx16 ", expected %04" PRIx16 "/%04" PRIx16 "!",
                   le2h16(resp->cpf_cai_item_type), le2h16(resp->cpf_cdi_item_type), CIP_EIP_ITEM_CAI, CIP_EIP_ITEM_CDI);
            return PLCTAG_ERR_BAD_DATA;
        }

        /*
         * Only meaningful once ForwardOpen has negotiated a connection.  Until then we send
         * connection ID zero and the target echoes zero back, so there is nothing to check.
         */
        if(conn->targ_connection_id != 0 && le2h32(resp->cpf_orig_conn_id) != conn->orig_connection_id) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Connected response is for connection %" PRIx32 " but ours is %" PRIx32 "!",
                   le2h32(resp->cpf_orig_conn_id), conn->orig_connection_id);
            return PLCTAG_ERR_BAD_DATA;
        }

        /*
         * Require the data item length to match what we received rather than merely fit,
         * otherwise the PLC can shorten the item and leave us reading bytes it never sent.
         */
        data_item_start = (size_t)((uint8_t *)(&resp->cpf_conn_seq_num) - conn->data);
        data_item_length = (size_t)le2h16(resp->cpf_cdi_item_length);

        if(data_item_start + data_item_length != (size_t)conn->data_size) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                   "Connected data item claims %zu bytes but the response is %u bytes with the item starting at %zu!",
                   data_item_length, conn->data_size, data_item_start);
            return PLCTAG_ERR_BAD_DATA;
        }
    } else {
        eip_cip_uc_resp *resp = (eip_cip_uc_resp *)(conn->data);
        size_t data_item_start = 0;
        size_t data_item_length = 0;

        if((size_t)conn->data_size < sizeof(eip_cip_uc_resp)) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unconnected response of %u bytes is too short for a CIP response!",
                   conn->data_size);
            return PLCTAG_ERR_TOO_SMALL;
        }

        if(le2h16(resp->cpf_item_count) != 2) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unconnected response has %u CPF items, expected 2!",
                   le2h16(resp->cpf_item_count));
            return PLCTAG_ERR_BAD_DATA;
        }

        if(le2h16(resp->cpf_nai_item_type) != CIP_EIP_ITEM_NAI || le2h16(resp->cpf_udi_item_type) != CIP_EIP_ITEM_UDI) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                   "Unconnected response CPF item types are %04" PRIx16 "/%04" PRIx16 ", expected %04" PRIx16 "/%04" PRIx16 "!",
                   le2h16(resp->cpf_nai_item_type), le2h16(resp->cpf_udi_item_type), CIP_EIP_ITEM_NAI, CIP_EIP_ITEM_UDI);
            return PLCTAG_ERR_BAD_DATA;
        }

        data_item_start = (size_t)((uint8_t *)(&resp->reply_service) - conn->data);
        data_item_length = (size_t)le2h16(resp->cpf_udi_item_length);

        if(data_item_start + data_item_length != (size_t)conn->data_size) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                   "Unconnected data item claims %zu bytes but the response is %u bytes with the item starting at %zu!",
                   data_item_length, conn->data_size, data_item_start);
            return PLCTAG_ERR_BAD_DATA;
        }
    }

    return PLCTAG_STATUS_OK;
}


/*
 * Find the CIP reply to request sub_packet of the exchange, with the EIP and CPF framing
 * stripped.  The reply stays in the connection buffer; it is valid until the next exchange.
 */
int find_reply(cip_conn_p conn, int32_t sub_packet, uint8_t **reply, int32_t *reply_size) {
    int rc = PLCTAG_STATUS_OK;
    eip_encap *header = (eip_encap *)(conn->data);
    size_t resp_offset = 0;
    uint8_t *resp = NULL;
    int resp_size = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    /* the tag never sees the framing, so check it here before throwing it away. */
    rc = validate_response_cpf(conn);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    /* trust the reply's own type rather than the connection's, as recv checked it. */
    resp_offset = (le2h16(header->encap_command) == CIP_EIP_CONNECTED_SEND)
                      ? offsetof(eip_cip_co_resp, reply_service)
                      : offsetof(eip_cip_uc_resp, reply_service);

    if(resp_offset > (size_t)conn->data_size) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Response of %u bytes is shorter than its own framing!",
               conn->data_size);
        return PLCTAG_ERR_TOO_SMALL;
    }

    resp = conn->data + resp_offset;
    resp_size = (int)((size_t)conn->data_size - resp_offset);

    if(resp_size > 0 && resp[0] == (uint8_t)(CIP_EIP_CMD_CIP_MULTI | CIP_EIP_CMD_CIP_OK)) {
        cip_multi_resp_header *multi = (cip_multi_resp_header *)resp;
        uint16_t total_responses = 0;
        size_t offsets_start = 0;
        size_t offsets_size = 0;
        uint8_t *pkt_start = NULL;
        uint8_t *pkt_end = NULL;
        uint8_t *buf_end = conn->data + conn->data_size;

        if((size_t)resp_size < sizeof(cip_multi_resp_header)) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Packed response is too short to hold its own header!");
            return PLCTAG_ERR_TOO_SMALL;
        }

        total_responses = le2h16(multi->request_count);

        /*
         * The count and the offsets all come from the wire.  Check that the offset array
         * itself is inside the data we received BEFORE reading any offset out of it.
         */
        offsets_start = resp_offset + offsetof(cip_multi_resp_header, request_offsets);
        offsets_size = (size_t)total_responses * sizeof(uint16_le);

        if(sub_packet < 0 || sub_packet >= (int32_t)total_responses || offsets_start > (size_t)conn->data_size
           || offsets_size > (size_t)conn->data_size - offsets_start) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                   "Packed response sub-packet %d is out of bounds of the received data!", sub_packet);
            return PLCTAG_ERR_OUT_OF_BOUNDS;
        }

        /* offsets are counted from the request count field. */
        pkt_start = (uint8_t *)(&multi->request_count) + le2h16(multi->request_offsets[sub_packet]);

        if((sub_packet + 1) < (int)total_responses) {
            pkt_end = (uint8_t *)(&multi->request_count) + le2h16(multi->request_offsets[sub_packet + 1]);
        } else {
            pkt_end = buf_end;
        }

        /* comparing the pointers directly would be undefined, so compare the values. */
        if((intptr_t)pkt_start < (intptr_t)(&multi->request_count) || (intptr_t)pkt_start > (intptr_t)buf_end
           || (intptr_t)pkt_end < (intptr_t)pkt_start || (intptr_t)pkt_end > (intptr_t)buf_end) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                   "Packed response sub-packet %d has an out of bounds data range!", sub_packet);
            return PLCTAG_ERR_OUT_OF_BOUNDS;
        }

        resp = pkt_start;
        resp_size = (int)(pkt_end - pkt_start);

        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Sub-packet %d of %d is %d bytes.", sub_packet,
               (int)total_responses, resp_size);
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Single response of %d bytes.", resp_size);
    }

    *reply = resp;
    *reply_size = (int32_t)resp_size;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "CIP response:");
    pdebug_dump_bytes(DEBUG_MODULE_CIP, DEBUG_INFO, 0, resp, resp_size);

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * Get_Attributes_All on the Identity object, unconnected.  A path wraps it in an Unconnected
 * Send to the CPU; without one it goes to the device at the gateway.
 */
int cip_encode_identity_request(cip_conn_p conn) {
    bool unrouted = (conn->conn_path_size == 0);
    uint8_t *payload = conn->data + cip_framing_size(conn, unrouted);
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    payload[0] = CIP_GET_ATTRIBUTES_ALL;
    payload[1] = 0x02; /* path size in 16-bit words */
    payload[2] = 0x20; /* class segment */
    payload[3] = 0x01; /* Identity */
    payload[4] = 0x24; /* instance segment */
    payload[5] = 0x01; /* instance 1 */

    rc = write_request_framing(conn, 6, unrouted);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return rc;
}


int cip_decode_identity_response(cip_conn_p conn, uint16_t *vendor_id, char *product_name, size_t name_capacity) {
    uint8_t *buf_end = conn->data + conn->data_size;
    uint8_t *reply = conn->data + offsetof(eip_cip_uc_resp, reply_service);
    uint8_t *data = NULL;
    size_t name_len = 0;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    rc = validate_response_cpf(conn);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    /* a routing failure comes back as the Unconnected Send's own reply. */
    if(reply[0] != (uint8_t)(CIP_GET_ATTRIBUTES_ALL | CIP_SVC_REPLY) || reply[2] != CIP_STATUS_OK) {
        size_t status_size = cip_error_data_size(reply + 2, buf_end);

        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Identity request failed, service 0x%02x: %s", (unsigned int)reply[0],
               decode_cip_error_long(reply + 2, status_size));

        return (reply[2] == CIP_STATUS_OK) ? PLCTAG_ERR_BAD_REPLY : decode_cip_error_code(reply + 2, status_size);
    }

    /*
     * The four-byte reply header and its status words, then vendor (2), device type (2),
     * product code (2), revision (2), status (2), serial number (4) and the product name as
     * a SHORT_STRING.
     */
    data = reply + 4 + ((size_t)reply[3] * 2);

    if(cip_error_data_size(data, buf_end) < 15) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Identity reply is too short!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    *vendor_id = (uint16_t)(data[0] | (data[1] << 8));
    name_len = data[14];

    if(cip_error_data_size(data + 15, buf_end) < name_len || name_len >= name_capacity) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Identity product name of %zu bytes does not fit!", name_len);
        return PLCTAG_ERR_TOO_SMALL;
    }

    mem_copy(product_name, data + 15, (int)name_len);
    product_name[name_len] = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Device is vendor %u, \"%s\".", (unsigned int)*vendor_id, product_name);

    return PLCTAG_STATUS_OK;
}
