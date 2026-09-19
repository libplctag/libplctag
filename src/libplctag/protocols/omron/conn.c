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

#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/cip/error_codes.h>
#include <libplctag/protocols/cip/request.h>
#include <libplctag/protocols/omron/cip.h>
#include <libplctag/protocols/omron/conn.h>
#include <libplctag/protocols/omron/defs.h>
#include <libplctag/protocols/omron/omron_common.h>
#include <libplctag/protocols/omron/tag.h>
#include <limits.h>
#include <utils/mem.h>
#include <utils/mutex.h>
#include <utils/nap.h>
#include <utils/rc.h>
#include <utils/socket.h>
#include <utils/spinlock.h>
#include <utils/str.h>
#include <utils/thread.h>
#include <utils/time.h>
#include <stdlib.h>
#include <time.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>
#include <utils/random_utils.h>

#define MAX_REQUESTS (400)


/*
 * The payload size to ASK FOR in a Forward Open.  These are opening bids, not
 * measured limits: a PLC that cannot manage the requested size rejects the
 * Forward Open and reports what it does support, and receive_forward_open_response()
 * clamps the guess to that and retries (see the PLCTAG_ERR_TOO_LARGE arm of the
 * connection state machine).  Asking too high therefore costs one extra round trip
 * at connect time, while asking too low is silent and permanent and costs
 * throughput on every transfer for the life of the connection -- so these err
 * high.
 *
 * Neither number is derived from a specification and neither has been checked
 * against hardware; an NJ/NX is reported to top out nearer 1892.  Settling it
 * needs one connection against a real PLC: the "unsupported size" branch logs the
 * size the PLC reports.  Until then the negotiation is doing the real work.
 */
#define MAX_CIP_OMRON_MSG_SIZE_EX (0xFFFF & 1990)
#define MAX_CIP_OMRON_MSG_SIZE (0x01FF & 502)


/*
 * Number of milliseconds to wait to try to set up the conn again
 * after a failure.
 */
#define RETRY_WAIT_INITIAL_MS (100)
#define RETRY_WAIT_MAX_MS (10000)

/* Idle time to wait before disconnecting.  Set it to one second less than we negotiate with the PLC. */

#define SOCKET_WAIT_TIMEOUT_MS (20)
#define CONN_IDLE_WAIT_TIME (100)



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
static int64_t calc_retry_time(unsigned int retry_count);
static int process_requests(omron_conn_p conn);
// static int check_packing(omron_conn_p conn, omron_request_p request);
static int get_payload_size(omron_request_p request) { return cip_get_payload_size(request); }
static int pack_requests(omron_conn_p conn, omron_request_p *requests, int num_requests) {
    return cip_pack_requests((cip_conn_p)conn, requests, num_requests);
}
static int prepare_request(omron_conn_p conn);
static int send_eip_request(omron_conn_p conn, int timeout);
static int recv_eip_response(omron_conn_p conn, int timeout);
// static int perform_forward_open(omron_conn_p conn);
// static int try_forward_open_ex(omron_conn_p conn, int *max_payload_size_guess);
// static int try_forward_open(omron_conn_p conn);
// static int send_forward_open_req(omron_conn_p conn);
// static int send_forward_open_req_ex(omron_conn_p conn);
// static int recv_forward_open_resp(omron_conn_p conn, int *max_payload_size_guess);
static int send_forward_open_request(omron_conn_p conn);
static int receive_forward_open_response(omron_conn_p conn);


static volatile mutex_p conn_mutex = NULL;
static volatile vector_p conns = NULL;

/* Track active handler threads for proper shutdown synchronization */
static atomic_int32_t handler_threads_active = ATOMIC_INT_STATIC_INIT;


/* the Forward Open itself is shared; see protocols/cip/conn.h. */
static const cip_conn_io_t conn_io = {
    .send_request = (int (*)(cip_conn_p, int))send_eip_request,
    .recv_response = (int (*)(cip_conn_p, int))recv_eip_response,
};


int conn_create_request(omron_conn_p conn, int tag_id, omron_request_p *req) {
    return cip_conn_create_request((cip_conn_p)conn, tag_id, req);
}




/* these are shared; see protocols/cip/conn.h. */
static int unpack_response(omron_conn_p conn, omron_request_p request, int sub_packet) {
    return cip_unpack_response((cip_conn_p)conn, request, sub_packet);
}


static int perform_forward_close(omron_conn_p conn) { return cip_perform_forward_close((cip_conn_p)conn, &conn_io); }


int conn_get_available_cip_payload_space(omron_conn_p conn) {
    return cip_conn_get_available_payload_space((cip_conn_p)conn);
}




static int send_forward_open_request(omron_conn_p conn) { return cip_send_forward_open((cip_conn_p)conn, &conn_io); }


static int receive_forward_open_response(omron_conn_p conn) {
    return cip_receive_forward_open_response((cip_conn_p)conn, &conn_io);
}


int conn_startup(void) {
    int rc = PLCTAG_STATUS_OK;

    if((rc = mutex_create((mutex_p *)&conn_mutex)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_ERROR, 0, "Unable to create conn mutex %s!", plc_tag_decode_error(rc));
        return rc;
    }

    if((conns = vector_create(25, 5)) == NULL) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_ERROR, 0, "Unable to create conn vector!");
        return PLCTAG_ERR_NO_MEM;
    }

    return rc;
}


void conn_teardown(void) {
    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Starting.");

    /* Mark all conns as terminating to wake handler threads quickly. */
    if(conns && conn_mutex) {
        critical_block(conn_mutex) {
            int n = vector_length(conns);
            for(int i = 0; i < n; i++) {
                omron_conn_p conn = vector_get(conns, i);
                if(conn) {
                    atomic_set_int32(&conn->terminating, 1);
                    if(conn->nap) { nap_interrupt(conn->nap); }
                }
            }
        }
    }

    if(conns && conn_mutex) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Waiting for conns to terminate.");

        while(1) {
            int remaining_conns = 0;

            critical_block(conn_mutex) { remaining_conns = vector_length(conns); }

            if(remaining_conns > 0) {
                sleep_ms(10);
            } else {
                break;
            }
        }

        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Connections all terminated.");

        vector_destroy(conns);

        conns = NULL;
    }

    /* Wait for handler threads BEFORE destroying conn_mutex — handler threads
     * may still be holding conn->mutex (not conn_mutex) during their final
     * cleanup, and musl returns freed memory to the OS immediately, making
     * any use-after-free a SIGSEGV. */
    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Waiting for handler threads to complete.");
    int64_t start_time = time_ms();
    int64_t timeout_ms = 5000;
    int active_count = 0;
    int64_t elapsed = 0;

    while((active_count = atomic_get_int32(&handler_threads_active)) > 0) {
        elapsed = time_ms() - start_time;

        if(elapsed >= timeout_ms) {
            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Timeout waiting for %d handler threads to complete.", active_count);
            break;
        }

        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Waiting for %d handler threads to complete. Elapsed: %" PRId64 "ms",
               active_count, elapsed);
        sleep_ms(20);
    }

    if(active_count == 0) { pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "All handler threads completed."); }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Destroying conn mutex.");

    if(conn_mutex) {
        mutex_destroy((mutex_p *)&conn_mutex);
        conn_mutex = NULL;
    }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Done.");
}


