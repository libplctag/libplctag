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


#define MAX_CIP_LGX_MSG_SIZE (0x01FF & 504)
#define MAX_CIP_LGX_MSG_SIZE_EX (0xFFFF & 4000)

#define MAX_CIP_MICRO800_MSG_SIZE (0x01FF & 504)
/*
 * Micro800 supports the Extended Forward Open and can negotiate a comms buffer
 * as large as 60000 bytes.  This is the opening ask; a PLC that supports less
 * answers with its own size and receive_forward_open_response() retries at that
 * size, down to min_payload_size.
 */
#define MAX_CIP_MICRO800_MSG_SIZE_EX (0xFFFF & 60000)

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

    while((active_count = atomic_get_int32(&(conn_list.handler_count))) > 0) {
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
            pdebug(DEBUG_MODULE_AB_SESSION, DEBUG_WARN, 0, "Unable to create *Logix PCCC session!");
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
            session->only_use_old_forward_open = false;
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
    session->owner_list = &conn_list;
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
/*****************************************************************
 **************** Session handling functions *********************
 ****************************************************************/




/* Set connection status and reason atomics, and push a ring buffer entry if the status changed.
 * Must only be called from the session handler thread (single writer).
 *
 * watch.status and the ring publish must change together under conn_list.mutex:
 * connection_tag_create() takes a paired snapshot of both (watch.ring_write_idx
 * and watch.status) to seed a freshly created connection tag, and needs the same
 * mutex to avoid reading one from before this transition and the other from after it --
 * see the comment there. */


/* new version of Forward Open */