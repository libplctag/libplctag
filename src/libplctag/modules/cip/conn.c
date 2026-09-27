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
 * EtherNet/IP connection handling shared by every CIP dialect.
 *
 * Functions land here as the AB and Omron copies are proven equivalent; the
 * dialect modules keep only what genuinely differs, chiefly how a connection
 * is constructed for a particular PLC family.
 */

#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/cip/wire.h>
#include <inttypes.h>
#include <limits.h>
#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>
#include <utils/rc.h>
#include <utils/random_utils.h>
#include <utils/vector.h>


uint16_t next_conn_serial_number(uint16_t current) {
    uint16_t next = (uint16_t)(current + 1);

    if(next == 0) { next = 1; }

    return next;
}

uint64_t session_get_new_seq_id_unsafe(cip_conn_p conn) {
    /* check for rollover; zero is not a valid sequence id. */
    if((++conn->session_seq_id) == 0) { conn->session_seq_id = 1; }

    return conn->session_seq_id;
}

uint64_t session_get_new_seq_id(cip_conn_p conn) {
    uint16_t res = 0;

    critical_block(conn->session_mutex) { res = (uint16_t)session_get_new_seq_id_unsafe(conn); }

    return res;
}

int session_match_valid(const char *host, const char *path, cip_conn_p conn) {
    if(!conn) { return 0; }

    if(!str_length(host)) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "New session host is NULL or zero length!");
        return 0;
    }

    if(!str_length(conn->host)) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Session host is NULL or zero length!");
        return 0;
    }

    if(str_cmp_i(host, conn->host)) { return 0; }

    if(str_cmp_i(path, conn->path)) { return 0; }

    return 1;
}

int session_close_socket(cip_conn_p conn) {
    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    if(conn->sock) {
        socket_close(conn->sock);
        socket_destroy(&(conn->sock));
        conn->sock = NULL;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}

void cip_request_destroy(void *req_arg) {
    cip_request_p req = req_arg;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Starting.");

    atomic_set_int32(&req->abort_request, 1);

    if(req->data) {
        mem_free(req->data);
        req->data = NULL;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Done.");
}

int session_request_increase_buffer(cip_request_p request, int new_capacity) {
    uint8_t *old_buffer = NULL;
    uint8_t *new_buffer = NULL;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, request->tag_id, "Starting.");

    new_buffer = (uint8_t *)mem_alloc(new_capacity);
    if(!new_buffer) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, request->tag_id, "Unable to allocate larger request buffer!");
        return PLCTAG_ERR_NO_MEM;
    }

    spin_block(&request->lock) {
        old_buffer = request->data;
        request->request_capacity = new_capacity;
        request->data = new_buffer;
    }

    mem_free(old_buffer);

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, request->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}

int session_get_available_cip_payload_space(cip_conn_p conn) {
    int result = 0;

    if(!conn) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Called with null session pointer!");
        return 0;
    }

    critical_block(conn->session_mutex) {
        int max_payload_size = GET_MAX_PAYLOAD_SIZE(conn);
        result = max_payload_size;

        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
               "Session payload calculation: max_payload_size=%d, fo_conn_size=%d, fo_ex_conn_size=%d, selected=%d",
               conn->max_payload_size, conn->fo_conn_size, conn->fo_ex_conn_size, max_payload_size);

        // Account for CPF data item overhead
        if(conn->use_connected_msg) {
            result -= (int)sizeof(cpf_connected_data_item);
        } else {
            result -= (int)sizeof(cpf_unconnected_data_item);
            result -= (int)(conn->conn_path_size) + 2; /* encoded path size plus two bytes for length and padding */
        }
    }
    if(result < 0) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Available payload space is negative (%d bytes)! This should not happen!",
               result);
        result = 0;
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Available payload space is %d bytes.", result);
    }

    return result;
}



int session_list_init(cip_conn_list_t *list) {
    int rc = PLCTAG_STATUS_OK;

    if((rc = mutex_create(&(list->mutex))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, 0, "Unable to create connection list mutex %s!", plc_tag_decode_error(rc));
        return rc;
    }

    if((list->conns = vector_create(25, 5)) == NULL) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, 0, "Unable to create connection vector!");
        mutex_destroy(&(list->mutex));
        list->mutex = NULL;
        return PLCTAG_ERR_NO_MEM;
    }

    return PLCTAG_STATUS_OK;
}



