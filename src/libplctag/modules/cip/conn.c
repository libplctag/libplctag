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

#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/cip.h>
#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/cip/error_codes.h>
#include <libplctag/modules/cip/path.h>
#include <libplctag/modules/cip/wire.h>
#include <limits.h>
#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/backoff.h>
#include <utils/debug.h>
#include <utils/random_utils.h>
#include <utils/rc.h>
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



/*
 * The bound a payload-only request is checked against.  Start from what the CPF
 * data item leaves and, for unconnected messaging, take off the Unconnected Send
 * wrapper as well -- session_get_available_cip_payload_space() leaves that to the
 * caller, which is how the framed builders ended up measuring it three different
 * ways.
 */
int cip_conn_max_cip_payload(cip_conn_p conn) {
    int result = session_get_available_cip_payload_space(conn);

    if(!conn->use_connected_msg) { result -= CIP_EIP_UC_SEND_OVERHEAD; }

    if(result < 0) { result = 0; }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Maximum CIP payload is %d bytes.", result);

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

    /*
     * Both variants are the same request with a different service code, so both get the
     * same send timeout -- SESSION_DEFAULT_TIMEOUT, as Register Session and the request
     * path use.  The old variant used to pass 0, which send_eip_request() reads as no
     * timeout at all, so a wedged socket parked the connect path instead of failing it.
     */
    if(conn->only_use_old_forward_open) {
        rc = cip_encode_forward_open_old(conn);
    } else {
        rc = cip_encode_forward_open_ex(conn);
    }

    if(rc == PLCTAG_STATUS_OK) { rc = send_eip_request(conn, SESSION_DEFAULT_TIMEOUT); }

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done");

    return rc;
}


/*
 * Forward Close is best effort.  It runs during teardown, and the PLC drops the connection
 * on its own once the connection times out, so a short send timeout here is deliberate:
 * failing to close costs nothing much, blocking shutdown on an unresponsive PLC does.  Do
 * not raise these to SESSION_DEFAULT_TIMEOUT to match Forward Open -- they are not the
 * same case.
 */
int send_forward_close_req(cip_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    rc = cip_encode_forward_close(conn);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to encode Forward Close request, %s!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = send_eip_request(conn, 100);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done");

    return rc;
}


int recv_forward_close_resp(cip_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    rc = recv_eip_response(conn, 150);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to receive Forward Close response, %s!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = cip_decode_forward_close_response(conn);

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
    if(!request || !request->data || request->request_size <= 0) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, request ? request->tag_id : 0, "Null request pointer or empty request data!");
        return INT_MAX;
    }

    /*
     * A request holds its CIP message and nothing else, so its size is the payload size.
     * This used to re-parse the length back out of the CPF header the builder had just
     * written, and warn when the two disagreed.
     */
    return request->request_size;
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


/*
 * Finish a request that a tag-level builder has filled in and hand it to the
 * connection's request queue.
 *
 * The caller owns the only reference on entry.  On success the queue holds a
 * reference of its own and the caller keeps theirs.  On failure the request is
 * released here and the caller must not touch it again, so a failed submission
 * leaves the caller nothing to roll back.
 */
int cip_submit_request(cip_conn_p conn, cip_request_p req, int request_size, bool allow_packing) {
    int rc = PLCTAG_STATUS_OK;

    if(!req) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Called with a null request!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, req->tag_id, "Starting.");

    /*
     * The builder walked a cursor across the request buffer to arrive at this size.  Nothing
     * up to this point has checked that the cursor stayed inside the buffer, so check it here
     * before the request goes anywhere.
     */
    if(request_size <= 0 || request_size > req->request_capacity) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, req->tag_id,
               "Built request of %d bytes does not fit the %d byte request buffer!", request_size, req->request_capacity);
        rc_dec(req);
        return PLCTAG_ERR_TOO_LARGE;
    }

    req->request_size = request_size;
    req->allow_packing = allow_packing;

    rc = session_add_request(conn, req);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, req->tag_id, "Unable to add the request to the connection, %s!",
               plc_tag_decode_error(rc));
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, req->tag_id, "rc_dec: Releasing the unsubmitted request.");
        rc_dec(req);
        return rc;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, req->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


int cip_submit_payload(cip_conn_p conn, cip_request_p req, int payload_size, bool allow_packing) {
    int max_payload = 0;

    if(!req) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Called with a null request!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, req->tag_id, "Starting.");

    max_payload = cip_conn_max_cip_payload(conn);

    if(payload_size > max_payload) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, req->tag_id,
               "CIP payload of %d bytes exceeds the %d bytes this connection can carry!", payload_size, max_payload);
        rc_dec(req);
        return PLCTAG_ERR_TOO_LARGE;
    }

    return cip_submit_request(conn, req, payload_size, allow_packing);
}


int cip_submit_unrouted_payload(cip_conn_p conn, cip_request_p req, int payload_size) {
    int max_payload = 0;

    if(!req) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Called with a null request!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, req->tag_id, "Starting.");

    /*
     * Nothing is routed onward, so the Unconnected Send wrapper and the route path that
     * cip_conn_max_cip_payload() subtracts are not in the way here.  Hand those bytes back.
     */
    max_payload = cip_conn_max_cip_payload(conn);

    if(!conn->use_connected_msg) { max_payload += CIP_EIP_UC_SEND_OVERHEAD + (int)conn->conn_path_size + 2; }

    if(payload_size > max_payload) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, req->tag_id,
               "CIP message of %d bytes exceeds the %d bytes this connection can carry!", payload_size, max_payload);
        rc_dec(req);
        return PLCTAG_ERR_TOO_LARGE;
    }

    req->unrouted = true;

    /* these are single-request services; nothing bundles them. */
    return cip_submit_request(conn, req, payload_size, false);
}