/* the shared version skips zero on rollover; see protocols/cip/conn.h. */
uint64_t conn_get_new_seq_id(omron_conn_p conn) { return cip_conn_get_new_seq_id((cip_conn_p)conn); }


int conn_get_max_payload(omron_conn_p conn) {
    int result = 0;

    if(!conn) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Called with null conn pointer!");
        return 0;
    }

    critical_block(conn->mutex) { result = GET_MAX_PAYLOAD_SIZE(conn); }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "max payload size is %d bytes.", result);

    return result;
}


int conn_find_or_create(omron_conn_p *tag_conn, attr attribs, int *is_new_conn) {
    /*int debug = attr_get_int(attribs,"debug",0);*/
    const char *conn_gw = attr_get_str(attribs, "gateway", "");
    const char *conn_path = attr_get_str(attribs, "path", "");
    int use_connected_msg = attr_get_int(attribs, "use_connected_msg", 0);
    // int conn_gw_port = attr_get_int(attribs, "gateway_port", EIP_DEFAULT_PORT);
    //  plc_type_t plc_type = get_plc_type(attribs);
    omron_conn_p conn = OMRON_CONN_NULL;
    int new_conn = 0;
    int shared_conn = attr_get_int(attribs, "share_conn", 1); /* share the conn by default. */
    int rc = PLCTAG_STATUS_OK;
    int connection_inactivity_timeout_ms = CIP_DISCONNECT_TIMEOUT;
    int connection_group_id = attr_get_int(attribs, "connection_group_id", 0);
    int only_use_old_forward_open = attr_get_int(attribs, "conn_only_use_old_forward_open", 0);

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Starting");

    connection_inactivity_timeout_ms = attr_get_int(attribs, "connection_inactivity_timeout_ms", CIP_DISCONNECT_TIMEOUT);
    if(connection_inactivity_timeout_ms < 1 || connection_inactivity_timeout_ms > CIP_DISCONNECT_TIMEOUT) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0,
               "Invalid connection_inactivity_timeout_ms %d. Must be between 1 and %d. Using default %d.",
               connection_inactivity_timeout_ms, CIP_DISCONNECT_TIMEOUT, CIP_DISCONNECT_TIMEOUT);
        connection_inactivity_timeout_ms = CIP_DISCONNECT_TIMEOUT;
    } else {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Setting connection_inactivity_timeout_ms to %dms.",
               connection_inactivity_timeout_ms);
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
            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Creating new conn.");

            conn = create_omron_njnx_conn_unsafe(conn_gw, conn_path, &use_connected_msg, connection_group_id);
            if(conn == OMRON_CONN_NULL) {
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "unable to create or find a conn!");
                rc = PLCTAG_ERR_BAD_GATEWAY;
            } else {
                atomic_init_int32(&conn->connection_inactivity_timeout_ms, connection_inactivity_timeout_ms);

                /* see if we have an attribute set for forcing the use of the older ForwardOpen */
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0,
                       "Passed attribute to prohibit use of extended ForwardOpen is %d.", only_use_old_forward_open);
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0,
                       "Existing attribute to prohibit use of extended ForwardOpen is %d.", conn->only_use_old_forward_open);
                conn->only_use_old_forward_open = (conn->only_use_old_forward_open ? 1 : only_use_old_forward_open);

                new_conn = 1;
            }
        } else {
            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Reusing existing conn.");
        }
    }

    /*
     * do this OUTSIDE the mutex in order to let other threads not block if
     * the conn creation process blocks.
     */

    if(new_conn) {
        rc = conn_init(conn);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "rc_dec: Releasing reference to the PLC connection.");
            rc_dec(conn);
            conn = OMRON_CONN_NULL;
        } else {
            /* save the status */
            // conn->status = rc;
        }
    }

    /* store it into the tag */
    *tag_conn = conn;

    if(is_new_conn) { *is_new_conn = new_conn; }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Done");

    return rc;
}


int add_conn_unsafe(omron_conn_p conn) {
    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Starting");

    if(!conn) { return PLCTAG_ERR_NULL_PTR; }

    vector_set(conns, vector_length(conns), conn);

    conn->on_list = 1;

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Done");

    return PLCTAG_STATUS_OK;
}


int add_conn(omron_conn_p s) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Starting.");

    critical_block(conn_mutex) { rc = add_conn_unsafe(s); }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


int remove_conn_unsafe(omron_conn_p conn) {
    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Starting");

    if(!conn || !conns) { return 0; }

    for(int i = 0; i < vector_length(conns); i++) {
        omron_conn_p tmp = vector_get(conns, i);

        if(tmp == conn) {
            vector_remove(conns, i);
            break;
        }
    }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Done");

    return PLCTAG_STATUS_OK;
}

int remove_conn(omron_conn_p s) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Starting.");

    if(s->on_list) {
        critical_block(conn_mutex) { rc = remove_conn_unsafe(s); }
    }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


