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
#include <libplctag/lib/conn_attribs.h>
#include <libplctag/lib/conn_timeouts.h>
#include <libplctag/lib/conn_watch.h>
#include <libplctag/lib/connection_tag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/lib/tag_driver.h>
#include <libplctag/modules/cip/cip.h>
#include <libplctag/modules/cip/cip_tag_create.h>
#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/cip/error_codes.h>
#include <libplctag/modules/cip/family.h>
#include <libplctag/modules/cip/path.h>
#include <libplctag/modules/cip/tag.h>
#include <libplctag/modules/cip/wire.h>
#include <limits.h>
#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/backoff.h>
#include <utils/debug.h>
#include <utils/random_utils.h>
#include <utils/rc.h>
#include <utils/vector.h>


static int session_list_init(cip_conn_list_t *list);
static int32_t session_list_count(cip_conn_list_t *list);
static void session_list_teardown(cip_conn_list_t *list, debug_module_t debug_module);
static int32_t build_exchange_unsafe(cip_conn_p conn, int64_t now);
static int dispatch_replies_unsafe(cip_conn_p conn);
static void requeue_slots_unsafe(cip_conn_p conn);
static void finish_tag_unsafe(cip_conn_p conn, cip_tag_p tag, int rc);
static int32_t fail_waiting_tags_unsafe(cip_conn_p conn, int rc);
static int find_or_create_on_list(cip_conn_list_t *list, const cip_family_t *family, attr attribs, cip_conn_p *tag_conn,
                                  int *is_new_conn);
static int session_create(cip_conn_list_t *list, const cip_family_t *family, const char *host, const char *path,
                          int use_connected_msg, int connection_group_id, cip_conn_p *conn_out);


/*
 * Every CIP connection, Rockwell and OMRON alike.  The family's PLC type is part of the
 * match key, so connections for two families never match each other.
 */
static cip_conn_list_t conn_list = {0};

/* the Message Router, which a connected path ends at.  Not const: mem_copy() takes a mutable source. */
static uint8_t cip_router_path[] = {0x20, 0x02, 0x24, 0x01};


int cip_conn_startup(void) { return session_list_init(&conn_list); }


void cip_conn_teardown(void) { session_list_teardown(&conn_list, DEBUG_MODULE_CIP); }


int32_t cip_conn_live_count(void) { return session_list_count(&conn_list); }


int cip_conn_find_or_create(cip_conn_p *tag_conn, attr attribs, int *is_new_conn) {
    const cip_family_t *family = cip_family_from_attributes(attribs);
    bool names_plc = (attr_get_str(attribs, "plc", attr_get_str(attribs, "cpu", NULL)) != NULL);

    /* no PLC named: the connection reads the Identity object to find out. */
    if(names_plc && !family) { return PLCTAG_ERR_BAD_DEVICE; }

    return find_or_create_on_list(&conn_list, family, attribs, tag_conn, is_new_conn);
}


const cip_family_t *cip_conn_family(cip_conn_p conn, int *identify_rc) {
    const cip_family_t *family = NULL;

    critical_block(conn->session_mutex) {
        family = conn->family;
        *identify_rc = conn->identify_rc;
    }

    return family;
}


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
               conn->max_payload_size, conn->family->fo_conn_size, conn->family->fo_ex_conn_size, max_payload_size);

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


static int session_list_init(cip_conn_list_t *list) {
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


static int32_t session_list_count(cip_conn_list_t *list) {
    int32_t count = 0;

    critical_block(list->mutex) { count = (int32_t)vector_length(list->conns); }

    return count;
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
                                            int connection_group_id, cip_plc_type_t plc_type) {
    for(int i = 0; i < vector_length(list->conns); i++) {
        cip_conn_p conn = vector_get(list->conns, i);

        /* is this connection in the process of destruction? */
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "rc_inc: Acquiring connection reference.");
        conn = rc_inc(conn);
        if(conn) {
            /*
             * The family is part of the key, not just the address.  Two families can name the
             * same gateway and path -- `plc=lgx` and `plc=lgx-pccc` are the same physical PLC
             * addressed two ways -- and they cannot share a connection: the family decides the
             * payload sizing, the messaging and the tag setup the connection hands out.  The
             * path is in the key too, so the dialect the path chose matches as well.
             */
            if(conn->connection_group_id == connection_group_id && conn->plc_type == plc_type
               && session_match_valid(host, path, conn)) {
                return conn;
            }

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


/*
 * Connection sizes to fall back to, largest first.
 *
 * These are not the opening ask -- that comes from the PLC family's own sizes, which is what
 * has been negotiating successfully all along.  This is only where to go after the PLC
 * refuses, so every rung is a size some real device is known to accept.
 *
 * 4002 is the measured Logix ceiling: 4082 comes back as an invalid connection size and
 * 4002 succeeds.  504 and 500 are the pre-extended-Forward-Open sizes, 500 being what
 * bridging through ControlNet leaves.  244 is PCCC embedded in CIP and 92 is PCCC on the
 * older SLC and MicroLogix.  1900 is for the OMRON models that sit between.
 *
 * Anything above CIP_FO_PLAIN_MAX_SIZE needs the extended Forward Open, so dropping past
 * that rung is also what decides to stop using it.
 */
static const uint16_t forward_open_size_ladder[] = {4002, 1900, 504, 500, 244, 92};

#define FORWARD_OPEN_LADDER_LEN ((int32_t)(sizeof(forward_open_size_ladder) / sizeof(forward_open_size_ladder[0])))


/* the largest rung at or below limit, or -1 when the ladder has none that small. */
static int32_t forward_open_rung_at_or_below(uint16_t limit) {
    for(int32_t idx = 0; idx < FORWARD_OPEN_LADDER_LEN; idx++) {
        if(forward_open_size_ladder[idx] <= limit) { return idx; }
    }

    return -1;
}


/*
 * Drop to the next connection size worth asking for.  Returns false when there is none,
 * which is the end of the negotiation rather than another round trip.
 */
static bool forward_open_step_size_down(cip_conn_p conn) {
    uint16_t limit = (conn->max_payload_guess > 0 ? (uint16_t)(conn->max_payload_guess - 1) : 0);
    int32_t next_idx = 0;

    /*
     * A PLC that names a size it supports saves several round trips, but it cannot be
     * taken at its word: Logix firmware v32 and later has been seen naming 4002 and then
     * refusing a request for exactly 4002.  Treat it as a ceiling rather than an answer
     * and land on a rung at or below it, so the next refusal still has somewhere to go.
     */
    if(conn->fo_offered_size > 0 && conn->fo_offered_size < limit) { limit = conn->fo_offered_size; }

    next_idx = forward_open_rung_at_or_below(limit);

    if(next_idx < 0 || forward_open_size_ladder[next_idx] < conn->family->min_payload_size) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
               "No connection size left to try at or below %u bytes that still clears the %u bytes this protocol needs "
               "for a single request!",
               limit, conn->family->min_payload_size);
        return false;
    }

    conn->fo_ladder_idx = next_idx;

    critical_block(conn->session_mutex) { conn->max_payload_guess = forward_open_size_ladder[next_idx]; }

    /* the size decides the service: only the extended Forward Open can carry a large one. */
    if(forward_open_size_ladder[next_idx] <= CIP_FO_PLAIN_MAX_SIZE) { conn->only_use_old_forward_open = true; }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Retrying the Forward Open at %u bytes using the %s service.",
           conn->max_payload_guess, (conn->only_use_old_forward_open ? "plain" : "extended"));

    return true;
}