int session_create_request(cip_conn_p conn, int tag_id, cip_request_p *req) {
    int rc = PLCTAG_STATUS_OK;
    cip_request_p res;
    size_t request_capacity = 0;
    uint8_t *buffer = NULL;

    /*
     * A request holds a CIP message and nothing else, going out or coming back, so the most
     * it ever needs is the largest CIP payload this connection can carry.  That bounds the
     * reply as well as the request, which is why unpack_response() never has to grow it.
     */
    critical_block(conn->session_mutex) { request_capacity = (size_t)GET_MAX_PAYLOAD_SIZE(conn); }

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
void session_destroy(void *conn_arg) {
    cip_conn_p conn = conn_arg;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    if(!conn) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Session ptr is null!");

        return;
    }

    /* so remove the conn from the list so no one else can reference it. */
    session_list_remove(conn->owner_list, conn);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Session sent %" PRId64 " packets.", conn->packet_count);

    /* terminate the conn thread first. */
    atomic_set_int32(&conn->terminating, 1);

    /* signal the condition variable in case it is waiting */
    if(conn->session_wait_cond) { cond_signal(conn->session_wait_cond); }

    /* get rid of the handler thread. */
    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Destroying session thread.");
    if(conn->handler_thread) {
        /* this cannot be guarded by the mutex since the conn thread also locks it. */
        thread_join(conn->handler_thread);

        /* FIXME - is this critical block needed? */
        critical_block(conn->session_mutex) {
            thread_destroy(&(conn->handler_thread));
            conn->handler_thread = NULL;
        }
    }


    /* this needs to be handled in the mutex to prevent double frees due to queued requests. */
    critical_block(conn->session_mutex) {
        /* close off the connection if is one. This helps the PLC clean up. */
        if(conn->targ_connection_id) {
            /*
             * we do not want the internal loop to immediately
             * return, so set the flag like we are not terminating.
             * There is still a timeout that applies.
             */
            atomic_set_int32(&conn->terminating, 0);
            perform_forward_close(conn);
            atomic_set_int32(&conn->terminating, 1);
        }

        /* try to be nice and un-register the conn */
        if(conn->session_handle) { session_unregister(conn); }

        if(conn->sock) { session_close_socket(conn); }

        /* release all the requests that are in the queue. */
        if(conn->requests) {
            for(int i = 0; i < vector_length(conn->requests); i++) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "rc_dec: Releasing request reference.");
                cip_request_p req = vector_get(conn->requests, i);

                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "rc_dec: Releasing request for tag %" PRId32 ".", req->tag_id);

                rc_dec(req);
            }

            vector_destroy(conn->requests);
            conn->requests = NULL;
        }
    }

    /* we are done with the condition variable, finally destroy it. */
    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Destroying session condition variable.");
    if(conn->session_wait_cond) {
        cond_destroy(&(conn->session_wait_cond));
        conn->session_wait_cond = NULL;
    }

    /* we are done with the mutex, finally destroy it. */
    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Destroying session mutex.");
    if(conn->session_mutex) {
        mutex_destroy(&(conn->session_mutex));
        conn->session_mutex = NULL;
    }

    if(!conn->data_buffer_is_static) { mem_free(conn->data); }

    /* these are all allocated in one large block. */


    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return;
}


int receive_forward_open_response(cip_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    /*
     * Never wait unboundedly for the Forward Open response.  A dropped connection must
     * surface as an error so the handler can log it and re-enter the connect/retry path
     * instead of parking.
     */
    rc = recv_eip_response(conn, SESSION_DEFAULT_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to receive Forward Open response.");
        return rc;
    }

    rc = cip_decode_forward_open_response(conn);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return rc;
}