int conn_match_valid(const char *host, const char *path, omron_conn_p conn) {
    if(!conn) { return 0; }

    /* don't use conns that failed immediately. */
    if(conn->failed) { return 0; }

    if(!str_length(host)) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "New conn host is NULL or zero length!");
        return 0;
    }

    if(!str_length(conn->host)) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Connection host is NULL or zero length!");
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
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "rc_inc: Acquiring reference to the PLC connection.");
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

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Starting.");

    do {
        conn = conn_create_unsafe(MAX_CIP_OMRON_MSG_SIZE_EX, true, host, path, OMRON_PLC_OMRON_NJNX, use_connected_msg,
                                  connection_group_id);
        if(conn != NULL) {
            conn->only_use_old_forward_open = false;
            conn->plc_config.fo_conn_size = MAX_CIP_OMRON_MSG_SIZE;
            conn->plc_config.fo_ex_conn_size = MAX_CIP_OMRON_MSG_SIZE_EX;
            conn->plc_config.min_payload_size = CIP_MIN_PAYLOAD_SIZE_CIP;
            /* NJ/NX has a 0x80 data segment mechanism, not implemented yet. */
            conn->plc_config.supports_fragmented_operations = false;
            conn->plc_config.supports_packed_requests = true;
            conn->max_payload_size = (uint16_t)conn->plc_config.fo_conn_size;
        } else {
            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to create *Logix conn!");
        }
    } while(0);

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Done.");

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

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Starting");

    /*
     * The host string is copied into this allocation verbatim, so its length is part of the
     * conn's size.  It comes straight from the "gateway" attribute with nothing between the
     * application and here, so bound it: without this a caller can size the conn object
     * arbitrarily.
     */
    if(!host || str_length(host) >= MAX_CONN_HOST_LEN) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Gateway string is missing or longer than the maximum of %d bytes!",
               MAX_CONN_HOST_LEN - 1);
        return OMRON_CONN_NULL;
    }

    /*
     * The path string is copied in verbatim too, and it is not self-limiting: spaces are
     * skipped everywhere in the path encoder, so an arbitrarily long string can still encode
     * to a valid short path.  Bound it against the encoded path buffer -- a real route is a
     * handful of hops, so this rejects nothing that describes real hardware.
     */
    if(path && str_length(path) >= MAX_CONN_PATH) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Path string is longer than the maximum of %d bytes!", MAX_CONN_PATH - 1);
        return OMRON_CONN_NULL;
    }

    if(*use_connected_msg) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Connection should use connected messaging.");
    } else {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Connection should not use connected messaging.");
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
    rc = omron_encode_path(path, use_connected_msg, &tmp_conn_path[0], &tmp_conn_path_size);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Unable to convert path string to binary path, error %s!",
               plc_tag_decode_error(rc));
        return NULL;
    }

    conn_path_offset = total_allocation_size;
    total_allocation_size += tmp_conn_path_size;

    /* allocate the conn struct and the buffer in the same allocation. */
    pdebug(
        DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0,
        "Allocating %d total bytes of memory with %d bytes for data buffer static data, %d bytes for the host name, %d bytes for the path, %d bytes for the encoded path.",
        total_allocation_size, (data_buffer_is_static ? data_buffer_capacity : 0), str_length(host) + 1,
        (path_offset == 0 ? 0 : str_length(path) + 1), tmp_conn_path_size);

    conn = (omron_conn_p)rc_alloc(total_allocation_size, conn_destroy);
    if(!conn) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Error allocating new conn!");
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
            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to allocate the connection data buffer!");
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
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to allocate vector for requests!");
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "rc_dec: Releasing reference to the PLC connection.");
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
    atomic_init_int32(&conn->connection_status, PLCTAG_CONN_STATUS_DOWN);

    /* conn_event_ring_write_idx always points at the ring slot holding the current
     * state, not the next free slot: conn_set_connection_status() dedups a new
     * event against ring[write_idx] before writing ring[write_idx+1], and a
     * connection tag seeds event_ring_read_idx to this same index to mean "I've
     * already seen this one." Both of those need ring[0] to hold a real DOWN
     * entry, not the zeroed garbage rc_alloc() leaves behind. */
    conn->conn_event_ring[0].event_type = PLCTAG_CONN_STATUS_DOWN + PLCTAG_EVENT_CONN_STATUS_OFFSET;
    conn->conn_event_ring[0].status = PLCTAG_STATUS_OK;
    atomic_init_int32(&conn->conn_event_ring_write_idx, 0);

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Setting connection_group_id to %d.", connection_group_id);
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

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Done");

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

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Starting.");

    /* create the conn mutex. */
    if((rc = mutex_create(&(conn->mutex))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to create conn mutex!");
        conn->failed = 1;
        return rc;
    }

    /* create the conn nap. */
    if((rc = nap_create(&(conn->nap))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to create conn condition var!");
        conn->failed = 1;
        return rc;
    }

    if((rc = thread_create((thread_p *)&(conn->handler_thread), conn_handler, 32 * 1024, conn)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to create conn thread!");
        conn->failed = 1;
        return rc;
    }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Done.");

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

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Starting.");

    /*
     * A reconnect reuses this conn, so drop the identity of the connection that just went
     * away.  Otherwise the checks in recv_eip_response() compare the new conn's
     * RegisterSession reply against the old handle and reject it, and the conn can never
     * come back up.
     */
    conn->conn_handle = 0;
    conn->req_encap_command = 0;
    conn->req_seq_id = 0;
    conn->req_sent = false;

    /* Open a socket for communication with the gateway. */
    rc = socket_create(&(conn->sock));

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to create socket for conn!");
        return rc;
    }

    server_port = str_split(conn->host, ":");
    if(!server_port) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to split server and port string!");
        return PLCTAG_ERR_BAD_CONFIG;
    }

    if(server_port[0] == NULL) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Server string is malformed or empty!");
        mem_free(server_port);
        return PLCTAG_ERR_BAD_CONFIG;
    }

    if(server_port[1] != NULL) {
        rc = str_to_int(server_port[1], &port);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to extract port number from server string \"%s\"!",
                   conn->host);
            mem_free(server_port);
            return PLCTAG_ERR_BAD_CONFIG;
        }

        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Using special port %d.", port);
    } else {
        port = EIP_DEFAULT_PORT;

        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Using default port %d.", port);
    }

    rc = socket_connect_tcp_start(conn->sock, server_port[0], port);

    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to connect socket for conn!");
        mem_free(server_port);
        return rc;
    }

    if(server_port) { mem_free(server_port); }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Done.");

    return rc;
}


int conn_register(omron_conn_p conn) {
    eip_session_reg_req *req;
    eip_encap *resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Starting.");

    /*
     * clear the conn data.
     *
     * We use the receiving buffer because we do not have a request and nothing can
     * be coming in (we hope) on the socket yet.
     */
    mem_set(conn->data, 0, sizeof(eip_session_reg_req));

    req = (eip_session_reg_req *)(conn->data);

    /* fill in the fields of the request */
    req->encap_command = h2le16(EIP_REGISTER_SESSION);
    req->encap_length = h2le16(sizeof(eip_session_reg_req) - sizeof(eip_encap));
    req->encap_session_handle = h2le32(/*conn->conn_handle*/ 0);
    req->encap_status = h2le32(0);
    req->encap_sender_context = h2le64((uint64_t)0);
    req->encap_options = h2le32(0);

    req->eip_version = h2le16(EIP_VERSION);
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

    rc = send_eip_request(conn, CONN_DEFAULT_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Error sending conn registration request %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* get the response from the gateway */
    rc = recv_eip_response(conn, CONN_DEFAULT_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Error receiving conn registration response %s!",
               plc_tag_decode_error(rc));
        return rc;
    }

    /* encap header is at the start of the buffer */
    resp = (eip_encap *)(conn->data);

    /* check the response status */
    if(le2h16(resp->encap_command) != EIP_REGISTER_SESSION) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "EIP unexpected response packet type: %" PRIu16 "!",
               le2h16(resp->encap_command));
        return PLCTAG_ERR_BAD_DATA;
    }

    if(le2h32(resp->encap_status) != EIP_OK) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "EIP command failed, response code: %d", le2h32(resp->encap_status));
        return PLCTAG_ERR_REMOTE_ERR;
    }

    /*
     * after all that, save the conn handle, we will
     * use it in future packets.
     */
    conn->conn_handle = le2h32(resp->encap_session_handle);

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


