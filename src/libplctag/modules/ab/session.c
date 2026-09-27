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
#include <libplctag/modules/ab/ab_common.h>
#include <libplctag/modules/ab/cip.h>
#include <libplctag/modules/ab/defs.h>
#include <libplctag/modules/cip/error_codes.h>
#include <libplctag/lib/conn_watch.h>
#include <libplctag/modules/ab/session.h>
#include <libplctag/modules/ab/tag.h>
#include <limits.h>
#include <platform.h>
#include <stdlib.h>
#include <time.h>
#include <utils/atomic_utils.h>
#include <utils/debug.h>
#include <utils/random_utils.h>

/* Track active session handler threads for proper shutdown synchronization */
static atomic_int32_t session_handlers_active = ATOMIC_INT_STATIC_INIT;

#define MAX_REQUESTS (400)


#define MAX_CIP_LGX_MSG_SIZE (0x01FF & 504)
#define MAX_CIP_LGX_MSG_SIZE_EX (0xFFFF & 4000)

#define MAX_CIP_MICRO800_MSG_SIZE (0x01FF & 504)
#define MAX_CIP_MICRO800_MSG_SIZE_EX (0xFFFF & 4000)

/* Omron is special */

/* maximum for PCCC embedded within CIP. */
#define MAX_CIP_PLC5_MSG_SIZE (244)
#define MAX_CIP_SLC_MSG_SIZE (244)
#define MAX_CIP_MLGX_MSG_SIZE (244)
#define MAX_CIP_LGX_PCCC_MSG_SIZE (244)

/*
 * Number of milliseconds to wait to try to set up the session again
 * after a failure.
 */

/* Idle timeout.  One second less than that negotiated with the PLC. */
#define SESSION_DISCONNECT_TIMEOUT (AB_EIP_CONN_TIMEOUT_MS - 1000)
#define SESSION_IDLE_WAIT_TIME (100)

/*
 * Smallest connection payload we can actually use, by protocol family.
 *
 * A ForwardOpen error can offer a smaller size than we asked for and we accept it, but there
 * is a floor: below the fixed per-request overhead there is no room left for a request, and
 * the "space minus overhead" arithmetic downstream goes negative.  PCCC needs about 92 bytes
 * for its command header and addressing; CIP needs 500 for a minimal fragmented transfer.
 * Both are payload only -- the EIP and CPF encapsulation is accounted for separately.
 */
#define MIN_PAYLOAD_SIZE_PCCC (92)

/* make sure we try hard to get a good payload size */


/* plc-specific session constructors */
static ab_session_p create_plc5_session_unsafe(const char *host, const char *path, int *use_connected_msg,
                                               int connection_group_id);
static ab_session_p create_slc_session_unsafe(const char *host, const char *path, int *use_connected_msg,
                                              int connection_group_id);
static ab_session_p create_mlgx_session_unsafe(const char *host, const char *path, int *use_connected_msg,
                                               int connection_group_id);
static ab_session_p create_lgx_session_unsafe(const char *host, const char *path, int *use_connected_msg,
                                              int connection_group_id);
static ab_session_p create_lgx_pccc_session_unsafe(const char *host, const char *path, int *use_connected_msg,
                                                   int connection_group_id);
static ab_session_p create_micro800_session_unsafe(const char *host, const char *path, int *use_connected_msg,
                                                   int connection_group_id);

static ab_session_p session_create_unsafe(int max_payload_capacity, bool data_buffer_is_static, const char *host,
                                          const char *path, ab_plc_type_t plc_type, int *use_connected_msg,
                                          int connection_group_id);
static void session_destroy(void *session);
static THREAD_FUNC(session_handler);
static int process_requests(ab_session_p session);
static int receive_forward_open_response(ab_session_p session);


static cip_conn_list_t conn_list = {0};


int session_startup(void) { return session_list_init(&conn_list); }