int process_requests(cip_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;
    cip_request_p request = NULL;
    cip_request_p bundled_requests[MAX_REQUESTS] = {NULL};
    int num_bundled_requests = 0;
    int remaining_request_space = 0;
    int remaining_response_space = 0;
    int allow_packing = 0;


    pdebug(DEBUG_MODULE_CIP, DEBUG_SPEW, 0, "Starting.");

    if(!conn) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Null session pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_SPEW, 0, "Checking for requests to process.");

    rc = PLCTAG_STATUS_OK;
    request = NULL;
    conn->data_size = 0;
    conn->data_offset = 0;

    /* grab a request off the front of the list. */
    critical_block(conn->session_mutex) {
        // FIXME - no logging in a mutex!
        // pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, "FIXME: available payload space %d", available_payload);

        /* is there anything to do? */
        if(vector_length(conn->requests)) {
            /* get rid of all aborted requests. */
            purge_aborted_requests_unsafe(conn);

            /* if there are still requests after purging all the aborted requests, process them. */

            /*
             * The total allowed space for requests is the negotiated packet capacity
             * less the overhead of the CPF data item. The rest of the space is for
             * the EIP encapsulation header and the CPF header and the CPF address item,
             * which are already accounted for in the buffer structure.
             */
            remaining_request_space = session_get_available_cip_payload_space(conn);

            /*
             * The reply has its own budget.  Everything the packed responses need
             * has to fit in one response packet, and the PLC will simply fail the
             * whole exchange if it does not, so track it alongside the request side.
             */
            remaining_response_space = GET_MAX_PAYLOAD_SIZE(conn) - CIP_MSP_REPLY_OVERHEAD - CIP_MSP_REPLY_SLACK;

            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Available payload space is %d bytes, reply space is %d bytes.",
                   remaining_request_space, remaining_response_space);

            /*
             * The logic below is a bit convoluted.
             *
             * - If the first request takes up all the space, we cannot pack any more requests.
             *
             * - If the first request is packable, we can keep packing requests
             *   until we run out of space or we reach the maximum number of requests.  We need to make sure
             *   that the overhead of the CIP packed request header is accounted for in the remaining space as well as the
             *   two-byte offset entry for each request.
             *
             * - If we are packing requests, and the next one is not packable, we stop packing.
             *
             * - If the first request is not packable, we can only pack it
             *   if it is the first one in the queue. And then can pack no more
             *   requests after that.
             */

            if(vector_length(conn->requests)) {
                /* Always process the first request, regardless of packability */
                request = vector_get(conn->requests, 0);
                int first_request_size = get_payload_size(request);

                /* Check if the first request fits at all */
                if(first_request_size <= remaining_request_space) {
                    bundled_requests[num_bundled_requests] = request;
                    num_bundled_requests++;
                    remaining_request_space -= first_request_size;
                    remaining_response_space -= reply_budget_cost(request);
                    vector_remove(conn->requests, 0);

                    /*
                     * A first read does not know how large its own reply will be, so it
                     * cannot be budgeted against the reply space and must not be packed.
                     *
                     * Multiple Service Packet also only ever goes out on a connection.
                     * pack_requests() reads the connected framing out of the first request,
                     * so bundling unconnected requests would have it parse the wrong layout.
                     */
                    allow_packing = request->allow_packing && !request->first_read && conn->use_connected_msg;

                    /* If the first request is packable, try to pack more requests */
                    if(allow_packing && vector_length(conn->requests) > 0) {
                        /* Account for CIP multi-request overhead now that we know we'll have multiple requests */
                        remaining_request_space -= (int)sizeof(cip_multi_req_header);

                        /* Account for 2-byte offset entry per request (including the first one already processed) */
                        int multi_request_overhead = CIP_MSP_OFFSET_ENTRY_SIZE;
                        remaining_request_space -= multi_request_overhead; /* for the first request */

                        while(vector_length(conn->requests) > 0 && num_bundled_requests < MAX_REQUESTS) {

                            request = vector_get(conn->requests, 0);

                            /*
                             * Only pack if this request is packable, its reply can be budgeted,
                             * and it is framed the same way as the first one -- pack_requests()
                             * handles one format per packet.
                             */
                            allow_packing = request->allow_packing && !request->first_read
                                            && (request->unrouted == bundled_requests[0]->unrouted);
                            if(!allow_packing) { break; }

                            int next_request_size = get_payload_size(request) + multi_request_overhead;

                            /* Check if this request fits in remaining space */
                            if(next_request_size > remaining_request_space) { break; }

                            /* and that its reply still fits in the single response packet */
                            int next_response_space = remaining_response_space - reply_budget_cost(request);
                            if(next_response_space < 0) { break; }

                            bundled_requests[num_bundled_requests] = request;
                            num_bundled_requests++;
                            remaining_request_space -= next_request_size;
                            remaining_response_space = next_response_space;
                            vector_remove(conn->requests, 0);
                        }
                    }
                    /* If first request is not packable, we stop here (only the first request is packed) */
                } else {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                           "First request size %d exceeds remaining space %d, cannot process any requests.", first_request_size,
                           remaining_request_space);
                }
            } else {
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "All requests in queue were aborted, nothing to do.");
            }
        }
    }

    if(num_bundled_requests > 0) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "%d requests to process.", num_bundled_requests);

        do {
            /* copy and pack the requests into the conn buffer. */
            /* FIXME - pack_requests() only returns PLCTAG_STATUS_OK */
            rc = pack_requests(conn, bundled_requests, num_bundled_requests);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error while packing requests, %s!", plc_tag_decode_error(rc));
                break;
            }

            /* fill in all the necessary parts to the request. */
            if((rc = prepare_request(conn)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to prepare request, %s!", plc_tag_decode_error(rc));
                break;
            }

            /* send the request */
            if((rc = send_eip_request(conn, SESSION_DEFAULT_TIMEOUT)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error sending packet %s!", plc_tag_decode_error(rc));
                break;
            }

            /* wait for the response */
            if((rc = recv_eip_response(conn, SESSION_DEFAULT_TIMEOUT)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error receiving packet response %s!", plc_tag_decode_error(rc));
                break;
            }

            /*
             * check the CIP status, but only if this is a bundled
             * response.   If it is a singleton, then we pass the
             * status back to the tag.
             */
            if(num_bundled_requests > 1) {
                cip_multi_resp_header *multi_resp = NULL;

                if(le2h16(((eip_encap *)(conn->data))->encap_command) == CIP_EIP_UNCONNECTED_SEND) {
                    eip_cip_uc_resp *resp = (eip_cip_uc_resp *)(conn->data);
                    uint16_t udi_item_length = 0;
                    size_t response_overhead = 0;
                    size_t response_size = 0;

                    /* we only know we got an EIP header, so check before reading CPF/CIP fields. */
                    if((size_t)conn->data_size < sizeof(*resp)) {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                               "Unconnected response of %u bytes is too short to hold a CIP response of %d bytes!",
                               conn->data_size, (int)sizeof(*resp));
                        rc = PLCTAG_ERR_TOO_SMALL;
                        break;
                    }

                    udi_item_length = le2h16(resp->cpf_udi_item_length);

                    multi_resp = (cip_multi_resp_header *)(&(resp->reply_service));

                    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Received unconnected packet with session sequence ID %llx",
                           resp->encap_sender_context);

                    /* punt if we got an overall error or it is not a partial/bundled error. */
                    if(resp->status != CIP_EIP_OK && resp->status != CIP_ERR_PARTIAL_ERROR) {
                        rc = decode_cip_error_code(&(resp->status),
                                                   cip_error_data_size(&resp->status, conn->data + conn->data_size));
                        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Command failed! (%d/%d) %s", resp->status, rc,
                               plc_tag_decode_error(rc));
                        break;
                    }

                    response_overhead = (size_t)((uint8_t *)multi_resp - conn->data);
                    response_size = (size_t)conn->data_size - response_overhead;

                    /* check the passed UDI data item size against what we really got. */
                    if((size_t)udi_item_length != response_size) {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                               "Incorrectly constructed response! UDI data length field is %zu but actual size is %zu!",
                               (size_t)udi_item_length, response_size);

                        rc = PLCTAG_ERR_BAD_DATA;
                        break;
                    }
                } else if(le2h16(((eip_encap *)(conn->data))->encap_command) == CIP_EIP_CONNECTED_SEND) {
                    eip_cip_co_resp *resp = (eip_cip_co_resp *)(conn->data);
                    uint16_t cdi_item_length = 0;
                    size_t response_overhead = 0;
                    size_t response_size = 0;

                    /* we only know we got an EIP header, so check before reading CPF/CIP fields. */
                    if((size_t)conn->data_size < sizeof(*resp)) {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                               "Connected response of %u bytes is too short to hold a CIP response of %d bytes!",
                               conn->data_size, (int)sizeof(*resp));
                        rc = PLCTAG_ERR_TOO_SMALL;
                        break;
                    }

                    cdi_item_length = le2h16(resp->cpf_cdi_item_length);

                    multi_resp = (cip_multi_resp_header *)(&(resp->reply_service));

                    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0,
                           "Received connected packet with connection ID %x and sequence ID %u(%x)",
                           le2h32(resp->cpf_orig_conn_id), le2h16(resp->cpf_conn_seq_num), le2h16(resp->cpf_conn_seq_num));

                    /* punt if we got an overall error or it is not a partial/bundled error. */
                    if(resp->status != CIP_EIP_OK && resp->status != CIP_ERR_PARTIAL_ERROR) {
                        size_t status_size = cip_error_data_size(&resp->status, conn->data + conn->data_size);

                        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Response status=%u", resp->status);
                        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Received CIP error %s (%s).",
                               decode_cip_error_long(&resp->status, status_size),
                               decode_cip_error_short(&resp->status, status_size));
                        rc = decode_cip_error_code(&(resp->status), status_size);
                        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Command failed! (%d/%d) %s", resp->status, rc,
                               plc_tag_decode_error(rc));
                        break;
                    }

                    response_overhead = (size_t)((uint8_t *)(&resp->cpf_conn_seq_num) - conn->data);
                    response_size = (size_t)conn->data_size - response_overhead;

                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "response_overhead=%zu", response_overhead);
                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "response_size=%zu", response_size);

                    /* check the passed CDI data item size against what we really got. */
                    if((size_t)cdi_item_length != response_size) {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                               "Incorrectly constructed response! CDI data length field is %zu but actual size is %zu!",
                               (size_t)cdi_item_length, response_size);

                        rc = PLCTAG_ERR_BAD_DATA;
                        break;
                    }
                } else {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unexpected EIP packet type, %04x!",
                           le2h16(((eip_encap *)(conn->data))->encap_command));
                    rc = PLCTAG_ERR_BAD_DATA;
                    break;
                }

                /*
                 * The count word and the offset array that follows it are both past the
                 * fixed part of the response we checked above, and the array is sized by a
                 * count the PLC controls.  A short response with a large count would have us
                 * reading offsets out of the buffer to decide whether the offsets are in the
                 * buffer, so bound the whole header before touching any of it.
                 */
                {
                    size_t offsets_start =
                        (size_t)((uint8_t *)multi_resp - conn->data) + offsetof(cip_multi_resp_header, request_offsets);
                    size_t offsets_size = (size_t)num_bundled_requests * sizeof(uint16_le);

                    if(offsets_start > (size_t)conn->data_size || offsets_size > (size_t)conn->data_size - offsets_start) {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                               "Response of %d bytes is too short to hold %d packed response offsets!", conn->data_size,
                               num_bundled_requests);
                        rc = PLCTAG_ERR_TOO_SMALL;
                        break;
                    }
                }

                /* we have multiple requests, sanity check the data. */
                if(le2h16(multi_resp->request_count) == num_bundled_requests) {
                    size_t offset_base = (size_t)((uint8_t *)(&multi_resp->request_count) - conn->data);

                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "offset_base=%zu", offset_base);

                    /* check all the offsets */
                    for(int resp_index = 0; resp_index < num_bundled_requests; resp_index++) {
                        size_t resp_offset = (size_t)le2h16(multi_resp->request_offsets[resp_index]) + offset_base;

                        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Response %d starts at byte offset %zu", resp_index,
                               resp_offset);

                        if(resp_offset >= (size_t)conn->data_size) {
                            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                                   "Response %d has offset %zu which is outside the session data!", resp_index, resp_offset);
                            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                            break;
                        }
                    }
                } else {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Expected %d packed responses back but got %zu!",
                           num_bundled_requests, (size_t)le2h16(multi_resp->request_count));
                    rc = PLCTAG_ERR_BAD_DATA;
                    break;
                }
            }

            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Got error %s when processing incoming response(s)!",
                       plc_tag_decode_error(rc));
                break;
            }

            /* copy the results back out. Every request gets a copy. */
            for(int i = 0; i < num_bundled_requests; i++) {
                rc = unpack_response(conn, bundled_requests[i], i);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, bundled_requests[i]->tag_id, "Unable to unpack response!");
                    break;
                }

                /* release our reference */
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, bundled_requests[i]->tag_id,
                       "rc_dec: Releasing request reference.");
                bundled_requests[i] = rc_dec(bundled_requests[i]);
            }

            rc = PLCTAG_STATUS_OK;
        } while(0);

        /* problem? push the requests back on the queue. */
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error sending or receiving requests!");

            pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Pushing %d requests back into the queue.", num_bundled_requests);

            /* conn->requests is also written by session_add_request() (tickler thread)
             * under conn->session_mutex, so this push-back needs the same lock. */
            critical_block(conn->session_mutex) {
                for(int i = num_bundled_requests - 1; i >= 0; i--) {
                    if(bundled_requests[i]) { vector_insert(conn->requests, 0, bundled_requests[i]); }
                }
            }
        }

        /* tickle the main tickler thread to note that we have responses. */
        plc_tag_tickler_wake();
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_SPEW, 0, "Done.");

    return rc;
}
void session_set_connection_status(cip_conn_p conn, int32_t new_status) {
    critical_block(conn->session_mutex) {
        int32_t old_status = atomic_get_int32(&conn->watch.status);

        atomic_set_int32(&conn->watch.status, new_status);

        if(old_status != new_status) {
            conn_watch_publish(&conn->watch, new_status + PLCTAG_EVENT_CONN_STATUS_OFFSET, PLCTAG_STATUS_OK);
        }
    }
}