int conn_unregister(omron_conn_p conn) {
    (void)conn;

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Starting.");

    /* nothing to do, perhaps. */

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


int conn_close_socket(omron_conn_p conn) {
    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Starting.");

    if(conn->sock) {
        socket_close(conn->sock);
        socket_destroy(&(conn->sock));
        conn->sock = NULL;
    }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


void conn_destroy(void *conn_arg) {
    omron_conn_p conn = conn_arg;

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Starting.");

    if(!conn) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Connection ptr is null!");

        return;
    }

    /* so remove the conn from the list so no one else can reference it. */
    remove_conn(conn);

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Connection sent %" PRId64 " packets.", conn->packet_count);

    /* terminate the conn thread first. */
    atomic_set_int32(&conn->terminating, 1);

    /* interrupt the nap in case the handler is sleeping */
    if(conn->nap) { nap_interrupt(conn->nap); }

    /* get rid of the handler thread. */
    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Destroying conn thread.");
    if(conn->handler_thread) {
        /* this cannot be guarded by the mutex since the conn thread also locks it. */
        thread_join(&(conn->handler_thread));
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
            atomic_set_int32(&conn->terminating, 0);
            perform_forward_close(conn);
            atomic_set_int32(&conn->terminating, 1);
        }

        /* try to be nice and un-register the conn */
        if(conn->conn_handle) { conn_unregister(conn); }

        if(conn->sock) { conn_close_socket(conn); }

        /* release all the requests that are in the queue. */
        if(conn->requests) {
            for(int i = 0; i < vector_length(conn->requests); i++) {
                omron_request_p req = vector_get(conn->requests, i);
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "rc_dec: Releasing reference request for tag %" PRId32 ".",
                       req->tag_id);
                rc_dec(req);
            }

            vector_destroy(conn->requests);
            conn->requests = NULL;
        }
    }

    /* we are done with the nap, finally destroy it. */
    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Destroying conn nap.");
    if(conn->nap) {
        nap_destroy(&(conn->nap));
        conn->nap = NULL;
    }

    /* we are done with the mutex, finally destroy it. */
    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Destroying conn mutex.");
    if(conn->mutex) {
        mutex_destroy(&(conn->mutex));
        conn->mutex = NULL;
    }

    if(!conn->data_buffer_is_static) { mem_free(conn->data); }

    /* these are all allocated in one large block. */

    // pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0,  "Cleaning up allocated memory for paths and host name.");
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

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Done.");

    return;
}


/*
 * conn_add_request_unsafe
 *
 * You must hold the mutex before calling this!
 */
int conn_add_request_unsafe(omron_conn_p conn, omron_request_p req) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, req->tag_id, "Starting.");

    if(!conn) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, req->tag_id, "Connection is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, req->tag_id, "rc_inc: Acquiring reference to the request.");
    req = rc_inc(req);

    if(!req) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Request is either null or in the process of being deleted.");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* make sure the request points to the conn */

    /* insert into the requests vector */
    vector_set(conn->requests, vector_length(conn->requests), req);

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, req->tag_id, "Total requests in the queue: %d", vector_length(conn->requests));

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, req->tag_id, "Done.");

    return rc;
}

/*
 * conn_add_request
 *
 * This is a thread-safe version of the above routine.
 */
int conn_add_request(omron_conn_p conn, omron_request_p req) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, req->tag_id, "Starting. conn=%p, req=%p", (void *)conn, (void *)req);

    critical_block(conn->mutex) { rc = conn_add_request_unsafe(conn, req); }

    nap_interrupt(conn->nap);

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, req->tag_id, "Done.");

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
    CONN_WAIT_ERR_RETRY,
    CONN_WAIT_IDLE_RECONNECT
} conn_state_t;


/* connection_status and the ring publish must change together under conn->mutex:
 * omron_connection_tag_create() takes a paired snapshot of both (conn_event_ring_write_idx
 * and connection_status) to seed a freshly created connection tag, and needs the same
 * mutex to avoid reading one from before this transition and the other from after it --
 * see the comment there. */
static inline void conn_set_connection_status(omron_conn_p conn, int32_t new_status) {
    critical_block(conn->mutex) {
        int32_t old_status = atomic_get_int32(&conn->connection_status);

        if(old_status != new_status) {
            atomic_set_int32(&conn->connection_status, new_status);
            int32_t cur_idx = atomic_get_int32(&conn->conn_event_ring_write_idx);
            int32_t event_type = new_status + PLCTAG_EVENT_CONN_STATUS_OFFSET;

            if(conn->conn_event_ring[cur_idx].event_type == event_type
               && conn->conn_event_ring[cur_idx].status == PLCTAG_STATUS_OK) {
                break;
            }

            cur_idx = (cur_idx + 1) & OMRON_CONN_EVENT_RING_MASK;
            conn->conn_event_ring[cur_idx].event_type = event_type;
            conn->conn_event_ring[cur_idx].status = PLCTAG_STATUS_OK;
            atomic_set_int32(&conn->conn_event_ring_write_idx, cur_idx);
            plc_tag_tickler_wake();
        }
    }
}


static inline void conn_publish_event(omron_conn_p conn, int32_t event_type, int32_t status) {
    int32_t cur_idx = atomic_get_int32(&conn->conn_event_ring_write_idx);
    cur_idx = (cur_idx + 1) & OMRON_CONN_EVENT_RING_MASK;
    conn->conn_event_ring[cur_idx].event_type = event_type;
    conn->conn_event_ring[cur_idx].status = status;
    atomic_set_int32(&conn->conn_event_ring_write_idx, cur_idx);
    plc_tag_tickler_wake();
}


int64_t calc_retry_time(unsigned int retry_count) {
    int64_t result = RETRY_WAIT_INITIAL_MS * (int64_t)(1 << retry_count);
    if(result > RETRY_WAIT_MAX_MS) { result = RETRY_WAIT_MAX_MS; }
    result += (int64_t)random_u64(RETRY_WAIT_INITIAL_MS) - (int64_t)(RETRY_WAIT_INITIAL_MS / 2);
    return result;
}