/*
 * Errors no further attempt can get past.  A wrong route or a device that does not do
 * connected messaging needs a person, not another round trip.
 *
 * PLCTAG_ERR_TOO_SMALL is deliberately not one of them.  Running out of connection sizes
 * to offer says what the PLC answered this time, not what it is configured to do, and a
 * device that refuses every size while it is busy starting up will accept one later.
 *
 * Nor is a network link that is offline (0x0800), though it maps to the same error as a port
 * that does not exist.  It is no different from a PLC that does not answer at its address: it
 * may be in maintenance or down for repair, and it will be back.
 */
static bool forward_open_error_is_configuration(cip_conn_p conn, int rc) {
    if(conn->fo_ext_status == CIP_EXT_ERR_LINK_OFFLINE) { return false; }

    return (rc == PLCTAG_ERR_BAD_DEVICE || rc == PLCTAG_ERR_BAD_PARAM || rc == PLCTAG_ERR_UNSUPPORTED);
}


/*
 * Start the negotiation over.  The size the PLC agreed to last time is kept -- the device
 * at the far end is almost always the same one -- but every count of how hard we had to
 * work to get there is cleared, so a reconnect gets a full set of retries.
 */
static void forward_open_reset_negotiation(cip_conn_p conn) {
    conn->fo_offered_size = 0;
    conn->fo_dup_retries = 0;
    conn->fo_tried_plain = false;
}


int send_forward_open_request(cip_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;
    uint16_t max_payload;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Flag prohibiting use of extended ForwardOpen is %d.",
           conn->only_use_old_forward_open);

    max_payload = (uint16_t)(conn->only_use_old_forward_open ? conn->family->fo_conn_size : conn->family->fo_ex_conn_size);

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

    /* the handler counts packets until it exits. */
    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Session sent %" PRId64 " packets.", conn->packet_count);


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

        /* every tag holds a reference to its connection, so no tag is left on the list. */
        tag_driver_destroy(&conn->driver);

        if(conn->staging) {
            mem_free(conn->staging);
            conn->staging = NULL;
        }

        conn_watch_destroy(&conn->watch);
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


/*
 * Read the Identity object and take the family from it.  Runs on the handler thread
 * before any tag request, so nothing else is using the buffer.
 *
 * A network failure is retried like any other connect failure.  An answer is not: a CIP
 * error, or a device the classifier refuses, is recorded so the waiting tags fail with it.
 */
static int session_identify(cip_conn_p conn) {
    const cip_family_t *family = NULL;
    uint16_t vendor_id = 0;
    char product_name[256];
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    rc = cip_encode_identity_request(conn);
    if(rc == PLCTAG_STATUS_OK) { rc = prepare_request(conn); }
    if(rc == PLCTAG_STATUS_OK) {
        conn->data_offset = 0;
        rc = send_eip_request(conn, SESSION_DEFAULT_TIMEOUT);
    }
    if(rc == PLCTAG_STATUS_OK) { rc = recv_eip_response(conn, SESSION_DEFAULT_TIMEOUT); }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to exchange the Identity request, %s!", plc_tag_decode_error(rc));
        return rc;
    }

    rc = cip_decode_identity_response(conn, &vendor_id, product_name, sizeof(product_name));
    if(rc == PLCTAG_STATUS_OK) { rc = cip_family_from_identity(vendor_id, product_name, conn->conn_path_size > 0, &family); }

    critical_block(conn->session_mutex) {
        if(rc != PLCTAG_STATUS_OK) {
            conn->identify_rc = rc;
        } else {
            conn->family = family;
            conn->only_use_old_forward_open = (conn->only_use_old_forward_open || family->only_use_old_forward_open);
            conn->max_payload_size = (uint16_t)family->fo_conn_size;
            /* ponytail: Logix's -1 means connected; unconnected Logix needs plc=lgx. */
            conn->use_connected_msg = (family->use_connected_msg != 0);

            /*
             * What cip_encode_path() appends for a connected family.  It could not when the
             * connection was created, and the Identity request had to go without it.
             */
            if(conn->use_connected_msg) {
                mem_copy(conn->conn_path + conn->conn_path_size, cip_router_path, (int)sizeof(cip_router_path));
                conn->conn_path_size = (uint8_t)(conn->conn_path_size + sizeof(cip_router_path));
            }
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Device refused, %s!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Identified a %s, %s.", family->name,
           (conn->use_connected_msg ? "connected" : "unconnected"));

    return PLCTAG_STATUS_OK;
}


int receive_forward_open_response(cip_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting");

    /*
     * Never wait unboundedly for the Forward Open response.  A dropped connection must
     * surface as an error so the handler can log it and re-enter the connect/retry path
     * instead of parking.
     */
    conn->fo_plc_answered = false;

    rc = recv_eip_response(conn, SESSION_DEFAULT_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to receive Forward Open response.");
        return rc;
    }

    conn->fo_plc_answered = true;

    rc = cip_decode_forward_open_response(conn);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done.");

    return rc;
}


