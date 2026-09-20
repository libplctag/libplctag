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

#include <libplctag/protocols/cip/conn.h>

#include <inttypes.h>
#include <limits.h>
#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/cip/defs.h>
#include <libplctag/protocols/cip/request.h>
#include <utils/rc.h>
#include <libplctag/protocols/cip/error_codes.h>
#include <libplctag/protocols/eip/defs.h>
#include <utils/byteorder.h>
#include <utils/mem.h>
#include <utils/debug.h>
#include <utils/mutex.h>
#include <utils/poller.h>
#include <utils/socket_fd.h>


extern uint64_t cip_conn_get_new_seq_id(cip_conn_p conn) {
    uint16_t res = 0;

    critical_block(conn->mutex) {
        /* check for rollover.  zero is never handed out. */
        if((++conn->conn_seq_id) == 0) { conn->conn_seq_id = 1; }

        res = (uint16_t)conn->conn_seq_id;
    }

    return res;
}

/*********************************************************************
 ** Socket I/O
 **
 ** This was socket_read()/socket_write()/socket_connect_tcp_check() in
 ** utils/socket.c, which hid a select() of the caller's timeout inside every
 ** transfer.  That model gives each socket its own thread and its own wake
 ** channel.  Here the waiting is a poller the connection owns and the
 ** transfers never sleep, which is the L1 adapter step 4 of
 ** docs/socket_layering_design.md asks for -- with one socket in the poller
 ** today because there is one handler thread per connection.
 *********************************************************************/

/*
 * Waits for any of `interest` on this connection's socket.  Returns the event
 * mask, POLLER_EVENT_TIMEOUT if nothing happened, or POLLER_EVENT_ERROR if the
 * poller itself failed.
 */
static int32_t conn_wait_events(cip_conn_p conn, int32_t interest, int32_t timeout_ms) {
    poller_event_t events[2];
    int32_t event_count = 0;
    int32_t result = POLLER_EVENT_NONE;
    int32_t index = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    if(!conn->poller || !conn->sock_registered) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Socket is not registered with a poller!");
        return POLLER_EVENT_ERROR;
    }

    rc = poller_modify(conn->poller, conn->sock, interest);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to set poller interest, error %s!", plc_tag_decode_error(rc));
        return POLLER_EVENT_ERROR;
    }

    rc = poller_wait(conn->poller, events, 2, timeout_ms, &event_count);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Poller wait failed, error %s!", plc_tag_decode_error(rc));
        return POLLER_EVENT_ERROR;
    }

    if(event_count == 0) { return POLLER_EVENT_TIMEOUT; }

    for(index = 0; index < event_count; index++) { result |= events[index].events; }

    return result;
}


extern int32_t cip_conn_socket_open(cip_conn_p conn, const char *host, int32_t port) {
    int32_t rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    /* a reconnect comes back through here, so start from a clean pair. */
    cip_conn_socket_close(conn);

    rc = poller_create(&(conn->poller), 1);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to create poller, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = socket_fd_open_tcp(&(conn->sock));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to open socket, error %s!", plc_tag_decode_error(rc));
        cip_conn_socket_close(conn);
        return rc;
    }

    rc = socket_fd_connect_start(conn->sock, host, port);
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to start connection to %s:%" PRId32 ", error %s!", host, port,
               plc_tag_decode_error(rc));
        cip_conn_socket_close(conn);
        return rc;
    }

    /*
     * Registered whether or not the connect completed immediately: the caller
     * goes straight to reading and writing in that case, and both need the
     * socket in the poller already.
     */
    {
        int32_t add_rc = poller_add(conn->poller, conn->sock, POLLER_EVENT_CONNECT, conn);

        if(add_rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to register the socket with the poller, error %s!",
                   plc_tag_decode_error(add_rc));
            cip_conn_socket_close(conn);
            return add_rc;
        }

        conn->sock_registered = true;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    /* PLCTAG_STATUS_OK if the connect finished already, PENDING if it is still running. */
    return rc;
}


extern int32_t cip_conn_socket_close(cip_conn_p conn) {
    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Starting.");

    if(conn->sock_registered) {
        poller_remove(conn->poller, conn->sock);
        conn->sock_registered = false;
    }

    socket_fd_close(&(conn->sock));

    if(conn->poller) { poller_destroy(&(conn->poller)); }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


extern int32_t cip_conn_connect_check(cip_conn_p conn, int32_t timeout_ms) {
    int32_t events = conn_wait_events(conn, POLLER_EVENT_CONNECT, timeout_ms);

    if(events == POLLER_EVENT_TIMEOUT) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Connection is not done yet.");
        return PLCTAG_ERR_TIMEOUT;
    }

    if(events & POLLER_EVENT_ERROR) {
        int32_t sock_rc = socket_fd_get_error(conn->sock);

        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Connection failed, error %s!", plc_tag_decode_error(sock_rc));

        return (sock_rc == PLCTAG_STATUS_OK) ? PLCTAG_ERR_OPEN : sock_rc;
    }

    /*
     * A socket in a non-blocking connect goes writable when the attempt
     * resolves, either way.  SO_ERROR is what says which, so a writable socket
     * is not by itself good news.
     */
    if(events & (POLLER_EVENT_CONNECT | POLLER_EVENT_CAN_WRITE | POLLER_EVENT_DISCONNECT)) {
        return socket_fd_connect_check(conn->sock);
    }

    return PLCTAG_ERR_TIMEOUT;
}


/*
 * Wait for `interest`, then transfer.  Shared by send and recv because
 * everything but the direction and the two calls at the end is the same.
 */