void session_teardown(void) {
    int remaining_sessions = 0;
    int64_t start_time = 0;
    int64_t timeout_ms = 5000;
    int active_count = 0;
    int64_t elapsed = 0;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    /* flag all open connections for termination */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Marking all open connections for termination.");

    if(conn_list.mutex) {
        critical_block(conn_list.mutex) {
            if(conn_list.conns == NULL) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Session list is already destroyed.");

                break;
            }

            remaining_sessions = vector_length(conn_list.conns);

            for(int sess_index = 0; sess_index < remaining_sessions; sess_index++) {
                ab_session_p session = vector_get(conn_list.conns, sess_index);

                if(session) { atomic_set_int32(&session->terminating, 1); }
            }
        }
    }

    /* flag the whole library shutting down. */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Setting library shutdown flag.");

    if(conn_list.conns && conn_list.mutex) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Waiting for connections to terminate.");

        while(1) {
            critical_block(conn_list.mutex) { remaining_sessions = vector_length(conn_list.conns); }

            /* wait for things to terminate. */
            if(remaining_sessions > 0) {
                sleep_ms(50);  // MAGIC
            } else {
                break;
            }
        }

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Sessions all terminated.");

        vector_destroy(conn_list.conns);

        conn_list.conns = NULL;
    }

    /* Wait for all active session handler threads to complete.
     * Use an atomic counter to track active handlers.
     * Wait up to 5 seconds (5000 ms) with 20ms polling intervals.
     */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Waiting for session handler threads to complete.");
    start_time = time_ms();

    while((active_count = atomic_get_int32(&session_handlers_active)) > 0) {
        elapsed = time_ms() - start_time;

        if(elapsed >= timeout_ms) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Timeout waiting for %d session handler threads to complete.",
                   active_count);
            break;
        }

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
               "Waiting for %d session handler threads to complete. Elapsed: %" PRId64 "ms", active_count, elapsed);
        sleep_ms(20);
    }

    if(active_count == 0) { pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "All session handler threads completed."); }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Destroying session mutex.");

    if(conn_list.mutex) {
        mutex_destroy(&(conn_list.mutex));
        conn_list.mutex = NULL;
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");
}
int session_find_or_create(ab_session_p *tag_session, attr attribs, int *is_new_session) {
    /*int debug = attr_get_int(attribs,"debug",0);*/
    const char *session_gw = attr_get_str(attribs, "gateway", "");
    const char *session_path = attr_get_str(attribs, "path", "");
    int use_connected_msg = attr_get_int(attribs, "use_connected_msg", 0);
    ab_plc_type_t plc_type = get_plc_type(attribs);
    ab_session_p session = AB_SESSION_NULL;
    int new_session = 0;
    int shared_session = attr_get_int(attribs, "share_session", 1); /* share the session by default. */
    int rc = PLCTAG_STATUS_OK;
    int connection_inactivity_timeout_ms = SESSION_DISCONNECT_TIMEOUT;
    int connection_group_id = attr_get_int(attribs, "connection_group_id", 0);
    int only_use_old_forward_open = attr_get_int(attribs, "conn_only_use_old_forward_open", 0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Starting");

    /* share_session is subsumed by connection_group_id.  Warn only if the tag string actually set it. */
    if(attr_get_str(attribs, "share_session", NULL)) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
               "The attribute \"share_session\" is deprecated and will be removed.  Use \"connection_group_id\" instead.");
    }

    connection_inactivity_timeout_ms = attr_get_int(attribs, "connection_inactivity_timeout_ms", SESSION_DISCONNECT_TIMEOUT);
    if(connection_inactivity_timeout_ms < 1 || connection_inactivity_timeout_ms > SESSION_DISCONNECT_TIMEOUT) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
               "Invalid connection_inactivity_timeout_ms %d. Must be between 1 and %d. Using default %d.",
               connection_inactivity_timeout_ms, SESSION_DISCONNECT_TIMEOUT, SESSION_DISCONNECT_TIMEOUT);
        connection_inactivity_timeout_ms = SESSION_DISCONNECT_TIMEOUT;
    } else {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Setting connection_inactivity_timeout_ms to %dms.",
               connection_inactivity_timeout_ms);
    }

    critical_block(conn_list.mutex) {
        /* if we are to share connections, then look for an existing one. */
        if(shared_session) {
            session = session_list_find_by_host_unsafe(&conn_list, session_gw, session_path, connection_group_id);
        } else {
            /* no sharing, create a new one */
            session = AB_SESSION_NULL;
        }

        if(session == AB_SESSION_NULL) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Creating new session.");

            switch(plc_type) {
                case AB_PLC_PLC5:
                    session = create_plc5_session_unsafe(session_gw, session_path, &use_connected_msg, connection_group_id);
                    break;

                case AB_PLC_SLC:
                    session = create_slc_session_unsafe(session_gw, session_path, &use_connected_msg, connection_group_id);
                    break;

                case AB_PLC_MLGX:
                    session = create_mlgx_session_unsafe(session_gw, session_path, &use_connected_msg, connection_group_id);
                    break;

                case AB_PLC_LGX:
                    session = create_lgx_session_unsafe(session_gw, session_path, &use_connected_msg, connection_group_id);
                    break;

                case AB_PLC_LGX_PCCC:
                    session = create_lgx_pccc_session_unsafe(session_gw, session_path, &use_connected_msg, connection_group_id);
                    break;

                case AB_PLC_MICRO800:
                    session = create_micro800_session_unsafe(session_gw, session_path, &use_connected_msg, connection_group_id);
                    break;

                case AB_PLC_GENERIC:
                    /* Generic PLC type uses unconnected messaging for stateless operations */
                    use_connected_msg = 0;
                    session = create_lgx_session_unsafe(session_gw, session_path, &use_connected_msg, connection_group_id);
                    break;


                default:
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unknown PLC type %d!", plc_type);
                    session = NULL;
                    break;
            }

            if(session == AB_SESSION_NULL) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "unable to create or find a session!");
                rc = PLCTAG_ERR_BAD_GATEWAY;
            } else {
                atomic_init_int32(&session->connection_inactivity_timeout_ms, connection_inactivity_timeout_ms);

                /* see if we have an attribute set for forcing the use of the older ForwardOpen */
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                       "Passed attribute to prohibit use of extended ForwardOpen is %d.", only_use_old_forward_open);
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                       "Existing attribute to prohibit use of extended ForwardOpen is %d.", session->only_use_old_forward_open);
                session->only_use_old_forward_open = (session->only_use_old_forward_open ? 1 : only_use_old_forward_open);

                new_session = 1;
            }
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Reusing existing session.");
        }
    }

    /*
     * Make sure that we have created the mutex and cond var first.
     */

    if(new_session) {
        if((rc = thread_create((thread_p *)&(session->handler_thread), session_handler, 32 * 1024, session))
           != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create session thread!");
        }

        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "rc_dec: Releasing session reference.");
            rc_dec(session);
            session = AB_SESSION_NULL;
        } else {
            /* save the status */
        }
    }

    /* store it into the tag */
    *tag_session = session;

    if(is_new_session) { *is_new_session = new_session; }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Done");

    return rc;
}
ab_session_p create_plc5_session_unsafe(const char *host, const char *path, int *use_connected_msg, int connection_group_id) {
    ab_session_p session = NULL;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    do {
        session =
            session_create_unsafe(MAX_CIP_PLC5_MSG_SIZE, true, host, path, AB_PLC_PLC5, use_connected_msg, connection_group_id);
        if(session != NULL) {
            session->only_use_old_forward_open = true;
            session->fo_conn_size = MAX_CIP_PLC5_MSG_SIZE;
            session->fo_ex_conn_size = 0;
            session->max_payload_size = (uint16_t)session->fo_conn_size;
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create PLC/5 session!");
        }
    } while(0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return session;
}


ab_session_p create_slc_session_unsafe(const char *host, const char *path, int *use_connected_msg, int connection_group_id) {
    ab_session_p session = NULL;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    do {
        session =
            session_create_unsafe(MAX_CIP_SLC_MSG_SIZE, true, host, path, AB_PLC_SLC, use_connected_msg, connection_group_id);
        if(session != NULL) {
            session->only_use_old_forward_open = true;
            session->fo_conn_size = MAX_CIP_SLC_MSG_SIZE;
            session->fo_ex_conn_size = 0;
            session->max_payload_size = (uint16_t)session->fo_conn_size;
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create SLC 500 session!");
        }
    } while(0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return session;
}


ab_session_p create_mlgx_session_unsafe(const char *host, const char *path, int *use_connected_msg, int connection_group_id) {
    ab_session_p session = NULL;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    do {
        session =
            session_create_unsafe(MAX_CIP_MLGX_MSG_SIZE, true, host, path, AB_PLC_MLGX, use_connected_msg, connection_group_id);
        if(session != NULL) {
            session->only_use_old_forward_open = true;
            session->fo_conn_size = MAX_CIP_MLGX_MSG_SIZE;
            session->fo_ex_conn_size = 0;
            session->max_payload_size = (uint16_t)session->fo_conn_size;
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create Micrologix session!");
        }
    } while(0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return session;
}


ab_session_p create_lgx_session_unsafe(const char *host, const char *path, int *use_connected_msg, int connection_group_id) {
    ab_session_p session = NULL;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    do {
        session =
            session_create_unsafe(MAX_CIP_LGX_MSG_SIZE_EX, true, host, path, AB_PLC_LGX, use_connected_msg, connection_group_id);
        if(session != NULL) {
            session->only_use_old_forward_open = false;
            session->fo_conn_size = MAX_CIP_LGX_MSG_SIZE;
            session->fo_ex_conn_size = MAX_CIP_LGX_MSG_SIZE_EX;
            session->max_payload_size = (uint16_t)session->fo_conn_size;
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create *Logix session!");
        }
    } while(0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return session;
}


ab_session_p create_lgx_pccc_session_unsafe(const char *host, const char *path, int *use_connected_msg, int connection_group_id) {
    ab_session_p session = NULL;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    do {
        session = session_create_unsafe(MAX_CIP_LGX_PCCC_MSG_SIZE, true, host, path, AB_PLC_LGX_PCCC, use_connected_msg,
                                        connection_group_id);
        if(session != NULL) {
            session->only_use_old_forward_open = true;
            session->fo_conn_size = MAX_CIP_LGX_PCCC_MSG_SIZE;
            session->fo_ex_conn_size = 0;
            session->max_payload_size = (uint16_t)session->fo_conn_size;
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create Micrologix session!");
        }
    } while(0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return session;
}


ab_session_p create_micro800_session_unsafe(const char *host, const char *path, int *use_connected_msg, int connection_group_id) {
    ab_session_p session = NULL;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    do {
        session = session_create_unsafe(MAX_CIP_MICRO800_MSG_SIZE_EX, true, host, path, AB_PLC_MICRO800, use_connected_msg,
                                        connection_group_id);
        if(session != NULL) {
            session->only_use_old_forward_open = true;
            session->fo_conn_size = MAX_CIP_MICRO800_MSG_SIZE;
            session->fo_ex_conn_size = MAX_CIP_MICRO800_MSG_SIZE_EX;
            session->max_payload_size = (uint16_t)session->fo_conn_size;
        } else {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create Micro800 session!");
        }
    } while(0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return session;
}


ab_session_p session_create_unsafe(int max_payload_capacity, bool data_buffer_is_static, const char *host, const char *path,
                                   ab_plc_type_t plc_type, int *use_connected_msg, int connection_group_id) {
    static volatile uint32_t connection_id = 0;

    int rc = PLCTAG_STATUS_OK;
    ab_session_p session = AB_SESSION_NULL;
    size_t total_allocation_size = sizeof(*session);
    size_t data_buffer_capacity =
        (size_t)EIP_CIP_PREFIX_SIZE + (size_t)max_payload_capacity + (size_t)32;  // MAGIC - FIXME this is just a bandaid.
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

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting");

    /*
     * The host string is copied into this allocation verbatim, so its length is part of the
     * session's size.  It comes straight from the "gateway" attribute with nothing between
     * the application and here, so bound it: without this a caller can size the session
     * object arbitrarily.
     */
    if(!host || str_length(host) >= MAX_SESSION_HOST_LEN) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Gateway string is missing or longer than the maximum of %d bytes!",
               MAX_SESSION_HOST_LEN - 1);
        return AB_SESSION_NULL;
    }

    /*
     * The path string is copied in verbatim too, and it is not self-limiting: spaces are
     * skipped everywhere in cip_encode_path(), so an arbitrarily long string can still
     * encode to a valid short path.  Bound it against the encoded path buffer -- a real
     * route is a handful of hops, so this rejects nothing that describes real hardware.
     */
    if(path && str_length(path) >= MAX_CONN_PATH) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Path string is longer than the maximum of %d bytes!", MAX_CONN_PATH - 1);
        return AB_SESSION_NULL;
    }

    if(*use_connected_msg) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Session should use connected messaging.");
    } else {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Session should not use connected messaging.");
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
    int dhp_kind = (plc_type == AB_PLC_PLC5 || plc_type == AB_PLC_SLC || plc_type == AB_PLC_MLGX) ? CIP_PLC_KIND_DHP_CAPABLE :
                                                                                                    CIP_PLC_KIND_OTHER;

    rc = cip_encode_path(path, use_connected_msg, dhp_kind, &tmp_conn_path[0], &tmp_conn_path_size, &is_dhp, &dhp_dest);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Unable to convert path string to binary path, error %s!",
               plc_tag_decode_error(rc));
        return NULL;
    }

    conn_path_offset = total_allocation_size;
    total_allocation_size += (size_t)tmp_conn_path_size;

    /* allocate the session struct and the buffer in the same allocation. */
    pdebug(
        DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
        "Allocating %d total bytes of memory with %d bytes for data buffer static data, %d bytes for the host name, %d bytes for the path, %d bytes for the encoded path.",
        total_allocation_size, (data_buffer_is_static ? data_buffer_capacity : 0), str_length(host) + 1,
        (path_offset == 0 ? 0 : str_length(path) + 1), tmp_conn_path_size);

    session = (ab_session_p)rc_alloc((int)total_allocation_size, session_destroy);
    if(!session) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error allocating new session!");
        return AB_SESSION_NULL;
    }

    /* fill in the interior pointers */

    /* fix up the data buffer. */
    session->data_buffer_is_static = data_buffer_is_static;
    session->data_capacity = (uint32_t)data_buffer_capacity;

    if(data_buffer_is_static) {
        session->data = (uint8_t *)(session) + data_buffer_offset;
    } else {
        session->data = (uint8_t *)mem_alloc((int)data_buffer_capacity);
        if(session->data == NULL) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to allocate the connection data buffer!");
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "rc_dec: Releasing session reference.");
            return rc_dec(session);
        }
    }

    /* point the host pointer just after the data. */
    session->host = (char *)(session) + host_name_offset;
    str_copy(session->host, (int)host_name_size, host);

    if(path_offset) {
        session->path = (char *)(session) + path_offset;
        str_copy(session->path, (int)path_size, path);
    }

    if(conn_path_offset) {
        session->conn_path = (uint8_t *)(session) + conn_path_offset;

        // FIXME - the path length cannot be 8 bits with a buffer length that is over 260.
        session->conn_path_size = (uint8_t)tmp_conn_path_size;
        mem_copy(session->conn_path, tmp_conn_path, tmp_conn_path_size);
    }


    /*
        TO DO
            remove mem_free from destructor for host, path, and conn_path.
    */

    session->requests = vector_create(SESSION_MIN_REQUESTS, SESSION_INC_REQUESTS);
    if(!session->requests) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to allocate vector for requests!");
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "rc_dec: Releasing session reference.");
        rc_dec(session);
        return NULL;
    }

    /* check for ID set up. This does not need to be thread safe since we just need a random value. */
    if(connection_id == 0) { connection_id = (uint32_t)(random_u64(UINT32_MAX) + 1); }

    /* fix up the rest of teh fields */
    session->plc_type = (int32_t)plc_type;
    session->min_payload_size = (uint16_t)((plc_type == AB_PLC_PLC5 || plc_type == AB_PLC_SLC || plc_type == AB_PLC_MLGX
                                            || plc_type == AB_PLC_LGX_PCCC)
                                               ? MIN_PAYLOAD_SIZE_PCCC
                                               : MIN_PAYLOAD_SIZE_CIP);
    session->dhp_capable = (dhp_kind == CIP_PLC_KIND_DHP_CAPABLE);
    session->use_connected_msg = *use_connected_msg;
    session->conn_serial_number = (uint16_t)(random_u64(UINT16_MAX) + 1);
    session->session_seq_id = (uint64_t)(random_u64(UINT32_MAX) + 1);
    session->is_dhp = is_dhp;
    session->dhp_dest = dhp_dest;
    conn_watch_init(&session->watch, PLCTAG_CONN_STATUS_DOWN);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Setting connection_group_id to %d.", connection_group_id);
    session->connection_group_id = connection_group_id;

    /*
     * Why is connection_id global?  Because it looks like the PLC might
     * be treating it globally.  I am seeing ForwardOpen errors that seem
     * to be because of duplicate connection IDs even though the session
     * was closed.
     *
     * So, this is more or less unique across all invocations of the library.
     * FIXME - this could collide.  The probability is low, but it could happen
     * as there are only 32 bits.
     */
    session->orig_connection_id = ++connection_id;

    /* create the session mutex. */
    if((rc = mutex_create(&(session->session_mutex))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create session mutex!");
        return rc_dec(session);
    }

    /* create the session condition variable. */
    if((rc = cond_create(&(session->session_wait_cond))) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create session condition var!");
        return rc_dec(session);
    }

    /* add the new session to the list. */
    session_list_add_unsafe(&conn_list, session);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done");

    return session;
}
void session_destroy(void *session_arg) {
    ab_session_p session = session_arg;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting.");

    if(!session) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Session ptr is null!");

        return;
    }

    /* so remove the session from the list so no one else can reference it. */
    session_list_remove(&conn_list, session);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Session sent %" PRId64 " packets.", session->packet_count);

    /* terminate the session thread first. */
    atomic_set_int32(&session->terminating, 1);

    /* signal the condition variable in case it is waiting */
    if(session->session_wait_cond) { cond_signal(session->session_wait_cond); }

    /* get rid of the handler thread. */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Destroying session thread.");
    if(session->handler_thread) {
        /* this cannot be guarded by the mutex since the session thread also locks it. */
        thread_join(session->handler_thread);

        /* FIXME - is this critical block needed? */
        critical_block(session->session_mutex) {
            thread_destroy(&(session->handler_thread));
            session->handler_thread = NULL;
        }
    }


    /* this needs to be handled in the mutex to prevent double frees due to queued requests. */
    critical_block(session->session_mutex) {
        /* close off the connection if is one. This helps the PLC clean up. */
        if(session->targ_connection_id) {
            /*
             * we do not want the internal loop to immediately
             * return, so set the flag like we are not terminating.
             * There is still a timeout that applies.
             */
            atomic_set_int32(&session->terminating, 0);
            perform_forward_close(session);
            atomic_set_int32(&session->terminating, 1);
        }

        /* try to be nice and un-register the session */
        if(session->session_handle) { session_unregister(session); }

        if(session->sock) { session_close_socket(session); }

        /* release all the requests that are in the queue. */
        if(session->requests) {
            for(int i = 0; i < vector_length(session->requests); i++) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "rc_dec: Releasing request reference.");
                rc_dec(vector_get(session->requests, i));
            }

            vector_destroy(session->requests);
            session->requests = NULL;
        }
    }

    /* we are done with the condition variable, finally destroy it. */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Destroying session condition variable.");
    if(session->session_wait_cond) {
        cond_destroy(&(session->session_wait_cond));
        session->session_wait_cond = NULL;
    }

    /* we are done with the mutex, finally destroy it. */
    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Destroying session mutex.");
    if(session->session_mutex) {
        mutex_destroy(&(session->session_mutex));
        session->session_mutex = NULL;
    }

    if(!session->data_buffer_is_static) { mem_free(session->data); }

    /* these are all allocated in one large block. */


    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return;
}
/*****************************************************************
 **************** Session handling functions *********************
 ****************************************************************/


