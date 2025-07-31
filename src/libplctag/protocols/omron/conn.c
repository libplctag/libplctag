/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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

#include <inttypes.h>
#include <libplctag/protocols/omron/cip.h>
#include <libplctag/protocols/omron/conn.h>
#include <libplctag/protocols/omron/defs.h>
#include <libplctag/protocols/omron/omron_common.h>
#include <libplctag/protocols/omron/tag.h>
#include <limits.h>
#include <platform.h>
#include <stdlib.h>
#include <time.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>
#include <utils/random_utils.h>

#define MAX_REQUESTS (400)

#define EIP_CIP_PREFIX_SIZE (44) /* bytes of encap header and CFP connected header */

/* Omron is special */
#define MAX_CIP_OMRON_MSG_SIZE_EX (0xFFFF & 1990)
#define MAX_CIP_OMRON_MSG_SIZE (0x01FF & 502)


/*
 * Number of milliseconds to wait to try to set up the conn again
 * after a failure.
 */
#define RETRY_WAIT_MS (5000)

#define CONN_DISCONNECT_TIMEOUT (5000)

#define SOCKET_WAIT_TIMEOUT_MS (20)
#define CONN_IDLE_WAIT_TIME (100)

/* make sure we try hard to get a good payload size */
#define GET_MAX_PAYLOAD_SIZE(conn)                             \
    ((conn->max_payload_size > 0) ? (conn->max_payload_size) : \
                                    ((conn->fo_conn_size > 0) ? (conn->fo_conn_size) : (conn->fo_ex_conn_size)))


/* plc-specific conn constructors */
static omron_conn_p create_omron_njnx_conn_unsafe(const char *host, const char *path, int *use_connected_msg,
                                                  int connection_group_id);

static omron_conn_p conn_create_unsafe(int max_payload_capacity, bool data_buffer_is_static, const char *host, const char *path,
                                       plc_type_t plc_type, int *use_connected_msg, int connection_group_id);
static int conn_init(omron_conn_p conn);
// static int get_plc_type(attr attribs);
static int add_conn_unsafe(omron_conn_p n);
static int remove_conn_unsafe(omron_conn_p n);
static omron_conn_p find_conn_by_host_unsafe(const char *gateway, const char *path, int connection_group_id);
static int conn_match_valid(const char *host, const char *path, omron_conn_p conn);
static int conn_add_request_unsafe(omron_conn_p conn, omron_request_p req);
static int conn_open_socket(omron_conn_p conn);
static void conn_destroy(void *conn);
static int conn_register(omron_conn_p conn);
static int conn_close_socket(omron_conn_p conn);
static int conn_unregister(omron_conn_p conn);
static THREAD_FUNC(conn_handler);
static int purge_aborted_requests_unsafe(omron_conn_p conn);
static int process_requests(omron_conn_p conn);
// static int check_packing(omron_conn_p conn, omron_request_p request);
static int get_payload_size(omron_request_p request);
static int pack_requests(omron_conn_p conn, omron_request_p *requests, int num_requests);
static int prepare_request(omron_conn_p conn);
static int send_eip_request(omron_conn_p conn, int timeout);
static int recv_eip_response(omron_conn_p conn, int timeout);
static int unpack_response(omron_conn_p conn, omron_request_p request, int sub_packet);
// static int perform_forward_open(omron_conn_p conn);
static int perform_forward_close(omron_conn_p conn);
// static int try_forward_open_ex(omron_conn_p conn, int *max_payload_size_guess);
// static int try_forward_open(omron_conn_p conn);
// static int send_forward_open_req(omron_conn_p conn);
// static int send_forward_open_req_ex(omron_conn_p conn);
// static int recv_forward_open_resp(omron_conn_p conn, int *max_payload_size_guess);
static int send_forward_close_req(omron_conn_p conn);
static int recv_forward_close_resp(omron_conn_p conn);
static int send_forward_open_request(omron_conn_p conn);
static int send_old_forward_open_request(omron_conn_p conn);
static int send_extended_forward_open_request(omron_conn_p conn);
static int receive_forward_open_response(omron_conn_p conn);
static void request_destroy(void *req_arg);
static int conn_request_increase_buffer(omron_request_p request, int new_capacity);


static volatile mutex_p conn_mutex = NULL;
static volatile vector_p conns = NULL;