int process_requests(cip_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;
    int32_t slot_count = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_SPEW, 0, "Starting.");

    if(!conn) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Null session pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    conn->data_size = 0;
    conn->data_offset = 0;

    critical_block(conn->session_mutex) {
        int64_t now = time_ms();

        tag_driver_tickle_due_unsafe(&conn->driver, now);
        slot_count = build_exchange_unsafe(conn, now);
    }

    if(slot_count > 0) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "%d requests to process.", (int)slot_count);

        do {
            rc = pack_exchange(conn);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error while packing requests, %s!", plc_tag_decode_error(rc));
                break;
            }

            /* fill in all the necessary parts to the request. */
            if((rc = prepare_request(conn)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to prepare request, %s!", plc_tag_decode_error(rc));
                break;
            }

            /*
             * A request on an established connection waits for as long as the caller said
             * they would tolerate silence, not for SESSION_DEFAULT_TIMEOUT.
             *
             * SESSION_DEFAULT_TIMEOUT is the compile-time maximum -- 32000ms -- and it is
             * the right budget for Register Session and Forward Open, where the number on
             * the wire is that same maximum.  Applying it to every ordinary request means
             * one lost reply parks the connection, and every tag on it, for 32 seconds with
             * nothing else bounding it.  connection_inactivity_timeout_ms is the caller's
             * own statement of how long a silent connection may stay silent, so it is the
             * honest deadline for a request too, and it is settable per connection.
             *
             * Taken as given above CIP_REQUEST_MIN_TIMEOUT_MS, which conn_create()'s own
             * clamp already keeps it well clear of.  A caller who lowers this is asking to
             * find out quickly that a PLC has stopped answering, and that is the whole
             * point of lowering it, so the floor sits where a reply is physically
             * impossible rather than merely slow.  A bridged path -- DH+ behind a gateway,
             * several hops -- wants a larger value and gets one without doing anything: the
             * attribute is 31000ms unless someone sets it.
             */
            int32_t request_timeout_ms = atomic_get_int32(&conn->connection_inactivity_timeout_ms);

            if(request_timeout_ms < CIP_REQUEST_MIN_TIMEOUT_MS) { request_timeout_ms = CIP_REQUEST_MIN_TIMEOUT_MS; }

            /* send the request */
            if((rc = send_eip_request(conn, (int)request_timeout_ms)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error sending packet %s!", plc_tag_decode_error(rc));
                break;
            }

            /* wait for the response */
            if((rc = recv_eip_response(conn, (int)request_timeout_ms)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error receiving packet response %s!", plc_tag_decode_error(rc));
                break;
            }

            /*
             * check the CIP status, but only if this is a bundled
             * response.   If it is a singleton, then we pass the
             * status back to the tag.
             */
            if(conn->slot_count > 1) {
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
                    size_t offsets_size = (size_t)conn->slot_count * sizeof(uint16_le);

                    if(offsets_start > (size_t)conn->data_size || offsets_size > (size_t)conn->data_size - offsets_start) {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                               "Response of %d bytes is too short to hold %d packed response offsets!", conn->data_size,
                               conn->slot_count);
                        rc = PLCTAG_ERR_TOO_SMALL;
                        break;
                    }
                }

                /* we have multiple requests, sanity check the data. */
                if(le2h16(multi_resp->request_count) == conn->slot_count) {
                    size_t offset_base = (size_t)((uint8_t *)(&multi_resp->request_count) - conn->data);

                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "offset_base=%zu", offset_base);

                    /* check all the offsets */
                    for(int resp_index = 0; resp_index < conn->slot_count; resp_index++) {
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
                           conn->slot_count, (size_t)le2h16(multi_resp->request_count));
                    rc = PLCTAG_ERR_BAD_DATA;
                    break;
                }
            }

        } while(0);

        critical_block(conn->session_mutex) {
            if(rc == PLCTAG_STATUS_OK) { rc = dispatch_replies_unsafe(conn); }

            /* a failed exchange is not an answer; whatever is left goes again once the connection is back. */
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Got error %s when processing incoming response(s)!",
                       plc_tag_decode_error(rc));
                requeue_slots_unsafe(conn);
            }

            conn->slot_count = 0;
        }
    }

    tag_driver_notify(&conn->driver);

    pdebug(DEBUG_MODULE_CIP, DEBUG_SPEW, 0, "Done.");

    return rc;
}


/*
 * Build the next exchange from the tags whose operations are due: encode them into
 * conn->staging until one does not fit or cannot share the packet.  Returns the number of
 * requests in it.
 *
 * Encoding needs the tag's API mutex, and this holds session_mutex, so it only tries for it.
 * A tag whose mutex is busy keeps its place and is taken next time.  A tag that cannot be
 * encoded at all is finished with the error.
 */
static int32_t build_exchange_unsafe(cip_conn_p conn, int64_t now) {
    int32_t request_space = session_get_available_cip_payload_space(conn);
    int32_t reply_space = (int32_t)GET_MAX_PAYLOAD_SIZE(conn) - CIP_MSP_REPLY_OVERHEAD - CIP_MSP_REPLY_SLACK;
    int32_t used = 0;
    int32_t index = 0;
    bool bundle = false;

    conn->slot_count = 0;

    while(index < vector_length(conn->driver.active) && conn->slot_count < MAX_REQUESTS) {
        cip_tag_p tag = vector_get(conn->driver.active, index);
        tag_op_plan_t plan = {0};
        bool is_read = tag_op_is_read(tag);
        tag_encode_func encode = is_read ? tag->vtable->encode_read : tag->vtable->encode_write;
        int32_t capacity = request_space;
        int rc = PLCTAG_STATUS_OK;

        /* the list is in time order, so nothing after this is due either. */
        if(tag->op_time > now) { break; }

        /* armed for an auto-sync deadline, or waiting for its tickle: no request to send. */
        if(!tag_op_busy(tag)) {
            index++;
            continue;
        }

        if(mutex_try_lock(tag->api_mutex) != PLCTAG_STATUS_OK) {
            index++;
            continue;
        }

        /* a bundle charges its header once and an offset entry for every request in it. */
        if(conn->slot_count > 0) {
            capacity = request_space - used - (int32_t)sizeof(cip_multi_req_header)
                       - (CIP_MSP_OFFSET_ENTRY_SIZE * (conn->slot_count + 1));
        }

        if(!encode) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "This tag cannot %s!", (is_read ? "read" : "write"));
            rc = PLCTAG_ERR_UNSUPPORTED;
        } else if(capacity <= 0) {
            rc = PLCTAG_ERR_TOO_SMALL;
        } else {
            rc = ((cip_tag_encode_func)encode)(tag, conn->staging + used, capacity, &plan);
        }

        /* it may fit next time, at the front of a packet of its own. */
        if(rc == PLCTAG_ERR_TOO_SMALL && conn->slot_count > 0) {
            mutex_unlock(tag->api_mutex);
            break;
        }

        /* alone in an empty packet, it never will. */
        if(rc == PLCTAG_ERR_TOO_SMALL) { rc = PLCTAG_ERR_TOO_LARGE; }

        if(rc != PLCTAG_STATUS_OK) {
            finish_tag_unsafe(conn, tag, rc);
            mutex_unlock(tag->api_mutex);
            continue;
        }

        if(conn->slot_count > 0
           && (!bundle || !plan.packable || tag->unrouted != conn->exchange_unrouted
               || reply_space - reply_budget_cost(plan.reply_size) < 0)) {
            mutex_unlock(tag->api_mutex);
            break;
        }

        if(conn->slot_count == 0) {
            /* Multiple Service Packet only ever goes out on a connection. */
            bundle = (plan.packable && conn->use_connected_msg);
            conn->exchange_unrouted = tag->unrouted;
        }

        conn->slots[conn->slot_count].tag = tag;
        conn->slots[conn->slot_count].offset = used;
        conn->slots[conn->slot_count].size = plan.req_size;
        conn->slots[conn->slot_count].is_read = is_read;
        conn->slot_count++;

        used += plan.req_size;
        reply_space -= reply_budget_cost(plan.reply_size);

        /* off the list while it is on the wire; it keeps its op_time in case it has to go again. */
        tag_driver_forget_unsafe(&conn->driver, (plc_tag_p)tag);
        mutex_unlock(tag->api_mutex);

        if(!bundle) { break; }
    }

    return conn->slot_count;
}