int session_list_add_unsafe(cip_conn_list_t *list, cip_conn_p conn) {
    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Starting");

    if(!conn) { return PLCTAG_ERR_NULL_PTR; }

    vector_set(list->conns, vector_length(list->conns), conn);

    conn->on_list = 1;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Done");

    return PLCTAG_STATUS_OK;
}


int session_list_add(cip_conn_list_t *list, cip_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Starting.");

    critical_block(list->mutex) { rc = session_list_add_unsafe(list, conn); }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


int session_list_remove_unsafe(cip_conn_list_t *list, cip_conn_p conn) {
    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Starting");

    if(!conn || !list->conns) { return 0; }

    for(int i = 0; i < vector_length(list->conns); i++) {
        cip_conn_p tmp = vector_get(list->conns, i);

        /* FIXME potential ABA problem here */
        if(tmp == conn) {
            vector_remove(list->conns, i);
            break;
        }
    }

    /* no longer on the list */
    conn->on_list = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Done");

    return PLCTAG_STATUS_OK;
}


int session_list_remove(cip_conn_list_t *list, cip_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Starting.");

    if(conn->on_list) {
        critical_block(list->mutex) { rc = session_list_remove_unsafe(list, conn); }
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Connection not on list, skipping removal.");
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


cip_conn_p session_list_find_by_host_unsafe(cip_conn_list_t *list, const char *host, const char *path,
                                            int connection_group_id) {
    for(int i = 0; i < vector_length(list->conns); i++) {
        cip_conn_p conn = vector_get(list->conns, i);

        /* is this connection in the process of destruction? */
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "rc_inc: Acquiring connection reference.");
        conn = rc_inc(conn);
        if(conn) {
            if(conn->connection_group_id == connection_group_id && session_match_valid(host, path, conn)) { return conn; }

            rc_dec(conn);
        }
    }

    return NULL;
}
int send_eip_request(cip_conn_p conn, int timeout) {
    int rc = PLCTAG_STATUS_OK;
    int32_t final_rc = PLCTAG_STATUS_OK;
    int64_t timeout_time = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    if(!conn) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Session pointer is null.");
        return PLCTAG_ERR_NULL_PTR;
    }

    conn_watch_publish(&conn->watch, TAG_CONN_EVENT_SEND_REQUEST_STARTED, PLCTAG_STATUS_OK);

    if(timeout > 0) {
        timeout_time = time_ms() + timeout;
    } else {
        timeout_time = INT64_MAX;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Sending packet of size %d", conn->data_size);
    pdebug_dump_bytes(DEBUG_MODULE_CIP, DEBUG_INFO, 0, conn->data, (int)(conn->data_size));

    conn->data_offset = 0;
    conn->packet_count++;

    /*
     * Remember what we are asking for.  recv_eip_response() has to be able to tell an answer
     * to this request from an unrelated packet, and the only identity the encapsulation layer
     * gives us is the command and the sender context we echo back.
     */
    if(conn->data_size >= sizeof(eip_encap)) {
        eip_encap *out_header = (eip_encap *)(conn->data);

        conn->req_encap_command = le2h16(out_header->encap_command);
        conn->req_seq_id = le2h64(out_header->encap_sender_context);
        conn->req_sent = true;
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Packet of %u bytes is too small to hold an EIP header!",
               conn->data_size);
        conn_watch_publish(&conn->watch, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, PLCTAG_ERR_TOO_SMALL);
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* send the packet */
    do {
        rc = socket_write(conn->sock, conn->data + conn->data_offset,
                          (int)conn->data_size - (int)conn->data_offset, SOCKET_WAIT_TIMEOUT_MS);

        if(rc >= 0) {
            conn->data_offset += (uint32_t)rc;
        } else {
            if(rc == PLCTAG_ERR_TIMEOUT) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Socket not yet ready to write.");
                rc = 0;
            }
        }

        /* give up the CPU if we still are looping */
    } while(!atomic_get_int32(&conn->terminating) && rc >= 0 && conn->data_offset < conn->data_size
            && timeout_time > time_ms());

    if(atomic_get_int32(&conn->terminating)) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Session is terminating.");
        final_rc = PLCTAG_ERR_ABORT;
        conn_watch_publish(&conn->watch, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, final_rc);
        return final_rc;
    }

    if(rc < 0) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error, %d, writing socket!", rc);
        final_rc = rc;
        conn_watch_publish(&conn->watch, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, final_rc);
        return final_rc;
    }

    if(timeout_time <= time_ms()) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Timed out waiting to send data!");
        final_rc = PLCTAG_ERR_TIMEOUT;
        conn_watch_publish(&conn->watch, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, final_rc);
        return final_rc;
    }

    conn_watch_publish(&conn->watch, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, PLCTAG_STATUS_OK);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