static int32_t conn_transfer(cip_conn_p conn, uint8_t *buf, int32_t len, int32_t *count, int32_t timeout_ms, bool sending) {
    int32_t events = POLLER_EVENT_NONE;
    int32_t rc = PLCTAG_STATUS_OK;

    *count = 0;

    events = conn_wait_events(conn, sending ? POLLER_EVENT_CAN_WRITE : POLLER_EVENT_CAN_READ, timeout_ms);

    if(events == POLLER_EVENT_TIMEOUT) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_SPEW, 0, "Socket is not ready yet.");
        return PLCTAG_STATUS_OK;
    }

    if(events & POLLER_EVENT_ERROR) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Socket error while waiting!");
        return sending ? PLCTAG_ERR_WRITE : PLCTAG_ERR_READ;
    }

    /*
     * A disconnect is reported alongside whatever is still readable, so a read
     * runs anyway and finds the peer's FIN as a zero-byte result below.  There
     * is nothing to do with a half-closed socket on the write side.
     */
    if(sending && (events & POLLER_EVENT_DISCONNECT) && !(events & POLLER_EVENT_CAN_WRITE)) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "The PLC closed the connection!");
        return PLCTAG_ERR_BAD_CONNECTION;
    }

    if(sending) {
        rc = socket_fd_send(conn->sock, buf, len, count);
    } else {
        rc = socket_fd_recv(conn->sock, buf, len, count);
    }

    if(rc == PLCTAG_STATUS_PENDING) {
        /* the readiness was stale.  Nothing moved; the caller tries again. */
        *count = 0;
        return PLCTAG_STATUS_OK;
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error, %s, on socket %s!", plc_tag_decode_error(rc),
               sending ? "write" : "read");
        return rc;
    }

    /*
     * Zero bytes read with no error is the peer's FIN.  The old socket_read()
     * could not say this -- it returned zero both for would-block and for a
     * closed connection -- so a PLC that hung up looked like a PLC that was
     * merely quiet, and the caller polled it until its own timeout expired.
     */
    if(!sending && *count == 0) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "The PLC closed the connection!");
        return PLCTAG_ERR_BAD_CONNECTION;
    }

    return PLCTAG_STATUS_OK;
}


extern int32_t cip_conn_send(cip_conn_p conn, const uint8_t *buf, int32_t len, int32_t *count, int32_t timeout_ms) {
    /* the cast is safe: conn_transfer() only reads the buffer when sending. */
    return conn_transfer(conn, (uint8_t *)(uintptr_t)buf, len, count, timeout_ms, true);
}


extern int32_t cip_conn_recv(cip_conn_p conn, uint8_t *buf, int32_t len, int32_t *count, int32_t timeout_ms) {
    return conn_transfer(conn, buf, len, count, timeout_ms, false);
}


/*********************************************************************
 ** Forward Open
 **
 ** These were the same four functions in ab/session.c and omron/conn.c.  What
 ** differed: the field names (renamed in 2.33), AB's DH+ parameter override (now
 ** cip_plc_config_t.conn_params_override), the module's own send call (now a
 ** callback), and two places where AB held the mutex and Omron did not.  AB's
 ** locking is kept -- max_payload_guess is read by the request builders.
 *********************************************************************/

/*
 * The next connection serial number, skipping zero.  A Forward Open uses this to
 * distinguish one connection attempt from another, so a repeat of zero could be
 * taken for a duplicate.
 */
static uint16_t next_conn_serial_number(uint16_t current) {
    uint16_t next = (uint16_t)(current + 1);

    if(next == 0) { next = 1; }

    return next;
}


static int send_old_forward_open(cip_conn_p conn, const cip_conn_io_t *io) {
    eip_forward_open_request_t *fo = NULL;
    uint8_t *data;
    int rc = PLCTAG_STATUS_OK;

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
    fo->encap_command = h2le16(EIP_UNCONNECTED_SEND); /* 0x006F EIP Send RR Data command */
    fo->encap_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->interface_handle))); /* total length of packet except for encap header */
    fo->encap_session_handle = h2le32(conn->conn_handle);
    fo->encap_sender_context = h2le64(++conn->conn_seq_id);
    fo->router_timeout = h2le16(1); /* one second is enough ? */

    /* CPF parts */
    fo->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    fo->cpf_nai_item_type = h2le16(EIP_ITEM_NAI); /* null address item type */
    fo->cpf_nai_item_length = h2le16(0);             /* no data, zero length */
    fo->cpf_udi_item_type = h2le16(EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fo->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->cm_service_code))); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fo->cm_service_code = CIP_CMD_FORWARD_OPEN; /* 0x54 Forward Open Request or 0x5B for Forward Open Extended */
    fo->cm_req_path_size = 2;                      /* size of path in 16-bit words */
    fo->cm_req_path[0] = 0x20;                     /* class */
    fo->cm_req_path[1] = 0x06;                     /* CM class */
    fo->cm_req_path[2] = 0x24;                     /* instance */
    fo->cm_req_path[3] = 0x01;                     /* instance 1 */

    /* Forward Open Params */
    fo->secs_per_tick = CIP_SECS_PER_TICK; /* seconds per tick, no used? */
    fo->timeout_ticks = CIP_TIMEOUT_TICKS; /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
    fo->orig_to_targ_conn_id = h2le32(0);     /* is this right?  Our connection id on the other machines? */
    fo->targ_to_orig_conn_id = h2le32(conn->orig_connection_id); /* Our connection id in the other direction. */
    /* this might need to be globally unique */
    conn->conn_serial_number = next_conn_serial_number(conn->conn_serial_number);
    fo->conn_serial_number = h2le16(conn->conn_serial_number); /* our connection SEQUENCE number. */
    fo->orig_vendor_id = h2le16(CIP_VENDOR_ID);                /* our unique :-) vendor ID */
    fo->orig_serial_number = h2le32(CIP_VENDOR_SN);            /* our serial number. */
    fo->conn_timeout_multiplier = CIP_TIMEOUT_MULTIPLIER;      /* timeout = mult * RPI */

    fo->orig_to_targ_rpi = h2le32(CIP_RPI); /* us to target RPI - Request Packet Interval in microseconds */

    /* a PCCC PLC reached over DH+ needs a fixed parameter word; see cip_plc_config_t. */
    fo->orig_to_targ_conn_params =
        h2le16(conn->plc_config.conn_params_override ? conn->plc_config.conn_params_override
                                                     : (uint16_t)(CIP_CONN_PARAM | conn->max_payload_guess));

    fo->targ_to_orig_rpi = h2le32(CIP_RPI); /* target to us RPI - not really used for explicit messages? */

    /* a PCCC PLC reached over DH+ needs a fixed parameter word; see cip_plc_config_t. */
    fo->targ_to_orig_conn_params =
        h2le16(conn->plc_config.conn_params_override ? conn->plc_config.conn_params_override
                                                     : (uint16_t)(CIP_CONN_PARAM | conn->max_payload_guess));

    fo->transport_class = CIP_TRANSPORT_CLASS_T3; /* 0xA3, server transport, class 3, application trigger */
    fo->path_size = conn->conn_path_size / 2;     /* size in 16-bit words */

    /* set the size of the request */
    conn->data_size = (uint32_t)(data - (conn->data));

    rc = io->send_request(conn, 0);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done");

    return rc;
}