typedef enum {
    SESSION_OPEN_SOCKET_START,
    SESSION_OPEN_SOCKET_WAIT,
    SESSION_REGISTER,
    SESSION_SEND_FORWARD_OPEN,
    SESSION_RECEIVE_FORWARD_OPEN,
    SESSION_IDLE,
    SESSION_DISCONNECT,
    SESSION_UNREGISTER,
    SESSION_CLOSE_SOCKET,
    SESSION_START_RETRY,
    SESSION_WAIT_ERR_RETRY,
    SESSION_WAIT_IDLE_RECONNECT
} session_state_t;


/* Set connection status and reason atomics, and push a ring buffer entry if the status changed.
 * Must only be called from the session handler thread (single writer).
 *
 * watch.status and the ring publish must change together under conn_list.mutex:
 * connection_tag_create() takes a paired snapshot of both (watch.ring_write_idx
 * and watch.status) to seed a freshly created connection tag, and needs the same
 * mutex to avoid reading one from before this transition and the other from after it --
 * see the comment there. */
static inline void session_set_connection_status(ab_session_p session, int32_t new_status) {
    critical_block(session->session_mutex) {
        int32_t old_status = atomic_get_int32(&session->watch.status);
        atomic_set_int32(&session->watch.status, new_status);
        if(old_status != new_status) {
            conn_watch_publish(&session->watch, new_status + PLCTAG_EVENT_CONN_STATUS_OFFSET, PLCTAG_STATUS_OK);
        }
    }
}