int recv_eip_response(cip_conn_p conn, int timeout) {
    uint32_t data_needed = 0;
    int rc = PLCTAG_STATUS_OK;
    int32_t final_rc = PLCTAG_STATUS_OK;
    int64_t timeout_time = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    if(!conn) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Called with null session!");
        return PLCTAG_ERR_NULL_PTR;
    }

    conn_watch_publish(&conn->watch, TAG_CONN_EVENT_RECEIVE_RESPONSE_STARTED, PLCTAG_STATUS_OK);


    if(timeout > 0) {
        timeout_time = time_ms() + timeout;
    } else {
        timeout_time = INT64_MAX;
    }

    conn->data_offset = 0;
    conn->data_size = 0;
    data_needed = sizeof(eip_encap);

    /*
     * Clear the buffer before reading into it.  Response handlers cast this buffer to
     * header structs, and a short response leaves whatever the previous response put
     * there.  Every length check guarding those casts is then the only thing between a
     * truncated packet and a PLC-groomed value being read as a status or connection ID.
     * Zeroing makes that failure mode boring instead of exploitable.
     */
    mem_set(conn->data, 0, (int)conn->data_capacity);

    do {
        rc = socket_read(conn->sock, conn->data + conn->data_offset, (int)(data_needed - conn->data_offset),
                         SOCKET_WAIT_TIMEOUT_MS);

        if(rc >= 0) {
            conn->data_offset += (uint32_t)rc;

            /*pdebug_dump_bytes(conn->debug, conn->data, conn->data_offset);*/

            /* recalculate the amount of data needed if we have just completed the read of an encap header */
            if(conn->data_offset >= sizeof(eip_encap)) {
                data_needed = (uint32_t)(sizeof(eip_encap) + le2h16(((eip_encap *)(conn->data))->encap_length));

                if(data_needed > conn->data_capacity) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                           "Packet response (%d) is larger than possible buffer size (%d)!", data_needed, conn->data_capacity);
                    final_rc = PLCTAG_ERR_TOO_LARGE;
                    conn_watch_publish(&conn->watch, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, final_rc);
                    return final_rc;
                }
            }
        } else {
            if(rc == PLCTAG_ERR_TIMEOUT) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Socket not yet ready to read.");
            } else {
                /* error! */
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error reading socket! rc=%d", rc);
                final_rc = rc;
                conn_watch_publish(&conn->watch, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, final_rc);
                return final_rc;
            }
        }
    } while(!atomic_get_int32(&conn->terminating) && conn->data_offset < data_needed && timeout_time > time_ms());

    if(atomic_get_int32(&conn->terminating)) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Session is terminating, returning...");
        final_rc = PLCTAG_ERR_ABORT;
        conn_watch_publish(&conn->watch, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, final_rc);
        return final_rc;
    }

    if(timeout_time <= time_ms()) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Timed out waiting for data to read!");
        final_rc = PLCTAG_ERR_TIMEOUT;
        conn_watch_publish(&conn->watch, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, final_rc);
        return final_rc;
    }

    conn->resp_seq_id = le2h64(((eip_encap *)(conn->data))->encap_sender_context);
    conn->data_size = data_needed;

    /*
     * Everything downstream of here decides how to parse this buffer from fields the PLC
     * chose.  Before any of that, make sure this packet is actually the answer to the request
     * we sent: the encapsulation layer gives us three things to check and all three are free.
     *
     * The command matters most.  The connected and unconnected CPF headers are different
     * lengths, so a reply that changes the command out from under us moves every field the
     * handlers read, including the CIP status they branch on.
     */
    {
        eip_encap *resp_header = (eip_encap *)(conn->data);
        uint16_t resp_command = le2h16(resp_header->encap_command);
        uint32_t resp_handle = le2h32(resp_header->encap_session_handle);

        if(conn->req_sent && resp_command != conn->req_encap_command) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                   "Received EIP command %04" PRIx16 " in response to command %04" PRIx16 "!", resp_command,
                   conn->req_encap_command);
            final_rc = PLCTAG_ERR_BAD_DATA;
            conn_watch_publish(&conn->watch, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, final_rc);
            return final_rc;
        }

        /*
         * Once the conn is registered every packet carries our handle.  A zero handle means
         * we are still registering, so there is nothing to compare against yet.
         */
        if(conn->session_handle != 0 && resp_handle != conn->session_handle) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                   "Received a response for session handle %" PRIx32 " but this session is %" PRIx32 "!", resp_handle,
                   conn->session_handle);
            final_rc = PLCTAG_ERR_BAD_DATA;
            conn_watch_publish(&conn->watch, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, final_rc);
            return final_rc;
        }

        /*
         * The target echoes the sender context on SendRRData.  That is what tells a reply to
         * the request we are waiting on from a late reply to one that already timed out --
         * without it a stale response gets applied to whichever tag is in flight now.
         *
         * Connected sends do not get this check: we do not fill the field in for them, so there
         * is nothing meaningful to echo.  Their identity is the connection ID and the connection
         * sequence number in the CPF header, which the tag layer checks instead.
         */
        if(conn->req_sent && resp_command == CIP_EIP_UNCONNECTED_SEND && conn->resp_seq_id != conn->req_seq_id) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                   "Received a response with sender context %" PRIx64 " but we sent %" PRIx64 "!", conn->resp_seq_id,
                   conn->req_seq_id);
            final_rc = PLCTAG_ERR_BAD_DATA;
            conn_watch_publish(&conn->watch, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, final_rc);
            return final_rc;
        }
    }

    rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "request received all needed data (%d bytes of %d).", conn->data_offset,
           data_needed);

    pdebug_dump_bytes(DEBUG_MODULE_CIP, DEBUG_INFO, 0, conn->data, (int)(conn->data_offset));

    /* check status. */
    if(le2h32(((eip_encap *)(conn->data))->encap_status) != CIP_EIP_OK) { rc = PLCTAG_ERR_BAD_STATUS; }

    conn_watch_publish(&conn->watch, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, rc);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return rc;
}