static int send_extended_forward_open(cip_conn_p conn, const cip_conn_io_t *io) {
    eip_forward_open_request_ex_t *fo = NULL;
    uint8_t *data;
    int rc = PLCTAG_STATUS_OK;

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
    fo->encap_command = h2le16(EIP_UNCONNECTED_SEND); /* 0x006F EIP Send RR Data command */
    fo->encap_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->interface_handle))); /* total length of packet except for encap header */
    fo->encap_session_handle = h2le32(conn->conn_handle);
    fo->encap_sender_context = h2le64(++conn->conn_seq_id);
    fo->router_timeout = h2le16(1); /* one second is enough ? */

    /* CPF parts */
    fo->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    fo->cpf_nai_item_type = h2le16(EIP_ITEM_NAI); /* null address item type */
    fo->cpf_nai_item_length = h2le16(0);             /* no data, zero length */
    fo->cpf_udi_item_type = h2le16(EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fo->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->cm_service_code))); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fo->cm_service_code = CIP_CMD_FORWARD_OPEN_EX; /* 0x54 Forward Open Request or 0x5B for Forward Open Extended */
    fo->cm_req_path_size = 2;                         /* size of path in 16-bit words */
    fo->cm_req_path[0] = 0x20;                        /* class */
    fo->cm_req_path[1] = 0x06;                        /* CM class */
    fo->cm_req_path[2] = 0x24;                        /* instance */
    fo->cm_req_path[3] = 0x01;                        /* instance 1 */

    /* Forward Open Params */
    fo->secs_per_tick = CIP_SECS_PER_TICK; /* seconds per tick, no used? */
    fo->timeout_ticks = CIP_TIMEOUT_TICKS; /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
    fo->orig_to_targ_conn_id = h2le32(0);     /* is this right?  Our connection id on the other machines? */
    fo->targ_to_orig_conn_id = h2le32(conn->orig_connection_id); /* Our connection id in the other direction. */
    /* this might need to be globally unique */
    conn->conn_serial_number = next_conn_serial_number(conn->conn_serial_number);
    fo->conn_serial_number = h2le16(conn->conn_serial_number); /* our connection ID/serial number. */
    fo->orig_vendor_id = h2le16(CIP_VENDOR_ID);                /* our unique :-) vendor ID */
    fo->orig_serial_number = h2le32(CIP_VENDOR_SN);            /* our serial number. */
    fo->conn_timeout_multiplier = CIP_TIMEOUT_MULTIPLIER;      /* timeout = mult * RPI */
    fo->orig_to_targ_rpi = h2le32(CIP_RPI);                    /* us to target RPI - Request Packet Interval in microseconds */
    fo->orig_to_targ_conn_params_ex = h2le32(
        CIP_CONN_PARAM_EX | conn->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    fo->targ_to_orig_rpi = h2le32(CIP_RPI);              /* target to us RPI - not really used for explicit messages? */
    fo->targ_to_orig_conn_params_ex = h2le32(
        CIP_CONN_PARAM_EX | conn->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    fo->transport_class = CIP_TRANSPORT_CLASS_T3;        /* 0xA3, server transport, class 3, application trigger */
    fo->path_size = conn->conn_path_size / 2;            /* size in 16-bit words */

    /* set the size of the request */
    conn->data_size = (uint32_t)(data - (conn->data));

    rc = io->send_request(conn, CIP_CONN_DEFAULT_TIMEOUT);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done");

    return rc;
}


extern int cip_send_forward_open(cip_conn_p conn, const cip_conn_io_t *io) {
    int rc = PLCTAG_STATUS_OK;
    uint16_t max_payload;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Flag prohibiting use of extended ForwardOpen is %d.",
           conn->only_use_old_forward_open);

    max_payload = (uint16_t)(conn->only_use_old_forward_open ? conn->plc_config.fo_conn_size : conn->plc_config.fo_ex_conn_size);

    /* set the max payload guess if it is larger than the maximum possible or if it is zero. */
    critical_block(conn->mutex) {
        conn->max_payload_guess =
            ((conn->max_payload_guess == 0) || (conn->max_payload_guess > max_payload) ? max_payload :
                                                                                               conn->max_payload_guess);
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Set Forward Open maximum payload size guess to %d bytes.",
           conn->max_payload_guess);

    if(conn->only_use_old_forward_open) {
        rc = send_old_forward_open(conn, io);
    } else {
        rc = send_extended_forward_open(conn, io);
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done");

    return rc;
}


extern int cip_receive_forward_open_response(cip_conn_p conn, const cip_conn_io_t *io) {
    eip_forward_open_response_t *fo_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    rc = io->recv_response(conn, 0);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to receive Forward Open response.");
        return rc;
    }

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

        if(le2h16(fo_resp->encap_command) != EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unexpected EIP packet type received: %" PRIu16 "!",
                   le2h16(fo_resp->encap_command));
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(fo_resp->encap_status) != EIP_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "EIP command failed, response code: %" PRIu32, le2h32(fo_resp->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(fo_resp->general_status != EIP_OK) {
            size_t general_status_size = cip_error_data_size(&fo_resp->general_status, conn->data + conn->data_size);

            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Forward Open command failed, response code: %s (%d)",
                   decode_cip_error_short(&fo_resp->general_status, general_status_size), fo_resp->general_status);
            if(fo_resp->general_status == CIP_ERR_UNSUPPORTED_SERVICE) {
                /* this type of command is not supported! */
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Received CIP command unsupported error from the PLC!");
                rc = PLCTAG_ERR_UNSUPPORTED;
            } else {
                rc = PLCTAG_ERR_REMOTE_ERR;

                /* comparing pointers directly is UB, so compare the integer values instead. */
                if(fo_resp->general_status == 0x01 && fo_resp->status_size >= 2
                   && (intptr_t)(&fo_resp->status_size + 5) <= (intptr_t)(conn->data + conn->data_size)) {
                    /* we might have an error that tells us the actual size to use. */
                    uint8_t *data = &fo_resp->status_size;
                    int extended_status = data[1] | (data[2] << 8);
                    uint16_t supported_size = (uint16_t)((uint16_t)data[3] | (uint16_t)((uint16_t)data[4] << (uint16_t)8));

                    if(extended_status == 0x109) { /* MAGIC */
                        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                               "Error from forward open request, unsupported size, but size %d is supported.", supported_size);

                        /*
                         * The PLC is telling us the size we asked for is unsupported and offering a size it
                         * does support.  That offered size must not exceed what we asked for -- conn->data
                         * was allocated based on our request, and a PLC claiming to "support" a larger size
                         * than we asked for is a protocol disagreement, not a legitimate response.
                         */
                        if(supported_size < conn->plc_config.min_payload_size) {
                            /*
                             * There is a floor as well as a ceiling.  Every protocol family has a
                             * fixed per-request overhead, and a payload below that leaves no room
                             * for a request at all -- the size arithmetic downstream then has an
                             * overhead larger than the space, which is where the underflows live.
                             * A PLC offering less than we can use is not a size we can negotiate to.
                             */
                            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                                   "PLC reported a supported size of %u, below the %d bytes this protocol needs for a "
                                   "single request!",
                                   supported_size, conn->plc_config.min_payload_size);
                            rc = PLCTAG_ERR_TOO_SMALL;
                        } else if(supported_size <= conn->max_payload_guess) {
                            critical_block(conn->mutex) { conn->max_payload_guess = supported_size; }
                            rc = PLCTAG_ERR_TOO_LARGE;
                        } else {
                            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                                   "PLC reported a supported size, %u, larger than what we requested, %u! This is a "
                                   "protocol disagreement and may indicate a malicious or misbehaving PLC; aborting.",
                                   supported_size, conn->max_payload_guess);
                            rc = PLCTAG_ERR_BAD_DATA;
                        }
                    } else if(extended_status == 0x100) { /* MAGIC */
                        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                               "Error from forward open request, duplicate connection ID.  Need to try again.");
                        rc = PLCTAG_ERR_DUPLICATE;
                    } else {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "CIP extended error %s (%s)!",
                               decode_cip_error_short(&fo_resp->general_status, general_status_size),
                               decode_cip_error_long(&fo_resp->general_status, general_status_size));
                    }
                } else {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "CIP error code %s (%s)!",
                           decode_cip_error_short(&fo_resp->general_status, general_status_size),
                           decode_cip_error_long(&fo_resp->general_status, general_status_size));
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

        /*
         * success!  session_create_request() reads max_payload_size (via
         * GET_MAX_PAYLOAD_SIZE) under mutex, and the connection IDs are read by
         * the request builders, so all three are committed together under that lock.
         */
        critical_block(conn->mutex) {
            conn->targ_connection_id = le2h32(fo_resp->orig_to_targ_conn_id);
            conn->orig_connection_id = le2h32(fo_resp->targ_to_orig_conn_id);
            conn->max_payload_size = conn->max_payload_guess;
        }

        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0,
               "ForwardOpen succeeded with our connection ID %x and the PLC connection ID %x with packet size %u.",
               conn->orig_connection_id, conn->targ_connection_id, conn->max_payload_size);

        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return rc;
}