/*
 * Hand each reply in the exchange to its tag's decoder.  A decoder that needs another
 * exchange puts the tag back on the list, due now; otherwise the operation is over.
 *
 * Decoding needs the tag's API mutex.  When it is busy this lets go of session_mutex while it
 * waits, so that whoever holds the API mutex can take session_mutex to abort the tag: never
 * blocking on one while holding the other is what keeps the two from deadlocking.
 */
static int dispatch_replies_unsafe(cip_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;
    int bad_packet_rc = PLCTAG_STATUS_OK;

    for(int32_t i = 0; i < conn->slot_count; i++) {
        cip_tag_p tag = conn->slots[i].tag;
        bool is_read = conn->slots[i].is_read;
        tag_decode_func decode = NULL;
        uint8_t *reply = NULL;
        int32_t reply_size = 0;

        while(tag && mutex_try_lock(tag->api_mutex) != PLCTAG_STATUS_OK) {
            mutex_unlock(conn->session_mutex);
            sleep_ms(1);
            mutex_lock(conn->session_mutex);
            tag = conn->slots[i].tag;
        }

        /* aborted or destroyed while the exchange was out. */
        if(!tag) { continue; }

        conn->slots[i].tag = NULL;

        /*
         * A reply that fails its own framing checks is the device's answer, and asking again
         * gets the same one, so it ends the operation.  The caller drops the connection.
         */
        rc = find_reply(conn, i, &reply, &reply_size);
        if(rc != PLCTAG_STATUS_OK) {
            bad_packet_rc = rc;
        } else {
            decode = is_read ? tag->vtable->decode_read : tag->vtable->decode_write;
            rc = decode ? ((cip_tag_decode_func)decode)(tag, reply, reply_size) : PLCTAG_ERR_UNSUPPORTED;
        }

        if(rc == PLCTAG_STATUS_PENDING) {
            tag->status = (int8_t)PLCTAG_STATUS_PENDING;
            tag_driver_queue_unsafe(&conn->driver, (plc_tag_p)tag, time_ms());
        } else {
            finish_tag_unsafe(conn, tag, rc);
        }

        mutex_unlock(tag->api_mutex);
    }

    /* the next fragment, or tags that did not fit this packet, are due now: skip the idle tick. */
    if(conn->slot_count > 0) { cond_signal(conn->session_wait_cond); }

    return bad_packet_rc;
}


/* put what is left of a failed exchange back where it was on the list, in order. */
static void requeue_slots_unsafe(cip_conn_p conn) {
    for(int32_t i = 0; i < conn->slot_count; i++) {
        cip_tag_p tag = conn->slots[i].tag;

        if(tag) {
            tag_driver_queue_unsafe(&conn->driver, (plc_tag_p)tag, tag->op_time);
            conn->slots[i].tag = NULL;
        }
    }
}


/*
 * End the tag's operation with rc: its status and completion events, its next auto-sync
 * deadline if it has one, and its callbacks once session_mutex is released.  Called with
 * session_mutex and the tag's API mutex held.
 */
static void finish_tag_unsafe(cip_conn_p conn, cip_tag_p tag, int rc) {
    cip_tag_op_done(tag, rc);
    tag_driver_done_unsafe(&conn->driver, (plc_tag_p)tag, time_ms());
}


/*
 * Fail every tag waiting to send on the connection with rc.  For a Forward Open the PLC
 * refused: the tags were waiting for a connection the PLC has said it will not give, so its
 * answer is theirs.  Tags only armed for an auto-sync deadline have nothing waiting and stay.
 *
 * The refusal comes once and the next attempt may be a long backoff away, so a tag whose API
 * mutex is busy is waited for rather than skipped -- with session_mutex released, as in
 * dispatch_replies_unsafe(), and the scan restarted because the list may have changed.
 */