int send_extended_forward_open_request(cip_conn_p conn) {
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
    fo->secs_per_tick = CIP_EIP_SECS_PER_TICK; /* seconds per tick, no used? */
    fo->timeout_ticks = CIP_EIP_TIMEOUT_TICKS; /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
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

    rc = send_eip_request(conn, SESSION_DEFAULT_TIMEOUT);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done");

    return rc;
}


int send_old_forward_open_request(cip_conn_p conn) {
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
    fo->secs_per_tick = CIP_EIP_SECS_PER_TICK; /* seconds per tick, no used? */
    fo->timeout_ticks = CIP_EIP_TIMEOUT_TICKS; /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
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
    if(conn->dhp_capable && conn->is_dhp) {
        fo->orig_to_targ_conn_params = h2le16(CIP_EIP_PLC5_PARAM);
    } else {
        fo->orig_to_targ_conn_params = h2le16(
            CIP_EIP_CONN_PARAM | conn->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    }

    fo->targ_to_orig_rpi = h2le32(CIP_EIP_RPI); /* target to us RPI - not really used for explicit messages? */

    /* screwy logic if this is a DH+ route! */
    if(conn->dhp_capable && conn->is_dhp) {
        fo->targ_to_orig_conn_params = h2le16(CIP_EIP_PLC5_PARAM);
    } else {
        fo->targ_to_orig_conn_params = h2le16(
            CIP_EIP_CONN_PARAM | conn->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    }

    fo->transport_class = CIP_EIP_TRANSPORT_CLASS_T3; /* 0xA3, server transport, class 3, application trigger */
    fo->path_size = conn->conn_path_size / 2;     /* size in 16-bit words */

    /* set the size of the request */
    conn->data_size = (uint32_t)(data - (conn->data));

    rc = send_eip_request(conn, 0);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done");

    return rc;
}


int send_forward_open_request(cip_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;
    uint16_t max_payload;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Flag prohibiting use of extended ForwardOpen is %d.",
           conn->only_use_old_forward_open);

    max_payload = (uint16_t)(conn->only_use_old_forward_open ? conn->fo_conn_size : conn->fo_ex_conn_size);

    /* set the max payload guess if it is larger than the maximum possible or if it is zero. */
    critical_block(conn->session_mutex) {
        conn->max_payload_guess =
            ((conn->max_payload_guess == 0) || (conn->max_payload_guess > max_payload) ? max_payload :
                                                                                               conn->max_payload_guess);
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Set Forward Open maximum payload size guess to %d bytes.",
           conn->max_payload_guess);

    if(conn->only_use_old_forward_open) {
        rc = send_old_forward_open_request(conn);
    } else {
        rc = send_extended_forward_open_request(conn);
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done");

    return rc;
}


int send_forward_close_req(cip_conn_p conn) {
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
    fc->secs_per_tick = CIP_EIP_SECS_PER_TICK;                     /* seconds per tick, no used? */
    fc->timeout_ticks = CIP_EIP_TIMEOUT_TICKS;                     /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
    fc->conn_serial_number = h2le16(conn->conn_serial_number); /* our connection SEQUENCE number. */
    fc->orig_vendor_id = h2le16(CIP_EIP_VENDOR_ID);                /* our unique :-) vendor ID */
    fc->orig_serial_number = h2le32(CIP_EIP_VENDOR_SN);            /* our serial number. */
    fc->path_size = conn->conn_path_size / 2;                  /* size in 16-bit words */
    fc->reserved = (uint8_t)0;                                    /* padding for the path. */

    /* set the size of the request */
    conn->data_size = (uint32_t)(data - (conn->data));

    rc = send_eip_request(conn, 100);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done");

    return rc;
}


int recv_forward_close_resp(cip_conn_p conn) {
    eip_forward_close_resp_t *fo_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    rc = recv_eip_response(conn, 150);
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


int perform_forward_close(cip_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    do {
        rc = send_forward_close_req(conn);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Sending forward close failed, %s!", plc_tag_decode_error(rc));
            break;
        }

        rc = recv_forward_close_resp(conn);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Forward close response not received, %s!", plc_tag_decode_error(rc));
            break;
        }
    } while(0);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return rc;
}


int session_open_socket(cip_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;
    char **server_port = NULL;
    int port = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    /*
     * A reconnect reuses this conn, so drop the identity of the connection that just
     * went away.  Otherwise the checks in recv_eip_response() compare the new conn's
     * RegisterSession reply against the old handle and reject it, and the conn can
     * never come back up.
     */
    conn->session_handle = 0;
    conn->req_encap_command = 0;
    conn->req_seq_id = 0;
    conn->req_sent = false;

    /* Open a socket for communication with the gateway. */
    rc = socket_create(&(conn->sock));

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to create socket for session!");
        return rc;
    }

    server_port = str_split(conn->host, ":");
    if(!server_port) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to split server and port string!");
        return PLCTAG_ERR_BAD_CONFIG;
    }

    if(server_port[0] == NULL) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Server string is malformed or empty!");
        mem_free(server_port);
        return PLCTAG_ERR_BAD_CONFIG;
    }

    if(server_port[1] != NULL) {
        rc = str_to_int(server_port[1], &port);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to extract port number from server string \"%s\"!",
                   conn->host);
            mem_free(server_port);
            return PLCTAG_ERR_BAD_CONFIG;
        }

        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Using special port %d.", port);
    } else {
        port = CIP_EIP_DEFAULT_PORT;

        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Using default port %d.", port);
    }

    /* record the effective port, default included, so it can be read back as an attribute. */
    conn->port = port;

    rc = socket_connect_tcp_start(conn->sock, server_port[0], port);

    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to connect socket for session!");
        mem_free(server_port);
        return rc;
    }

    if(server_port) { mem_free(server_port); }

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return rc;
}