/*********************************************************************
 ** Forward Close, response unpacking, and payload accounting
 **
 ** perform_forward_close and send_forward_close_req were character-identical
 ** between the two modules; recv_forward_close_resp and unpack_response differed
 ** only in line wrapping.
 *********************************************************************/

/* the body of the payload calculation; the caller holds conn->mutex. */
static int available_payload_unsafe(cip_conn_p conn) {
    int result = GET_MAX_PAYLOAD_SIZE(conn);

    /* account for the CPF data item that wraps the payload. */
    if(conn->use_connected_msg) {
        result -= (int)sizeof(cpf_connected_data_item);
    } else {
        result -= (int)sizeof(cpf_unconnected_data_item);
        result -= (int)(conn->conn_path_size) + 2; /* encoded path size plus two bytes for length and padding */
    }

    return (result < 0) ? 0 : result;
}


extern int cip_conn_get_available_payload_space(cip_conn_p conn) {
    int result = 0;

    if(!conn) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Called with null connection pointer!");
        return 0;
    }

    critical_block(conn->mutex) { result = available_payload_unsafe(conn); }

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Available payload space is %d bytes.", result);

    return result;
}


/*
 * Allocate a request and its buffer.
 *
 * The capacity is the available payload plus EIP_CIP_PREFIX_SIZE, which is exactly
 * the worst case with no slack: the builders measure a request's payload from
 * cpf_conn_seq_num onwards, so prefix plus payload is capacity by construction.
 * Omron used to allocate max_payload_size rather than the available payload, which
 * over-allocated by the size of the CPF data item.
 */