THREAD_FUNC(session_handler) {
    cip_conn_p conn = arg;
    int rc = PLCTAG_STATUS_OK;
    session_state_t state = SESSION_OPEN_SOCKET_START;
    int64_t now = 0;
    int64_t timeout_time = 0;
    int64_t wait_until_time = 0;
    int32_t inactivity_timeout_ms = atomic_get_int32(&conn->connection_inactivity_timeout_ms);
    int64_t auto_disconnect_time = time_ms() + inactivity_timeout_ms;
    backoff_t retry_backoff;
    int64_t retry_wait_ms = 0;
    int auto_disconnect = 0;


    backoff_init(&retry_backoff, RETRY_WAIT_INITIAL_MS, RETRY_WAIT_MAX_MS);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting thread for session %p", conn);

    /* Increment the count of active conn handlers */
    atomic_add_int32(&(conn->owner_list->handler_count), 1);

    while(!atomic_get_int32(&conn->terminating) && atomic_get_bool(&lib_active)) {
        now = time_ms();

        /* how long should we wait if nothing wakes us? */
        wait_until_time = now + SESSION_IDLE_WAIT_TIME;

        /*
         * Do this on every cycle.   This keeps the queue clean(ish).
         *
         * Make sure we get rid of all the aborted requests queued.
         * This keeps the overall memory usage lower.
         */

        pdebug(DEBUG_MODULE_CIP, DEBUG_SPEW, 0, "Critical block.");
        critical_block(conn->session_mutex) { purge_aborted_requests_unsafe(conn); }

        switch(state) {
            case SESSION_OPEN_SOCKET_START:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_OPEN_SOCKET_START state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

                /* we must connect to the gateway*/
                rc = session_open_socket(conn);
                if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Session connect failed %s!", plc_tag_decode_error(rc));
                    state = SESSION_CLOSE_SOCKET;
                } else {
                    if(rc == PLCTAG_STATUS_OK) {
                        /* bump auto disconnect time into the future so that we do not accidentally disconnect immediately. */
                        inactivity_timeout_ms = atomic_get_int32(&conn->connection_inactivity_timeout_ms);
                        auto_disconnect_time = now + inactivity_timeout_ms;

                        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                               "Connect complete immediately, going to state SESSION_REGISTER.");

                        state = SESSION_REGISTER;
                    } else {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                               "Connect started, going to state SESSION_OPEN_SOCKET_WAIT.");

                        state = SESSION_OPEN_SOCKET_WAIT;
                    }
                }

                /* in all cases, don't wait. */
                cond_signal(conn->session_wait_cond);

                break;

            case SESSION_OPEN_SOCKET_WAIT:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_OPEN_SOCKET_WAIT state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

                /* we must connect to the gateway */
                rc = socket_connect_tcp_check(conn->sock, 20); /* MAGIC */
                if(rc == PLCTAG_STATUS_OK) {
                    /* connected! */
                    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Socket connection succeeded.");

                    /* calculate the disconnect time. */
                    inactivity_timeout_ms = atomic_get_int32(&conn->connection_inactivity_timeout_ms);
                    auto_disconnect_time = now + inactivity_timeout_ms;

                    state = SESSION_REGISTER;
                } else if(rc == PLCTAG_ERR_TIMEOUT) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Still waiting for connection to succeed.");

                    /* don't wait more.  The TCP connect check will wait in select(). */
                } else {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Session connect failed %s!", plc_tag_decode_error(rc));

                    state = SESSION_CLOSE_SOCKET;
                }

                /* in all cases, don't wait. */
                cond_signal(conn->session_wait_cond);

                break;

            case SESSION_REGISTER:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_REGISTER state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

                if((rc = session_register(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Session registration failed %s!", plc_tag_decode_error(rc));
                    state = SESSION_CLOSE_SOCKET;
                } else {
                    /*
                     * The session is up.  Restart the backoff here and not at socket connect:
                     * both the immediate and the asynchronous connect paths pass through this
                     * state, and a socket that opens but will not register is still a failure.
                     */
                    backoff_reset(&retry_backoff);

                    if(conn->use_connected_msg) {
                        state = SESSION_SEND_FORWARD_OPEN;
                    } else {
                        state = SESSION_IDLE;
                    }
                }
                cond_signal(conn->session_wait_cond);
                break;

            case SESSION_SEND_FORWARD_OPEN:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_SEND_FORWARD_OPEN state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

                if((rc = send_forward_open_request(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Send Forward Open failed %s!", plc_tag_decode_error(rc));
                    state = SESSION_UNREGISTER;
                } else {

                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                           "Send Forward Open succeeded, going to SESSION_RECEIVE_FORWARD_OPEN state.");
                    state = SESSION_RECEIVE_FORWARD_OPEN;
                }
                cond_signal(conn->session_wait_cond);
                break;

            case SESSION_RECEIVE_FORWARD_OPEN:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_RECEIVE_FORWARD_OPEN state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

                if((rc = receive_forward_open_response(conn)) != PLCTAG_STATUS_OK) {
                    if(rc == PLCTAG_ERR_DUPLICATE) {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                               "Duplicate connection error received, trying again with different connection ID.");
                        state = SESSION_SEND_FORWARD_OPEN;
                    } else if(rc == PLCTAG_ERR_TOO_LARGE) {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                               "Requested packet size too large, retrying with smaller size.");
                        state = SESSION_SEND_FORWARD_OPEN;
                    } else if(rc == PLCTAG_ERR_UNSUPPORTED && !conn->only_use_old_forward_open) {
                        /* if we got an unsupported error and we are trying with ForwardOpenEx, then try the old command. */
                        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                               "PLC does not support ForwardOpenEx, trying old ForwardOpen.");
                        conn->only_use_old_forward_open = 1;
                        state = SESSION_SEND_FORWARD_OPEN;
                    } else {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Receive Forward Open failed %s!",
                               plc_tag_decode_error(rc));
                        state = SESSION_UNREGISTER;
                    }
                } else {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Send Forward Open succeeded, going to SESSION_IDLE state.");
                    state = SESSION_IDLE;
                }
                cond_signal(conn->session_wait_cond);
                break;

            case SESSION_IDLE:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_IDLE state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_UP);

                /* make sure that our timeout period has not changed */
                if(inactivity_timeout_ms != atomic_get_int32(&conn->connection_inactivity_timeout_ms)) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                           "Inactivity timeout changed from %" PRId32 "ms to %" PRId32 "ms, updating auto disconnect time.",
                           inactivity_timeout_ms, atomic_get_int32(&conn->connection_inactivity_timeout_ms));
                    inactivity_timeout_ms = atomic_get_int32(&conn->connection_inactivity_timeout_ms);
                    auto_disconnect_time = now + inactivity_timeout_ms;
                }

                /* if there is work to do, make sure we do not disconnect. */
                critical_block(conn->session_mutex) {
                    int num_reqs = vector_length(conn->requests);
                    if(num_reqs > 0) {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                               "There are %d requests pending before cleanup and sending.", num_reqs);
                        inactivity_timeout_ms = atomic_get_int32(&conn->connection_inactivity_timeout_ms);
                        auto_disconnect_time = now + inactivity_timeout_ms;
                    }
                }

                if((rc = process_requests(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error while processing requests %s!",
                           plc_tag_decode_error(rc));
                    if(conn->use_connected_msg) {
                        state = SESSION_DISCONNECT;
                    } else {
                        state = SESSION_UNREGISTER;
                    }

                    cond_signal(conn->session_wait_cond);
                }

                /* check if we should disconnect */
                if(auto_disconnect_time < now) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Disconnecting due to inactivity.");

                    auto_disconnect = 1;

                    if(conn->use_connected_msg) {
                        state = SESSION_DISCONNECT;
                    } else {
                        state = SESSION_UNREGISTER;
                    }
                    cond_signal(conn->session_wait_cond);
                }

                /* if there is work to do, make sure we signal the condition var. */
                critical_block(conn->session_mutex) {
                    int num_reqs = vector_length(conn->requests);
                    if(num_reqs > 0) {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                               "There are %d requests still pending after abort purge and sending.", num_reqs);
                        cond_signal(conn->session_wait_cond);
                    }
                }

                break;

            case SESSION_DISCONNECT:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_DISCONNECT state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_DISCONNECTING);

                if((rc = perform_forward_close(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Forward close failed %s!", plc_tag_decode_error(rc));
                }

                state = SESSION_UNREGISTER;
                cond_signal(conn->session_wait_cond);
                break;

            case SESSION_UNREGISTER:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_UNREGISTER state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_DISCONNECTING);

                if((rc = session_unregister(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unregistering session failed %s!", plc_tag_decode_error(rc));
                }

                state = SESSION_CLOSE_SOCKET;
                cond_signal(conn->session_wait_cond);
                break;

            case SESSION_CLOSE_SOCKET:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_CLOSE_SOCKET state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_DOWN);

                if((rc = session_close_socket(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Closing session socket failed %s!", plc_tag_decode_error(rc));
                }

                if(auto_disconnect) {
                    state = SESSION_WAIT_IDLE_RECONNECT;
                } else {
                    state = SESSION_START_RETRY;
                }
                cond_signal(conn->session_wait_cond);
                break;

            case SESSION_START_RETRY:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_START_RETRY state.");

                /* FIXME - make the backoff bounds tag attributes. */
                retry_wait_ms = backoff_wait_ms(&retry_backoff);

                timeout_time = now + retry_wait_ms;

                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                       "Retry %" PRIu32 ", waiting %" PRId64 "ms before reconnecting.", retry_backoff.attempts,
                       retry_wait_ms);

                /* start waiting. */
                state = SESSION_WAIT_ERR_RETRY;

                cond_signal(conn->session_wait_cond);
                break;

            case SESSION_WAIT_ERR_RETRY:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_WAIT_ERR_RETRY state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_ERR_WAIT);

                if(timeout_time < now) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Transitioning to SESSION_OPEN_SOCKET_START.");
                    state = SESSION_OPEN_SOCKET_START;
                    cond_signal(conn->session_wait_cond);
                } else {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Wait not complete, still %dms to go.",
                           (int)(timeout_time - now));
                }

                break;

            case SESSION_WAIT_IDLE_RECONNECT:
                /* wait for at least one request to queue before reconnecting. */
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_WAIT_IDLE_RECONNECT state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_IDLE_WAIT);

                auto_disconnect = 0;

                /* if there is work to do, reconnect.. */
                pdebug(DEBUG_MODULE_CIP, DEBUG_SPEW, 0, "Critical block.");
                critical_block(conn->session_mutex) {
                    if(vector_length(conn->requests) > 0) {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                               "There are requests waiting, reopening connection to PLC.");

                        state = SESSION_OPEN_SOCKET_START;
                        cond_signal(conn->session_wait_cond);
                    }
                }

                break;


            default:
                pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, 0, "Unknown state %d!", state);

                /* FIXME - this logic is not complete.  We might be here without
                 * a connected conn or a registered conn. */

                if(conn->use_connected_msg) {
                    state = SESSION_DISCONNECT;
                } else {
                    state = SESSION_UNREGISTER;
                }

                cond_signal(conn->session_wait_cond);
                break;
        }

        /*
         * give up the CPU a bit, but only if we are not
         * doing some linked states.
         */
        if(wait_until_time > 0) {
            int64_t time_left = wait_until_time - now;

            if(time_left > 0) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Waiting up to %" PRId64 "ms for something to happen.",
                       time_left);
                cond_wait(conn->session_wait_cond, (int)time_left);
            }
        }
    }

    /*
     * One last time before we exit.
     */
    critical_block(conn->session_mutex) { purge_aborted_requests_unsafe(conn); }

    /* Decrement the count of active conn handlers */
    atomic_add_int32(&(conn->owner_list->handler_count), -1);

    THREAD_RETURN(0);
}