THREAD_FUNC(session_handler) {
    ab_session_p session = arg;
    int rc = PLCTAG_STATUS_OK;
    session_state_t state = SESSION_OPEN_SOCKET_START;
    int64_t now = 0;
    int64_t timeout_time = 0;
    int64_t wait_until_time = 0;
    int32_t inactivity_timeout_ms = atomic_get_int32(&session->connection_inactivity_timeout_ms);
    int64_t auto_disconnect_time = time_ms() + inactivity_timeout_ms;
    unsigned int retry_count = 0;
    int64_t retry_wait_ms = 0;
    int auto_disconnect = 0;


    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting thread for session %p", session);

    /* Increment the count of active session handlers */
    atomic_add_int32(&session_handlers_active, 1);

    while(!atomic_get_int32(&session->terminating) && atomic_get_bool(&lib_active)) {
        now = time_ms();

        /* how long should we wait if nothing wakes us? */
        wait_until_time = now + SESSION_IDLE_WAIT_TIME;

        /*
         * Do this on every cycle.   This keeps the queue clean(ish).
         *
         * Make sure we get rid of all the aborted requests queued.
         * This keeps the overall memory usage lower.
         */

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_SPEW, 0, "Critical block.");
        critical_block(session->session_mutex) { purge_aborted_requests_unsafe(session); }

        switch(state) {
            case SESSION_OPEN_SOCKET_START:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_OPEN_SOCKET_START state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_CONNECTING);

                /* we must connect to the gateway*/
                rc = session_open_socket(session);
                if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "session connect failed %s!", plc_tag_decode_error(rc));
                    state = SESSION_CLOSE_SOCKET;
                } else {
                    if(rc == PLCTAG_STATUS_OK) {
                        /* bump auto disconnect time into the future so that we do not accidentally disconnect immediately. */
                        inactivity_timeout_ms = atomic_get_int32(&session->connection_inactivity_timeout_ms);
                        auto_disconnect_time = now + inactivity_timeout_ms;

                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "Connect complete immediately, going to state SESSION_REGISTER.");

                        state = SESSION_REGISTER;

                        retry_count = 0;
                    } else {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "Connect started, going to state SESSION_OPEN_SOCKET_WAIT.");

                        state = SESSION_OPEN_SOCKET_WAIT;
                    }
                }

                /* in all cases, don't wait. */
                cond_signal(session->session_wait_cond);

                break;

            case SESSION_OPEN_SOCKET_WAIT:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_OPEN_SOCKET_WAIT state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_CONNECTING);

                /* we must connect to the gateway */
                rc = socket_connect_tcp_check(session->sock, 20); /* MAGIC */
                if(rc == PLCTAG_STATUS_OK) {
                    /* connected! */
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Socket connection succeeded.");

                    /* calculate the disconnect time. */
                    inactivity_timeout_ms = atomic_get_int32(&session->connection_inactivity_timeout_ms);
                    auto_disconnect_time = now + inactivity_timeout_ms;

                    state = SESSION_REGISTER;
                } else if(rc == PLCTAG_ERR_TIMEOUT) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Still waiting for connection to succeed.");

                    /* don't wait more.  The TCP connect check will wait in select(). */
                } else {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Session connect failed %s!", plc_tag_decode_error(rc));

                    state = SESSION_CLOSE_SOCKET;
                }

                /* in all cases, don't wait. */
                cond_signal(session->session_wait_cond);

                break;

            case SESSION_REGISTER:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_REGISTER state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_CONNECTING);

                if((rc = session_register(session)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "session registration failed %s!", plc_tag_decode_error(rc));
                    state = SESSION_CLOSE_SOCKET;
                } else {
                    retry_wait_ms = RETRY_WAIT_INITIAL_MS;

                    if(session->use_connected_msg) {
                        state = SESSION_SEND_FORWARD_OPEN;
                    } else {
                        state = SESSION_IDLE;
                    }
                }
                cond_signal(session->session_wait_cond);
                break;

            case SESSION_SEND_FORWARD_OPEN:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_SEND_FORWARD_OPEN state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_CONNECTING);

                if((rc = send_forward_open_request(session)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Send Forward Open failed %s!", plc_tag_decode_error(rc));
                    state = SESSION_UNREGISTER;
                } else {
                    retry_wait_ms = RETRY_WAIT_INITIAL_MS;

                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                           "Send Forward Open succeeded, going to SESSION_RECEIVE_FORWARD_OPEN state.");
                    state = SESSION_RECEIVE_FORWARD_OPEN;
                }
                cond_signal(session->session_wait_cond);
                break;

            case SESSION_RECEIVE_FORWARD_OPEN:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_RECEIVE_FORWARD_OPEN state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_CONNECTING);

                if((rc = receive_forward_open_response(session)) != PLCTAG_STATUS_OK) {
                    if(rc == PLCTAG_ERR_DUPLICATE) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "Duplicate connection error received, trying again with different connection ID.");
                        state = SESSION_SEND_FORWARD_OPEN;
                    } else if(rc == PLCTAG_ERR_TOO_LARGE) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "Requested packet size too large, retrying with smaller size.");
                        state = SESSION_SEND_FORWARD_OPEN;
                    } else if(rc == PLCTAG_ERR_UNSUPPORTED && !session->only_use_old_forward_open) {
                        /* if we got an unsupported error and we are trying with ForwardOpenEx, then try the old command. */
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "PLC does not support ForwardOpenEx, trying old ForwardOpen.");
                        session->only_use_old_forward_open = 1;
                        state = SESSION_SEND_FORWARD_OPEN;
                    } else {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Receive Forward Open failed %s!",
                               plc_tag_decode_error(rc));
                        state = SESSION_UNREGISTER;
                    }
                } else {
                    retry_wait_ms = RETRY_WAIT_INITIAL_MS;
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Send Forward Open succeeded, going to SESSION_IDLE state.");
                    state = SESSION_IDLE;
                }
                cond_signal(session->session_wait_cond);
                break;

            case SESSION_IDLE:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_IDLE state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_UP);

                /* make sure that our timeout period has not changed */
                if(inactivity_timeout_ms != atomic_get_int32(&session->connection_inactivity_timeout_ms)) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                           "Inactivity timeout changed from %" PRId32 "ms to %" PRId32 "ms, updating auto disconnect time.",
                           inactivity_timeout_ms, atomic_get_int32(&session->connection_inactivity_timeout_ms));
                    inactivity_timeout_ms = atomic_get_int32(&session->connection_inactivity_timeout_ms);
                    auto_disconnect_time = now + inactivity_timeout_ms;
                }

                /* if there is work to do, make sure we do not disconnect. */
                critical_block(session->session_mutex) {
                    int num_reqs = vector_length(session->requests);
                    if(num_reqs > 0) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "There are %d requests pending before cleanup and sending.", num_reqs);
                        inactivity_timeout_ms = atomic_get_int32(&session->connection_inactivity_timeout_ms);
                        auto_disconnect_time = now + inactivity_timeout_ms;
                    }
                }

                if((rc = process_requests(session)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error while processing requests %s!",
                           plc_tag_decode_error(rc));
                    if(session->use_connected_msg) {
                        state = SESSION_DISCONNECT;
                    } else {
                        state = SESSION_UNREGISTER;
                    }

                    cond_signal(session->session_wait_cond);
                }

                /* check if we should disconnect */
                if(auto_disconnect_time < now) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Disconnecting due to inactivity.");

                    auto_disconnect = 1;

                    if(session->use_connected_msg) {
                        state = SESSION_DISCONNECT;
                    } else {
                        state = SESSION_UNREGISTER;
                    }
                    cond_signal(session->session_wait_cond);
                }

                /* if there is work to do, make sure we signal the condition var. */
                critical_block(session->session_mutex) {
                    int num_reqs = vector_length(session->requests);
                    if(num_reqs > 0) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "There are %d requests still pending after abort purge and sending.", num_reqs);
                        cond_signal(session->session_wait_cond);
                    }
                }

                break;

            case SESSION_DISCONNECT:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_DISCONNECT state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_DISCONNECTING);

                if((rc = perform_forward_close(session)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Forward close failed %s!", plc_tag_decode_error(rc));
                }

                state = SESSION_UNREGISTER;
                cond_signal(session->session_wait_cond);
                break;

            case SESSION_UNREGISTER:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_UNREGISTER state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_DISCONNECTING);

                if((rc = session_unregister(session)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unregistering session failed %s!", plc_tag_decode_error(rc));
                }

                state = SESSION_CLOSE_SOCKET;
                cond_signal(session->session_wait_cond);
                break;

            case SESSION_CLOSE_SOCKET:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_CLOSE_SOCKET state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_DOWN);

                if((rc = session_close_socket(session)) != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Closing session socket failed %s!", plc_tag_decode_error(rc));
                }

                if(auto_disconnect) {
                    state = SESSION_WAIT_IDLE_RECONNECT;
                } else {
                    state = SESSION_START_RETRY;
                }
                cond_signal(session->session_wait_cond);
                break;

            case SESSION_START_RETRY:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_START_RETRY state.");

                /* FIXME - make this a tag attribute. */
                timeout_time = now + calc_retry_time(retry_count);
                retry_count++;

                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Waiting %dms before trying to reconnect.",
                       (int)(retry_wait_ms));

                /* start waiting. */
                state = SESSION_WAIT_ERR_RETRY;

                cond_signal(session->session_wait_cond);
                break;

            case SESSION_WAIT_ERR_RETRY:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_WAIT_ERR_RETRY state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_ERR_WAIT);

                if(timeout_time < now) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Transitioning to SESSION_OPEN_SOCKET_START.");
                    state = SESSION_OPEN_SOCKET_START;
                    cond_signal(session->session_wait_cond);
                } else {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Wait not complete, still %dms to go.",
                           (int)(timeout_time - now));
                }

                break;

            case SESSION_WAIT_IDLE_RECONNECT:
                /* wait for at least one request to queue before reconnecting. */
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "in SESSION_WAIT_IDLE_RECONNECT state.");
                session_set_connection_status(session, PLCTAG_CONN_STATUS_IDLE_WAIT);

                auto_disconnect = 0;

                /* if there is work to do, reconnect.. */
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_SPEW, 0, "Critical block.");
                critical_block(session->session_mutex) {
                    if(vector_length(session->requests) > 0) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0,
                               "There are requests waiting, reopening connection to PLC.");

                        state = SESSION_OPEN_SOCKET_START;
                        cond_signal(session->session_wait_cond);
                    }
                }

                break;


            default:
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_ERROR, 0, "Unknown state %d!", state);

                /* FIXME - this logic is not complete.  We might be here without
                 * a connected session or a registered session. */

                if(session->use_connected_msg) {
                    state = SESSION_DISCONNECT;
                } else {
                    state = SESSION_UNREGISTER;
                }

                cond_signal(session->session_wait_cond);
                break;
        }

        /*
         * give up the CPU a bit, but only if we are not
         * doing some linked states.
         */
        if(wait_until_time > 0) {
            int64_t time_left = wait_until_time - now;

            if(time_left > 0) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Waiting up to %" PRId64 "ms for something to happen.",
                       time_left);
                cond_wait(session->session_wait_cond, (int)time_left);
            }
        }
    }

    /*
     * One last time before we exit.
     */
    critical_block(session->session_mutex) { purge_aborted_requests_unsafe(session); }

    /* Decrement the count of active session handlers */
    atomic_add_int32(&session_handlers_active, -1);

    THREAD_RETURN(0);
}
int process_requests(ab_session_p session) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p request = NULL;
    ab_request_p bundled_requests[MAX_REQUESTS] = {NULL};
    int num_bundled_requests = 0;
    int remaining_space = 0;


    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_SPEW, 0, "Starting.");

    if(!session) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Null session pointer!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_SPEW, 0, "Checking for requests to process.");

    rc = PLCTAG_STATUS_OK;
    request = NULL;
    session->data_size = 0;
    session->data_offset = 0;

    /* grab a request off the front of the list. */
    critical_block(session->session_mutex) {
        // FIXME - no logging in a mutex!
        // pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, "FIXME: available payload space %d", available_payload);

        /* is there anything to do? */
        if(vector_length(session->requests)) {
            /* get rid of all aborted requests. */
            purge_aborted_requests_unsafe(session);

            /* if there are still requests after purging all the aborted requests, process them. */

            /*
             * The total allowed space for requests is the negotiated packet capacity
             * less the overhead of the CPF data item. The rest of the space is for
             * the EIP encapsulation header and the CPF header and the CPF address item,
             * which are already accounted for in the buffer structure.
             */
            remaining_space = session_get_available_cip_payload_space(session);

            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Available payload space is %d bytes.", remaining_space);

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

            if(vector_length(session->requests)) {
                /* Always process the first request, regardless of packability */
                request = vector_get(session->requests, 0);
                int first_request_size = get_payload_size(request);

                /* Check if the first request fits at all */
                if(first_request_size <= remaining_space) {
                    bundled_requests[num_bundled_requests] = request;
                    num_bundled_requests++;
                    remaining_space -= first_request_size;
                    vector_remove(session->requests, 0);

                    /* If the first request is packable, try to pack more requests */
                    if(request->allow_packing && vector_length(session->requests) > 0) {
                        /* Account for CIP multi-request overhead now that we know we'll have multiple requests */
                        remaining_space -= (int)sizeof(cip_multi_req_header);

                        /* Account for 2-byte offset entry per request (including the first one already processed) */
                        int multi_request_overhead = 2;            /* 2-byte offset entry per additional request */
                        remaining_space -= multi_request_overhead; /* for the first request */

                        while(vector_length(session->requests) > 0 && num_bundled_requests < MAX_REQUESTS) {

                            request = vector_get(session->requests, 0);

                            /* Only pack if this request is packable */
                            if(!request->allow_packing) { break; }

                            int next_request_size = get_payload_size(request) + multi_request_overhead;

                            /* Check if this request fits in remaining space */
                            if(next_request_size > remaining_space) { break; }

                            bundled_requests[num_bundled_requests] = request;
                            num_bundled_requests++;
                            remaining_space -= next_request_size;
                            vector_remove(session->requests, 0);
                        }
                    }
                    /* If first request is not packable, we stop here (only the first request is packed) */
                } else {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                           "First request size %d exceeds remaining space %d, cannot process any requests.", first_request_size,
                           remaining_space);
                }
            } else {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "All requests in queue were aborted, nothing to do.");
            }
        }
    }

    if(num_bundled_requests > 0) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "%d requests to process.", num_bundled_requests);

        do {
            /* copy and pack the requests into the session buffer. */
            /* FIXME - pack_requests() only returns PLCTAG_STATUS_OK */
            rc = pack_requests(session, bundled_requests, num_bundled_requests);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error while packing requests, %s!", plc_tag_decode_error(rc));
                break;
            }

            /* fill in all the necessary parts to the request. */
            if((rc = prepare_request(session)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to prepare request, %s!", plc_tag_decode_error(rc));
                break;
            }

            /* send the request */
            if((rc = send_eip_request(session, SESSION_DEFAULT_TIMEOUT)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error sending packet %s!", plc_tag_decode_error(rc));
                break;
            }

            /* wait for the response */
            if((rc = recv_eip_response(session, SESSION_DEFAULT_TIMEOUT)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error receiving packet response %s!", plc_tag_decode_error(rc));
                break;
            }

            /*
             * check the CIP status, but only if this is a bundled
             * response.   If it is a singleton, then we pass the
             * status back to the tag.
             */
            if(num_bundled_requests > 1) {
                cip_multi_resp_header *multi_resp = NULL;

                if(le2h16(((eip_encap *)(session->data))->encap_command) == AB_EIP_UNCONNECTED_SEND) {
                    eip_cip_uc_resp *resp = (eip_cip_uc_resp *)(session->data);
                    uint16_t udi_item_length = 0;
                    size_t response_overhead = 0;
                    size_t response_size = 0;

                    /* we only know we got an EIP header, so check before reading CPF/CIP fields. */
                    if((size_t)session->data_size < sizeof(*resp)) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                               "Unconnected response of %u bytes is too short to hold a CIP response of %d bytes!",
                               session->data_size, (int)sizeof(*resp));
                        rc = PLCTAG_ERR_TOO_SMALL;
                        break;
                    }

                    udi_item_length = le2h16(resp->cpf_udi_item_length);

                    multi_resp = (cip_multi_resp_header *)(&(resp->reply_service));

                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Received unconnected packet with session sequence ID %llx",
                           resp->encap_sender_context);

                    /* punt if we got an overall error or it is not a partial/bundled error. */
                    if(resp->status != AB_EIP_OK && resp->status != AB_CIP_ERR_PARTIAL_ERROR) {
                        rc = decode_cip_error_code(&(resp->status),
                                                   cip_error_data_size(&resp->status, session->data + session->data_size));
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Command failed! (%d/%d) %s", resp->status, rc,
                               plc_tag_decode_error(rc));
                        break;
                    }

                    response_overhead = (size_t)((uint8_t *)multi_resp - session->data);
                    response_size = (size_t)session->data_size - response_overhead;

                    /* check the passed UDI data item size against what we really got. */
                    if((size_t)udi_item_length != response_size) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                               "Incorrectly constructed response! UDI data length field is %zu but actual size is %zu!",
                               (size_t)udi_item_length, response_size);

                        rc = PLCTAG_ERR_BAD_DATA;
                        break;
                    }
                } else if(le2h16(((eip_encap *)(session->data))->encap_command) == AB_EIP_CONNECTED_SEND) {
                    eip_cip_co_resp *resp = (eip_cip_co_resp *)(session->data);
                    uint16_t cdi_item_length = 0;
                    size_t response_overhead = 0;
                    size_t response_size = 0;

                    /* we only know we got an EIP header, so check before reading CPF/CIP fields. */
                    if((size_t)session->data_size < sizeof(*resp)) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                               "Connected response of %u bytes is too short to hold a CIP response of %d bytes!",
                               session->data_size, (int)sizeof(*resp));
                        rc = PLCTAG_ERR_TOO_SMALL;
                        break;
                    }

                    cdi_item_length = le2h16(resp->cpf_cdi_item_length);

                    multi_resp = (cip_multi_resp_header *)(&(resp->reply_service));

                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0,
                           "Received connected packet with connection ID %x and sequence ID %u(%x)",
                           le2h32(resp->cpf_orig_conn_id), le2h16(resp->cpf_conn_seq_num), le2h16(resp->cpf_conn_seq_num));

                    /* punt if we got an overall error or it is not a partial/bundled error. */
                    if(resp->status != AB_EIP_OK && resp->status != AB_CIP_ERR_PARTIAL_ERROR) {
                        size_t status_size = cip_error_data_size(&resp->status, session->data + session->data_size);

                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Response status=%u", resp->status);
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Received CIP error %s (%s).",
                               decode_cip_error_long(&resp->status, status_size),
                               decode_cip_error_short(&resp->status, status_size));
                        rc = decode_cip_error_code(&(resp->status), status_size);
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Command failed! (%d/%d) %s", resp->status, rc,
                               plc_tag_decode_error(rc));
                        break;
                    }

                    response_overhead = (size_t)((uint8_t *)(&resp->cpf_conn_seq_num) - session->data);
                    response_size = (size_t)session->data_size - response_overhead;

                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "response_overhead=%zu", response_overhead);
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "response_size=%zu", response_size);

                    /* check the passed CDI data item size against what we really got. */
                    if((size_t)cdi_item_length != response_size) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                               "Incorrectly constructed response! CDI data length field is %zu but actual size is %zu!",
                               (size_t)cdi_item_length, response_size);

                        rc = PLCTAG_ERR_BAD_DATA;
                        break;
                    }
                } else {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unexpected EIP packet type, %04x!",
                           le2h16(((eip_encap *)(session->data))->encap_command));
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
                        (size_t)((uint8_t *)multi_resp - session->data) + offsetof(cip_multi_resp_header, request_offsets);
                    size_t offsets_size = (size_t)num_bundled_requests * sizeof(uint16_le);

                    if(offsets_start > (size_t)session->data_size || offsets_size > (size_t)session->data_size - offsets_start) {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                               "Response of %d bytes is too short to hold %d packed response offsets!", session->data_size,
                               num_bundled_requests);
                        rc = PLCTAG_ERR_TOO_SMALL;
                        break;
                    }
                }

                /* we have multiple requests, sanity check the data. */
                if(le2h16(multi_resp->request_count) == num_bundled_requests) {
                    size_t offset_base = (size_t)((uint8_t *)(&multi_resp->request_count) - session->data);

                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "offset_base=%zu", offset_base);

                    /* check all the offsets */
                    for(int resp_index = 0; resp_index < num_bundled_requests; resp_index++) {
                        size_t resp_offset = (size_t)le2h16(multi_resp->request_offsets[resp_index]) + offset_base;

                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, 0, "Response %d starts at byte offset %zu", resp_index,
                               resp_offset);

                        if(resp_offset >= (size_t)session->data_size) {
                            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                                   "Response %d has offset %zu which is outside the session data!", resp_index, resp_offset);
                            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
                            break;
                        }
                    }
                } else {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Expected %d packed responses back but got %zu!",
                           num_bundled_requests, (size_t)le2h16(multi_resp->request_count));
                    rc = PLCTAG_ERR_BAD_DATA;
                    break;
                }
            }

            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Got error %s when processing incoming response(s)!",
                       plc_tag_decode_error(rc));
                break;
            }

            /* copy the results back out. Every request gets a copy. */
            for(int i = 0; i < num_bundled_requests; i++) {
                rc = unpack_response(session, bundled_requests[i], i);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, bundled_requests[i]->tag_id, "Unable to unpack response!");
                    break;
                }

                /* release our reference */
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_DETAIL, bundled_requests[i]->tag_id,
                       "rc_dec: Releasing request reference.");
                bundled_requests[i] = rc_dec(bundled_requests[i]);
            }

            rc = PLCTAG_STATUS_OK;
        } while(0);

        /* problem? push the requests back on the queue. */
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Error sending or receiving requests!");

            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Pushing %d requests back into the queue.", num_bundled_requests);

            /* session->requests is also written by session_add_request() (tickler thread)
             * under session->session_mutex, so this push-back needs the same lock. */
            critical_block(session->session_mutex) {
                for(int i = num_bundled_requests - 1; i >= 0; i--) {
                    if(bundled_requests[i]) { vector_insert(session->requests, 0, bundled_requests[i]); }
                }
            }
        }

        /* tickle the main tickler thread to note that we have responses. */
        plc_tag_tickler_wake();
    }

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_SPEW, 0, "Done.");

    return rc;
}
/* new version of Forward Open */
int receive_forward_open_response(ab_session_p session) {
    eip_forward_open_response_t *fo_resp;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Starting");

    /* Never wait unboundedly for the forward open response. A dropped session must surface as an */
    /* error so the handler can log it and re-enter the connect/retry path instead of parking. */
    rc = recv_eip_response(session, SESSION_DEFAULT_TIMEOUT);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to receive Forward Open response.");
        return rc;
    }

    fo_resp = (eip_forward_open_response_t *)(session->data);

    do {
        /*
         * recv_eip_response() only guarantees that we got an EIP header.  We are about to
         * read the CIP reply status, so require everything up to and including status_size.
         * An error reply legitimately stops there -- it carries extended status instead of
         * the connection IDs -- so do not demand the whole struct here.  The buffer is not
         * cleared between packets, so a short response would otherwise be read as stale
         * data from the previous one.
         */
        if((size_t)session->data_size < offsetof(eip_forward_open_response_t, orig_to_targ_conn_id)) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                   "Forward Open response of %u bytes is too short to hold the CIP reply status at %d bytes!", session->data_size,
                   (int)offsetof(eip_forward_open_response_t, orig_to_targ_conn_id));
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        if(le2h16(fo_resp->encap_command) != AB_EIP_UNCONNECTED_SEND) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unexpected EIP packet type received: %d!", fo_resp->encap_command);
            rc = PLCTAG_ERR_BAD_DATA;
            break;
        }

        if(le2h32(fo_resp->encap_status) != AB_EIP_OK) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "EIP command failed, response code: %d", fo_resp->encap_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(fo_resp->general_status != AB_EIP_OK) {
            size_t general_status_size = cip_error_data_size(&fo_resp->general_status, session->data + session->data_size);

            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Forward Open command failed, response code: %s (%d)",
                   decode_cip_error_short(&fo_resp->general_status, general_status_size), fo_resp->general_status);
            if(fo_resp->general_status == AB_CIP_ERR_UNSUPPORTED_SERVICE) {
                /* this type of command is not supported! */
                pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Received CIP command unsupported error from the PLC!");
                rc = PLCTAG_ERR_UNSUPPORTED;
            } else {
                rc = PLCTAG_ERR_REMOTE_ERR;

                /* comparing pointers directly is UB, so compare the integer values instead. */
                if(fo_resp->general_status == 0x01 && fo_resp->status_size >= 2
                   && (intptr_t)(&fo_resp->status_size + 5) <= (intptr_t)(session->data + session->data_size)) {
                    /* we might have an error that tells us the actual size to use. */
                    uint8_t *data = &fo_resp->status_size;
                    int extended_status = data[1] | (data[2] << 8);
                    uint16_t supported_size = (uint16_t)((uint16_t)data[3] | (uint16_t)((uint16_t)data[4] << (uint16_t)8));

                    if(extended_status == 0x109) { /* MAGIC */
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                               "Error from forward open request, unsupported size, but size %d is supported.", supported_size);

                        /*
                         * The PLC is telling us the size we asked for is unsupported and offering a size it
                         * does support.  That offered size must not exceed what we asked for -- session->data
                         * was allocated based on our request, and a PLC claiming to "support" a larger size
                         * than we asked for is a protocol disagreement, not a legitimate response.
                         */
                        if(supported_size < session->min_payload_size) {
                            /*
                             * There is a floor as well as a ceiling.  Every protocol family has a
                             * fixed per-request overhead, and a payload below that leaves no room
                             * for a request at all -- the size arithmetic downstream then has an
                             * overhead larger than the space, which is where the underflows live.
                             * A PLC offering less than we can use is not a size we can negotiate to.
                             */
                            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                                   "PLC reported a supported size of %u, below the %d bytes this protocol needs for a "
                                   "single request!",
                                   supported_size, session->min_payload_size);
                            rc = PLCTAG_ERR_TOO_SMALL;
                        } else if(supported_size <= session->max_payload_guess) {
                            critical_block(session->session_mutex) { session->max_payload_guess = supported_size; }
                            rc = PLCTAG_ERR_TOO_LARGE;
                        } else {
                            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                                   "PLC reported a supported size, %u, larger than what we requested, %u! This is a "
                                   "protocol disagreement and may indicate a malicious or misbehaving PLC; aborting.",
                                   supported_size, session->max_payload_guess);
                            rc = PLCTAG_ERR_BAD_DATA;
                        }
                    } else if(extended_status == 0x100) { /* MAGIC */
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                               "Error from forward open request, duplicate connection ID.  Need to try again.");
                        rc = PLCTAG_ERR_DUPLICATE;
                    } else {
                        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "CIP extended error %s (%s)!",
                               decode_cip_error_short(&fo_resp->general_status, general_status_size),
                               decode_cip_error_long(&fo_resp->general_status, general_status_size));
                    }
                } else {
                    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "CIP error code %s (%s)!",
                           decode_cip_error_short(&fo_resp->general_status, general_status_size),
                           decode_cip_error_long(&fo_resp->general_status, general_status_size));
                }
            }

            break;
        }

        /* a success reply must carry the connection IDs and the rest of the fixed fields. */
        if((size_t)session->data_size < sizeof(*fo_resp)) {
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0,
                   "Successful Forward Open response of %u bytes is too short to hold the connection data of %d bytes!",
                   session->data_size, (int)sizeof(*fo_resp));
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        /* success! */
        session->targ_connection_id = le2h32(fo_resp->orig_to_targ_conn_id);
        session->orig_connection_id = le2h32(fo_resp->targ_to_orig_conn_id);

        critical_block(session->session_mutex) { session->max_payload_size = session->max_payload_guess; }

        pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0,
               "ForwardOpen succeeded with our connection ID %x and the PLC connection ID %x with packet size %u.",
               session->orig_connection_id, session->targ_connection_id, session->max_payload_size);

        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_INFO, 0, "Done.");

    return rc;
}