extern int cip_conn_create_request(cip_conn_p conn, int tag_id, cip_request_p *req) {
    int rc = PLCTAG_STATUS_OK;
    cip_request_p res = NULL;
    size_t request_capacity = 0;
    uint8_t *buffer = NULL;

    critical_block(conn->mutex) { request_capacity = (size_t)(available_payload_unsafe(conn) + EIP_CIP_PREFIX_SIZE); }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Starting.");

    buffer = (uint8_t *)mem_alloc((int)request_capacity);

    if(!buffer) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to allocate request buffer!");
        *req = NULL;
        return PLCTAG_ERR_NO_MEM;
    }

    res = (cip_request_p)rc_alloc((int)sizeof(struct cip_request_t), cip_request_destroy);

    if(!res) {
        mem_free(buffer);
        *req = NULL;
        rc = PLCTAG_ERR_NO_MEM;
    } else {
        res->data = buffer;
        res->tag_id = tag_id;
        res->request_capacity = (int)request_capacity;
        atomic_init_bool(&res->lock, false);

        *req = res;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


extern int cip_unpack_response(cip_conn_p conn, cip_request_p request, int sub_packet) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp *packed_resp = (eip_cip_co_resp *)(conn->data);
    eip_cip_co_resp *unpacked_resp = NULL;
    uint8_t *pkt_start = NULL;
    uint8_t *pkt_end = NULL;
    int new_eip_len = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, request->tag_id, "Starting.");

    /* clear out the request data. */
    mem_set(request->data, 0, request->request_capacity);

    /* change what we do depending on the type. */
    if(packed_resp->reply_service != (CIP_CMD_MULTI | CIP_CMD_OK)) {
        /* copy the data back into the request buffer. */
        new_eip_len = (int)conn->data_size;
        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, request->tag_id, "Got single response packet.  Copying %d bytes unchanged.",
               new_eip_len);

        if(new_eip_len > request->request_capacity) {
            int request_capacity = 0;

            pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, request->tag_id, "Request buffer too small, allocating larger buffer.");

            critical_block(conn->mutex) {
                int max_payload_size = GET_MAX_PAYLOAD_SIZE(conn);

                // FIXME - no logging in a mutex!
                // pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, "FIXME: max payload size %d", max_payload_size);

                request_capacity = (int)(max_payload_size + EIP_CIP_PREFIX_SIZE);
            }

            /* make sure it will fit. */
            if(new_eip_len > request_capacity) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, request->tag_id,
                       "something is very wrong, packet length is %d but allowable capacity is %d!", new_eip_len,
                       request_capacity);
                return PLCTAG_ERR_TOO_LARGE;
            }

            rc = cip_request_increase_buffer(request, request_capacity);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, request->tag_id,
                       "Unable to increase request buffer size to %d bytes!", request_capacity);
                return rc;
            }
        }

        mem_copy(request->data, conn->data, new_eip_len);
    } else {
        cip_multi_resp_header *multi = (cip_multi_resp_header *)(&packed_resp->reply_service);
        uint16_t total_responses = le2h16(multi->request_count);
        int pkt_len = 0;

        /* this is a packed response. */
        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, request->tag_id, "Got multiple response packet, subpacket %d", sub_packet);

        uint8_t *buf_end = conn->data + conn->data_size;

        /*
         * The response count, the offsets and encap_length all come from the wire.  Check
         * that the offset array itself is inside the data we received BEFORE reading any
         * offset out of it -- otherwise the read that decides whether the array is in bounds
         * is itself out of bounds.
         */
        size_t offsets_start = (size_t)((uint8_t *)multi - conn->data) + offsetof(cip_multi_resp_header, request_offsets);
        size_t offsets_size = (size_t)total_responses * sizeof(uint16_le);

        /* FIXME - conn->data_size is uint32_t, so that test is always false.  Check carefully
         * before removing it: the bounds below depend on data_size being sane. */
        if(sub_packet < 0 || sub_packet >= (int)total_responses || conn->data_size < 0
           || offsets_start > (size_t)conn->data_size || offsets_size > (size_t)conn->data_size - offsets_start) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, request->tag_id,
                   "Packed response sub-packet %d is out of bounds of the received data!", sub_packet);
            return PLCTAG_ERR_OUT_OF_BOUNDS;
        }

        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, request->tag_id, "Our result offset is %d bytes.",
               (int)le2h16(multi->request_offsets[sub_packet]));

        pkt_start = ((uint8_t *)(&multi->request_count) + le2h16(multi->request_offsets[sub_packet]));

        /* calculate the end of the data. */
        if((sub_packet + 1) < total_responses) {
            /* not the last response */
            pkt_end = (uint8_t *)(&multi->request_count) + le2h16(multi->request_offsets[sub_packet + 1]);
        } else {
            pkt_end = (conn->data + le2h16(packed_resp->encap_length) + sizeof(eip_encap));
        }

        /*
         * Now bound pkt_start/pkt_end against the bytes we actually received before trusting
         * them as a memcpy source range.  Comparing pointers directly is UB, so compare the
         * integer values instead.
         */
        if((intptr_t)pkt_start < (intptr_t)(&multi->request_count) || (intptr_t)pkt_start > (intptr_t)buf_end
           || (intptr_t)pkt_end < (intptr_t)pkt_start || (intptr_t)pkt_end > (intptr_t)buf_end) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, request->tag_id,
                   "Packed response sub-packet %d has an out of bounds data range!", sub_packet);
            return PLCTAG_ERR_OUT_OF_BOUNDS;
        }

        pkt_len = (int)(pkt_end - pkt_start);

        /* replace the request buffer if it is not big enough. */
        new_eip_len = pkt_len + (int)sizeof(eip_cip_co_generic_response);
        if(new_eip_len > request->request_capacity) {
            int request_capacity = 0;

            pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, request->tag_id, "Request buffer too small, allocating larger buffer.");

            critical_block(conn->mutex) {
                int max_payload_size = GET_MAX_PAYLOAD_SIZE(conn);

                // FIXME: no logging in a mutex!
                // pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, "max payload size %d", max_payload_size);

                request_capacity = (int)(max_payload_size + EIP_CIP_PREFIX_SIZE);
            }

            /* make sure it will fit. */
            if(new_eip_len > request_capacity) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, request->tag_id,
                       "something is very wrong, packet length is %d but allowable capacity is %d!", new_eip_len,
                       request_capacity);
                return PLCTAG_ERR_TOO_LARGE;
            }

            rc = cip_request_increase_buffer(request, request_capacity);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, request->tag_id,
                       "Unable to increase request buffer size to %d bytes!", request_capacity);
                return rc;
            }
        }

        /* point to the response buffer in a structured way. */
        unpacked_resp = (eip_cip_co_resp *)(request->data);

        /* copy the header down */
        mem_copy(request->data, conn->data, (int)sizeof(eip_cip_co_resp));

        /* size of the new packet */
        new_eip_len = (uint16_t)(((uint8_t *)(&unpacked_resp->reply_service) + pkt_len) /* end of the packet */
                                 - (uint8_t *)(request->data));                         /* start of the packet */

        /* now copy the packet over that. */
        mem_copy(&unpacked_resp->reply_service, pkt_start, pkt_len);

        /* stitch up the packet sizes. */
        unpacked_resp->cpf_cdi_item_length =
            h2le16((uint16_t)(pkt_len + (int)sizeof(uint16_le))); /* extra for the connection sequence */
        unpacked_resp->encap_length = h2le16((uint16_t)(new_eip_len - (uint16_t)sizeof(eip_encap)));
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, request->tag_id, "Unpacked packet:");
    pdebug_dump_bytes(DEBUG_MODULE_CIP, DEBUG_INFO, request->tag_id, request->data, new_eip_len);

    /* notify the reading thread that the request is ready */
    spin_block(&request->lock) {
        request->status = PLCTAG_STATUS_OK;
        request->request_size = new_eip_len;
        request->resp_received = 1;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, request->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


static int send_forward_close_req(cip_conn_p conn, const cip_conn_io_t *io) {
    eip_forward_close_req_t *fc;
    uint8_t *data;
    int rc = PLCTAG_STATUS_OK;

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
    fc->encap_command = h2le16(EIP_UNCONNECTED_SEND); /* 0x006F EIP Send RR Data command */
    fc->encap_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fc->interface_handle))); /* total length of packet except for encap header */
    fc->encap_sender_context = h2le64(++conn->conn_seq_id);
    fc->router_timeout = h2le16(1); /* one second is enough ? */

    /* CPF parts */
    fc->cpf_item_count = h2le16(2);                  /* ALWAYS 2 */
    fc->cpf_nai_item_type = h2le16(EIP_ITEM_NAI); /* null address item type */
    fc->cpf_nai_item_length = h2le16(0);             /* no data, zero length */
    fc->cpf_udi_item_type = h2le16(EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fc->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fc->cm_service_code))); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fc->cm_service_code = CIP_CMD_FORWARD_CLOSE; /* 0x4E Forward Close Request */
    fc->cm_req_path_size = 2;                       /* size of path in 16-bit words */
    fc->cm_req_path[0] = 0x20;                      /* class */
    fc->cm_req_path[1] = 0x06;                      /* CM class */
    fc->cm_req_path[2] = 0x24;                      /* instance */
    fc->cm_req_path[3] = 0x01;                      /* instance 1 */

    /* Forward Open Params */
    fc->secs_per_tick = CIP_SECS_PER_TICK;                     /* seconds per tick, no used? */
    fc->timeout_ticks = CIP_TIMEOUT_TICKS;                     /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
    fc->conn_serial_number = h2le16(conn->conn_serial_number); /* our connection SEQUENCE number. */
    fc->orig_vendor_id = h2le16(CIP_VENDOR_ID);                /* our unique :-) vendor ID */
    fc->orig_serial_number = h2le32(CIP_VENDOR_SN);            /* our serial number. */
    fc->path_size = conn->conn_path_size / 2;                  /* size in 16-bit words */
    fc->reserved = (uint8_t)0;                                    /* padding for the path. */

    /* set the size of the request */
    conn->data_size = (uint32_t)(data - (conn->data));

    rc = io->send_request(conn, 100);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done");

    return rc;
}