cip_conn_p session_create_from_profile(cip_conn_list_t *list, const cip_conn_profile_t *profile, const char *host,
                                       const char *path, int *use_connected_msg, int connection_group_id) {
    const int max_payload_capacity = profile->max_payload_capacity;
    const bool data_buffer_is_static = true;
    static volatile uint32_t connection_id = 0;

    int rc = PLCTAG_STATUS_OK;
    cip_conn_p conn = NULL;
    size_t total_allocation_size = sizeof(*conn);
    /* the connection buffer holds the whole assembled packet: framing, route path and payload. */
    size_t data_buffer_capacity = (size_t)CIP_MAX_FRAMING_SIZE + (size_t)max_payload_capacity;
    size_t data_buffer_offset = 0;
    size_t host_name_offset = 0;
    size_t host_name_size = 0;
    size_t path_offset = 0;
    size_t path_size = 0;
    size_t conn_path_offset = 0;
    uint8_t tmp_conn_path[MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN];
    int tmp_conn_path_size = MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN;
    int is_dhp = 0;
    uint16_t dhp_dest = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    /*
     * The host string is copied into this allocation verbatim, so its length is part of the
     * conn's size.  It comes straight from the "gateway" attribute with nothing between
     * the application and here, so bound it: without this a caller can size the conn
     * object arbitrarily.
     */
    if(!host || str_length(host) >= MAX_SESSION_HOST_LEN) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Gateway string is missing or longer than the maximum of %d bytes!",
               MAX_SESSION_HOST_LEN - 1);
        return NULL;
    }

    /*
     * The path string is copied in verbatim too, and it is not self-limiting: spaces are
     * skipped everywhere in cip_encode_path(), so an arbitrarily long string can still
     * encode to a valid short path.  Bound it against the encoded path buffer -- a real
     * route is a handful of hops, so this rejects nothing that describes real hardware.
     */
    if(path && str_length(path) >= MAX_CONN_PATH) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Path string is longer than the maximum of %d bytes!", MAX_CONN_PATH - 1);
        return NULL;
    }

    if(*use_connected_msg) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Session should use connected messaging.");
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Session should not use connected messaging.");
    }

    /* add in space for the data buffer. */
    if(data_buffer_is_static) {
        data_buffer_offset = total_allocation_size;
        total_allocation_size += data_buffer_capacity;
    } else {
        data_buffer_offset = 0;
    }

    /* add in space for the host name.  + 1 for the NUL terminator. */
    host_name_offset = total_allocation_size;
    host_name_size = (size_t)str_length(host) + 1;
    total_allocation_size += host_name_size;

    /* add in space for the path copy. */
    if(path && str_length(path) > 0) {
        path_offset = total_allocation_size;
        path_size = (size_t)str_length(path) + 1;
        total_allocation_size += path_size;
    } else {
        path_offset = 0;
    }

    /* encode the path */
    /*
     * The shared encoder knows only whether the family can bridge DH+, not which AB
     * family this is.  PLC-5, SLC and MicroLogix can; everything else rejects a DH+
     * segment rather than ignoring it.
     */
    int dhp_kind = (profile->dhp_capable ? CIP_PLC_KIND_DHP_CAPABLE : CIP_PLC_KIND_OTHER);

    rc = cip_encode_path(path, use_connected_msg, dhp_kind, &tmp_conn_path[0], &tmp_conn_path_size, &is_dhp, &dhp_dest);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Unable to convert path string to binary path, error %s!",
               plc_tag_decode_error(rc));
        return NULL;
    }

    conn_path_offset = total_allocation_size;
    total_allocation_size += (size_t)tmp_conn_path_size;

    /* allocate the conn struct and the buffer in the same allocation. */
    pdebug(
        DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
        "Allocating %d total bytes of memory with %d bytes for data buffer static data, %d bytes for the host name, %d bytes for the path, %d bytes for the encoded path.",
        total_allocation_size, (data_buffer_is_static ? data_buffer_capacity : 0), str_length(host) + 1,
        (path_offset == 0 ? 0 : str_length(path) + 1), tmp_conn_path_size);

    conn = (cip_conn_p)rc_alloc((int)total_allocation_size, session_destroy);
    if(!conn) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error allocating new session!");
        return NULL;
    }

    /* fill in the interior pointers */

    /* fix up the data buffer. */
    conn->data_buffer_is_static = data_buffer_is_static;
    conn->data_capacity = (uint32_t)data_buffer_capacity;

    if(data_buffer_is_static) {
        conn->data = (uint8_t *)(conn) + data_buffer_offset;
    } else {
        conn->data = (uint8_t *)mem_alloc((int)data_buffer_capacity);
        if(conn->data == NULL) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to allocate the connection data buffer!");
            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "rc_dec: Releasing session reference.");
            return rc_dec(conn);
        }
    }

    /* point the host pointer just after the data. */
    conn->host = (char *)(conn) + host_name_offset;
    str_copy(conn->host, (int)host_name_size, host);

    if(path_offset) {
        conn->path = (char *)(conn) + path_offset;
        str_copy(conn->path, (int)path_size, path);
    }

    if(conn_path_offset) {
        conn->conn_path = (uint8_t *)(conn) + conn_path_offset;

        // FIXME - the path length cannot be 8 bits with a buffer length that is over 260.
        conn->conn_path_size = (uint8_t)tmp_conn_path_size;
        mem_copy(conn->conn_path, tmp_conn_path, tmp_conn_path_size);
    }


    /*
        TO DO
            remove mem_free from destructor for host, path, and conn_path.
    */

    conn->requests = vector_create(SESSION_MIN_REQUESTS, SESSION_INC_REQUESTS);
    if(!conn->requests) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to allocate vector for requests!");
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "rc_dec: Releasing session reference.");
        rc_dec(conn);
        return NULL;
    }

    /* check for ID set up. This does not need to be thread safe since we just need a random value. */
    if(connection_id == 0) { connection_id = (uint32_t)(random_u64(UINT32_MAX) + 1); }

    /* fix up the rest of teh fields */
    conn->owner_list = list;
    conn->plc_type = profile->plc_type;
    conn->min_payload_size = profile->min_payload_size;
    conn->dhp_capable = profile->dhp_capable;

    /* what the family will negotiate for, straight off the row */
    conn->only_use_old_forward_open = profile->only_use_old_forward_open;
    conn->fo_conn_size = profile->fo_conn_size;
    conn->fo_ex_conn_size = profile->fo_ex_conn_size;
    conn->max_payload_size = (uint16_t)profile->fo_conn_size;
    conn->use_connected_msg = *use_connected_msg;
    conn->conn_serial_number = (uint16_t)(random_u64(UINT16_MAX) + 1);
    conn->session_seq_id = (uint64_t)(random_u64(UINT32_MAX) + 1);
    conn->is_dhp = is_dhp;
    conn->dhp_dest = dhp_dest;
    conn_watch_init(&conn->watch, PLCTAG_CONN_STATUS_DOWN);

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Setting connection_group_id to %d.", connection_group_id);
    conn->connection_group_id = connection_group_id;

    /*
     * Why is connection_id global?  Because it looks like the PLC might
     * be treating it globally.  I am seeing ForwardOpen errors that seem
     * to be because of duplicate connection IDs even though the conn
     * was closed.
     *
     * So, this is more or less unique across all invocations of the library.
     * FIXME - this could collide.  The probability is low, but it could happen
     * as there are only 32 bits.
     */
    conn->orig_connection_id = ++connection_id;

    /* create the conn mutex. */
    if((rc = mutex_create(&(conn->session_mutex))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to create session mutex!");
        return rc_dec(conn);
    }

    /* create the conn condition variable. */
    if((rc = cond_create(&(conn->session_wait_cond))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to create session condition var!");
        return rc_dec(conn);
    }

    /* add the new conn to the list. */
    session_list_add_unsafe(list, conn);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done");

    return conn;
}
int cip_conn_find_or_create(cip_conn_list_t *list, const cip_conn_profile_t *profile, attr attribs,
                            cip_conn_p *tag_conn, int *is_new_conn) {
    /*int debug = attr_get_int(attribs,"debug",0);*/
    const char *session_gw = attr_get_str(attribs, "gateway", "");
    const char *session_path = attr_get_str(attribs, "path", "");
    int use_connected_msg = attr_get_int(attribs, "use_connected_msg", 0);
    cip_conn_p session = NULL;
    int new_session = 0;
    /*
     * Connections are shared by default.  Two spellings of the same deprecated
     * attribute reached here from the two dialects, so both are still honoured.
     */
    int shared_session = attr_get_int(attribs, "share_session", attr_get_int(attribs, "share_conn", 1));
    int rc = PLCTAG_STATUS_OK;
    int connection_inactivity_timeout_ms = SESSION_DISCONNECT_TIMEOUT;
    int connection_group_id = attr_get_int(attribs, "connection_group_id", 0);
    int only_use_old_forward_open = attr_get_int(attribs, "conn_only_use_old_forward_open", 0);

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Starting");

    /* both are subsumed by connection_group_id.  Warn only if the tag string actually set one. */
    if(attr_get_str(attribs, "share_session", NULL)) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
               "The attribute \"share_session\" is deprecated and will be removed.  Use \"connection_group_id\" instead.");
    }

    if(attr_get_str(attribs, "share_conn", NULL)) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
               "The attribute \"share_conn\" is deprecated and will be removed.  Use \"connection_group_id\" instead.");
    }

    connection_inactivity_timeout_ms = attr_get_int(attribs, "connection_inactivity_timeout_ms", SESSION_DISCONNECT_TIMEOUT);
    if(connection_inactivity_timeout_ms < 1 || connection_inactivity_timeout_ms > SESSION_DISCONNECT_TIMEOUT) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
               "Invalid connection_inactivity_timeout_ms %d. Must be between 1 and %d. Using default %d.",
               connection_inactivity_timeout_ms, SESSION_DISCONNECT_TIMEOUT, SESSION_DISCONNECT_TIMEOUT);
        connection_inactivity_timeout_ms = SESSION_DISCONNECT_TIMEOUT;
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Setting connection_inactivity_timeout_ms to %dms.",
               connection_inactivity_timeout_ms);
    }

    critical_block(list->mutex) {
        /* if we are to share connections, then look for an existing one. */
        if(shared_session) {
            session = session_list_find_by_host_unsafe(list, session_gw, session_path, connection_group_id);
        } else {
            /* no sharing, create a new one */
            session = NULL;
        }

        if(session == NULL) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Creating new session.");

            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Creating %s session.", profile->name);

            /* a stateless family cannot use connected messaging whatever the tag asked for */
            if(profile->force_unconnected) { use_connected_msg = 0; }

            session = session_create_from_profile(list, profile, session_gw, session_path, &use_connected_msg,
                                                  connection_group_id);

            if(session == NULL) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "unable to create or find a session!");
                rc = PLCTAG_ERR_BAD_GATEWAY;
            } else {
                atomic_init_int32(&session->connection_inactivity_timeout_ms, connection_inactivity_timeout_ms);

                /* see if we have an attribute set for forcing the use of the older ForwardOpen */
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                       "Passed attribute to prohibit use of extended ForwardOpen is %d.", only_use_old_forward_open);
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                       "Existing attribute to prohibit use of extended ForwardOpen is %d.", session->only_use_old_forward_open);
                session->only_use_old_forward_open = (session->only_use_old_forward_open ? 1 : only_use_old_forward_open);

                new_session = 1;
            }
        } else {
            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Reusing existing session.");
        }
    }

    /*
     * Make sure that we have created the mutex and cond var first.
     */

    if(new_session) {
        if((rc = thread_create((thread_p *)&(session->handler_thread), session_handler, 32 * 1024, session))
           != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to create session thread!");
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "rc_dec: Releasing session reference.");
            rc_dec(session);
            session = NULL;
        } else {
            /* save the status */
        }
    }

    /* store it into the tag */
    *tag_conn = session;

    if(is_new_conn) { *is_new_conn = new_session; }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Done");

    return rc;
}


