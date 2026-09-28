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
#include <libplctag/api/libplctag.h>
#include <libplctag/modules/omron/cip.h>
#include <libplctag/modules/omron/conn.h>
#include <libplctag/modules/omron/defs.h>
#include <libplctag/modules/omron/omron_common.h>
#include <libplctag/modules/omron/tag.h>
#include <limits.h>
#include <platform.h>
#include <stdlib.h>
#include <time.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>
#include <utils/random_utils.h>


/* Omron is special */
#define MAX_CIP_OMRON_MSG_SIZE_EX (0xFFFF & 1990)
#define MAX_CIP_OMRON_MSG_SIZE (0x01FF & 502)


/*
 * Number of milliseconds to wait to try to set up the conn again
 * after a failure.
 */

/* Idle time to wait before disconnecting.  Set it to one second less than we negotiate with the PLC. */
#define SESSION_DISCONNECT_TIMEOUT (OMRON_EIP_CONN_TIMEOUT_MS - 1000)


/* make sure we try hard to get a good payload size */


/* plc-specific conn constructors */
static omron_conn_p create_omron_njnx_conn_unsafe(const char *host, const char *path, int *use_connected_msg,
                                                  int connection_group_id);

static omron_conn_p conn_create_unsafe(int max_payload_capacity, bool data_buffer_is_static, const char *host, const char *path,
                                       omron_plc_type_t plc_type, int *use_connected_msg, int connection_group_id);


static cip_conn_list_t conn_list = {0};

/* Track active handler threads for proper shutdown synchronization */


int conn_startup(void) { return session_list_init(&conn_list); }


void conn_teardown(void) {
    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Starting.");

    /* Mark all connections as terminating to wake handler threads quickly. */
    if(conn_list.conns && conn_list.mutex) {
        critical_block(conn_list.mutex) {
            int n = vector_length(conn_list.conns);
            for(int i = 0; i < n; i++) {
                omron_conn_p conn = vector_get(conn_list.conns, i);
                if(conn) {
                    atomic_set_int32(&conn->terminating, 1);
                    if(conn->session_wait_cond) { cond_signal(conn->session_wait_cond); }
                }
            }
        }
    }

    if(conn_list.conns && conn_list.mutex) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Waiting for connections to terminate.");

        while(1) {
            int remaining_conns = 0;

            critical_block(conn_list.mutex) { remaining_conns = vector_length(conn_list.conns); }

            if(remaining_conns > 0) {
                sleep_ms(10);
            } else {
                break;
            }
        }

        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Connections all terminated.");

        vector_destroy(conn_list.conns);

        conn_list.conns = NULL;
    }

    /* Wait for handler threads BEFORE destroying conn_list.mutex — handler threads
     * may still be holding conn->session_mutex (not conn_list.mutex) during their final
     * cleanup, and musl returns freed memory to the OS immediately, making
     * any use-after-free a SIGSEGV. */
    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Waiting for handler threads to complete.");
    int64_t start_time = time_ms();
    int64_t timeout_ms = 5000;
    int active_count = 0;
    int64_t elapsed = 0;

    while((active_count = atomic_get_int32(&(conn_list.handler_count))) > 0) {
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

    if(conn_list.mutex) {
        mutex_destroy(&(conn_list.mutex));
        conn_list.mutex = NULL;
    }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Done.");
}
int conn_find_or_create(omron_conn_p *tag_conn, attr attribs, int *is_new_conn) {
    /*int debug = attr_get_int(attribs,"debug",0);*/
    const char *conn_gw = attr_get_str(attribs, "gateway", "");
    const char *conn_path = attr_get_str(attribs, "path", "");
    int use_connected_msg = attr_get_int(attribs, "use_connected_msg", 0);
    omron_conn_p conn = OMRON_CONN_NULL;
    int new_conn = 0;
    int shared_conn = attr_get_int(attribs, "share_conn", 1); /* share the conn by default. */
    int rc = PLCTAG_STATUS_OK;
    int connection_inactivity_timeout_ms = SESSION_DISCONNECT_TIMEOUT;
    int connection_group_id = attr_get_int(attribs, "connection_group_id", 0);
    int only_use_old_forward_open = attr_get_int(attribs, "conn_only_use_old_forward_open", 0);

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Starting");

    /* share_conn is subsumed by connection_group_id.  Warn only if the tag string actually set it. */
    if(attr_get_str(attribs, "share_conn", NULL)) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0,
               "The attribute \"share_conn\" is deprecated and will be removed.  Use \"connection_group_id\" instead.");
    }

    connection_inactivity_timeout_ms = attr_get_int(attribs, "connection_inactivity_timeout_ms", SESSION_DISCONNECT_TIMEOUT);
    if(connection_inactivity_timeout_ms < 1 || connection_inactivity_timeout_ms > SESSION_DISCONNECT_TIMEOUT) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0,
               "Invalid connection_inactivity_timeout_ms %d. Must be between 1 and %d. Using default %d.",
               connection_inactivity_timeout_ms, SESSION_DISCONNECT_TIMEOUT, SESSION_DISCONNECT_TIMEOUT);
        connection_inactivity_timeout_ms = SESSION_DISCONNECT_TIMEOUT;
    } else {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Setting connection_inactivity_timeout_ms to %dms.",
               connection_inactivity_timeout_ms);
    }

    critical_block(conn_list.mutex) {
        /* if we are to share connections, then look for an existing one. */
        if(shared_conn) {
            conn = session_list_find_by_host_unsafe(&conn_list, conn_gw, conn_path, connection_group_id);
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
     * The mutex and condition variable are already made, so the connection is safe for another
     * thread to use.  Start the handler thread outside the conn mutex so that a thread that
     * blocks here does not block every other thread looking for a connection.
     */

    if(new_conn) {
        if((rc = thread_create((thread_p *)&(conn->handler_thread), session_handler, 32 * 1024, conn)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to create conn thread!");
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "rc_dec: Releasing reference to the PLC connection.");
            rc_dec(conn);
            conn = OMRON_CONN_NULL;
        } else {
            /* save the status */
        }
    }

    /* store it into the tag */
    *tag_conn = conn;

    if(is_new_conn) { *is_new_conn = new_conn; }

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "Done");

    return rc;
}
omron_conn_p create_omron_njnx_conn_unsafe(const char *host, const char *path, int *use_connected_msg, int connection_group_id) {
    omron_conn_p conn = NULL;

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Starting.");

    do {
        conn = conn_create_unsafe(MAX_CIP_OMRON_MSG_SIZE_EX, true, host, path, OMRON_PLC_OMRON_NJNX, use_connected_msg,
                                  connection_group_id);
        if(conn != NULL) {
            conn->only_use_old_forward_open = false;
            conn->fo_conn_size = MAX_CIP_OMRON_MSG_SIZE;
            conn->fo_ex_conn_size = MAX_CIP_OMRON_MSG_SIZE_EX;
            conn->max_payload_size = (uint16_t)conn->fo_conn_size;
        } else {
            pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to create *Logix conn!");
        }
    } while(0);

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Done.");

    return conn;
}