THREAD_FUNC(conn_handler) {
    omron_conn_p conn = arg;
    int rc = PLCTAG_STATUS_OK;
    conn_state_t state = CONN_OPEN_SOCKET_START;
    int64_t timeout_time = 0;
    int64_t wait_until_time = 0;
    int32_t inactivity_timeout_ms = atomic_get_int32(&conn->connection_inactivity_timeout_ms);
    int64_t auto_disconnect_time = time_ms() + inactivity_timeout_ms;
    unsigned int retry_count = 0;
    int auto_disconnect = 0;


    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Starting thread for conn %p", (void *)conn);

    /* Increment the count of active handler threads */
    atomic_add_int32(&handler_threads_active, 1);

    while(!atomic_get_int32(&conn->terminating) && atomic_get_bool(&lib_active)) {
        /* how long should we wait if nothing wakes us? */
        wait_until_time = time_ms() + CONN_IDLE_WAIT_TIME;

        /*
         * Do this on every cycle.   This keeps the queue clean(ish).
         *
         * Make sure we get rid of all the aborted requests queued.
         * This keeps the overall memory usage lower.
         */

        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_SPEW, 0, "Critical block.");
        critical_block(conn->mutex) { purge_aborted_requests_unsafe(conn); }

        switch(state) {
            case CONN_OPEN_SOCKET_START:
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "in CONN_OPEN_SOCKET_START state.");
                conn_set_connection_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

                /* we must connect to the gateway*/
                rc = conn_open_socket(conn);
                if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "conn connect failed %s!", plc_tag_decode_error(rc));
                    state = CONN_CLOSE_SOCKET;
                } else {
                    if(rc == PLCTAG_STATUS_OK) {
                        /* bump auto disconnect time into the future so that we do not accidentally disconnect immediately. */
                        inactivity_timeout_ms = atomic_get_int32(&conn->connection_inactivity_timeout_ms);
                        auto_disconnect_time = time_ms() + inactivity_timeout_ms;

                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0,
                               "Connect complete immediately, going to state CONN_REGISTER.");

                        state = CONN_REGISTER;

                        retry_count = 0;
                    } else {
                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0,
                               "Connect started, going to state CONN_OPEN_SOCKET_WAIT.");

                        state = CONN_OPEN_SOCKET_WAIT;
                    }
                }

                /* in all cases, don't wait. */
                nap_interrupt(conn->nap);

                break;

            case CONN_OPEN_SOCKET_WAIT:
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "in CONN_OPEN_SOCKET_WAIT state.");
                conn_set_connection_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

                /* we must connect to the gateway */
                rc = socket_connect_tcp_check(conn->sock, 20); /* MAGIC */
                if(rc == PLCTAG_STATUS_OK) {
                    /* connected! */
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Socket connection succeeded.");

                    /* calculate the disconnect time. */
                    inactivity_timeout_ms = atomic_get_int32(&conn->connection_inactivity_timeout_ms);
                    auto_disconnect_time = time_ms() + inactivity_timeout_ms;

                    state = CONN_REGISTER;
                } else if(rc == PLCTAG_ERR_TIMEOUT) {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Still waiting for connection to succeed.");

                    /* don't wait more.  The TCP connect check will wait in select(). */
                } else {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Connection connect failed %s!", plc_tag_decode_error(rc));
                    state = CONN_CLOSE_SOCKET;
                }

                /* in all cases, don't wait. */
                nap_interrupt(conn->nap);

                break;

            case CONN_REGISTER:
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "in CONN_REGISTER state.");
                conn_set_connection_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

                if((rc = conn_register(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "conn registration failed %s!", plc_tag_decode_error(rc));
                    state = CONN_CLOSE_SOCKET;
                } else {
                    if(conn->use_connected_msg) {
                        state = CONN_SEND_FORWARD_OPEN;
                    } else {
                        state = CONN_IDLE;
                    }
                }
                nap_interrupt(conn->nap);
                break;

            case CONN_SEND_FORWARD_OPEN:
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "in CONN_SEND_FORWARD_OPEN state.");
                conn_set_connection_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

                if((rc = send_forward_open_request(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Send Forward Open failed %s!", plc_tag_decode_error(rc));
                    state = CONN_UNREGISTER;
                } else {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0,
                           "Send Forward Open succeeded, going to CONN_RECEIVE_FORWARD_OPEN state.");
                    state = CONN_RECEIVE_FORWARD_OPEN;
                }
                nap_interrupt(conn->nap);
                break;

            case CONN_RECEIVE_FORWARD_OPEN:
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "in CONN_RECEIVE_FORWARD_OPEN state.");
                conn_set_connection_status(conn, PLCTAG_CONN_STATUS_CONNECTING);

                if((rc = receive_forward_open_response(conn)) != PLCTAG_STATUS_OK) {
                    if(rc == PLCTAG_ERR_DUPLICATE) {
                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0,
                               "Duplicate connection error received, trying again with different connection ID.");
                        state = CONN_SEND_FORWARD_OPEN;
                    } else if(rc == PLCTAG_ERR_TOO_LARGE) {
                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0,
                               "Requested packet size too large, retrying with smaller size.");
                        state = CONN_SEND_FORWARD_OPEN;
                    } else if(rc == PLCTAG_ERR_UNSUPPORTED && !conn->only_use_old_forward_open) {
                        /* if we got an unsupported error and we are trying with ForwardOpenEx, then try the old command. */
                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0,
                               "PLC does not support ForwardOpenEx, trying old ForwardOpen.");
                        conn->only_use_old_forward_open = 1;
                        state = CONN_SEND_FORWARD_OPEN;
                    } else {
                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Receive Forward Open failed %s!",
                               plc_tag_decode_error(rc));
                        state = CONN_UNREGISTER;
                    }
                } else {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Send Forward Open succeeded, going to CONN_IDLE state.");
                    state = CONN_IDLE;
                }
                nap_interrupt(conn->nap);
                break;

            case CONN_IDLE:
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "in CONN_IDLE state.");
                conn_set_connection_status(conn, PLCTAG_CONN_STATUS_UP);

                /* make sure that our timeout period has not changed */
                if(inactivity_timeout_ms != atomic_get_int32(&conn->connection_inactivity_timeout_ms)) {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0,
                           "Inactivity timeout changed from %" PRId32 "ms to %" PRId32 "ms, updating auto disconnect time.",
                           inactivity_timeout_ms, atomic_get_int32(&conn->connection_inactivity_timeout_ms));
                    inactivity_timeout_ms = atomic_get_int32(&conn->connection_inactivity_timeout_ms);
                    auto_disconnect_time = time_ms() + inactivity_timeout_ms;
                }

                /* if there is work to do, make sure we do not disconnect. */
                critical_block(conn->mutex) {
                    int num_reqs = vector_length(conn->requests);
                    if(num_reqs > 0) {
                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0,
                               "There are %d requests pending before cleanup and sending.", num_reqs);
                        inactivity_timeout_ms = atomic_get_int32(&conn->connection_inactivity_timeout_ms);
                        auto_disconnect_time = time_ms() + inactivity_timeout_ms;
                    }
                }

                if((rc = process_requests(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Error while processing requests %s!",
                           plc_tag_decode_error(rc));
                    if(conn->use_connected_msg) {
                        state = CONN_DISCONNECT;
                    } else {
                        state = CONN_UNREGISTER;
                    }
                    nap_interrupt(conn->nap);
                }

                /* check if we should disconnect */
                if(auto_disconnect_time < time_ms()) {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Disconnecting due to inactivity.");

                    auto_disconnect = 1;

                    if(conn->use_connected_msg) {
                        state = CONN_DISCONNECT;
                    } else {
                        state = CONN_UNREGISTER;
                    }
                    nap_interrupt(conn->nap);
                }

                /* if there is work to do, make sure we signal the condition var. */
                critical_block(conn->mutex) {
                    int num_reqs = vector_length(conn->requests);
                    if(num_reqs > 0) {
                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0,
                               "There are %d requests still pending after abort purge and sending.", num_reqs);
                        nap_interrupt(conn->nap);
                    }
                }

                break;

            case CONN_DISCONNECT:
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "in CONN_DISCONNECT state.");
                conn_set_connection_status(conn, PLCTAG_CONN_STATUS_DISCONNECTING);

                if((rc = perform_forward_close(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Forward close failed %s!", plc_tag_decode_error(rc));
                }

                state = CONN_UNREGISTER;
                nap_interrupt(conn->nap);
                break;

            case CONN_UNREGISTER:
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "in CONN_UNREGISTER state.");
                conn_set_connection_status(conn, PLCTAG_CONN_STATUS_DISCONNECTING);

                if((rc = conn_unregister(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unregistering conn failed %s!", plc_tag_decode_error(rc));
                }

                state = CONN_CLOSE_SOCKET;
                nap_interrupt(conn->nap);
                break;

            case CONN_CLOSE_SOCKET:
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "in CONN_CLOSE_SOCKET state.");
                conn_set_connection_status(conn, PLCTAG_CONN_STATUS_DOWN);

                if((rc = conn_close_socket(conn)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Closing conn socket failed %s!", plc_tag_decode_error(rc));
                }

                if(auto_disconnect) {
                    state = CONN_WAIT_IDLE_RECONNECT;
                } else {
                    state = CONN_START_RETRY;
                }
                nap_interrupt(conn->nap);
                break;

            case CONN_START_RETRY:
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "in CONN_START_RETRY state.");

                /* FIXME - make this a tag attribute. */
                timeout_time = time_ms() + calc_retry_time(retry_count);
                retry_count++;

                /* start waiting. */
                state = CONN_WAIT_ERR_RETRY;

                nap_interrupt(conn->nap);
                break;

            case CONN_WAIT_ERR_RETRY:
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "in CONN_WAIT_ERR_RETRY state.");
                conn_set_connection_status(conn, PLCTAG_CONN_STATUS_ERR_WAIT);

                if(timeout_time < time_ms()) {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Transitioning to CONN_OPEN_SOCKET_START.");
                    state = CONN_OPEN_SOCKET_START;
                    nap_interrupt(conn->nap);
                }

                break;

            case CONN_WAIT_IDLE_RECONNECT:
                /* wait for at least one request to queue before reconnecting. */
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "in CONN_WAIT_IDLE_RECONNECT state.");
                conn_set_connection_status(conn, PLCTAG_CONN_STATUS_IDLE_WAIT);

                auto_disconnect = 0;

                /* if there is work to do, reconnect.. */
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_SPEW, 0, "Critical block.");
                critical_block(conn->mutex) {
                    if(vector_length(conn->requests) > 0) {
                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0,
                               "There are requests waiting, reopening connection to PLC.");

                        state = CONN_OPEN_SOCKET_START;
                        nap_interrupt(conn->nap);
                    }
                }

                break;


            default:
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_ERROR, 0, "Unknown state %d!", state);

                /* FIXME - this logic is not complete.  We might be here without
                 * a connected conn or a registered conn. */
                if(conn->use_connected_msg) {
                    state = CONN_DISCONNECT;
                } else {
                    state = CONN_UNREGISTER;
                }

                nap_interrupt(conn->nap);
                break;
        }

        /*
         * give up the CPU a bit, but only if we are not
         * doing some linked states.
         */
        if(wait_until_time > 0) {
            int64_t time_left = wait_until_time - time_ms();

            if(time_left > 0) { nap_wait(conn->nap, (int)time_left); }
        }
    }

    /*
     * One last time before we exit.
     */
    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Critical block.");
    critical_block(conn->mutex) { purge_aborted_requests_unsafe(conn); }

    /* Decrement the count of active handler threads */
    atomic_add_int32(&handler_threads_active, -1);

    THREAD_RETURN(0);
}