/*
 * Bring a module's whole connection list down.
 *
 * The order matters: the list is emptied and freed first, but the mutex cannot
 * go until the handler threads have finished, because a handler may still be
 * in its own cleanup holding conn->session_mutex.  An allocator that returns
 * freed pages to the OS immediately turns any use-after-free here into a
 * SIGSEGV rather than something harmless.
 */
void session_list_teardown(cip_conn_list_t *list, debug_module_t debug_module) {
    int64_t start_time = 0;
    int active_count = 0;

    pdebug(debug_module, DEBUG_INFO, 0, "Starting.");

    /* tell every connection to stop, and wake its handler so it notices now */
    pdebug(debug_module, DEBUG_INFO, 0, "Marking all open connections for termination.");

    if(list->conns && list->mutex) {
        critical_block(list->mutex) {
            int num_conns = vector_length(list->conns);

            for(int i = 0; i < num_conns; i++) {
                cip_conn_p conn = vector_get(list->conns, i);

                if(conn) {
                    atomic_set_int32(&conn->terminating, 1);
                    if(conn->session_wait_cond) { cond_signal(conn->session_wait_cond); }
                }
            }
        }
    }

    if(list->conns && list->mutex) {
        int remaining_conns = 0;

        pdebug(debug_module, DEBUG_DETAIL, 0, "Waiting for connections to terminate.");

        start_time = time_ms();

        while(1) {
            critical_block(list->mutex) { remaining_conns = vector_length(list->conns); }

            if(remaining_conns == 0) { break; }

            if(time_ms() - start_time >= SESSION_TEARDOWN_TIMEOUT_MS) {
                pdebug(debug_module, DEBUG_WARN, 0, "Timeout waiting for %d connections to terminate.", remaining_conns);
                break;
            }

            sleep_ms(10);
        }

        if(remaining_conns == 0) { pdebug(debug_module, DEBUG_DETAIL, 0, "Connections all terminated."); }

        vector_destroy(list->conns);
        list->conns = NULL;
    }

    pdebug(debug_module, DEBUG_DETAIL, 0, "Waiting for handler threads to complete.");

    start_time = time_ms();

    while((active_count = atomic_get_int32(&(list->handler_count))) > 0) {
        int64_t elapsed = time_ms() - start_time;

        if(elapsed >= SESSION_TEARDOWN_TIMEOUT_MS) {
            pdebug(debug_module, DEBUG_WARN, 0, "Timeout waiting for %d handler threads to complete.", active_count);
            break;
        }

        pdebug(debug_module, DEBUG_DETAIL, 0, "Waiting for %d handler threads to complete. Elapsed: %" PRId64 "ms",
               active_count, elapsed);

        sleep_ms(20);
    }

    if(active_count == 0) { pdebug(debug_module, DEBUG_INFO, 0, "All handler threads completed."); }

    pdebug(debug_module, DEBUG_DETAIL, 0, "Destroying connection list mutex.");

    if(list->mutex) {
        mutex_destroy(&(list->mutex));
        list->mutex = NULL;
    }

    pdebug(debug_module, DEBUG_INFO, 0, "Done.");
}