omron_conn_p conn_create_unsafe(int max_payload_capacity, bool data_buffer_is_static, const char *host, const char *path,
                                omron_plc_type_t plc_type, int *use_connected_msg, int connection_group_id) {
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
    /* an NJ/NX cannot bridge DH+, so a DH+ segment in the path is rejected. */
    rc = CIP.encode_path(path, use_connected_msg, CIP_PLC_KIND_OTHER, &tmp_conn_path[0], &tmp_conn_path_size, &is_dhp, &dhp_dest);
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

    conn = (omron_conn_p)rc_alloc(total_allocation_size, session_destroy);
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

    conn->requests = vector_create(SESSION_MIN_REQUESTS, SESSION_INC_REQUESTS);
    if(!conn->requests) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to allocate vector for requests!");
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_DETAIL, 0, "rc_dec: Releasing reference to the PLC connection.");
        rc_dec(conn);
        return NULL;
    }

    /* check for ID set up. This does not need to be thread safe since we just need a random value. */
    if(connection_id == 0) { connection_id = (uint32_t)random_u64(UINT32_MAX) + 1; }

    /* fix up the rest of the fields */
    conn->owner_list = &conn_list;
    conn->plc_type = (int32_t)plc_type;
    conn->min_payload_size = MIN_PAYLOAD_SIZE_CIP;
    conn->dhp_capable = false;
    conn->use_connected_msg = *use_connected_msg;
    conn->conn_serial_number = (uint16_t)(random_u64(UINT16_MAX) + 1);
    conn->session_seq_id = (random_u64(UINT32_MAX) + 1);
    conn->is_dhp = is_dhp;
    conn->dhp_dest = dhp_dest;
    conn_watch_init(&conn->watch, PLCTAG_CONN_STATUS_DOWN);

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

    /*
     * Make the mutex and the condition variable before the connection goes into the list.  Once
     * it is in the list another thread can find it and call session_add_request(), which locks the
     * one and signals the other.
     */

    /* create the conn mutex. */
    if((rc = mutex_create(&(conn->session_mutex))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to create conn mutex!");
        return rc_dec(conn);
    }

    /* create the conn condition variable. */
    if((rc = cond_create(&(conn->session_wait_cond))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_WARN, 0, "Unable to create conn condition var!");
        return rc_dec(conn);
    }

    /* add the new conn to the list. */
    session_list_add_unsafe(&conn_list, conn);

    pdebug(DEBUG_MODULE_OMRON_CONN, DEBUG_INFO, 0, "Done");

    return conn;
}
/*****************************************************************
 **************** Connection handling functions *********************
 ****************************************************************/




/* watch.status and the ring publish must change together under conn->session_mutex:
 * connection_tag_create() takes a paired snapshot of both (watch.ring_write_idx
 * and watch.status) to seed a freshly created connection tag, and needs the same
 * mutex to avoid reading one from before this transition and the other from after it --
 * see the comment there. */
/* new version of Forward Open */