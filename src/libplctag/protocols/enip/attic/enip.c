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
 * ENIP Protocol Entry Point
 *
 * STATUS: Phase 3. Global registry implemented.
 *
 * This file is the public entry point registered with the tag dispatch table.
 * enip_tag_create routes to either a @connection tag or a normal protocol tag,
 * sharing connections by gateway+route_path through a global registry.
 */
#include <libplctag/protocols/enip/enip.h>
#include <libplctag/protocols/enip/enip_conn.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/rc.h>
#include <string.h>

/* Global connection registry (mirroring modbus.c pattern) */
static enip_connection_t *enip_connections = NULL;
static mutex_p enip_registry_mutex = NULL;

int enip_init(void) {
    int32_t rc = mutex_create(&enip_registry_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_ERROR, 0, "ENIP: Failed to create registry mutex");
        return rc;
    }
    return PLCTAG_STATUS_OK;
}

void enip_teardown(void) {
    if(enip_registry_mutex) {
        critical_block(enip_registry_mutex) {
            /* Signal all connections to shut down and join their threads */
            enip_connection_t *conn = enip_connections;
            while(conn) {
                conn->shutdown_requested = true;
                if(conn->link.socket) {
                    socket_wake(conn->link.socket);
                }
                conn = conn->next;
            }
        }

        /* Drop all refs and clear the list */
        critical_block(enip_registry_mutex) {
            enip_connection_t *conn = enip_connections;
            while(conn) {
                enip_connection_t *next = conn->next;
                rc_dec(conn);
                conn = next;
            }
            enip_connections = NULL;
        }

        mutex_destroy(&enip_registry_mutex);
    }
}

/* Find or create a connection by gateway+route_path (plan §3.1).
 * Multiple tags to the same gateway share one connection, socket, and ForwardOpen. */
enip_connection_t *enip_registry_find_or_create(attr attribs) {
    const char *gateway = attr_get_str(attribs, "gateway", NULL);
    const char *path = attr_get_str(attribs, "path", "");

    if(!gateway) {
        return NULL;
    }

    enip_connection_t *conn = NULL;

    critical_block(enip_registry_mutex) {
        /* Search the list for a matching connection */
        enip_connection_t **walker = &enip_connections;

        while(*walker) {
            /* Match if gateway and path are the same, and rc_inc succeeds */
            if(str_cmp_i(gateway, (*walker)->link.host) == 0 &&
               str_cmp_i(path, (const char *)(*walker)->link.route_path) == 0 &&
               rc_inc(*walker)) {
                /* Found a match */
                conn = *walker;
                break;
            }
            walker = &((*walker)->next);
        }

        if(!conn) {
            /* No match found, create a new connection */
            pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "ENIP: Creating new connection for gateway='%s' path='%s'",
                   gateway, path);

            conn = enip_connection_create(attribs);
            if(conn) {
                /* enip_connection_create returns refcount 1.  That ref belongs to
                 * the registry list (released in enip_teardown).  The caller (tag)
                 * needs its OWN ref, matching the found-connection path above and
                 * the "already rc_inc'd by registry" contract in enip_tag.c.
                 * Without this rc_inc the tag and the list share one ref; dropping
                 * the last tag frees the connection while it is still linked, and
                 * enip_teardown then walks a dangling pointer (heap-use-after-free). */
                conn->next = enip_connections;
                enip_connections = conn;
                rc_inc(conn);
            }
        }
    }

    return conn;
}

plc_tag_p enip_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                          void *userdata, plc_tag_p src_tag) {
    const char *name = attr_get_str(attribs, "name", NULL);
    const char *gateway = attr_get_str(attribs, "gateway", NULL);

    /* @connection tags are handled separately */
    if(name && str_cmp_i(name, "@connection") == 0) {
        return enip_connection_tag_create(attribs, tag_callback_func, userdata, src_tag);
    }

    if(src_tag && src_tag->protocol_type == TAG_PROTOCOL_ENIP_CONNECTION) {
        return enip_connection_tag_create(attribs, tag_callback_func, userdata, src_tag);
    }

    /* Validate gateway is present */
    if(!gateway || str_length(gateway) == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_ERROR, 0, "ENIP: Missing required 'gateway' attribute");
        return NULL;
    }

    return enip_protocol_tag_create(attribs, tag_callback_func, userdata, src_tag);
}