static int32_t fail_waiting_tags_unsafe(cip_conn_p conn, int rc) {
    int32_t failed = 0;
    int32_t index = 0;

    while(index < vector_length(conn->driver.active)) {
        cip_tag_p tag = vector_get(conn->driver.active, index);

        if(!tag_op_busy(tag)) {
            index++;
            continue;
        }

        if(mutex_try_lock(tag->api_mutex) != PLCTAG_STATUS_OK) {
            mutex_unlock(conn->session_mutex);
            sleep_ms(1);
            mutex_lock(conn->session_mutex);
            index = 0;
            continue;
        }

        finish_tag_unsafe(conn, tag, rc);
        failed++;

        mutex_unlock(tag->api_mutex);

        /* finishing may have re-armed it further along the list or taken it off; start over. */
        index = 0;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Failed %d waiting tags with %s.", (int)failed, plc_tag_decode_error(rc));

    return failed;
}


int cip_conn_queue_tag(cip_conn_p conn, struct cip_tag_t *tag) {
    if(!conn || !tag) { return PLCTAG_ERR_NULL_PTR; }

    critical_block(conn->session_mutex) { tag_driver_queue_unsafe(&conn->driver, (plc_tag_p)tag, time_ms()); }

    cond_signal(conn->session_wait_cond);

    return PLCTAG_STATUS_OK;
}


void cip_conn_forget_tag(cip_conn_p conn, struct cip_tag_t *tag) {
    if(!conn || !tag) { return; }

    critical_block(conn->session_mutex) {
        tag_driver_forget_unsafe(&conn->driver, (plc_tag_p)tag);

        for(int32_t i = 0; i < conn->slot_count; i++) {
            if(conn->slots[i].tag == tag) { conn->slots[i].tag = NULL; }
        }
    }
}
void session_set_connection_status(cip_conn_p conn, int32_t new_status) {
    conn_watch_set_status(&conn->watch, conn->session_mutex, new_status);
}


THREAD_FUNC(session_handler) {
    cip_conn_p conn = arg;
    int rc = PLCTAG_STATUS_OK;
    conn->state = SESSION_OPEN_SOCKET_START;
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

        switch(conn->state) {
            case SESSION_OPEN_SOCKET_START:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_OPEN_SOCKET_START state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

                /* we must connect to the gateway*/
                rc = session_open_socket(conn);
                if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Session connect failed %s!", plc_tag_decode_error(rc));
                    conn->state = SESSION_CLOSE_SOCKET;
                } else {
                    if(rc == PLCTAG_STATUS_OK) {
                        /* bump auto disconnect time into the future so that we do not accidentally disconnect immediately. */
                        inactivity_timeout_ms = atomic_get_int32(&conn->connection_inactivity_timeout_ms);
                        auto_disconnect_time = now + inactivity_timeout_ms;

                        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                               "Connect complete immediately, going to state SESSION_REGISTER.");

                        conn->state = SESSION_REGISTER;
                    } else {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                               "Connect started, going to state SESSION_OPEN_SOCKET_WAIT.");

                        conn->state = SESSION_OPEN_SOCKET_WAIT;
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

                    conn->state = SESSION_REGISTER;
                } else if(rc == PLCTAG_ERR_TIMEOUT) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Still waiting for connection to succeed.");

                    /* don't wait more.  The TCP connect check will wait in poll() (POSIX) or select() (Windows). */
                } else {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Session connect failed %s!", plc_tag_decode_error(rc));

                    conn->state = SESSION_CLOSE_SOCKET;
                }

                /* in all cases, don't wait. */
                cond_signal(conn->session_wait_cond);

                break;

            case SESSION_REGISTER:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_REGISTER state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

                if((rc = session_register(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Session registration failed %s!", plc_tag_decode_error(rc));
                    conn->state = SESSION_CLOSE_SOCKET;
                } else if(!conn->family) {
                    conn->state = SESSION_IDENTIFY;
                } else {
                    if(conn->use_connected_msg) {
                        /* a fresh session gets a fresh set of Forward Open retries. */
                        forward_open_reset_negotiation(conn);

                        conn->state = SESSION_SEND_FORWARD_OPEN;
                    } else {
                        conn->state = SESSION_IDLE;
                    }
                }
                cond_signal(conn->session_wait_cond);
                break;

            case SESSION_IDENTIFY:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_IDENTIFY state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

                if((rc = session_identify(conn)) != PLCTAG_STATUS_OK) {
                    /* a refusal is the answer for the tags waiting to be set up. */
                    critical_block(conn->session_mutex) { tag_driver_tickle_creating_unsafe(&conn->driver); }
                    tag_driver_notify(&conn->driver);

                    conn->state = SESSION_UNREGISTER;
                } else if(conn->use_connected_msg) {
                    forward_open_reset_negotiation(conn);
                    conn->state = SESSION_SEND_FORWARD_OPEN;
                } else {
                    conn->state = SESSION_IDLE;
                }
                cond_signal(conn->session_wait_cond);
                break;

            case SESSION_SEND_FORWARD_OPEN:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_SEND_FORWARD_OPEN state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

                if((rc = send_forward_open_request(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Send Forward Open failed %s!", plc_tag_decode_error(rc));
                    conn->state = SESSION_UNREGISTER;
                } else {

                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                           "Send Forward Open succeeded, going to SESSION_RECEIVE_FORWARD_OPEN state.");
                    conn->state = SESSION_RECEIVE_FORWARD_OPEN;
                }
                cond_signal(conn->session_wait_cond);
                break;

            case SESSION_RECEIVE_FORWARD_OPEN:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_RECEIVE_FORWARD_OPEN state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

                rc = receive_forward_open_response(conn);

                if(rc == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Forward Open succeeded, going to SESSION_IDLE state.");

                    conn->state = SESSION_IDLE;
                    cond_signal(conn->session_wait_cond);
                    break;
                }

                /*
                 * A refusal is not always the end of the attempt.  The status code says what
                 * the PLC objected to and the negotiation state says whether anything about
                 * the request can still be changed; only both together decide.  Retries stay
                 * inside the session -- the socket and the registration are already up, and a
                 * PLC that will not grant one connection size may well grant another.
                 */
                conn->state = SESSION_UNREGISTER;

                if(rc == PLCTAG_ERR_DUPLICATE) {
                    conn->fo_dup_retries++;

                    if(conn->fo_dup_retries <= CIP_FO_MAX_DUP_RETRIES) {
                        /* the serial number is redrawn on every encode, so simply asking again is enough. */
                        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                               "Duplicate connection ID, retry %" PRIu32 " of %d with a new one.", conn->fo_dup_retries,
                               CIP_FO_MAX_DUP_RETRIES);

                        conn->state = SESSION_SEND_FORWARD_OPEN;
                    } else {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                               "The PLC refused %d connection IDs in a row; it has no connection to give us.",
                               CIP_FO_MAX_DUP_RETRIES);

                        rc = PLCTAG_ERR_NO_RESOURCES;
                    }
                } else if(rc == PLCTAG_ERR_TOO_LARGE || rc == PLCTAG_ERR_NO_RESOURCES) {
                    /*
                     * Both answers get a smaller request.  A size refusal obviously does.
                     * "No resources" does because some firmware answers that to an oversized
                     * request instead of naming a size, and because a device at its connection
                     * limit can still have room for a small connection.
                     */
                    if(forward_open_step_size_down(conn)) {
                        conn->state = SESSION_SEND_FORWARD_OPEN;
                    } else {
                        rc = PLCTAG_ERR_TOO_SMALL;
                    }
                } else if(rc == PLCTAG_ERR_UNSUPPORTED) {
                    if(!conn->only_use_old_forward_open) {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                               "PLC does not support the extended Forward Open, trying the plain one.");

                        conn->only_use_old_forward_open = true;
                        conn->fo_tried_plain = true;

                        conn->state = SESSION_SEND_FORWARD_OPEN;
                    } else {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, 0,
                               "The PLC refused both Forward Open services, so it does not support connected messaging.  "
                               "Set the use_connected_msg attribute to 0 for this device.");
                    }
                }

                if(conn->state == SESSION_UNREGISTER) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Forward Open failed, %s.", plc_tag_decode_error(rc));

                    /*
                     * Nothing the library can vary will fix a wrong route or a device that does
                     * not do connected messaging, so stop hammering it: saturate the backoff and
                     * reconnect at its slowest rate.  Still not permanent -- a route can come
                     * back and an empty slot can be filled.
                     */
                    if(forward_open_error_is_configuration(conn, rc)) { backoff_saturate(&retry_backoff); }

                    /*
                     * Only a refusal that says the request can never succeed is the answer for
                     * the tags waiting on this connection: no such route, a bad path, a device
                     * with no connected messaging.  The refusals that have a remedy -- a
                     * duplicate connection ID, a size too large, no extended Forward Open --
                     * were retried above, and running out of those retries, a timeout, or no
                     * reply at all says the PLC has nothing to give right now, not that it
                     * never will.  Those tags wait for the reconnect.
                     */
                    if(conn->fo_plc_answered && forward_open_error_is_configuration(conn, rc)) {
                        critical_block(conn->session_mutex) { fail_waiting_tags_unsafe(conn, rc); }

                        tag_driver_notify(&conn->driver);
                    }
                }

                cond_signal(conn->session_wait_cond);
                break;

            case SESSION_IDLE:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_IDLE state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_UP);

                /*
                 * Reaching this conn->state is the only thing that counts as a successful connect,
                 * so it is the only place the backoff restarts.  Every earlier step -- the
                 * socket, the registration, the Forward Open -- can succeed and still leave a
                 * session that cannot carry a request, and resetting at one of those makes the
                 * next step's failure retry at the floor rate for as long as it keeps failing.
                 */
                backoff_reset(&retry_backoff);

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
                    if(tag_driver_has_due_unsafe(&conn->driver, now)) {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "There are tags due before sending.");
                        inactivity_timeout_ms = atomic_get_int32(&conn->connection_inactivity_timeout_ms);
                        auto_disconnect_time = now + inactivity_timeout_ms;
                    }
                }

                if((rc = process_requests(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error while processing requests %s!",
                           plc_tag_decode_error(rc));
                    if(conn->use_connected_msg) {
                        conn->state = SESSION_DISCONNECT;
                    } else {
                        conn->state = SESSION_UNREGISTER;
                    }

                    cond_signal(conn->session_wait_cond);
                }

                /* check if we should disconnect */
                if(auto_disconnect_time < now) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Disconnecting due to inactivity.");

                    auto_disconnect = 1;

                    if(conn->use_connected_msg) {
                        conn->state = SESSION_DISCONNECT;
                    } else {
                        conn->state = SESSION_UNREGISTER;
                    }
                    cond_signal(conn->session_wait_cond);
                }


                break;

            case SESSION_DISCONNECT:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_DISCONNECT state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_DISCONNECTING);

                if((rc = perform_forward_close(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Forward close failed %s!", plc_tag_decode_error(rc));
                }

                conn->state = SESSION_UNREGISTER;
                cond_signal(conn->session_wait_cond);
                break;

            case SESSION_UNREGISTER:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_UNREGISTER state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_DISCONNECTING);

                if((rc = session_unregister(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unregistering session failed %s!", plc_tag_decode_error(rc));
                }

                conn->state = SESSION_CLOSE_SOCKET;
                cond_signal(conn->session_wait_cond);
                break;

            case SESSION_CLOSE_SOCKET:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_CLOSE_SOCKET state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_DOWN);

                if((rc = session_close_socket(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Closing session socket failed %s!", plc_tag_decode_error(rc));
                }

                if(auto_disconnect) {
                    conn->state = SESSION_WAIT_IDLE_RECONNECT;
                } else {
                    conn->state = SESSION_START_RETRY;
                }
                cond_signal(conn->session_wait_cond);
                break;

            case SESSION_START_RETRY:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_START_RETRY state.");

                /* FIXME - make the backoff bounds tag attributes. */
                /* the maximum is a live tag attribute, so it is read fresh on every retry. */
                backoff_set_max_ms(&retry_backoff, atomic_get_int32(&conn->connection_retry_max_delay_ms));

                retry_wait_ms = backoff_wait_ms(&retry_backoff);

                timeout_time = now + retry_wait_ms;

                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                       "Retry %" PRIu32 ", waiting %" PRId64 "ms before reconnecting.", retry_backoff.attempts,
                       retry_wait_ms);

                /* start waiting. */
                conn->state = SESSION_WAIT_ERR_RETRY;

                cond_signal(conn->session_wait_cond);
                break;

            case SESSION_WAIT_ERR_RETRY:
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "in SESSION_WAIT_ERR_RETRY state.");
                session_set_connection_status(conn, PLCTAG_CONN_STATUS_ERR_WAIT);

                if(timeout_time < now) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Transitioning to SESSION_OPEN_SOCKET_START.");
                    conn->state = SESSION_OPEN_SOCKET_START;
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
                    if(tag_driver_has_due_unsafe(&conn->driver, now)) {
                        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "There are tags due, reopening connection to PLC.");

                        conn->state = SESSION_OPEN_SOCKET_START;
                        cond_signal(conn->session_wait_cond);
                    }
                }

                break;


            default:
                pdebug(DEBUG_MODULE_CIP, DEBUG_ERROR, 0, "Unknown state %d!", conn->state);

                /* FIXME - this logic is not complete.  We might be here without
                 * a connected conn or a registered conn. */

                if(conn->use_connected_msg) {
                    conn->state = SESSION_DISCONNECT;
                } else {
                    conn->state = SESSION_UNREGISTER;
                }

                cond_signal(conn->session_wait_cond);
                break;
        }

        /*
         * give up the CPU a bit, but only if we are not
         * doing some linked states.
         */
        /* this pass's connection events, to the @connection tags watching. */
        connection_tag_notify_watchers(&conn->watch, conn->session_mutex);

        /*
         * Wake for the next tag deadline if it comes first, in the states that act on tags; a tag
         * already due that could not be served waits a tick.  The connecting states keep their
         * own pace, so a due tag does not spin the handler through a backoff.
         */
        if(conn->state == SESSION_IDLE || conn->state == SESSION_WAIT_IDLE_RECONNECT) {
            critical_block(conn->session_mutex) {
                int64_t next_tag_time = tag_driver_next_time_unsafe(&conn->driver);

                if(next_tag_time > 0 && wait_until_time > 0 && next_tag_time < wait_until_time) {
                    wait_until_time = (next_tag_time > now) ? next_tag_time : now + 1;
                }
            }
        }

        if(wait_until_time > 0) {
            int64_t time_left = wait_until_time - now;

            if(time_left > 0) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Waiting up to %" PRId64 "ms for something to happen.",
                       time_left);
                cond_wait(conn->session_wait_cond, (int)time_left);
            }
        }
    }

    /* Decrement the count of active conn handlers */
    atomic_add_int32(&(conn->owner_list->handler_count), -1);

    THREAD_RETURN(0);
}