int prepare_request(cip_conn_p conn) {
    eip_encap *encap = NULL;
    int payload_size = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    encap = (eip_encap *)(conn->data);
    payload_size = (int)conn->data_size - (int)sizeof(eip_encap);

    /* FIXME - why is this check here? Haven't we checked this up the call chain? */
    if(!conn) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Called with null session!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* fill in the fields of the request. */

    encap->encap_length = h2le16((uint16_t)payload_size);
    encap->encap_session_handle = h2le32(conn->session_handle);
    encap->encap_status = h2le32(0);
    encap->encap_options = h2le32(0);

    /* FIXME - support other kinds of requests? */

    /* set up the conn sequence ID for this transaction */
    if(le2h16(encap->encap_command) == CIP_EIP_UNCONNECTED_SEND) {
        /* get new ID */
        conn->session_seq_id++;

        encap->encap_sender_context = h2le64(conn->session_seq_id); /* link up the request seq ID and the packet seq ID */

        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Preparing unconnected packet with session sequence ID %llx",
               conn->session_seq_id);
    } else if(le2h16(encap->encap_command) == CIP_EIP_CONNECTED_SEND) {
        eip_cip_co_req *conn_req = (eip_cip_co_req *)(conn->data);

        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "cpf_targ_conn_id=%x", conn->targ_connection_id);

        /* set up the connection information */
        conn_req->cpf_targ_conn_id = h2le32(conn->targ_connection_id);

        conn->conn_seq_num++;
        conn_req->cpf_conn_seq_num = h2le16(conn->conn_seq_num);

        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Preparing connected packet with connection ID %x and sequence ID %u(%x)",
               conn->orig_connection_id, conn->conn_seq_num, conn->conn_seq_num);
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unsupported packet type %x!", le2h16(encap->encap_command));
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* display the data */
    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Prepared packet of size %d", conn->data_size);
    pdebug_dump_bytes(DEBUG_MODULE_CIP, DEBUG_INFO, 0, conn->data, (int)conn->data_size);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