int conn_startup(void) {
    int rc = PLCTAG_STATUS_OK;

    if((rc = mutex_create((mutex_p *)&conn_mutex)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to create conn mutex %s!", plc_tag_decode_error(rc));
        return rc;
    }

    if((conns = vector_create(25, 5)) == NULL) {
        pdebug(DEBUG_ERROR, "Unable to create conn vector!");
        return PLCTAG_ERR_NO_MEM;
    }

    return rc;
}


void conn_teardown(void) {
    pdebug(DEBUG_INFO, "Starting.");

    if(conns && conn_mutex) {
        pdebug(DEBUG_DETAIL, "Waiting for conns to terminate.");

        while(1) {
            int remaining_conns = 0;

            critical_block(conn_mutex) { remaining_conns = vector_length(conns); }

            /* wait for things to terminate. */
            if(remaining_conns > 0) {
                sleep_ms(10);  // MAGIC
            } else {
                break;
            }
        }

        pdebug(DEBUG_DETAIL, "Connections all terminated.");

        vector_destroy(conns);

        conns = NULL;
    }

    pdebug(DEBUG_DETAIL, "Destroying conn mutex.");

    if(conn_mutex) {
        mutex_destroy((mutex_p *)&conn_mutex);
        conn_mutex = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");
}


/*
 * conn_get_new_seq_id_unsafe
 *
 * A wrapper to get a new conn sequence ID.  Not thread safe.
 *
 * Note that this is dangerous to use in threaded applications
 * because 32-bit processors will not implement a 64-bit
 * integer as an atomic entity.
 */

uint64_t conn_get_new_seq_id_unsafe(omron_conn_p conn) { return conn->conn_seq_id++; }

/*
 * conn_get_new_seq_id
 *
 * A thread-safe function to get a new conn sequence ID.
 */

uint64_t conn_get_new_seq_id(omron_conn_p conn) {
    uint16_t res = 0;

    // pdebug(DEBUG_DETAIL, "entering critical block %p",conn_mutex);
    critical_block(conn->mutex) { res = (uint16_t)conn_get_new_seq_id_unsafe(conn); }
    // pdebug(DEBUG_DETAIL, "leaving critical block %p", conn_mutex);

    return res;
}


int conn_get_max_payload(omron_conn_p conn) {
    int result = 0;

    if(!conn) {
        pdebug(DEBUG_WARN, "Called with null conn pointer!");
        return 0;
    }

    critical_block(conn->mutex) { result = GET_MAX_PAYLOAD_SIZE(conn); }

    pdebug(DEBUG_DETAIL, "max payload size is %d bytes.", result);

    return result;
}


int conn_get_available_cip_payload_space(omron_conn_p conn) {
    int result = 0;

    if(!conn) {
        pdebug(DEBUG_WARN, "Called with null conn pointer!");
        return 0;
    }

    critical_block(conn->mutex) {
        int max_payload_size = GET_MAX_PAYLOAD_SIZE(conn);
        result = max_payload_size;

        pdebug(DEBUG_DETAIL, "Session payload calculation: max_payload_size=%d, fo_conn_size=%d, fo_ex_conn_size=%d, selected=%d",
               conn->max_payload_size, conn->fo_conn_size, conn->fo_ex_conn_size, max_payload_size);

        // Account for CPF data item overhead
        if(conn->use_connected_msg) {
            result -= (int)sizeof(cpf_connected_data_item);
        } else {
            result -= (int)sizeof(cpf_unconnected_data_item);
            result -= (int)(conn->conn_path_size);
        }
    }
    if(result < 0) {
        pdebug(DEBUG_WARN, "Available payload space is negative (%d bytes)! This should not happen!", result);
        result = 0;
    } else {
        pdebug(DEBUG_INFO, "Available payload space is %d bytes.", result);
    }

    return result;
}


int conn_find_or_create(omron_conn_p *tag_conn, attr attribs) {
    /*int debug = attr_get_int(attribs,"debug",0);*/
    const char *conn_gw = attr_get_str(attribs, "gateway", "");
    const char *conn_path = attr_get_str(attribs, "path", "");
    int use_connected_msg = attr_get_int(attribs, "use_connected_msg", 0);
    // int conn_gw_port = attr_get_int(attribs, "gateway_port", OMRON_EIP_DEFAULT_PORT);
    //  plc_type_t plc_type = get_plc_type(attribs);
    omron_conn_p conn = OMRON_CONN_NULL;
    int new_conn = 0;
    int shared_conn = attr_get_int(attribs, "share_conn", 1); /* share the conn by default. */
    int rc = PLCTAG_STATUS_OK;
    int auto_disconnect_enabled = 0;
    int auto_disconnect_timeout_ms = INT_MAX;
    int connection_group_id = attr_get_int(attribs, "connection_group_id", 0);
    int only_use_old_forward_open = attr_get_int(attribs, "conn_only_use_old_forward_open", 0);

    pdebug(DEBUG_DETAIL, "Starting");

    auto_disconnect_timeout_ms = attr_get_int(attribs, "auto_disconnect_ms", INT_MAX);
    if(auto_disconnect_timeout_ms != INT_MAX) {
        pdebug(DEBUG_DETAIL, "Setting auto-disconnect after %dms.", auto_disconnect_timeout_ms);
        auto_disconnect_enabled = 1;
    }

    // if(plc_type == OMRON_PLC_PLC5 && str_length(conn_path) > 0) {
    //     /* this means it is DH+ */
    //     use_connected_msg = 1;
    //     attr_set_int(attribs, "use_connected_msg", 1);
    // }

    critical_block(conn_mutex) {
        /* if we are to share conns, then look for an existing one. */
        if(shared_conn) {
            conn = find_conn_by_host_unsafe(conn_gw, conn_path, connection_group_id);
        } else {
            /* no sharing, create a new one */
            conn = OMRON_CONN_NULL;
        }

        if(conn == OMRON_CONN_NULL) {
            pdebug(DEBUG_DETAIL, "Creating new conn.");

            conn = create_omron_njnx_conn_unsafe(conn_gw, conn_path, &use_connected_msg, connection_group_id);
            if(conn == OMRON_CONN_NULL) {
                pdebug(DEBUG_WARN, "unable to create or find a conn!");
                rc = PLCTAG_ERR_BAD_GATEWAY;
            } else {
                conn->auto_disconnect_enabled = auto_disconnect_enabled;
                conn->auto_disconnect_timeout_ms = auto_disconnect_timeout_ms;

                /* see if we have an attribute set for forcing the use of the older ForwardOpen */
                pdebug(DEBUG_DETAIL, "Passed attribute to prohibit use of extended ForwardOpen is %d.",
                       only_use_old_forward_open);
                pdebug(DEBUG_DETAIL, "Existing attribute to prohibit use of extended ForwardOpen is %d.",
                       conn->only_use_old_forward_open);
                conn->only_use_old_forward_open = (conn->only_use_old_forward_open ? 1 : only_use_old_forward_open);

                new_conn = 1;
            }
        } else {
            /* turn on auto disconnect if we need to. */
            if(!conn->auto_disconnect_enabled && auto_disconnect_enabled) {
                conn->auto_disconnect_enabled = auto_disconnect_enabled;
            }

            /* disconnect period always goes down. */
            if(conn->auto_disconnect_enabled && conn->auto_disconnect_timeout_ms > auto_disconnect_timeout_ms) {
                conn->auto_disconnect_timeout_ms = auto_disconnect_timeout_ms;
            }

            pdebug(DEBUG_DETAIL, "Reusing existing conn.");
        }
    }

    /*
     * do this OUTSIDE the mutex in order to let other threads not block if
     * the conn creation process blocks.
     */

    if(new_conn) {
        rc = conn_init(conn);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to the PLC connection.");
            rc_dec(conn);
            conn = OMRON_CONN_NULL;
        } else {
            /* save the status */
            // conn->status = rc;
        }
    }

    /* store it into the tag */
    *tag_conn = conn;

    pdebug(DEBUG_DETAIL, "Done");

    return rc;
}


int add_conn_unsafe(omron_conn_p conn) {
    pdebug(DEBUG_DETAIL, "Starting");

    if(!conn) { return PLCTAG_ERR_NULL_PTR; }

    vector_set(conns, vector_length(conns), conn);

    conn->on_list = 1;

    pdebug(DEBUG_DETAIL, "Done");

    return PLCTAG_STATUS_OK;
}


int add_conn(omron_conn_p s) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    critical_block(conn_mutex) { rc = add_conn_unsafe(s); }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int remove_conn_unsafe(omron_conn_p conn) {
    pdebug(DEBUG_DETAIL, "Starting");

    if(!conn || !conns) { return 0; }

    for(int i = 0; i < vector_length(conns); i++) {
        omron_conn_p tmp = vector_get(conns, i);

        if(tmp == conn) {
            vector_remove(conns, i);
            break;
        }
    }

    pdebug(DEBUG_DETAIL, "Done");

    return PLCTAG_STATUS_OK;
}

int remove_conn(omron_conn_p s) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(s->on_list) {
        critical_block(conn_mutex) { rc = remove_conn_unsafe(s); }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int conn_match_valid(const char *host, const char *path, omron_conn_p conn) {
    if(!conn) { return 0; }

    /* don't use conns that failed immediately. */
    if(conn->failed) { return 0; }

    if(!str_length(host)) {
        pdebug(DEBUG_WARN, "New conn host is NULL or zero length!");
        return 0;
    }

    if(!str_length(conn->host)) {
        pdebug(DEBUG_WARN, "Connection host is NULL or zero length!");
        return 0;
    }

    if(str_cmp_i(host, conn->host)) { return 0; }

    if(str_cmp_i(path, conn->path)) { return 0; }

    return 1;
}


omron_conn_p find_conn_by_host_unsafe(const char *host, const char *path, int connection_group_id) {
    for(int i = 0; i < vector_length(conns); i++) {
        omron_conn_p conn = vector_get(conns, i);

        /* is this conn in the process of destruction? */
        pdebug(DEBUG_DETAIL, "rc_inc: Acquiring reference to the PLC connection.");
        conn = rc_inc(conn);
        if(conn) {
            if(conn->connection_group_id == connection_group_id && conn_match_valid(host, path, conn)) { return conn; }

            rc_dec(conn);
        }
    }

    return NULL;
}

omron_conn_p create_omron_njnx_conn_unsafe(const char *host, const char *path, int *use_connected_msg, int connection_group_id) {
    omron_conn_p conn = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    do {
        conn = conn_create_unsafe(MAX_CIP_OMRON_MSG_SIZE_EX, true, host, path, OMRON_PLC_OMRON_NJNX, use_connected_msg,
                                  connection_group_id);
        if(conn != NULL) {
            conn->only_use_old_forward_open = false;
            conn->fo_conn_size = MAX_CIP_OMRON_MSG_SIZE;
            conn->fo_ex_conn_size = MAX_CIP_OMRON_MSG_SIZE_EX;
            conn->max_payload_size = (uint16_t)conn->fo_conn_size;
        } else {
            pdebug(DEBUG_WARN, "Unable to create *Logix conn!");
        }
    } while(0);

    pdebug(DEBUG_INFO, "Done.");

    return conn;
}


omron_conn_p conn_create_unsafe(int max_payload_capacity, bool data_buffer_is_static, const char *host, const char *path,
                                plc_type_t plc_type, int *use_connected_msg, int connection_group_id) {
    static volatile uint32_t connection_id = 0;

    int rc = PLCTAG_STATUS_OK;
    omron_conn_p conn = OMRON_CONN_NULL;
    int total_allocation_size = sizeof(*conn);
    int data_buffer_capacity = EIP_CIP_PREFIX_SIZE + max_payload_capacity
                               + 32;  // MAGIC - this is just padding until we get to the bottom of the overwrite bug.
    int data_buffer_offset = 0;
    int host_name_offset = 0;
    int host_name_size = 0;
    int path_offset = 0;
    int path_size = 0;
    int conn_path_offset = 0;
    uint8_t tmp_conn_path[MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN];
    int tmp_conn_path_size = MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN;
    int is_dhp = 0;
    uint16_t dhp_dest = 0;

    pdebug(DEBUG_INFO, "Starting");

    if(*use_connected_msg) {
        pdebug(DEBUG_DETAIL, "Connection should use connected messaging.");
    } else {
        pdebug(DEBUG_DETAIL, "Connection should not use connected messaging.");
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
    host_name_size = str_length(host) + 1;
    total_allocation_size += host_name_size;

    /* add in space for the path copy. */
    if(path && str_length(path) > 0) {
        path_offset = total_allocation_size;
        path_size = str_length(path) + 1;
        total_allocation_size += path_size;
    } else {
        path_offset = 0;
    }

    /* encode the path */
    rc = CIP.encode_path(path, use_connected_msg, &tmp_conn_path[0], &tmp_conn_path_size, &is_dhp, &dhp_dest);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Unable to convert path string to binary path, error %s!", plc_tag_decode_error(rc));
        return NULL;
    }

    conn_path_offset = total_allocation_size;
    total_allocation_size += tmp_conn_path_size;

    /* allocate the conn struct and the buffer in the same allocation. */
    pdebug(
        DEBUG_DETAIL,
        "Allocating %d total bytes of memory with %d bytes for data buffer static data, %d bytes for the host name, %d bytes for the path, %d bytes for the encoded path.",
        total_allocation_size, (data_buffer_is_static ? data_buffer_capacity : 0), str_length(host) + 1,
        (path_offset == 0 ? 0 : str_length(path) + 1), tmp_conn_path_size);

    conn = (omron_conn_p)rc_alloc(total_allocation_size, conn_destroy);
    if(!conn) {
        pdebug(DEBUG_WARN, "Error allocating new conn!");
        return OMRON_CONN_NULL;
    }

    /* fill in the interior pointers */

    /* fix up the data buffer. */
    conn->data_buffer_is_static = data_buffer_is_static;
    conn->data_capacity = (uint32_t)(unsigned int)max_payload_capacity;

    if(data_buffer_is_static) {
        conn->data = (uint8_t *)(conn) + data_buffer_offset;
        // conn->data_capacity = max_buffer_size;
    } else {
        conn->data = (uint8_t *)mem_alloc(data_buffer_capacity);
        if(conn->data == NULL) {
            pdebug(DEBUG_WARN, "Unable to allocate the connection data buffer!");
            return rc_dec(conn);
        }
    }

    /* point the host pointer just after the data. */
    conn->host = (char *)(conn) + host_name_offset;
    str_copy(conn->host, host_name_size, host);

    if(path_offset) {
        conn->path = (char *)(conn) + path_offset;
        str_copy(conn->path, path_size, path);
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

    conn->requests = vector_create(CONN_MIN_REQUESTS, CONN_INC_REQUESTS);
    if(!conn->requests) {
        pdebug(DEBUG_WARN, "Unable to allocate vector for requests!");
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to the PLC connection.");
        rc_dec(conn);
        return NULL;
    }

    /* check for ID set up. This does not need to be thread safe since we just need a random value. */
    if(connection_id == 0) { connection_id = (uint32_t)random_u64(UINT32_MAX) + 1; }

    /* fix up the rest of the fields */
    conn->plc_type = plc_type;
    conn->use_connected_msg = *use_connected_msg;
    conn->failed = 0;
    conn->conn_serial_number = (uint16_t)(random_u64(UINT16_MAX) + 1);
    conn->conn_seq_id = (random_u64(UINT32_MAX) + 1);
    conn->is_dhp = is_dhp;
    conn->dhp_dest = dhp_dest;

    pdebug(DEBUG_DETAIL, "Setting connection_group_id to %d.", connection_group_id);
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

    /* add the new conn to the list. */
    add_conn_unsafe(conn);

    pdebug(DEBUG_INFO, "Done");

    return conn;
}


/*
 * conn_init
 *
 * This calls several blocking methods and so must not keep the main mutex
 * locked during them.
 */
int conn_init(omron_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /* create the conn mutex. */
    if((rc = mutex_create(&(conn->mutex))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create conn mutex!");
        conn->failed = 1;
        return rc;
    }

    /* create the conn condition variable. */
    if((rc = cond_create(&(conn->wait_cond))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create conn condition var!");
        conn->failed = 1;
        return rc;
    }

    if((rc = thread_create((thread_p *)&(conn->handler_thread), conn_handler, 32 * 1024, conn)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create conn thread!");
        conn->failed = 1;
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


/*
 * conn_open_socket()
 *
 * Connect to the host/port passed via TCP.
 */

int conn_open_socket(omron_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;
    char **server_port = NULL;
    int port = 0;

    pdebug(DEBUG_INFO, "Starting.");

    /* Open a socket for communication with the gateway. */
    rc = socket_create(&(conn->sock));

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to create socket for conn!");
        return rc;
    }

    server_port = str_split(conn->host, ":");
    if(!server_port) {
        pdebug(DEBUG_WARN, "Unable to split server and port string!");
        return PLCTAG_ERR_BAD_CONFIG;
    }

    if(server_port[0] == NULL) {
        pdebug(DEBUG_WARN, "Server string is malformed or empty!");
        mem_free(server_port);
        return PLCTAG_ERR_BAD_CONFIG;
    }

    if(server_port[1] != NULL) {
        rc = str_to_int(server_port[1], &port);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to extract port number from server string \"%s\"!", conn->host);
            mem_free(server_port);
            return PLCTAG_ERR_BAD_CONFIG;
        }

        pdebug(DEBUG_DETAIL, "Using special port %d.", port);
    } else {
        port = OMRON_EIP_DEFAULT_PORT;

        pdebug(DEBUG_DETAIL, "Using default port %d.", port);
    }

    rc = socket_connect_tcp_start(conn->sock, server_port[0], port);

    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_WARN, "Unable to connect socket for conn!");
        mem_free(server_port);
        return rc;
    }

    if(server_port) { mem_free(server_port); }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


int conn_register(omron_conn_p conn) {
    eip_conn_reg_req *req;
    eip_encap *resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /*
     * clear the conn data.
     *
     * We use the receiving buffer because we do not have a request and nothing can
     * be coming in (we hope) on the socket yet.
     */
    mem_set(conn->data, 0, sizeof(eip_conn_reg_req));

    req = (eip_conn_reg_req *)(conn->data);

    /* fill in the fields of the request */
    req->encap_command = h2le16(OMRON_EIP_REGISTER_CONN);
    req->encap_length = h2le16(sizeof(eip_conn_reg_req) - sizeof(eip_encap));
    req->encap_conn_handle = h2le32(/*conn->conn_handle*/ 0);
    req->encap_status = h2le32(0);
    req->encap_sender_context = h2le64((uint64_t)0);
    req->encap_options = h2le32(0);

    req->eip_version = h2le16(OMRON_EIP_VERSION);
    req->option_flags = h2le16(0);

    /*
     * socket ops here are _ASYNCHRONOUS_!
     *
     * This is done this way because we do not have everything
     * set up for a request to be handled by the thread.  I think.
     */

    /* send registration to the gateway */
    conn->data_size = sizeof(eip_conn_reg_req);
    conn->data_offset = 0;

    rc = send_eip_request(conn, CONN_DEFAULT_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error sending conn registration request %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* get the response from the gateway */
    rc = recv_eip_response(conn, CONN_DEFAULT_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error receiving conn registration response %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* encap header is at the start of the buffer */
    resp = (eip_encap *)(conn->data);

    /* check the response status */
    if(le2h16(resp->encap_command) != OMRON_EIP_REGISTER_CONN) {
        pdebug(DEBUG_WARN, "EIP unexpected response packet type: %d!", resp->encap_command);
        return PLCTAG_ERR_BAD_DATA;
    }

    if(le2h32(resp->encap_status) != OMRON_EIP_OK) {
        pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(resp->encap_status));
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /*
     * after all that, save the conn handle, we will
     * use it in future packets.
     */
    conn->conn_handle = le2h32(resp->encap_conn_handle);

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int conn_unregister(omron_conn_p conn) {
    (void)conn;

    pdebug(DEBUG_INFO, "Starting.");

    /* nothing to do, perhaps. */

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int conn_close_socket(omron_conn_p conn) {
    pdebug(DEBUG_INFO, "Starting.");

    if(conn->sock) {
        socket_close(conn->sock);
        socket_destroy(&(conn->sock));
        conn->sock = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


void conn_destroy(void *conn_arg) {
    omron_conn_p conn = conn_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!conn) {
        pdebug(DEBUG_WARN, "Connection ptr is null!");

        return;
    }

    /* so remove the conn from the list so no one else can reference it. */
    remove_conn(conn);

    pdebug(DEBUG_INFO, "Connection sent %" PRId64 " packets.", conn->packet_count);

    /* terminate the conn thread first. */
    conn->terminating = 1;

    /* signal the condition variable in case it is waiting */
    if(conn->wait_cond) { cond_signal(conn->wait_cond); }

    /* get rid of the handler thread. */
    pdebug(DEBUG_DETAIL, "Destroying conn thread.");
    if(conn->handler_thread) {
        /* this cannot be guarded by the mutex since the conn thread also locks it. */
        thread_join(conn->handler_thread);

        /* FIXME - is this critical block needed? */
        critical_block(conn->mutex) {
            thread_destroy(&(conn->handler_thread));
            conn->handler_thread = NULL;
        }
    }

    /* this needs to be handled in the mutex to prevent double frees due to queued requests. */
    critical_block(conn->mutex) {
        /* close off the connection if is one. This helps the PLC clean up. */
        if(conn->targ_connection_id) {
            /*
             * we do not want the internal loop to immediately
             * return, so set the flag like we are not terminating.
             * There is still a timeout that applies.
             */
            conn->terminating = 0;
            perform_forward_close(conn);
            conn->terminating = 1;
        }

        /* try to be nice and un-register the conn */
        if(conn->conn_handle) { conn_unregister(conn); }

        if(conn->sock) { conn_close_socket(conn); }

        /* release all the requests that are in the queue. */
        if(conn->requests) {
            for(int i = 0; i < vector_length(conn->requests); i++) {
                omron_request_p req = vector_get(conn->requests, i);
                pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference request for tag %" PRId32 ".", req->tag_id);
                rc_dec(req);
            }

            vector_destroy(conn->requests);
            conn->requests = NULL;
        }
    }

    /* we are done with the condition variable, finally destroy it. */
    pdebug(DEBUG_DETAIL, "Destroying conn condition variable.");
    if(conn->wait_cond) {
        cond_destroy(&(conn->wait_cond));
        conn->wait_cond = NULL;
    }

    /* we are done with the mutex, finally destroy it. */
    pdebug(DEBUG_DETAIL, "Destroying conn mutex.");
    if(conn->mutex) {
        mutex_destroy(&(conn->mutex));
        conn->mutex = NULL;
    }

    if(!conn->data_buffer_is_static) { mem_free(conn->data); }

    /* these are all allocated in one large block. */

    // pdebug(DEBUG_DETAIL, "Cleaning up allocated memory for paths and host name.");
    // if(conn->conn_path) {
    //     mem_free(conn->conn_path);
    //     conn->conn_path = NULL;
    // }

    // if(conn->path) {
    //     mem_free(conn->path);
    //     conn->path = NULL;
    // }

    // if(conn->host) {
    //     mem_free(conn->host);
    //     conn->host = NULL;
    // }

    pdebug(DEBUG_INFO, "Done.");

    return;
}


/*
 * conn_add_request_unsafe
 *
 * You must hold the mutex before calling this!
 */
int conn_add_request_unsafe(omron_conn_p conn, omron_request_p req) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!conn) {
        pdebug(DEBUG_WARN, "Connection is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_DETAIL, "rc_inc: Acquiring reference to the request.");
    req = rc_inc(req);

    if(!req) {
        pdebug(DEBUG_WARN, "Request is either null or in the process of being deleted.");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* make sure the request points to the conn */

    /* insert into the requests vector */
    vector_set(conn->requests, vector_length(conn->requests), req);

    pdebug(DEBUG_DETAIL, "Total requests in the queue: %d", vector_length(conn->requests));

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}

/*
 * conn_add_request
 *
 * This is a thread-safe version of the above routine.
 */
int conn_add_request(omron_conn_p conn, omron_request_p req) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting. conn=%p, req=%p", conn, req);

    critical_block(conn->mutex) { rc = conn_add_request_unsafe(conn, req); }

    cond_signal(conn->wait_cond);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


/*****************************************************************
 **************** Connection handling functions *********************
 ****************************************************************/


typedef enum {
    CONN_OPEN_SOCKET_START,
    CONN_OPEN_SOCKET_WAIT,
    CONN_REGISTER,
    CONN_SEND_FORWARD_OPEN,
    CONN_RECEIVE_FORWARD_OPEN,
    CONN_IDLE,
    CONN_DISCONNECT,
    CONN_UNREGISTER,
    CONN_CLOSE_SOCKET,
    CONN_START_RETRY,
    CONN_WAIT_RETRY,
    CONN_WAIT_RECONNECT
} conn_state_t;


THREAD_FUNC(conn_handler) {
    omron_conn_p conn = arg;
    int rc = PLCTAG_STATUS_OK;
    conn_state_t state = CONN_OPEN_SOCKET_START;
    int64_t timeout_time = 0;
    int64_t wait_until_time = 0;
    int64_t auto_disconnect_time = time_ms() + CONN_DISCONNECT_TIMEOUT;
    int auto_disconnect = 0;


    pdebug(DEBUG_INFO, "Starting thread for conn %p", conn);

    while(!conn->terminating && !atomic_get_bool(&library_terminating)) {
        /* how long should we wait if nothing wakes us? */
        wait_until_time = time_ms() + CONN_IDLE_WAIT_TIME;

        /*
         * Do this on every cycle.   This keeps the queue clean(ish).
         *
         * Make sure we get rid of all the aborted requests queued.
         * This keeps the overall memory usage lower.
         */

        pdebug(DEBUG_SPEW, "Critical block.");
        critical_block(conn->mutex) { purge_aborted_requests_unsafe(conn); }

        switch(state) {
            case CONN_OPEN_SOCKET_START:
                pdebug(DEBUG_DETAIL, "in CONN_OPEN_SOCKET_START state.");

                /* we must connect to the gateway*/
                rc = conn_open_socket(conn);
                if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
                    pdebug(DEBUG_WARN, "conn connect failed %s!", plc_tag_decode_error(rc));
                    state = CONN_CLOSE_SOCKET;
                } else {
                    if(rc == PLCTAG_STATUS_OK) {
                        /* bump auto disconnect time into the future so that we do not accidentally disconnect immediately. */
                        auto_disconnect_time = time_ms() + CONN_DISCONNECT_TIMEOUT;

                        pdebug(DEBUG_DETAIL, "Connect complete immediately, going to state CONN_REGISTER.");

                        state = CONN_REGISTER;
                    } else {
                        pdebug(DEBUG_DETAIL, "Connect started, going to state CONN_OPEN_SOCKET_WAIT.");

                        state = CONN_OPEN_SOCKET_WAIT;
                    }
                }

                /* in all cases, don't wait. */
                cond_signal(conn->wait_cond);

                break;

            case CONN_OPEN_SOCKET_WAIT:
                pdebug(DEBUG_DETAIL, "in CONN_OPEN_SOCKET_WAIT state.");

                /* we must connect to the gateway */
                rc = socket_connect_tcp_check(conn->sock, 20); /* MAGIC */
                if(rc == PLCTAG_STATUS_OK) {
                    /* connected! */
                    pdebug(DEBUG_INFO, "Socket connection succeeded.");

                    /* calculate the disconnect time. */
                    auto_disconnect_time = time_ms() + CONN_DISCONNECT_TIMEOUT;

                    state = CONN_REGISTER;
                } else if(rc == PLCTAG_ERR_TIMEOUT) {
                    pdebug(DEBUG_DETAIL, "Still waiting for connection to succeed.");

                    /* don't wait more.  The TCP connect check will wait in select(). */
                } else {
                    pdebug(DEBUG_WARN, "Connection connect failed %s!", plc_tag_decode_error(rc));
                    state = CONN_CLOSE_SOCKET;
                }

                /* in all cases, don't wait. */
                cond_signal(conn->wait_cond);

                break;

            case CONN_REGISTER:
                pdebug(DEBUG_DETAIL, "in CONN_REGISTER state.");

                if((rc = conn_register(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "conn registration failed %s!", plc_tag_decode_error(rc));
                    state = CONN_CLOSE_SOCKET;
                } else {
                    if(conn->use_connected_msg) {
                        state = CONN_SEND_FORWARD_OPEN;
                    } else {
                        state = CONN_IDLE;
                    }
                }
                cond_signal(conn->wait_cond);
                break;

            case CONN_SEND_FORWARD_OPEN:
                pdebug(DEBUG_DETAIL, "in CONN_SEND_FORWARD_OPEN state.");

                if((rc = send_forward_open_request(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Send Forward Open failed %s!", plc_tag_decode_error(rc));
                    state = CONN_UNREGISTER;
                } else {
                    pdebug(DEBUG_DETAIL, "Send Forward Open succeeded, going to CONN_RECEIVE_FORWARD_OPEN state.");
                    state = CONN_RECEIVE_FORWARD_OPEN;
                }
                cond_signal(conn->wait_cond);
                break;

            case CONN_RECEIVE_FORWARD_OPEN:
                pdebug(DEBUG_DETAIL, "in CONN_RECEIVE_FORWARD_OPEN state.");

                if((rc = receive_forward_open_response(conn)) != PLCTAG_STATUS_OK) {
                    if(rc == PLCTAG_ERR_DUPLICATE) {
                        pdebug(DEBUG_DETAIL, "Duplicate connection error received, trying again with different connection ID.");
                        state = CONN_SEND_FORWARD_OPEN;
                    } else if(rc == PLCTAG_ERR_TOO_LARGE) {
                        pdebug(DEBUG_DETAIL, "Requested packet size too large, retrying with smaller size.");
                        state = CONN_SEND_FORWARD_OPEN;
                    } else if(rc == PLCTAG_ERR_UNSUPPORTED && !conn->only_use_old_forward_open) {
                        /* if we got an unsupported error and we are trying with ForwardOpenEx, then try the old command. */
                        pdebug(DEBUG_DETAIL, "PLC does not support ForwardOpenEx, trying old ForwardOpen.");
                        conn->only_use_old_forward_open = 1;
                        state = CONN_SEND_FORWARD_OPEN;
                    } else {
                        pdebug(DEBUG_WARN, "Receive Forward Open failed %s!", plc_tag_decode_error(rc));
                        state = CONN_UNREGISTER;
                    }
                } else {
                    pdebug(DEBUG_DETAIL, "Send Forward Open succeeded, going to CONN_IDLE state.");
                    state = CONN_IDLE;
                }
                cond_signal(conn->wait_cond);
                break;

            case CONN_IDLE:
                pdebug(DEBUG_DETAIL, "in CONN_IDLE state.");

                /* if there is work to do, make sure we do not disconnect. */
                critical_block(conn->mutex) {
                    int num_reqs = vector_length(conn->requests);
                    if(num_reqs > 0) {
                        pdebug(DEBUG_DETAIL, "There are %d requests pending before cleanup and sending.", num_reqs);
                        auto_disconnect_time = time_ms() + CONN_DISCONNECT_TIMEOUT;
                    }
                }

                if((rc = process_requests(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Error while processing requests %s!", plc_tag_decode_error(rc));
                    if(conn->use_connected_msg) {
                        state = CONN_DISCONNECT;
                    } else {
                        state = CONN_UNREGISTER;
                    }
                    cond_signal(conn->wait_cond);
                }

                /* check if we should disconnect */
                if(auto_disconnect_time < time_ms()) {
                    pdebug(DEBUG_DETAIL, "Disconnecting due to inactivity.");

                    auto_disconnect = 1;

                    if(conn->use_connected_msg) {
                        state = CONN_DISCONNECT;
                    } else {
                        state = CONN_UNREGISTER;
                    }
                    cond_signal(conn->wait_cond);
                }

                /* if there is work to do, make sure we signal the condition var. */
                critical_block(conn->mutex) {
                    int num_reqs = vector_length(conn->requests);
                    if(num_reqs > 0) {
                        pdebug(DEBUG_DETAIL, "There are %d requests still pending after abort purge and sending.", num_reqs);
                        cond_signal(conn->wait_cond);
                    }
                }

                break;

            case CONN_DISCONNECT:
                pdebug(DEBUG_DETAIL, "in CONN_DISCONNECT state.");

                if((rc = perform_forward_close(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Forward close failed %s!", plc_tag_decode_error(rc));
                }

                state = CONN_UNREGISTER;
                cond_signal(conn->wait_cond);
                break;

            case CONN_UNREGISTER:
                pdebug(DEBUG_DETAIL, "in CONN_UNREGISTER state.");

                if((rc = conn_unregister(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Unregistering conn failed %s!", plc_tag_decode_error(rc));
                }

                state = CONN_CLOSE_SOCKET;
                cond_signal(conn->wait_cond);
                break;

            case CONN_CLOSE_SOCKET:
                pdebug(DEBUG_DETAIL, "in CONN_CLOSE_SOCKET state.");

                if((rc = conn_close_socket(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Closing conn socket failed %s!", plc_tag_decode_error(rc));
                }

                if(auto_disconnect) {
                    state = CONN_WAIT_RECONNECT;
                } else {
                    state = CONN_START_RETRY;
                }
                cond_signal(conn->wait_cond);
                break;

            case CONN_START_RETRY:
                pdebug(DEBUG_DETAIL, "in CONN_START_RETRY state.");

                /* FIXME - make this a tag attribute. */
                timeout_time = time_ms() + RETRY_WAIT_MS;

                /* start waiting. */
                state = CONN_WAIT_RETRY;

                cond_signal(conn->wait_cond);
                break;

            case CONN_WAIT_RETRY:
                pdebug(DEBUG_DETAIL, "in CONN_WAIT_RETRY state.");

                if(timeout_time < time_ms()) {
                    pdebug(DEBUG_DETAIL, "Transitioning to CONN_OPEN_SOCKET_START.");
                    state = CONN_OPEN_SOCKET_START;
                    cond_signal(conn->wait_cond);
                }

                break;

            case CONN_WAIT_RECONNECT:
                /* wait for at least one request to queue before reconnecting. */
                pdebug(DEBUG_DETAIL, "in CONN_WAIT_RECONNECT state.");

                auto_disconnect = 0;

                /* if there is work to do, reconnect.. */
                pdebug(DEBUG_SPEW, "Critical block.");
                critical_block(conn->mutex) {
                    if(vector_length(conn->requests) > 0) {
                        pdebug(DEBUG_DETAIL, "There are requests waiting, reopening connection to PLC.");

                        state = CONN_OPEN_SOCKET_START;
                        cond_signal(conn->wait_cond);
                    }
                }

                break;


            default:
                pdebug(DEBUG_ERROR, "Unknown state %d!", state);

                /* FIXME - this logic is not complete.  We might be here without
                 * a connected conn or a registered conn. */
                if(conn->use_connected_msg) {
                    state = CONN_DISCONNECT;
                } else {
                    state = CONN_UNREGISTER;
                }

                cond_signal(conn->wait_cond);
                break;
        }

        /*
         * give up the CPU a bit, but only if we are not
         * doing some linked states.
         */
        if(wait_until_time > 0) {
            int64_t time_left = wait_until_time - time_ms();

            if(time_left > 0) { cond_wait(conn->wait_cond, (int)time_left); }
        }
    }

    /*
     * One last time before we exit.
     */
    pdebug(DEBUG_DETAIL, "Critical block.");
    critical_block(conn->mutex) { purge_aborted_requests_unsafe(conn); }

    THREAD_RETURN(0);
}


/*
 * This must be called with the conn mutex held!
 */
int purge_aborted_requests_unsafe(omron_conn_p conn) {
    int purge_count = 0;
    omron_request_p request = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    /* remove the aborted requests. */
    for(int i = 0; i < vector_length(conn->requests); i++) {
        request = vector_get(conn->requests, i);

        /* filter out the aborts. */
        if(request && request->abort_request) {
            purge_count++;

            /* remove it from the queue. */
            vector_remove(conn->requests, i);

            /* set the debug tag to the owning tag. */
            debug_set_tag_id(request->tag_id);

            pdebug(DEBUG_DETAIL, "Connection thread releasing aborted request %p.", request);

            request->status = PLCTAG_ERR_ABORT;
            request->request_size = 0;
            request->resp_received = 1;

            /* release our hold on it. */
            pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference request for tag %" PRId32 ".", request->tag_id);
            rc_dec(request);

            /* vector size has changed, back up one. */
            i--;
        }
    }

    if(purge_count > 0) { pdebug(DEBUG_DETAIL, "Removed %d aborted requests.", purge_count); }

    pdebug(DEBUG_SPEW, "Done.");

    return purge_count;
}


int process_requests(omron_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;
    omron_request_p request = NULL;
    omron_request_p bundled_requests[MAX_REQUESTS] = {NULL};
    int num_bundled_requests = 0;
    int remaining_request_space = 0;
    int remaining_response_space = 0;
    int allow_packing = 0;

    debug_set_tag_id(0);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!conn) {
        pdebug(DEBUG_WARN, "Null conn pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_SPEW, "Checking for requests to process.");

    rc = PLCTAG_STATUS_OK;
    request = NULL;
    conn->data_size = 0;
    conn->data_offset = 0;

    /* grab a request off the front of the list. */
    critical_block(conn->mutex) {
        int max_payload_size = GET_MAX_PAYLOAD_SIZE(conn);

        // FIXME - no logging in a mutex!
        // pdebug(DEBUG_DETAIL, "FIXME: max payload size %d", max_payload_size);

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
            remaining_request_space = max_payload_size;

            /*
             * we need to account for the overhead of the CPF data item, and
             * that depends on whether the conn is connected or not.
             * For connected sessions, we need 6 bytes for the CPF connected data item
             * and the connection sequence number.
             */
            if(conn->use_connected_msg) {
                remaining_request_space -= (int)sizeof(cpf_connected_data_item);
            } else {
                remaining_request_space -= (int)sizeof(cpf_unconnected_data_item);
            }

            /* -2 bytes for the msp (multi service packet) number of packets and -4 bytes for the cip response header,
             * we later subtract 2 bytes for each packet, this is to allow for the INT offset to the packet within the msp
             * finally we subtract 10 bytes just to give a comfort blanket as I had seen PLC_TAG_TOO_LARGE issue without it due to
             * too much data being packed into a single packet */
            remaining_response_space = max_payload_size - 2 - 4 - 10;

            /*
             * The logic below follows the same pattern as the AB implementation:
             *
             * - If the first request takes up all the space, we cannot pack any more requests.
             *
             * - If the first request is packable, we can keep packing requests
             *   until we run out of space or we reach the maximum number of requests. We need to make sure
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

                    /* calculate response space for first request */
                    remaining_response_space = remaining_response_space - request->response_size - 8 - 2;

                    /* remove it from the queue. */
                    vector_remove(conn->requests, 0);

                    /* The tag must have packing enabled and the plc must either support fragmented reads or this tag must have
                     * been read before, so that its response size is known */
                    allow_packing = request->allow_packing && (request->supports_fragmented_read || !request->first_read);

                    /* If the first request is packable, try to pack more requests */
                    if(allow_packing && vector_length(conn->requests) > 0) {
                        /* Account for CIP multi-request overhead now that we know we'll have multiple requests */
                        remaining_request_space -= (int)sizeof(cip_multi_req_header);

                        /* Account for 2-byte offset entry per request (including the first one already processed) */
                        int multi_request_overhead = 2;                    /* 2-byte offset entry per additional request */
                        remaining_request_space -= multi_request_overhead; /* for the first request */

                        while(vector_length(conn->requests) > 0 && num_bundled_requests < MAX_REQUESTS) {
                            request = vector_get(conn->requests, 0);

                            /* Only pack if this request is packable */
                            if(!request->allow_packing) { break; }

                            /* The tag must have packing enabled and the plc must either support fragmented reads or this tag must
                             * have been read before, so that its response size is known */
                            allow_packing = request->allow_packing && (request->supports_fragmented_read || !request->first_read);
                            if(!allow_packing) { break; }

                            int next_request_size = get_payload_size(request) + multi_request_overhead;

                            /* Check if this request fits in remaining space */
                            if(next_request_size > remaining_request_space) { break; }

                            /* calculate if all of the response data from the packed requests will fit into a single response
                             * packet. the -8 bytes is the maximum padding between packets, the -2 bytes is from the offset
                             * integer which stores the offset to this packet */
                            int next_response_space = remaining_response_space - request->response_size - 8 - 2;

                            /* Check response space only if fragmented reads are not supported */
                            if(!request->supports_fragmented_read && next_response_space < 0) { break; }

                            bundled_requests[num_bundled_requests] = request;
                            num_bundled_requests++;
                            remaining_request_space -= next_request_size;
                            remaining_response_space = next_response_space;
                            vector_remove(conn->requests, 0);
                        }
                    }
                    /* If first request is not packable, we stop here (only the first request is packed) */
                } else {
                    pdebug(DEBUG_WARN, "First request size %d exceeds remaining space %d, cannot process any requests.",
                           first_request_size, remaining_request_space);
                }
            } else {
                pdebug(DEBUG_DETAIL, "All requests in queue were aborted, nothing to do.");
            }
        }
    }

    /* output debug display as no particular tag. */
    debug_set_tag_id(0);

    if(num_bundled_requests > 0) {

        pdebug(DEBUG_INFO, "%d requests to process.", num_bundled_requests);

        do {
            /* copy and pack the requests into the conn buffer. */
            rc = pack_requests(conn, bundled_requests, num_bundled_requests);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error while packing requests, %s!", plc_tag_decode_error(rc));
                break;
            }

            /* fill in all the necessary parts to the request. */
            if((rc = prepare_request(conn)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to prepare request, %s!", plc_tag_decode_error(rc));
                break;
            }

            /* send the request */
            if((rc = send_eip_request(conn, CONN_DEFAULT_TIMEOUT)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error sending packet %s!", plc_tag_decode_error(rc));
                break;
            }

            /* wait for the response */
            if((rc = recv_eip_response(conn, CONN_DEFAULT_TIMEOUT)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error receiving packet response %s!", plc_tag_decode_error(rc));
                break;
            }

            /*
             * check the CIP status, but only if this is a bundled
             * response.   If it is a singleton, then we pass the
             * status back to the tag.
             */
            if(num_bundled_requests > 1) {
                cip_multi_resp_header *multi_resp = NULL;

                if(le2h16(((eip_encap *)(conn->data))->encap_command) == OMRON_EIP_UNCONNECTED_SEND) {
                    eip_cip_uc_resp *resp = (eip_cip_uc_resp *)(conn->data);
                    uint16_t udi_item_length = le2h16(resp->cpf_udi_item_length);
                    size_t response_overhead = 0;
                    size_t response_size = 0;

                    multi_resp = (cip_multi_resp_header *)(&(resp->reply_service));

                    pdebug(DEBUG_INFO, "Received unconnected packet with conn sequence ID %llx", resp->encap_sender_context);

                    /* punt if we got an overall error or it is not a partial/bundled error. */
                    if(resp->status != OMRON_EIP_OK && resp->status != OMRON_CIP_ERR_PARTIAL_ERROR) {
                        rc = CIP.decode_cip_error_code(&(resp->status));
                        pdebug(DEBUG_WARN, "Command failed! (%d/%d) %s", resp->status, rc, plc_tag_decode_error(rc));
                        break;
                    }

                    response_overhead = (size_t)((uint8_t *)multi_resp - conn->data);
                    response_size = (size_t)conn->data_size - response_overhead;

                    /* check the passed UDI data item size against what we really got. */
                    if((size_t)udi_item_length != response_size) {
                        pdebug(DEBUG_WARN,
                               "Incorrectly constructed response! UDI data length field is %zu but actual size is %zu!",
                               (size_t)udi_item_length, response_size);

                        rc = PLCTAG_ERR_BAD_DATA;
                        break;
                    }
                } else if(le2h16(((eip_encap *)(conn->data))->encap_command) == OMRON_EIP_CONNECTED_SEND) {
                    eip_cip_co_resp *resp = (eip_cip_co_resp *)(conn->data);
                    uint16_t cdi_item_length = le2h16(resp->cpf_cdi_item_length);
                    size_t response_overhead = 0;
                    size_t response_size = 0;

                    multi_resp = (cip_multi_resp_header *)(&(resp->reply_service));

                    pdebug(DEBUG_INFO, "Received connected packet with connection ID %x and sequence ID %u(%x)",
                           le2h32(resp->cpf_orig_conn_id), le2h16(resp->cpf_conn_seq_num), le2h16(resp->cpf_conn_seq_num));

                    /* punt if we got an overall error or it is not a partial/bundled error. */
                    if(resp->status != OMRON_EIP_OK && resp->status != OMRON_CIP_ERR_PARTIAL_ERROR) {
                        rc = CIP.decode_cip_error_code(&(resp->status));
                        pdebug(DEBUG_WARN, "Command failed! (%d/%d) %s", resp->status, rc, plc_tag_decode_error(rc));
                        break;
                    }

                    response_overhead = (size_t)((uint8_t *)(&resp->cpf_conn_seq_num) - conn->data);
                    response_size = (size_t)conn->data_size - response_overhead;

                    pdebug(DEBUG_DETAIL, "response_overhead=%zu", response_overhead);
                    pdebug(DEBUG_DETAIL, "response_size=%zu", response_size);

                    /* check the passed CDI data item size against what we really got. */
                    if((size_t)cdi_item_length != response_size) {
                        pdebug(DEBUG_WARN,
                               "Incorrectly constructed response! CDI data length field is %zu but actual size is %zu!",
                               (size_t)cdi_item_length, response_size);

                        rc = PLCTAG_ERR_BAD_DATA;
                        break;
                    }
                } else {
                    pdebug(DEBUG_WARN, "Unexpected EIP packet type, %04x!", le2h16(((eip_encap *)(conn->data))->encap_command));
                    rc = PLCTAG_ERR_BAD_DATA;
                    break;
                }

                /* we have multiple requests, sanity check the data. */
                if(le2h16(multi_resp->request_count) == num_bundled_requests) {
                    size_t offset_base = (size_t)((uint8_t *)(&multi_resp->request_count) - conn->data);

                    pdebug(DEBUG_DETAIL, "offset_base=%zu", offset_base);

                    /* check all the offsets */
                    for(int resp_index = 0; resp_index < num_bundled_requests; resp_index++) {
                        size_t resp_offset = (size_t)le2h16(multi_resp->request_offsets[resp_index]) + offset_base;

                        pdebug(DEBUG_DETAIL, "Response %d starts at byte offset %zu", resp_offset);

                        if(resp_offset >= (size_t)conn->data_size) {
                            pdebug(DEBUG_WARN, "Response %d has offset %zu which is outside the conn data!", resp_offset);
                            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                            break;
                        }
                    }
                } else {
                    pdebug(DEBUG_WARN, "Expected %d packed responses back but got %zu!", num_bundled_requests,
                           (size_t)le2h16(multi_resp->request_count));
                    rc = PLCTAG_ERR_BAD_DATA;
                    break;
                }

                /* we have multiple requests, sanity check the data. */
                if(le2h16(multi_resp->request_count) == num_bundled_requests) {
                    size_t offset_base = (size_t)((uint8_t *)(&multi_resp->request_count) - conn->data);

                    pdebug(DEBUG_DETAIL, "offset_base=%zu", offset_base);

                    /* check all the offsets */
                    for(int resp_index = 0; resp_index < num_bundled_requests; resp_index++) {
                        size_t resp_offset = (size_t)le2h16(multi_resp->request_offsets[resp_index]) + offset_base;

                        pdebug(DEBUG_DETAIL, "Response %d starts at byte offset %zu", resp_offset);

                        if(resp_offset >= (size_t)conn->data_size) {
                            pdebug(DEBUG_WARN, "Response %d has offset %zu which is outside the conn data!", resp_offset);
                            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                            break;
                        }
                    }
                } else {
                    pdebug(DEBUG_WARN, "Expected %d packed responses back but got %zu!", num_bundled_requests,
                           (size_t)le2h16(multi_resp->request_count));
                    rc = PLCTAG_ERR_BAD_DATA;
                    break;
                }
            }

            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Got error %s when processing incoming response(s)!", plc_tag_decode_error(rc));
                break;
            }

            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Got error %s when processing incoming response(s)!", plc_tag_decode_error(rc));
                break;
            }

            /* copy the results back out. Every request gets a copy. */
            for(int i = 0; i < num_bundled_requests; i++) {
                debug_set_tag_id(bundled_requests[i]->tag_id);

                rc = unpack_response(conn, bundled_requests[i], i);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Unable to unpack response!");
                    break;
                }

                /* release our reference */
                pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference request for tag %" PRId32 ".", bundled_requests[i]->tag_id);
                bundled_requests[i] = rc_dec(bundled_requests[i]);
            }

            rc = PLCTAG_STATUS_OK;
        } while(0);

        /* problem? push the requests back on the queue. */
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Error sending or receiving requests!");

            pdebug(DEBUG_INFO, "Pushing %d requests back into the queue.", num_bundled_requests);

            for(int i = num_bundled_requests - 1; i >= 0; i--) {
                if(bundled_requests[i]) { vector_insert(conn->requests, 0, bundled_requests[i]); }
            }
        }

        /* tickle the main tickler thread to note that we have responses. */
        plc_tag_tickler_wake();
    }

    debug_set_tag_id(0);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}


int unpack_response(omron_conn_p conn, omron_request_p request, int sub_packet) {
    int rc = PLCTAG_STATUS_OK;
    eip_cip_co_resp *packed_resp = (eip_cip_co_resp *)(conn->data);
    eip_cip_co_resp *unpacked_resp = NULL;
    uint8_t *pkt_start = NULL;
    uint8_t *pkt_end = NULL;
    int new_eip_len = 0;

    pdebug(DEBUG_INFO, "Starting.");

    /* clear out the request data. */
    mem_set(request->data, 0, request->request_capacity);

    /* change what we do depending on the type. */
    if(packed_resp->reply_service != (OMRON_EIP_CMD_CIP_MULTI | OMRON_EIP_CMD_CIP_OK)) {
        /* copy the data back into the request buffer. */
        new_eip_len = (int)conn->data_size;
        pdebug(DEBUG_INFO, "Got single response packet.  Copying %d bytes unchanged.", new_eip_len);

        if(new_eip_len > request->request_capacity) {
            int request_capacity = 0;

            pdebug(DEBUG_INFO, "Request buffer too small, allocating larger buffer.");

            critical_block(conn->mutex) {
                int max_payload_size = GET_MAX_PAYLOAD_SIZE(conn);

                // FIXME - no logging in a mutex!
                // pdebug(DEBUG_DETAIL, "FIXME: max payload size %d", max_payload_size);

                request_capacity = (int)(max_payload_size + EIP_CIP_PREFIX_SIZE);
            }

            /* make sure it will fit. */
            if(new_eip_len > request_capacity) {
                pdebug(DEBUG_WARN, "something is very wrong, packet length is %d but allowable capacity is %d!", new_eip_len,
                       request_capacity);
                return PLCTAG_ERR_TOO_LARGE;
            }

            rc = conn_request_increase_buffer(request, request_capacity);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to increase request buffer size to %d bytes!", request_capacity);
                return rc;
            }
        }

        mem_copy(request->data, conn->data, new_eip_len);
    } else {
        cip_multi_resp_header *multi = (cip_multi_resp_header *)(&packed_resp->reply_service);
        uint16_t total_responses = le2h16(multi->request_count);
        int pkt_len = 0;

        /* this is a packed response. */
        pdebug(DEBUG_INFO, "Got multiple response packet, subpacket %d", sub_packet);

        pdebug(DEBUG_INFO, "Our result offset is %d bytes.", (int)le2h16(multi->request_offsets[sub_packet]));

        pkt_start = ((uint8_t *)(&multi->request_count) + le2h16(multi->request_offsets[sub_packet]));

        /* calculate the end of the data. */
        if((sub_packet + 1) < total_responses) {
            /* not the last response */
            pkt_end = (uint8_t *)(&multi->request_count) + le2h16(multi->request_offsets[sub_packet + 1]);
        } else {
            pkt_end = (conn->data + le2h16(packed_resp->encap_length) + sizeof(eip_encap));
        }

        pkt_len = (int)(pkt_end - pkt_start);

        /* replace the request buffer if it is not big enough. */
        new_eip_len = pkt_len + (int)sizeof(eip_cip_co_generic_response);
        if(new_eip_len > request->request_capacity) {
            int request_capacity = 0;

            pdebug(DEBUG_INFO, "Request buffer too small, allocating larger buffer.");

            critical_block(conn->mutex) {
                int max_payload_size = GET_MAX_PAYLOAD_SIZE(conn);

                // FIXME: no logging in a mutex!
                // pdebug(DEBUG_DETAIL, "max payload size %d", max_payload_size);

                request_capacity = (int)(max_payload_size + EIP_CIP_PREFIX_SIZE);
            }

            /* make sure it will fit. */
            if(new_eip_len > request_capacity) {
                pdebug(DEBUG_WARN, "something is very wrong, packet length is %d but allowable capacity is %d!", new_eip_len,
                       request_capacity);
                return PLCTAG_ERR_TOO_LARGE;
            }

            rc = conn_request_increase_buffer(request, request_capacity);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to increase request buffer size to %d bytes!", request_capacity);
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

    pdebug(DEBUG_INFO, "Unpacked packet:");
    pdebug_dump_bytes(DEBUG_INFO, request->data, new_eip_len);

    /* notify the reading thread that the request is ready */
    spin_block(&request->lock) {
        request->status = PLCTAG_STATUS_OK;
        request->request_size = new_eip_len;
        request->resp_received = 1;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int get_payload_size(omron_request_p request) {
    int request_data_size = 0;
    eip_encap *header = (eip_encap *)(request->data);
    eip_cip_co_req *co_req = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(le2h16(header->encap_command) == OMRON_EIP_CONNECTED_SEND) {
        co_req = (eip_cip_co_req *)(request->data);
        /* get length of new request */
        request_data_size = le2h16(co_req->cpf_cdi_item_length) - 2 /* for connection sequence ID */
                            + 2                                     /* for multipacket offset */
            ;
    } else {
        pdebug(DEBUG_DETAIL, "Not a supported type EIP packet type %d to get the payload size.", le2h16(header->encap_command));
        request_data_size = INT_MAX;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return request_data_size;
}


int pack_requests(omron_conn_p conn, omron_request_p *requests, int num_requests) {
    eip_cip_co_req *new_req = NULL;
    eip_cip_co_req *packed_req = NULL;
    /* FIXME - is this the right way to check? */
    int header_size = 0;
    cip_multi_req_header *multi_header = NULL;
    int current_offset = 0;
    uint8_t *pkt_start = NULL;
    int pkt_len = 0;
    uint8_t *first_pkt_data = NULL;
    uint8_t *next_pkt_data = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    debug_set_tag_id(requests[0]->tag_id);

    /* get the header info from the first request. Just copy the whole thing. */
    mem_copy(conn->data, requests[0]->data, requests[0]->request_size);
    conn->data_size = (uint32_t)requests[0]->request_size;

    /* special case the case where there is just one request. */
    if(num_requests == 1) {
        pdebug(DEBUG_INFO, "Only one request, so done.");

        debug_set_tag_id(0);

        return PLCTAG_STATUS_OK;
    }

    /* set up multi-packet header. */

    header_size =
        (int)(sizeof(cip_multi_req_header) + (sizeof(uint16_le) * (size_t)num_requests)); /* offsets for each request. */

    pdebug(DEBUG_INFO, "header size %d", header_size);

    packed_req = (eip_cip_co_req *)(conn->data);

    /* make room in the request packet in the conn for the header. */
    pkt_start = (uint8_t *)(&packed_req->cpf_conn_seq_num) + sizeof(packed_req->cpf_conn_seq_num);
    pkt_len = (int)le2h16(packed_req->cpf_cdi_item_length) - (int)sizeof(packed_req->cpf_conn_seq_num);

    pdebug(DEBUG_INFO, "packet 0 is of length %d.", pkt_len);

    /* point to where we want the current packet to start. */
    first_pkt_data = pkt_start + header_size;

    /* move the data over to make room */
    mem_move(first_pkt_data, pkt_start, pkt_len);

    /* now fill in the header. Use pkt_start as it is pointing to the right location. */
    multi_header = (cip_multi_req_header *)pkt_start;
    multi_header->service_code = OMRON_EIP_CMD_CIP_MULTI;
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
        debug_set_tag_id(requests[i]->tag_id);

        /* set up the offset */
        multi_header->request_offsets[i] = h2le16((uint16_t)current_offset);

        /* get a pointer to the request. */
        new_req = (eip_cip_co_req *)(requests[i]->data);

        /* calculate the request start and length */
        pkt_start = (uint8_t *)(&new_req->cpf_conn_seq_num) + sizeof(new_req->cpf_conn_seq_num);
        pkt_len = (int)le2h16(new_req->cpf_cdi_item_length) - (int)sizeof(new_req->cpf_conn_seq_num);

        pdebug(DEBUG_INFO, "packet %d is of length %d.", i, pkt_len);

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

    debug_set_tag_id(0);

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int prepare_request(omron_conn_p conn) {
    eip_encap *encap = NULL;
    int payload_size = 0;

    pdebug(DEBUG_INFO, "Starting.");

    encap = (eip_encap *)(conn->data);
    payload_size = (int)conn->data_size - (int)sizeof(eip_encap);

    if(!conn) {
        pdebug(DEBUG_WARN, "Called with null conn!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* fill in the fields of the request. */

    encap->encap_length = h2le16((uint16_t)payload_size);
    encap->encap_conn_handle = h2le32(conn->conn_handle);
    encap->encap_status = h2le32(0);
    encap->encap_options = h2le32(0);

    /* set up the conn sequence ID for this transaction */
    if(le2h16(encap->encap_command) == OMRON_EIP_UNCONNECTED_SEND) {
        /* get new ID */
        conn->conn_seq_id++;

        // request->conn_seq_id = conn->conn_seq_id;
        encap->encap_sender_context = h2le64(conn->conn_seq_id); /* link up the request seq ID and the packet seq ID */

        pdebug(DEBUG_INFO, "Preparing unconnected packet with conn sequence ID %llx", conn->conn_seq_id);
    } else if(le2h16(encap->encap_command) == OMRON_EIP_CONNECTED_SEND) {
        eip_cip_co_req *conn_req = (eip_cip_co_req *)(conn->data);

        pdebug(DEBUG_DETAIL, "cpf_targ_conn_id=%x", conn->targ_connection_id);

        /* set up the connection information */
        conn_req->cpf_targ_conn_id = h2le32(conn->targ_connection_id);

        conn->conn_seq_num++;
        conn_req->cpf_conn_seq_num = h2le16(conn->conn_seq_num);

        pdebug(DEBUG_INFO, "Preparing connected packet with connection ID %x and sequence ID %u(%x)", conn->orig_connection_id,
               conn->conn_seq_num, conn->conn_seq_num);
    } else {
        pdebug(DEBUG_WARN, "Unsupported packet type %x!", le2h16(encap->encap_command));
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* display the data */
    pdebug(DEBUG_INFO, "Prepared packet of size %d", conn->data_size);
    pdebug_dump_bytes(DEBUG_INFO, conn->data, (int)conn->data_size);

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int send_eip_request(omron_conn_p conn, int timeout) {
    int rc = PLCTAG_STATUS_OK;
    int64_t timeout_time = 0;

    pdebug(DEBUG_INFO, "Starting.");

    if(!conn) {
        pdebug(DEBUG_WARN, "Connection pointer is null.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(timeout > 0) {
        timeout_time = time_ms() + timeout;
    } else {
        timeout_time = INT64_MAX;
    }

    pdebug(DEBUG_INFO, "Sending packet of size %d", conn->data_size);
    pdebug_dump_bytes(DEBUG_INFO, conn->data, (int)(conn->data_size));

    conn->data_offset = 0;
    conn->packet_count++;

    /* send the packet */
    do {
        rc = socket_write(conn->sock, conn->data + conn->data_offset, (int)conn->data_size - (int)conn->data_offset,
                          SOCKET_WAIT_TIMEOUT_MS);

        if(rc >= 0) {
            conn->data_offset += (uint32_t)rc;
        } else {
            if(rc == PLCTAG_ERR_TIMEOUT) {
                pdebug(DEBUG_DETAIL, "Socket not yet ready to write.");
                rc = 0;
            }
        }

        /* give up the CPU if we still are looping */
        // if(!conn->terminating && rc >= 0 && conn->data_offset < conn->data_size) {
        //     sleep_ms(1);
        // }
    } while(!conn->terminating && rc >= 0 && conn->data_offset < conn->data_size && timeout_time > time_ms());

    if(conn->terminating) {
        pdebug(DEBUG_WARN, "Connection is terminating.");
        return PLCTAG_ERR_ABORT;
    }

    if(rc < 0) {
        pdebug(DEBUG_WARN, "Error, %d, writing socket!", rc);
        return rc;
    }

    if(timeout_time <= time_ms()) {
        pdebug(DEBUG_WARN, "Timed out waiting to send data!");
        return PLCTAG_ERR_TIMEOUT;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * recv_eip_response
 *
 * Look at the passed conn and read any data we can
 * to fill in a packet.  If we already have a full packet,
 * punt.
 */
int recv_eip_response(omron_conn_p conn, int timeout) {
    uint32_t data_needed = 0;
    int rc = PLCTAG_STATUS_OK;
    int64_t timeout_time = 0;

    pdebug(DEBUG_INFO, "Starting.");

    if(!conn) {
        pdebug(DEBUG_WARN, "Called with null conn!");
        return PLCTAG_ERR_NULL_PTR;
    }


    if(timeout > 0) {
        timeout_time = time_ms() + timeout;
    } else {
        timeout_time = INT64_MAX;
    }

    conn->data_offset = 0;
    conn->data_size = 0;
    data_needed = sizeof(eip_encap);

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
                    pdebug(DEBUG_WARN, "Packet response (%d) is larger than possible buffer size (%d)!", data_needed,
                           conn->data_capacity);
                    return PLCTAG_ERR_TOO_LARGE;
                }
            }
        } else {
            if(rc == PLCTAG_ERR_TIMEOUT) {
                pdebug(DEBUG_DETAIL, "Socket not yet ready to read.");
            } else {
                /* error! */
                pdebug(DEBUG_WARN, "Error reading socket! rc=%d", rc);
                return rc;
            }
        }
    } while(!conn->terminating && conn->data_offset < data_needed && timeout_time > time_ms());

    if(conn->terminating) {
        pdebug(DEBUG_INFO, "Connection is terminating, returning...");
        return PLCTAG_ERR_ABORT;
    }

    if(timeout_time <= time_ms()) {
        pdebug(DEBUG_WARN, "Timed out waiting for data to read!");
        return PLCTAG_ERR_TIMEOUT;
    }

    conn->resp_seq_id = le2h64(((eip_encap *)(conn->data))->encap_sender_context);
    conn->data_size = data_needed;

    rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "request received all needed data (%d bytes of %d).", conn->data_offset, data_needed);

    pdebug_dump_bytes(DEBUG_INFO, conn->data, (int)(conn->data_offset));

    /* check status. */
    if(le2h32(((eip_encap *)(conn->data))->encap_status) != OMRON_EIP_OK) { rc = PLCTAG_ERR_BAD_STATUS; }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


int perform_forward_close(omron_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    do {
        rc = send_forward_close_req(conn);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Sending forward close failed, %s!", plc_tag_decode_error(rc));
            break;
        }

        rc = recv_forward_close_resp(conn);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Forward close response not received, %s!", plc_tag_decode_error(rc));
            break;
        }
    } while(0);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


int send_forward_open_request(omron_conn_p conn) {
    int rc = PLCTAG_STATUS_OK;
    uint16_t max_payload;

    pdebug(DEBUG_INFO, "Starting");

    pdebug(DEBUG_DETAIL, "Flag prohibiting use of extended ForwardOpen is %d.", conn->only_use_old_forward_open);

    max_payload = (conn->only_use_old_forward_open ? (uint16_t)conn->fo_conn_size : (uint16_t)conn->fo_ex_conn_size);

    /* set the max payload guess if it is larger than the maximum possible or if it is zero. */
    conn->max_payload_guess =
        ((conn->max_payload_guess == 0) || (conn->max_payload_guess > max_payload) ? max_payload : conn->max_payload_guess);

    pdebug(DEBUG_DETAIL, "Set Forward Open maximum payload size guess to %d bytes.", conn->max_payload_guess);

    if(conn->only_use_old_forward_open) {
        rc = send_old_forward_open_request(conn);
    } else {
        rc = send_extended_forward_open_request(conn);
    }

    pdebug(DEBUG_INFO, "Done");

    return rc;
}


int send_old_forward_open_request(omron_conn_p conn) {
    eip_forward_open_request_t *fo = NULL;
    uint8_t *data;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    mem_set(conn->data, 0, (int)(sizeof(*fo) + conn->conn_path_size));

    fo = (eip_forward_open_request_t *)(conn->data);

    /* point to the end of the struct */
    data = (conn->data) + sizeof(eip_forward_open_request_t);

    /* set up the path information. */
    mem_copy(data, conn->conn_path, conn->conn_path_size);
    data += conn->conn_path_size;

    /* fill in the static parts */

    /* encap header parts */
    fo->encap_command = h2le16(OMRON_EIP_UNCONNECTED_SEND); /* 0x006F EIP Send RR Data command */
    fo->encap_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->interface_handle))); /* total length of packet except for encap header */
    fo->encap_conn_handle = h2le32(conn->conn_handle);
    fo->encap_sender_context = h2le64(++conn->conn_seq_id);
    fo->router_timeout = h2le16(1); /* one second is enough ? */

    /* CPF parts */
    fo->cpf_item_count = h2le16(2);                     /* ALWAYS 2 */
    fo->cpf_nai_item_type = h2le16(OMRON_EIP_ITEM_NAI); /* null address item type */
    fo->cpf_nai_item_length = h2le16(0);                /* no data, zero length */
    fo->cpf_udi_item_type = h2le16(OMRON_EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fo->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->cm_service_code))); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fo->cm_service_code = OMRON_EIP_CMD_FORWARD_OPEN; /* 0x54 Forward Open Request or 0x5B for Forward Open Extended */
    fo->cm_req_path_size = 2;                         /* size of path in 16-bit words */
    fo->cm_req_path[0] = 0x20;                        /* class */
    fo->cm_req_path[1] = 0x06;                        /* CM class */
    fo->cm_req_path[2] = 0x24;                        /* instance */
    fo->cm_req_path[3] = 0x01;                        /* instance 1 */

    /* Forward Open Params */
    fo->secs_per_tick = OMRON_EIP_SECS_PER_TICK;                 /* seconds per tick, no used? */
    fo->timeout_ticks = OMRON_EIP_TIMEOUT_TICKS;                 /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
    fo->orig_to_targ_conn_id = h2le32(0);                        /* is this right?  Our connection id on the other machines? */
    fo->targ_to_orig_conn_id = h2le32(conn->orig_connection_id); /* Our connection id in the other direction. */
    /* this might need to be globally unique */
    fo->conn_serial_number = h2le16(++(conn->conn_serial_number)); /* our connection SEQUENCE number. */
    fo->orig_vendor_id = h2le16(OMRON_EIP_VENDOR_ID);              /* our unique :-) vendor ID */
    fo->orig_serial_number = h2le32(OMRON_EIP_VENDOR_SN);          /* our serial number. */
    fo->conn_timeout_multiplier = OMRON_EIP_TIMEOUT_MULTIPLIER;    /* timeout = mult * RPI */

    fo->orig_to_targ_rpi = h2le32(OMRON_EIP_RPI); /* us to target RPI - Request Packet Interval in microseconds */

    fo->orig_to_targ_conn_params = h2le16(
        OMRON_EIP_CONN_PARAM | conn->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */

    fo->targ_to_orig_rpi = h2le32(OMRON_EIP_RPI); /* target to us RPI - not really used for explicit messages? */

    fo->targ_to_orig_conn_params = h2le16(
        OMRON_EIP_CONN_PARAM | conn->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */

    fo->transport_class = OMRON_EIP_TRANSPORT_CLASS_T3; /* 0xA3, server transport, class 3, application trigger */
    fo->path_size = conn->conn_path_size / 2;           /* size in 16-bit words */

    /* set the size of the request */
    conn->data_size = (uint32_t)(data - (conn->data));

    rc = send_eip_request(conn, 0);

    pdebug(DEBUG_INFO, "Done");

    return rc;
}


/* new version of Forward Open */
int send_extended_forward_open_request(omron_conn_p conn) {
    eip_forward_open_request_ex_t *fo = NULL;
    uint8_t *data;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    mem_set(conn->data, 0, (int)(sizeof(*fo) + conn->conn_path_size));

    fo = (eip_forward_open_request_ex_t *)(conn->data);

    /* point to the end of the struct */
    data = (conn->data) + sizeof(*fo);

    /* set up the path information. */
    mem_copy(data, conn->conn_path, conn->conn_path_size);
    data += conn->conn_path_size;

    /* fill in the static parts */

    /* encap header parts */
    fo->encap_command = h2le16(OMRON_EIP_UNCONNECTED_SEND); /* 0x006F EIP Send RR Data command */
    fo->encap_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->interface_handle))); /* total length of packet except for encap header */
    fo->encap_conn_handle = h2le32(conn->conn_handle);
    fo->encap_sender_context = h2le64(++conn->conn_seq_id);
    fo->router_timeout = h2le16(1); /* one second is enough ? */

    /* CPF parts */
    fo->cpf_item_count = h2le16(2);                     /* ALWAYS 2 */
    fo->cpf_nai_item_type = h2le16(OMRON_EIP_ITEM_NAI); /* null address item type */
    fo->cpf_nai_item_length = h2le16(0);                /* no data, zero length */
    fo->cpf_udi_item_type = h2le16(OMRON_EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fo->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fo->cm_service_code))); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fo->cm_service_code = OMRON_EIP_CMD_FORWARD_OPEN_EX; /* 0x54 Forward Open Request or 0x5B for Forward Open Extended */
    fo->cm_req_path_size = 2;                            /* size of path in 16-bit words */
    fo->cm_req_path[0] = 0x20;                           /* class */
    fo->cm_req_path[1] = 0x06;                           /* CM class */
    fo->cm_req_path[2] = 0x24;                           /* instance */
    fo->cm_req_path[3] = 0x01;                           /* instance 1 */

    /* Forward Open Params */
    fo->secs_per_tick = OMRON_EIP_SECS_PER_TICK;                 /* seconds per tick, no used? */
    fo->timeout_ticks = OMRON_EIP_TIMEOUT_TICKS;                 /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
    fo->orig_to_targ_conn_id = h2le32(0);                        /* is this right?  Our connection id on the other machines? */
    fo->targ_to_orig_conn_id = h2le32(conn->orig_connection_id); /* Our connection id in the other direction. */
    /* this might need to be globally unique */
    fo->conn_serial_number = h2le16(++(conn->conn_serial_number)); /* our connection ID/serial number. */
    fo->orig_vendor_id = h2le16(OMRON_EIP_VENDOR_ID);              /* our unique :-) vendor ID */
    fo->orig_serial_number = h2le32(OMRON_EIP_VENDOR_SN);          /* our serial number. */
    fo->conn_timeout_multiplier = OMRON_EIP_TIMEOUT_MULTIPLIER;    /* timeout = mult * RPI */
    fo->orig_to_targ_rpi = h2le32(OMRON_EIP_RPI); /* us to target RPI - Request Packet Interval in microseconds */
    fo->orig_to_targ_conn_params_ex = h2le32(
        OMRON_EIP_CONN_PARAM_EX | conn->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    fo->targ_to_orig_rpi = h2le32(OMRON_EIP_RPI);           /* target to us RPI - not really used for explicit messages? */
    fo->targ_to_orig_conn_params_ex = h2le32(
        OMRON_EIP_CONN_PARAM_EX | conn->max_payload_guess); /* packet size and some other things, based on protocol/cpu type */
    fo->transport_class = OMRON_EIP_TRANSPORT_CLASS_T3;     /* 0xA3, server transport, class 3, application trigger */
    fo->path_size = conn->conn_path_size / 2;               /* size in 16-bit words */

    /* set the size of the request */
    conn->data_size = (uint32_t)(data - (conn->data));

    rc = send_eip_request(conn, CONN_DEFAULT_TIMEOUT);

    pdebug(DEBUG_INFO, "Done");

    return rc;
}


int receive_forward_open_response(omron_conn_p conn) {
    eip_forward_open_response_t *fo_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    rc = recv_eip_response(conn, 0);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to receive Forward Open response.");
        return rc;
    }

    fo_resp = (eip_forward_open_response_t *)(conn->data);

    do {
        if(le2h16(fo_resp->encap_command) != OMRON_EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", fo_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(fo_resp->encap_status) != OMRON_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", fo_resp->encap_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(fo_resp->general_status != OMRON_EIP_OK) {
            pdebug(DEBUG_WARN, "Forward Open command failed, response code: %s (%d)",
                   CIP.decode_cip_error_short(&fo_resp->general_status), fo_resp->general_status);
            if(fo_resp->general_status == OMRON_CIP_ERR_UNSUPPORTED_SERVICE) {
                /* this type of command is not supported! */
                pdebug(DEBUG_WARN, "Received CIP command unsupported error from the PLC!");
                rc = PLCTAG_ERR_UNSUPPORTED;
            } else {
                rc = PLCTAG_ERR_REMOTE_ERR;

                if(fo_resp->general_status == 0x01 && fo_resp->status_size >= 2) {
                    /* we might have an error that tells us the actual size to use. */
                    uint8_t *data = &fo_resp->status_size;
                    int extended_status = data[1] | (data[2] << 8);
                    uint16_t supported_size = (uint16_t)((uint16_t)data[3] | (uint16_t)((uint16_t)data[4] << (uint16_t)8));

                    if(extended_status == 0x109) { /* MAGIC */
                        pdebug(DEBUG_WARN, "Error from forward open request, unsupported size, but size %d is supported.",
                               supported_size);
                        conn->max_payload_guess = supported_size;
                        rc = PLCTAG_ERR_TOO_LARGE;
                    } else if(extended_status == 0x100) { /* MAGIC */
                        pdebug(DEBUG_WARN, "Error from forward open request, duplicate connection ID.  Need to try again.");
                        rc = PLCTAG_ERR_DUPLICATE;
                    } else {
                        pdebug(DEBUG_WARN, "CIP extended error %s (%s)!", CIP.decode_cip_error_short(&fo_resp->general_status),
                               CIP.decode_cip_error_long(&fo_resp->general_status));
                    }
                } else {
                    pdebug(DEBUG_WARN, "CIP error code %s (%s)!", CIP.decode_cip_error_short(&fo_resp->general_status),
                           CIP.decode_cip_error_long(&fo_resp->general_status));
                }
            }

            break;
        }

        /* success! */
        conn->targ_connection_id = le2h32(fo_resp->orig_to_targ_conn_id);
        conn->orig_connection_id = le2h32(fo_resp->targ_to_orig_conn_id);

        conn->max_payload_size = conn->max_payload_guess;

        pdebug(DEBUG_INFO, "ForwardOpen succeeded with our connection ID %x and the PLC connection ID %x with packet size %u.",
               conn->orig_connection_id, conn->targ_connection_id, conn->max_payload_size);

        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


int send_forward_close_req(omron_conn_p conn) {
    eip_forward_close_req_t *fc;
    uint8_t *data;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    fc = (eip_forward_close_req_t *)(conn->data);

    /* point to the end of the struct */
    data = (conn->data) + sizeof(*fc);

    /* set up the path information. */
    mem_copy(data, conn->conn_path, conn->conn_path_size);
    data += conn->conn_path_size;

    /* FIXME DEBUG */
    pdebug(DEBUG_DETAIL, "Forward Close connection path:");
    pdebug_dump_bytes(DEBUG_DETAIL, conn->conn_path, conn->conn_path_size);

    /* fill in the static parts */

    /* encap header parts */
    fc->encap_command = h2le16(OMRON_EIP_UNCONNECTED_SEND); /* 0x006F EIP Send RR Data command */
    fc->encap_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fc->interface_handle))); /* total length of packet except for encap header */
    fc->encap_sender_context = h2le64(++conn->conn_seq_id);
    fc->router_timeout = h2le16(1); /* one second is enough ? */

    /* CPF parts */
    fc->cpf_item_count = h2le16(2);                     /* ALWAYS 2 */
    fc->cpf_nai_item_type = h2le16(OMRON_EIP_ITEM_NAI); /* null address item type */
    fc->cpf_nai_item_length = h2le16(0);                /* no data, zero length */
    fc->cpf_udi_item_type = h2le16(OMRON_EIP_ITEM_UDI); /* unconnected data item, 0x00B2 */
    fc->cpf_udi_item_length =
        h2le16((uint16_t)(data - (uint8_t *)(&fc->cm_service_code))); /* length of remaining data in UC data item */

    /* Connection Manager parts */
    fc->cm_service_code = OMRON_EIP_CMD_FORWARD_CLOSE; /* 0x4E Forward Close Request */
    fc->cm_req_path_size = 2;                          /* size of path in 16-bit words */
    fc->cm_req_path[0] = 0x20;                         /* class */
    fc->cm_req_path[1] = 0x06;                         /* CM class */
    fc->cm_req_path[2] = 0x24;                         /* instance */
    fc->cm_req_path[3] = 0x01;                         /* instance 1 */

    /* Forward Open Params */
    fc->secs_per_tick = OMRON_EIP_SECS_PER_TICK;               /* seconds per tick, no used? */
    fc->timeout_ticks = OMRON_EIP_TIMEOUT_TICKS;               /* timeout = srd_secs_per_tick * src_timeout_ticks, not used? */
    fc->conn_serial_number = h2le16(conn->conn_serial_number); /* our connection SEQUENCE number. */
    fc->orig_vendor_id = h2le16(OMRON_EIP_VENDOR_ID);          /* our unique :-) vendor ID */
    fc->orig_serial_number = h2le32(OMRON_EIP_VENDOR_SN);      /* our serial number. */
    fc->path_size = conn->conn_path_size / 2;                  /* size in 16-bit words */
    fc->reserved = (uint8_t)0;                                 /* padding for the path. */

    /* set the size of the request */
    conn->data_size = (uint32_t)(data - (conn->data));

    rc = send_eip_request(conn, 100);

    pdebug(DEBUG_INFO, "Done");

    return rc;
}


int recv_forward_close_resp(omron_conn_p conn) {
    eip_forward_close_resp_t *fo_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting");

    rc = recv_eip_response(conn, 150);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to receive Forward Close response, %s!", plc_tag_decode_error(rc));
        return rc;
    }

    fo_resp = (eip_forward_close_resp_t *)(conn->data);

    do {
        if(le2h16(fo_resp->encap_command) != OMRON_EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_WARN, "Unexpected EIP packet type received: %d!", fo_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(fo_resp->encap_status) != OMRON_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", fo_resp->encap_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(fo_resp->general_status != OMRON_EIP_OK) {
            pdebug(DEBUG_WARN, "Forward Close command failed, response code: %d", fo_resp->general_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        pdebug(DEBUG_INFO, "Connection close succeeded.");

        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


int conn_create_request(omron_conn_p conn, int tag_id, omron_request_p *req) {
    int rc = PLCTAG_STATUS_OK;
    omron_request_p res;
    size_t request_capacity = 0;
    uint8_t *buffer = NULL;

    critical_block(conn->mutex) {
        int max_payload_size = GET_MAX_PAYLOAD_SIZE(conn);

        // FIXME: no logging in a mutex!
        // pdebug(DEBUG_DETAIL, "FIXME: max payload size %d", max_payload_size);

        request_capacity = (size_t)(max_payload_size + EIP_CIP_PREFIX_SIZE);
    }

    pdebug(DEBUG_DETAIL, "Starting.");

    buffer = (uint8_t *)mem_alloc((int)request_capacity);
    if(!buffer) {
        pdebug(DEBUG_WARN, "Unable to allocate request buffer!");
        *req = NULL;
        return PLCTAG_ERR_NO_MEM;
    }

    res = (omron_request_p)rc_alloc((int)sizeof(struct omron_request_t), request_destroy);
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

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


/*
 * request_destroy
 *
 * The request must be removed from any lists before this!
 */

void request_destroy(void *req_arg) {
    omron_request_p req = req_arg;

    pdebug(DEBUG_DETAIL, "Starting.");

    req->abort_request = 1;

    if(req->data) {
        mem_free(req->data);
        req->data = NULL;
    }

    pdebug(DEBUG_DETAIL, "Done.");
}


int conn_request_increase_buffer(omron_request_p request, int new_capacity) {
    uint8_t *old_buffer = NULL;
    uint8_t *new_buffer = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    new_buffer = (uint8_t *)mem_alloc(new_capacity);
    if(!new_buffer) {
        pdebug(DEBUG_WARN, "Unable to allocate larger request buffer!");
        return PLCTAG_ERR_NO_MEM;
    }

    spin_block(&request->lock) {
        old_buffer = request->data;
        request->request_capacity = new_capacity;
        request->data = new_buffer;
    }

    mem_free(old_buffer);

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}