/*
 * This must be called with the conn mutex held!
 */
int purge_aborted_requests_unsafe(omron_conn_p conn) {
    int purge_count = 0;
    omron_request_p request = NULL;

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_SPEW, 0, "Starting.");

    /* remove the aborted requests. */
    for(int i = 0; i < vector_length(conn->requests); i++) {
        request = vector_get(conn->requests, i);

        /* filter out the aborts. */
        if(request && atomic_get_int32(&request->abort_request)) {
            purge_count++;

            /* remove it from the queue. */
            vector_remove(conn->requests, i);

            /* set the debug tag to the owning tag. */

            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Connection thread releasing aborted request %p.", (void *)request);

            request->status = PLCTAG_ERR_ABORT;
            request->request_size = 0;
            request->resp_received = 1;

            /* release our hold on it. */
            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "rc_dec: Releasing reference request for tag %" PRId32 ".",
                   request->tag_id);
            rc_dec(request);

            /* vector size has changed, back up one. */
            i--;
        }
    }

    if(purge_count > 0) { pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Removed %d aborted requests.", purge_count); }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_SPEW, 0, "Done.");

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


    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_SPEW, 0, "Starting.");

    if(!conn) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Null conn pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_SPEW, 0, "Checking for requests to process.");

    rc = PLCTAG_STATUS_OK;
    request = NULL;
    conn->data_size = 0;
    conn->data_offset = 0;

    /* grab a request off the front of the list. */
    critical_block(conn->mutex) {
        int max_payload_size = GET_MAX_PAYLOAD_SIZE(conn);

        // FIXME - no logging in a mutex!
        // pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0,  "FIXME: max payload size %d", max_payload_size);

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
                    allow_packing = request->allow_packing && (request->supports_fragmented_operations || !request->first_read);

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
                            allow_packing = request->allow_packing && (request->supports_fragmented_operations || !request->first_read);
                            if(!allow_packing) { break; }

                            int next_request_size = get_payload_size(request) + multi_request_overhead;

                            /* Check if this request fits in remaining space */
                            if(next_request_size > remaining_request_space) { break; }

                            /* calculate if all of the response data from the packed requests will fit into a single response
                             * packet. the -8 bytes is the maximum padding between packets, the -2 bytes is from the offset
                             * integer which stores the offset to this packet */
                            int next_response_space = remaining_response_space - request->response_size - 8 - 2;

                            /* Check response space only if fragmented reads are not supported */
                            if(!request->supports_fragmented_operations && next_response_space < 0) { break; }

                            bundled_requests[num_bundled_requests] = request;
                            num_bundled_requests++;
                            remaining_request_space -= next_request_size;
                            remaining_response_space = next_response_space;
                            vector_remove(conn->requests, 0);
                        }
                    }
                    /* If first request is not packable, we stop here (only the first request is packed) */
                } else {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0,
                           "First request size %d exceeds remaining space %d, cannot process any requests.", first_request_size,
                           remaining_request_space);
                }
            } else {
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "All requests in queue were aborted, nothing to do.");
            }
        }
    }

    /* output debug display as no particular tag. */

    if(num_bundled_requests > 0) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "%d requests to process.", num_bundled_requests);

        do {
            /* copy and pack the requests into the conn buffer. */
            rc = pack_requests(conn, bundled_requests, num_bundled_requests);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Error while packing requests, %s!", plc_tag_decode_error(rc));
                break;
            }

            /* fill in all the necessary parts to the request. */
            if((rc = prepare_request(conn)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to prepare request, %s!", plc_tag_decode_error(rc));
                break;
            }

            /* send the request */
            conn_publish_event(conn, TAG_CONN_EVENT_SEND_REQUEST_STARTED, PLCTAG_STATUS_OK);
            if((rc = send_eip_request(conn, CONN_DEFAULT_TIMEOUT)) != PLCTAG_STATUS_OK) {
                conn_publish_event(conn, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, rc);
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Error sending packet %s!", plc_tag_decode_error(rc));
                break;
            }
            conn_publish_event(conn, TAG_CONN_EVENT_SEND_REQUEST_COMPLETED, PLCTAG_STATUS_OK);

            /* wait for the response */
            conn_publish_event(conn, TAG_CONN_EVENT_RECEIVE_RESPONSE_STARTED, PLCTAG_STATUS_OK);
            if((rc = recv_eip_response(conn, CONN_DEFAULT_TIMEOUT)) != PLCTAG_STATUS_OK) {
                conn_publish_event(conn, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, rc);
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Error receiving packet response %s!", plc_tag_decode_error(rc));
                break;
            }
            conn_publish_event(conn, TAG_CONN_EVENT_RECEIVE_RESPONSE_COMPLETED, PLCTAG_STATUS_OK);

            /*
             * check the CIP status, but only if this is a bundled
             * response.   If it is a singleton, then we pass the
             * status back to the tag.
             */
            if(num_bundled_requests > 1) {
                cip_multi_resp_header *multi_resp = NULL;

                if(le2h16(((eip_encap *)(conn->data))->encap_command) == EIP_UNCONNECTED_SEND) {
                    eip_cip_uc_resp *resp = (eip_cip_uc_resp *)(conn->data);
                    uint16_t udi_item_length = 0;
                    size_t response_overhead = 0;
                    size_t response_size = 0;

                    /* we only know we got an EIP header, so check before reading CPF/CIP fields. */
                    if((size_t)conn->data_size < sizeof(*resp)) {
                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0,
                               "Unconnected response of %u bytes is too short to hold a CIP response of %d bytes!",
                               conn->data_size, (int)sizeof(*resp));
                        rc = PLCTAG_ERR_TOO_SMALL;
                        break;
                    }

                    udi_item_length = le2h16(resp->cpf_udi_item_length);

                    multi_resp = (cip_multi_resp_header *)(&(resp->reply_service));

                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0,
                           "Received unconnected packet with conn sequence ID %" PRIx64 ".", le2h64(resp->encap_sender_context));

                    /* punt if we got an overall error or it is not a partial/bundled error. */
                    if(resp->status != EIP_OK && resp->status != CIP_ERR_PARTIAL_ERROR) {
                        rc = decode_cip_error_code(&(resp->status),
                                                       cip_error_data_size(&resp->status, conn->data + conn->data_size));
                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Command failed! (%d/%d) %s", resp->status, rc,
                               plc_tag_decode_error(rc));
                        break;
                    }

                    response_overhead = (size_t)((uint8_t *)multi_resp - conn->data);
                    response_size = (size_t)conn->data_size - response_overhead;

                    /* check the passed UDI data item size against what we really got. */
                    if((size_t)udi_item_length != response_size) {
                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0,
                               "Incorrectly constructed response! UDI data length field is %zu but actual size is %zu!",
                               (size_t)udi_item_length, response_size);

                        rc = PLCTAG_ERR_BAD_DATA;
                        break;
                    }
                } else if(le2h16(((eip_encap *)(conn->data))->encap_command) == EIP_CONNECTED_SEND) {
                    eip_cip_co_resp *resp = (eip_cip_co_resp *)(conn->data);
                    uint16_t cdi_item_length = 0;
                    size_t response_overhead = 0;
                    size_t response_size = 0;

                    /* we only know we got an EIP header, so check before reading CPF/CIP fields. */
                    if((size_t)conn->data_size < sizeof(*resp)) {
                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0,
                               "Connected response of %u bytes is too short to hold a CIP response of %d bytes!", conn->data_size,
                               (int)sizeof(*resp));
                        rc = PLCTAG_ERR_TOO_SMALL;
                        break;
                    }

                    cdi_item_length = le2h16(resp->cpf_cdi_item_length);

                    multi_resp = (cip_multi_resp_header *)(&(resp->reply_service));

                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0,
                           "Received connected packet with connection ID %x and sequence ID %u(%x)",
                           le2h32(resp->cpf_orig_conn_id), le2h16(resp->cpf_conn_seq_num), le2h16(resp->cpf_conn_seq_num));

                    /* punt if we got an overall error or it is not a partial/bundled error. */
                    if(resp->status != EIP_OK && resp->status != CIP_ERR_PARTIAL_ERROR) {
                        rc = decode_cip_error_code(&(resp->status),
                                                       cip_error_data_size(&resp->status, conn->data + conn->data_size));
                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Command failed! (%d/%d) %s", resp->status, rc,
                               plc_tag_decode_error(rc));
                        break;
                    }

                    response_overhead = (size_t)((uint8_t *)(&resp->cpf_conn_seq_num) - conn->data);
                    response_size = (size_t)conn->data_size - response_overhead;

                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "response_overhead=%zu", response_overhead);
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "response_size=%zu", response_size);

                    /* check the passed CDI data item size against what we really got. */
                    if((size_t)cdi_item_length != response_size) {
                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0,
                               "Incorrectly constructed response! CDI data length field is %zu but actual size is %zu!",
                               (size_t)cdi_item_length, response_size);

                        rc = PLCTAG_ERR_BAD_DATA;
                        break;
                    }
                } else {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unexpected EIP packet type, %04x!",
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

                    /* FIXME - conn->data_size is uint32_t, so this test is always false.  Check carefully
                     * before removing it: the guard that follows depends on data_size being sane. */
                    if(conn->data_size < 0 || offsets_start > (size_t)conn->data_size
                       || offsets_size > (size_t)conn->data_size - offsets_start) {
                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0,
                               "Response of %d bytes is too short to hold %d packed response offsets!", conn->data_size,
                               num_bundled_requests);
                        rc = PLCTAG_ERR_TOO_SMALL;
                        break;
                    }
                }

                /* we have multiple requests, sanity check the data. */
                if(le2h16(multi_resp->request_count) == num_bundled_requests) {
                    size_t offset_base = (size_t)((uint8_t *)(&multi_resp->request_count) - conn->data);

                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "offset_base=%zu", offset_base);

                    /* check all the offsets */
                    for(int resp_index = 0; resp_index < num_bundled_requests; resp_index++) {
                        size_t resp_offset = (size_t)le2h16(multi_resp->request_offsets[resp_index]) + offset_base;

                        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Response %d starts at byte offset %zu", resp_index,
                               resp_offset);

                        if(resp_offset >= (size_t)conn->data_size) {
                            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0,
                                   "Response %d has offset %zu which is outside the conn data!", resp_index, resp_offset);
                            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                            break;
                        }
                    }
                } else {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Expected %d packed responses back but got %zu!",
                           num_bundled_requests, (size_t)le2h16(multi_resp->request_count));
                    rc = PLCTAG_ERR_BAD_DATA;
                    break;
                }
            }

            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Got error %s when processing incoming response(s)!",
                       plc_tag_decode_error(rc));
                break;
            }

            /* copy the results back out. Every request gets a copy. */
            for(int i = 0; i < num_bundled_requests; i++) {
                rc = unpack_response(conn, bundled_requests[i], i);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, bundled_requests[i]->tag_id, "Unable to unpack response!");
                    break;
                }

                /* release our reference */
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, bundled_requests[i]->tag_id,
                       "rc_dec: Releasing reference request for tag %" PRId32 ".", bundled_requests[i]->tag_id);
                bundled_requests[i] = rc_dec(bundled_requests[i]);
            }

            rc = PLCTAG_STATUS_OK;
        } while(0);

        /* problem? push the requests back on the queue. */
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Error sending or receiving requests!");

            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Pushing %d requests back into the queue.", num_bundled_requests);

            for(int i = num_bundled_requests - 1; i >= 0; i--) {
                if(bundled_requests[i]) { vector_insert(conn->requests, 0, bundled_requests[i]); }
            }
        }

        /* tickle the main tickler thread to note that we have responses. */
        plc_tag_tickler_wake();
    }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_SPEW, 0, "Done.");

    return rc;
}