static int recv_forward_close_resp(cip_conn_p conn, const cip_conn_io_t *io) {
    eip_forward_close_resp_t *fo_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    rc = io->recv_response(conn, 150);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to receive Forward Close response, %s!", plc_tag_decode_error(rc));
        return rc;
    }

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

        if(le2h16(fo_resp->encap_command) != EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unexpected EIP packet type received: %" PRIu16 "!",
                   le2h16(fo_resp->encap_command));
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(fo_resp->encap_status) != EIP_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "EIP command failed, response code: %" PRIu32, le2h32(fo_resp->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(fo_resp->general_status != EIP_OK) {
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


extern int cip_perform_forward_close(cip_conn_p conn, const cip_conn_io_t *io) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    do {
        rc = send_forward_close_req(conn, io);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Sending forward close failed, %s!", plc_tag_decode_error(rc));
            break;
        }

        rc = recv_forward_close_resp(conn, io);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Forward close response not received, %s!", plc_tag_decode_error(rc));
            break;
        }
    } while(0);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return rc;
}


/*
 * The size of a built request's CIP payload, for deciding whether another one
 * will fit in the same packet.  INT_MAX means "cannot tell", which keeps the
 * caller from packing it with anything.
 *
 * Omron's copy had no unconnected branch and so returned INT_MAX for every
 * unconnected request, which is why unconnected messaging could never be enabled
 * there: nothing could be scheduled.
 */