/*
 * Create a connection for a PLC family.  The path is parsed here and only here, and the parse
 * picks the family: a DH+ path takes the family's DH+ form, anything else the family itself.
 */
static int session_create(cip_conn_list_t *list, const cip_family_t *family, const char *host, const char *path,
                          int use_connected_msg, int connection_group_id, cip_conn_p *conn_out) {
    const cip_family_t *dhp_family = (family ? family->dhp : NULL);
    const bool data_buffer_is_static = true;
    static volatile uint32_t connection_id = 0;

    int rc = PLCTAG_STATUS_OK;
    cip_conn_p conn = NULL;
    size_t total_allocation_size = sizeof(*conn);
    /* the connection buffer holds the whole assembled packet: framing, route path and payload. */
    size_t data_buffer_capacity = 0;
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
    bool is_bridged = false;

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
        return PLCTAG_ERR_BAD_GATEWAY;
    }

    /*
     * The path string is copied in verbatim too, and it is not self-limiting: spaces are
     * skipped everywhere in cip_encode_path(), so an arbitrarily long string can still
     * encode to a valid short path.  Bound it against the encoded path buffer -- a real
     * route is a handful of hops, so this rejects nothing that describes real hardware.
     */
    if(path && str_length(path) >= MAX_CONN_PATH) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Path string is longer than the maximum of %d bytes!", MAX_CONN_PATH - 1);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /*
     * The encoder knows only whether the family can bridge DH+.  One that cannot rejects a
     * DH+ segment rather than ignoring it.  A DH+ path forces a connection.
     */
    rc = cip_encode_path(path, &use_connected_msg, (dhp_family ? CIP_PLC_KIND_DHP_CAPABLE : CIP_PLC_KIND_OTHER),
                         &tmp_conn_path[0], &tmp_conn_path_size, &is_dhp, &dhp_dest, &is_bridged);
    if(rc != PLCTAG_STATUS_OK) {
        /* BAD_PARAM for a segment that does not parse, TOO_LARGE for a path that encodes too long. */
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to convert path string to binary path, error %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    if(is_dhp) { family = dhp_family; }

    /*
     * A PCCC CPU sits directly on a network, never in a ControlLogix chassis, so a path to one
     * must leave the gateway's chassis.  A DH+ path already does.
     */
    if(dhp_family && !is_dhp && str_length(path) && !is_bridged) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "A path to a %s PLC must bridge out of the chassis!", family->name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(family) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Creating %s connection, %s.", family->name,
               (use_connected_msg ? "connected" : "unconnected"));

        data_buffer_capacity = (size_t)CIP_MAX_FRAMING_SIZE + (size_t)family->max_payload_capacity;
    } else {
        /*
         * No PLC named, so the Identity probe picks the family.  The buffer has to exist before
         * that, so it is sized for the largest family.  The DH+ path the encoder rejected
         * above is the one route Identity cannot see past.
         */
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Creating a connection that will identify the device.");

        data_buffer_capacity = (size_t)CIP_MAX_FRAMING_SIZE + (size_t)CIP_FAMILY_MAX_PAYLOAD_CAPACITY;
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

    /* a probed connection may turn out to need the router suffix the encoder left off. */
    conn_path_offset = total_allocation_size;
    total_allocation_size += (size_t)tmp_conn_path_size + (family ? 0 : sizeof(cip_router_path));

    /* allocate the conn struct and the buffer in the same allocation. */
    pdebug(
        DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
        "Allocating %d total bytes of memory with %d bytes for data buffer static data, %d bytes for the host name, %d bytes for the path, %d bytes for the encoded path.",
        total_allocation_size, (data_buffer_is_static ? data_buffer_capacity : 0), str_length(host) + 1,
        (path_offset == 0 ? 0 : str_length(path) + 1), tmp_conn_path_size);

    conn = (cip_conn_p)rc_alloc((int)total_allocation_size, session_destroy);
    if(!conn) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Error allocating new session!");
        return PLCTAG_ERR_NO_MEM;
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
            rc_dec(conn);
            return PLCTAG_ERR_NO_MEM;
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

    if(tag_driver_init(&conn->driver) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to allocate the tag lists!");
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "rc_dec: Releasing session reference.");
        rc_dec(conn);
        return PLCTAG_ERR_NO_MEM;
    }

    /* the encoders write a whole exchange here; it can be no larger than the packet buffer. */
    conn->staging = (uint8_t *)mem_alloc((int)conn->data_capacity);
    if(!conn->staging) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to allocate the staging buffer!");
        rc_dec(conn);
        return PLCTAG_ERR_NO_MEM;
    }

    /* check for ID set up. This does not need to be thread safe since we just need a random value. */
    if(connection_id == 0) { connection_id = (uint32_t)(random_u64(UINT32_MAX) + 1); }

    /* fix up the rest of teh fields */
    conn->owner_list = list;
    conn->plc_type = (family ? family->plc_type : CIP_PLC_NONE);
    conn->family = family;
    conn->identify_rc = PLCTAG_STATUS_OK;

    /* the one family value the negotiation is allowed to change */
    conn->only_use_old_forward_open = (family ? family->only_use_old_forward_open : false);

    /* the opening ask comes from the family, so the fallback ladder has not been touched yet. */
    conn->fo_ladder_idx = -1;
    conn->fo_offered_size = 0;
    conn->fo_dup_retries = 0;
    conn->fo_tried_plain = false;
    /* a probed connection frames the Identity request unconnected; the family decides after. */
    conn->max_payload_size = (uint16_t)(family ? family->fo_conn_size : 0);
    conn->use_connected_msg = (family ? use_connected_msg : false);
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
        rc_dec(conn);
        return rc;
    }

    /* create the conn condition variable. */
    if((rc = cond_create(&(conn->session_wait_cond))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to create session condition var!");
        rc_dec(conn);
        return rc;
    }

    /* add the new conn to the list. */
    session_list_add_unsafe(list, conn);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Done");

    *conn_out = conn;

    return PLCTAG_STATUS_OK;
}