int prepare_request(omron_conn_p conn) {
    eip_encap *encap = NULL;
    int payload_size = 0;

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Starting.");

    encap = (eip_encap *)(conn->data);
    payload_size = (int)conn->data_size - (int)sizeof(eip_encap);

    if(!conn) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Called with null conn!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* fill in the fields of the request. */

    encap->encap_length = h2le16((uint16_t)payload_size);
    encap->encap_session_handle = h2le32(conn->conn_handle);
    encap->encap_status = h2le32(0);
    encap->encap_options = h2le32(0);

    /* set up the conn sequence ID for this transaction */
    if(le2h16(encap->encap_command) == EIP_UNCONNECTED_SEND) {
        /* get new ID */
        conn->conn_seq_id++;

        // request->conn_seq_id = conn->conn_seq_id;
        encap->encap_sender_context = h2le64(conn->conn_seq_id); /* link up the request seq ID and the packet seq ID */

        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Preparing unconnected packet with conn sequence ID %llx",
               conn->conn_seq_id);
    } else if(le2h16(encap->encap_command) == EIP_CONNECTED_SEND) {
        eip_cip_co_req *conn_req = (eip_cip_co_req *)(conn->data);

        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "cpf_targ_conn_id=%x", conn->targ_connection_id);

        /* set up the connection information */
        conn_req->cpf_targ_conn_id = h2le32(conn->targ_connection_id);

        conn->conn_seq_num++;
        conn_req->cpf_conn_seq_num = h2le16(conn->conn_seq_num);

        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Preparing connected packet with connection ID %x and sequence ID %u(%x)",
               conn->orig_connection_id, conn->conn_seq_num, conn->conn_seq_num);
    } else {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unsupported packet type %x!", le2h16(encap->encap_command));
        return PLCTAG_ERR_UNSUPPORTED;
    }

    /* display the data */
    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Prepared packet of size %d", conn->data_size);
    pdebug_dump_bytes(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, conn->data, (int)conn->data_size);

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