extern int cip_get_payload_size(cip_request_p request) {
    int request_data_size = 0;
    eip_encap *header = NULL;

    if(!request || !request->data || request->request_size <= 0) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, request ? request->tag_id : 0, "Null request pointer or empty request data!");
        return INT_MAX;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, request->tag_id, "Starting.");

    header = (eip_encap *)(request->data);

    if(le2h16(header->encap_command) == EIP_CONNECTED_SEND) {
        eip_cpf_co_header *co_req = (eip_cpf_co_header *)(request->data);
        /* get length of new request */
        request_data_size = le2h16(co_req->cpf_cdi_item_length) - 2; /* for connection sequence ID */

        /* FIXME - calculate the amount of data in the request by the length of the request and cross check */
    } else if(le2h16(header->encap_command) == EIP_UNCONNECTED_SEND) {
        eip_cpf_uc_header *uc_req = (eip_cpf_uc_header *)(request->data);

        /* get length of embedded command */
        uint16_t cip_packet_size = le2h16(uc_req->cpf_udi_item_length);
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, request->tag_id, "Unconnected request packet size is %d bytes.", cip_packet_size);

        request_data_size = (int)le2h16(uc_req->cpf_udi_item_length);

        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, request->tag_id, "Unconnected request data size is %d bytes.", request_data_size);

        /* FIXME - calculate the amount of data in the request by the length of the request and cross check */
        ptrdiff_t cal_req_size =
            (ptrdiff_t)(request->request_size) - (((uint8_t *)(&uc_req->cpf_udi_item_length) + 2) - request->data);
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, request->tag_id, "Calculated request size is %" PRId64 " bytes.",
               (int64_t)cal_req_size);

        if(cal_req_size < 0) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, request->tag_id, "Calculated request size is negative, something is wrong!");
            request_data_size = 0;
        } else if((uint16_t)cal_req_size != request_data_size) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, request->tag_id,
                   "Calculated request size %" PRId64 " does not match the request data size %d!", (int64_t)cal_req_size,
                   request_data_size);
        }
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, request->tag_id,
               "Not a supported type EIP packet type %d to get the payload size.", le2h16(header->encap_command));
        request_data_size = INT_MAX;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, request->tag_id, "Done, payload size: %d bytes.", request_data_size);

    return request_data_size;
}


/*
 * Concatenate already-built requests into one CIP Multiple Service Packet.
 *
 * This is pure assembly -- whether the requests may be packed at all, and how much
 * room is left, are decided by the caller.  That is process_requests(), which does
 * still differ between the modules.
 */