static int find_or_create_on_list(cip_conn_list_t *list, const cip_family_t *family, attr attribs, cip_conn_p *tag_conn,
                                  int *is_new_conn) {
    /*int debug = attr_get_int(attribs,"debug",0);*/
    const char *session_gw = attr_get_str(attribs, "gateway", "");
    const char *session_path = attr_get_str(attribs, "path", "");
    /*
     * The family decides, so every tag on the connection, @connection included, gets
     * the same answer.  Only Logix lets the tag string override it, and it defaults to 1.  A
     * DH+ path forces it on when the connection is created.
     */
    cip_plc_type_t plc_type = (family ? family->plc_type : CIP_PLC_NONE);
    int use_connected_msg = (family ? cip_family_use_connected_msg(family, attribs) : 0);
    cip_conn_p session = NULL;
    int new_session = 0;
    /*
     * Connections are shared by default.  Two spellings of the same deprecated
     * attribute reached here from the two dialects, so both are still honoured.
     */
    int shared_session = attr_get_int(attribs, "share_session", attr_get_int(attribs, "share_conn", 1));
    int rc = PLCTAG_STATUS_OK;
    int32_t connection_inactivity_timeout_ms = CONN_INACTIVITY_TIMEOUT_MAX_MS;
    int32_t connection_retry_max_delay_ms = RETRY_WAIT_MAX_MS;
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

    connection_inactivity_timeout_ms = attr_get_int(attribs, "connection_inactivity_timeout_ms", CONN_INACTIVITY_TIMEOUT_MAX_MS);
    conn_clamp_ms_attrib(&connection_inactivity_timeout_ms, CONN_INACTIVITY_TIMEOUT_MIN_MS, CONN_INACTIVITY_TIMEOUT_MAX_MS,
                         "connection_inactivity_timeout_ms", 0, DEBUG_MODULE_CIP);

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Setting connection_inactivity_timeout_ms to %" PRId32 "ms.",
           connection_inactivity_timeout_ms);

    connection_retry_max_delay_ms = attr_get_int(attribs, "connection_retry_max_delay_ms", RETRY_WAIT_MAX_MS);
    conn_clamp_ms_attrib(&connection_retry_max_delay_ms, CONN_RETRY_MAX_DELAY_MIN_MS, CONN_RETRY_MAX_DELAY_LIMIT_MS,
                         "connection_retry_max_delay_ms", 0, DEBUG_MODULE_CIP);

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Setting connection_retry_max_delay_ms to %" PRId32 "ms.",
           connection_retry_max_delay_ms);

    critical_block(list->mutex) {
        /* if we are to share connections, then look for an existing one. */
        if(shared_session) {
            session = session_list_find_by_host_unsafe(list, session_gw, session_path, connection_group_id, plc_type);

            /*
             * One connection cannot be both connected and unconnected.  Asked of its own family,
             * which may be DH+.  A probed connection has no request to compare: the family decides.
             */
            if(session && session->plc_type != CIP_PLC_NONE) {
                use_connected_msg = (cip_family_use_connected_msg(session->family, attribs) ? 1 : 0);
            }

            if(session && session->plc_type != CIP_PLC_NONE && session->use_connected_msg != use_connected_msg) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0,
                       "use_connected_msg=%d conflicts with the existing connection's %d!  Use a different "
                       "connection_group_id.",
                       use_connected_msg, session->use_connected_msg);
                rc_dec(session);
                session = NULL;
                rc = PLCTAG_ERR_BAD_PARAM;
            }
        } else {
            /* no sharing, create a new one */
            session = NULL;
        }

        if(rc != PLCTAG_STATUS_OK) {
            /* refused above; nothing to create. */
        } else if(session == NULL) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Creating new session.");

            rc = session_create(list, family, session_gw, session_path, use_connected_msg, connection_group_id, &session);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to create a session, error %s!", plc_tag_decode_error(rc));
                session = NULL;
            } else {
                atomic_init_int32(&session->connection_inactivity_timeout_ms, connection_inactivity_timeout_ms);
                atomic_init_int32(&session->connection_retry_max_delay_ms, connection_retry_max_delay_ms);

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

            /*
             * These belong to the connection and the connection already exists, so the tag
             * that created it set them.  Say so rather than silently dropping this tag's
             * values, and name the way to change them that does work.
             */
            if(atomic_get_int32(&session->connection_inactivity_timeout_ms) != connection_inactivity_timeout_ms
               || atomic_get_int32(&session->connection_retry_max_delay_ms) != connection_retry_max_delay_ms) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0,
                       "This session already has connection_inactivity_timeout_ms=%" PRId32
                       " and connection_retry_max_delay_ms=%" PRId32 "; this tag asked for %d and %d.  Use "
                       "plc_tag_set_int_attribute() on a tag to change them.",
                       atomic_get_int32(&session->connection_inactivity_timeout_ms),
                       atomic_get_int32(&session->connection_retry_max_delay_ms), connection_inactivity_timeout_ms,
                       connection_retry_max_delay_ms);
            }
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
 * Bring the whole connection list down.
 *
 * The order matters: the list is emptied and freed first, but the mutex cannot
 * go until the handler threads have finished, because a handler may still be
 * in its own cleanup holding conn->session_mutex.  An allocator that returns
 * freed pages to the OS immediately turns any use-after-free here into a
 * SIGSEGV rather than something harmless.
 */
static void session_list_teardown(cip_conn_list_t *list, debug_module_t debug_module) {
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