int send_eip_request(omron_conn_p conn, int timeout) {
    int rc = PLCTAG_STATUS_OK;
    int64_t timeout_time = 0;

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Starting.");

    if(!conn) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Connection pointer is null.");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(timeout > 0) {
        timeout_time = time_ms() + timeout;
    } else {
        timeout_time = INT64_MAX;
    }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Sending packet of size %d", conn->data_size);
    pdebug_dump_bytes(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, conn->data, (int)(conn->data_size));

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
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Packet of %u bytes is too small to hold an EIP header!", conn->data_size);
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* send the packet */
    do {
        rc = socket_write(conn->sock, conn->data + conn->data_offset, (int)conn->data_size - (int)conn->data_offset,
                          SOCKET_WAIT_TIMEOUT_MS);

        if(rc >= 0) {
            conn->data_offset += (uint32_t)rc;
        } else {
            if(rc == PLCTAG_ERR_TIMEOUT) {
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Socket not yet ready to write.");
                rc = 0;
            }
        }

        /* give up the CPU if we still are looping */
        // if(!conn->terminating && rc >= 0 && conn->data_offset < conn->data_size) {
        //     sleep_ms(1);
        // }
    } while(!atomic_get_int32(&conn->terminating) && rc >= 0 && conn->data_offset < conn->data_size && timeout_time > time_ms());

    if(atomic_get_int32(&conn->terminating)) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Connection is terminating.");
        return PLCTAG_ERR_ABORT;
    }

    if(rc < 0) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Error, %d, writing socket!", rc);
        return rc;
    }

    if(timeout_time <= time_ms()) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Timed out waiting to send data!");
        return PLCTAG_ERR_TIMEOUT;
    }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Done.");

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

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Starting.");

    if(!conn) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Called with null conn!");
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
                    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0,
                           "Packet response (%d) is larger than possible buffer size (%d)!", data_needed, conn->data_capacity);
                    return PLCTAG_ERR_TOO_LARGE;
                }
            }
        } else {
            if(rc == PLCTAG_ERR_TIMEOUT) {
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Socket not yet ready to read.");
            } else {
                /* error! */
                pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Error reading socket! rc=%d", rc);
                return rc;
            }
        }
    } while(!atomic_get_int32(&conn->terminating) && conn->data_offset < data_needed && timeout_time > time_ms());

    if(atomic_get_int32(&conn->terminating)) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Connection is terminating, returning...");
        return PLCTAG_ERR_ABORT;
    }

    if(timeout_time <= time_ms()) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Timed out waiting for data to read!");
        return PLCTAG_ERR_TIMEOUT;
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
            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0,
                   "Received EIP command %04" PRIx16 " in response to command %04" PRIx16 "!", resp_command,
                   conn->req_encap_command);
            return PLCTAG_ERR_BAD_DATA;
        }

        /*
         * Once the connection is registered every packet carries our handle.  A zero handle
         * means we are still registering, so there is nothing to compare against yet.
         */
        if(conn->conn_handle != 0 && resp_handle != conn->conn_handle) {
            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0,
                   "Received a response for connection handle %" PRIx32 " but this connection is %" PRIx32 "!", resp_handle,
                   conn->conn_handle);
            return PLCTAG_ERR_BAD_DATA;
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
        if(conn->req_sent && resp_command == EIP_UNCONNECTED_SEND && conn->resp_seq_id != conn->req_seq_id) {
            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0,
                   "Received a response with sender context %" PRIx64 " but we sent %" PRIx64 "!", conn->resp_seq_id,
                   conn->req_seq_id);
            return PLCTAG_ERR_BAD_DATA;
        }
    }

    rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "request received all needed data (%d bytes of %d).", conn->data_offset,
           data_needed);

    pdebug_dump_bytes(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, conn->data, (int)(conn->data_offset));

    /* check status. */
    if(le2h32(((eip_encap *)(conn->data))->encap_status) != EIP_OK) { rc = PLCTAG_ERR_BAD_STATUS; }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Done.");

    return rc;
}


/*
 * Advance the connection serial number, skipping zero.
 *
 * The field is 16 bits and is meant to cycle, but zero is not a usable serial
 * number -- conn setup seeds it into 1..65535 for exactly that reason -- so the
 * wrap has to land on 1 rather than 0. Doing the arithmetic in uint16_t here
 * also keeps -fsanitize=implicit-integer-truncation quiet: a bare ++ on a
 * uint16_t promotes to int and narrows again on store.
 */
/* new version of Forward Open */
/*
 * cip_request_destroy
 *
 * The request must be removed from any lists before this!
 */