extern int cip_pack_requests(cip_conn_p conn, cip_request_p *requests, int num_requests) {
    eip_cip_co_req *new_req = NULL;
    eip_cip_co_req *packed_req = NULL;
    /* FIXME - is this the right way to check? */
    int header_size = 0;
    cip_multi_req_header *multi_header = NULL;
    int current_offset = 0;
    uint8_t *pkt_start = NULL;
    int pkt_len = 0;
    size_t pkt_offset = 0;
    size_t src_offset = 0;
    uint8_t *first_pkt_data = NULL;
    uint8_t *next_pkt_data = NULL;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, requests[0]->tag_id, "Starting.");

    if((uint32_t)requests[0]->request_size > conn->data_capacity) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, requests[0]->tag_id,
               "Request of %d bytes exceeds the conn buffer capacity of %u bytes!", requests[0]->request_size,
               conn->data_capacity);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* get the header info from the first request. Just copy the whole thing. */
    mem_copy(conn->data, requests[0]->data, requests[0]->request_size);
    conn->data_size = (uint32_t)requests[0]->request_size;

    /* special case the case where there is just one request. */
    if(num_requests == 1) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, requests[0]->tag_id, "Only one request, so done.");

        return PLCTAG_STATUS_OK;
    }

    /* set up multi-packet header. */

    header_size =
        (int)(sizeof(cip_multi_req_header) + (sizeof(uint16_le) * (size_t)num_requests)); /* offsets for each request. */

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, requests[0]->tag_id, "header size %d", header_size);

    packed_req = (eip_cip_co_req *)(conn->data);

    /* make room in the request packet in the conn for the header. */
    pkt_offset = (size_t)((uint8_t *)(&packed_req->cpf_conn_seq_num) - conn->data) + sizeof(packed_req->cpf_conn_seq_num);
    pkt_len = (int)le2h16(packed_req->cpf_cdi_item_length) - (int)sizeof(packed_req->cpf_conn_seq_num);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, requests[0]->tag_id, "packet 0 is of length %d.", pkt_len);

    /*
     * Bounds check before shifting data forward to make room for the multi-request header.
     *
     * Do this in offsets rather than pointers.  Forming first_pkt_data + pkt_len is itself
     * undefined when the result would land outside the buffer, so a pointer comparison cannot
     * be what decides whether it does -- the compiler may assume the addition stayed in bounds
     * and fold the test away.  Each subtraction below is guarded by the term before it so none
     * of them can wrap.
     *
     * pkt_len is signed and comes from a length field, so reject a negative one here too: it
     * would move the destination backwards and hand mem_move() a negative size.
     */
    if(pkt_len < 0 || header_size < 0 || pkt_offset > (size_t)conn->data_capacity
       || (size_t)header_size > (size_t)conn->data_capacity - pkt_offset
       || (size_t)pkt_len > (size_t)conn->data_capacity - pkt_offset - (size_t)header_size) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, requests[0]->tag_id,
               "Bundled request header does not fit in the conn buffer of %u bytes!", conn->data_capacity);
        return PLCTAG_ERR_TOO_LARGE;
    }

    pkt_start = conn->data + pkt_offset;

    /* point to where we want the current packet to start. */
    first_pkt_data = pkt_start + header_size;

    /* move the data over to make room */
    mem_move(first_pkt_data, pkt_start, pkt_len);

    /* now fill in the header. Use pkt_start as it is pointing to the right location. */
    multi_header = (cip_multi_req_header *)pkt_start;
    multi_header->service_code = CIP_CMD_MULTI;
    multi_header->req_path_size = 0x02; /* length of path in words */
    multi_header->req_path[0] = 0x20;   /* Class */
    multi_header->req_path[1] = 0x02;   /* CM */
    multi_header->req_path[2] = 0x24;   /* Instance */
    multi_header->req_path[3] = 0x01;   /* #1 */
    multi_header->request_count = h2le16((uint16_t)num_requests);

    /* set up the offset for the first request. */
    current_offset = (int)(sizeof(uint16_le) + (sizeof(uint16_le) * (size_t)num_requests));
    multi_header->request_offsets[0] = h2le16((uint16_t)current_offset);

    next_pkt_data = first_pkt_data + pkt_len;
    current_offset = current_offset + pkt_len;

    /* now process the rest of the requests. */
    for(int i = 1; i < num_requests; i++) {

        /* set up the offset */
        multi_header->request_offsets[i] = h2le16((uint16_t)current_offset);

        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, (requests[i] ? requests[i]->tag_id : 0), "new_req=%p", (void *)requests[i]);

        /* get a pointer to the request. */
        new_req = (eip_cip_co_req *)(requests[i]->data);

        /* calculate the request start and length */
        pkt_start = (uint8_t *)(&new_req->cpf_conn_seq_num) + sizeof(new_req->cpf_conn_seq_num);
        pkt_len = (int)le2h16(new_req->cpf_cdi_item_length) - (int)sizeof(new_req->cpf_conn_seq_num);

        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, (requests[i] ? requests[i]->tag_id : 0), "packet %d is of length %d.", i,
               pkt_len);

        /* as above: bound in offsets, and reject a negative length. */
        if(pkt_len < 0) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, (requests[i] ? requests[i]->tag_id : 0),
                   "Bundled request %d has a negative payload length of %d!", i, pkt_len);
            return PLCTAG_ERR_TOO_LARGE;
        }

        /*
         * pkt_len has to fit the SOURCE as well.  It comes from a length field in the request
         * buffer, so bounding it only against the destination leaves mem_copy() free to read
         * past the end of requests[i]->data.
         */
        src_offset = (size_t)(pkt_start - requests[i]->data);

        if(requests[i]->request_size < 0 || src_offset > (size_t)requests[i]->request_size
           || (size_t)pkt_len > (size_t)requests[i]->request_size - src_offset) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, (requests[i] ? requests[i]->tag_id : 0),
                   "Bundled request %d claims %d payload bytes but its buffer only holds %d!", i, pkt_len,
                   requests[i]->request_size);
            return PLCTAG_ERR_TOO_LARGE;
        }

        pkt_offset = (size_t)(next_pkt_data - conn->data);

        if(pkt_offset > (size_t)conn->data_capacity || (size_t)pkt_len > (size_t)conn->data_capacity - pkt_offset) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, (requests[i] ? requests[i]->tag_id : 0),
                   "Bundled requests do not fit in the conn buffer of %u bytes!", conn->data_capacity);
            return PLCTAG_ERR_TOO_LARGE;
        }

        /* copy the request into the conn buffer. */
        mem_copy(next_pkt_data, pkt_start, pkt_len);

        /* calculate the next packet info. */
        next_pkt_data += pkt_len;
        current_offset += pkt_len;
    }

    /* stitch up the CPF packet length */
    packed_req->cpf_cdi_item_length = h2le16((uint16_t)(next_pkt_data - (uint8_t *)(&packed_req->cpf_conn_seq_num)));

    /* stick up the EIP packet length */
    packed_req->encap_length = h2le16((uint16_t)((size_t)(next_pkt_data - conn->data) - sizeof(eip_encap)));

    /* set the total data size */
    conn->data_size = (uint32_t)(next_pkt_data - conn->data);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, requests[0]->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}