int session_unregister(cip_conn_p conn) {
    (void)conn;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    /* nothing to do, perhaps. */

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


int get_payload_size(cip_request_p request) {
    int request_data_size = 0;
    eip_encap *header = NULL;

    if(!request || !request->data || request->request_size <= 0) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, request ? request->tag_id : 0, "Null request pointer or empty request data!");
        return INT_MAX;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, request->tag_id, "Starting.");

    header = (eip_encap *)(request->data);

    if(le2h16(header->encap_command) == CIP_EIP_CONNECTED_SEND) {
        eip_cpf_co_header *co_req = (eip_cpf_co_header *)(request->data);
        /* get length of new request */
        request_data_size = le2h16(co_req->cpf_cdi_item_length) - 2; /* for connection sequence ID */

        /* FIXME - calculate the amount of data in the request by the length of the request and cross check */
    } else if(le2h16(header->encap_command) == CIP_EIP_UNCONNECTED_SEND) {
        eip_cpf_uc_header *uc_req = (eip_cpf_uc_header *)(request->data);

        /* get length of embedded command */
        uint16_t cip_packet_size = le2h16(uc_req->cpf_udi_item_length);
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, request->tag_id, "Unconnected request packet size is %d bytes.",
               cip_packet_size);

        request_data_size = (int)le2h16(uc_req->cpf_udi_item_length);

        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, request->tag_id, "Unconnected request data size is %d bytes.",
               request_data_size);

        /* FIXME - calculate the amount of data in the request by the length of the request and cross check */
        ptrdiff_t cal_req_size =
            (ptrdiff_t)(request->request_size) - (((uint8_t *)(&uc_req->cpf_udi_item_length) + 2) - request->data);
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, request->tag_id, "Calculated request size is %td bytes.", cal_req_size);

        if(cal_req_size < 0) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, request->tag_id,
                   "Calculated request size is negative, something is wrong!");
            request_data_size = 0;
        } else if((uint16_t)cal_req_size != request_data_size) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, request->tag_id,
                   "Calculated request size %td does not match the request data size %d!", cal_req_size, request_data_size);
        }
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, request->tag_id,
               "Not a supported type EIP packet type %d to get the payload size.", le2h16(header->encap_command));
        request_data_size = INT_MAX;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, request->tag_id, "Done, payload size: %d bytes.", request_data_size);

    return request_data_size;
}


int purge_aborted_requests_unsafe(cip_conn_p conn) {
    int purge_count = 0;
    cip_request_p request = NULL;

    pdebug(DEBUG_MODULE_CIP, DEBUG_SPEW, 0, "Starting.");

    /* remove the aborted requests. */
    for(int i = 0; i < vector_length(conn->requests); i++) {
        request = vector_get(conn->requests, i);

        /* filter out the aborts. */
        if(request && atomic_get_int32(&request->abort_request)) {
            purge_count++;

            /* remove it from the queue. */
            vector_remove(conn->requests, i);

            /* set the debug tag to the owning tag. */

            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Session thread releasing aborted request %p.", request);

            request->status = PLCTAG_ERR_ABORT;
            request->request_size = 0;
            request->resp_received = 1;

            /* release our hold on it. */
            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "rc_dec: Releasing request reference.");
            rc_dec(request);

            /* vector size has changed, back up one. */
            i--;
        }
    }

    if(purge_count > 0) { pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Removed %d aborted requests.", purge_count); }

    pdebug(DEBUG_MODULE_CIP, DEBUG_SPEW, 0, "Done.");

    return purge_count;
}
int pack_requests(cip_conn_p conn, cip_request_p *requests, int num_requests) {
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
               "Request of %d bytes exceeds the session buffer capacity of %u bytes!", requests[0]->request_size,
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
               "Bundled request header does not fit in the session buffer of %u bytes!", conn->data_capacity);
        return PLCTAG_ERR_TOO_LARGE;
    }

    pkt_start = conn->data + pkt_offset;

    /* point to where we want the current packet to start. */
    first_pkt_data = pkt_start + header_size;

    /* move the data over to make room */
    mem_move(first_pkt_data, pkt_start, pkt_len);

    /* now fill in the header. Use pkt_start as it is pointing to the right location. */
    multi_header = (cip_multi_req_header *)pkt_start;
    multi_header->service_code = CIP_EIP_CMD_CIP_MULTI;
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

        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, (requests[i] ? requests[i]->tag_id : 0), "new_req=%p", requests[i]);

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
                   "Bundled requests do not fit in the session buffer of %u bytes!", conn->data_capacity);
            return PLCTAG_ERR_TOO_LARGE;
        }

        /* copy the request into the session buffer. */
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


int unpack_response(cip_conn_p conn, cip_request_p request, int sub_packet) {
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
    if(packed_resp->reply_service != (CIP_EIP_CMD_CIP_MULTI | CIP_EIP_CMD_CIP_OK)) {
        /* copy the data back into the request buffer. */
        new_eip_len = (int)conn->data_size;
        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, request->tag_id, "Got single response packet.  Copying %d bytes unchanged.",
               new_eip_len);

        if(new_eip_len > request->request_capacity) {
            int request_capacity = 0;

            pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, request->tag_id, "Request buffer too small, allocating larger buffer.");

            critical_block(conn->session_mutex) {
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

            rc = session_request_increase_buffer(request, request_capacity);
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

        if(sub_packet < 0 || sub_packet >= (int)total_responses || offsets_start > (size_t)conn->data_size
           || offsets_size > (size_t)conn->data_size - offsets_start) {
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

            critical_block(conn->session_mutex) {
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

            rc = session_request_increase_buffer(request, request_capacity);
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


int session_add_request_unsafe(cip_conn_p conn, cip_request_p req) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, req->tag_id, "Starting.");

    if(!conn) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, req->tag_id, "Session is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, req->tag_id, "rc_inc: Acquiring request reference.");
    req = rc_inc(req);

    if(!req) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Request is either null or in the process of being deleted.");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* insert into the requests vector */
    vector_set(conn->requests, vector_length(conn->requests), req);

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, req->tag_id, "Total requests in the queue: %d",
           vector_length(conn->requests));

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, req->tag_id, "Done.");

    return rc;
}


int session_add_request(cip_conn_p conn, cip_request_p req) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, req->tag_id, "Starting. session=%p, req=%p", conn, req);

    /*
     * Check before taking the mutex: critical_block() has to dereference
     * conn to lock it, and an early return from inside the block would skip
     * the unlock in its loop increment.
     */
    if(!conn) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, req->tag_id, "Session is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    critical_block(conn->session_mutex) { rc = session_add_request_unsafe(conn, req); }

    /* wake up the conn thread because we added something to process. */
    cond_signal(conn->session_wait_cond);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, req->tag_id, "Done.");

    return rc;
}


int64_t calc_retry_time(unsigned int retry_count) {
    int64_t result = 0;
    result = RETRY_WAIT_INITIAL_MS * (int64_t)(1 << retry_count);

    if(result > RETRY_WAIT_MAX_MS) { result = RETRY_WAIT_MAX_MS; }

    result += (int64_t)random_u64(RETRY_WAIT_INITIAL_MS) - (int64_t)(RETRY_WAIT_INITIAL_MS / 2);

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Retry count %u for retry time delay of %" PRId64 "ms.", retry_count,
           result);

    return result;
}


int session_create_request(cip_conn_p conn, int tag_id, cip_request_p *req) {
    int rc = PLCTAG_STATUS_OK;
    cip_request_p res;
    size_t request_capacity = 0;
    uint8_t *buffer = NULL;

    critical_block(conn->session_mutex) {
        int available_payload = session_get_available_cip_payload_space(conn);

        // FIXME: no logging in a mutex!
        // pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, "FIXME: available payload space %d", available_payload);

        request_capacity = (size_t)(available_payload + EIP_CIP_PREFIX_SIZE);
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Starting.");

    buffer = (uint8_t *)mem_alloc((int)request_capacity);
    if(!buffer) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to allocate request buffer!");
        *req = NULL;
        return PLCTAG_ERR_NO_MEM;
    }

    res = (cip_request_p)rc_alloc((int)sizeof(cip_request_t), cip_request_destroy);
    if(!res) {
        mem_free(buffer);
        *req = NULL;
        rc = PLCTAG_ERR_NO_MEM;
    } else {
        res->data = buffer;
        res->tag_id = tag_id;
        res->request_capacity = (int)request_capacity;
        res->lock = LOCK_INIT;

        *req = res;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


int session_register(cip_conn_p conn) {
    eip_session_reg_req *req;
    eip_encap *resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    /*
     * clear the conn data.
     *
     * We use the receiving buffer because we do not have a request and nothing can
     * be coming in (we hope) on the socket yet.
     */
    mem_set(conn->data, 0, sizeof(eip_session_reg_req));

    req = (eip_session_reg_req *)(conn->data);

    /* fill in the fields of the request */
    req->encap_command = h2le16(CIP_EIP_REGISTER_SESSION);
    req->encap_length = h2le16(sizeof(eip_session_reg_req) - sizeof(eip_encap));
    req->encap_session_handle = h2le32(/*conn->session_handle*/ 0);
    req->encap_status = h2le32(0);
    req->encap_sender_context = h2le64((uint64_t)0);
    req->encap_options = h2le32(0);

    req->eip_version = h2le16(CIP_EIP_VERSION);
    req->option_flags = h2le16(0);

    /*
     * socket ops here are _ASYNCHRONOUS_!
     *
     * This is done this way because we do not have everything
     * set up for a request to be handled by the thread.  I think.
     */

    /* send registration to the gateway */
    conn->data_size = sizeof(eip_session_reg_req);
    conn->data_offset = 0;

    rc = send_eip_request(conn, SESSION_DEFAULT_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error sending session registration request %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    /* get the response from the gateway */
    rc = recv_eip_response(conn, SESSION_DEFAULT_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error receiving session registration response %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    /* encap header is at the start of the buffer */
    resp = (eip_encap *)(conn->data);

    /* check the response status */
    if(le2h16(resp->encap_command) != CIP_EIP_REGISTER_SESSION) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "EIP unexpected response packet type: %d!", resp->encap_command);
        return PLCTAG_ERR_BAD_DATA;
    }

    if(le2h32(resp->encap_status) != CIP_EIP_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "EIP command failed, response code: %d", le2h32(resp->encap_status));
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /*
     * after all that, save the conn handle, we will
     * use it in future packets.
     */
    conn->session_handle = le2h32(resp->encap_session_handle);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}
