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

#include "udp_server.h"
#include "eip.h"
#include "log.h"
#include "plc.h"
#include "slice.h"
#include "socket.h"
#include "thread.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define EIP_LIST_IDENTITY_COMMAND ((uint16_t)0x0063)
#define UDP_BUFFER_SIZE (600)
#define UDP_POLL_MS (500)

struct udp_server_s {
    SOCKET sock;
    thread_p thread;
    plc_s plc;
    volatile sig_atomic_t *terminate;
};

static THREAD_FUNC(udp_handler);


udp_server_p udp_server_start(const char *port, plc_s *plc, volatile sig_atomic_t *terminate) {
    udp_server_p server = calloc(1, sizeof(*server));

    if(!server) {
        log_error("Unable to allocate the UDP server!");
        return NULL;
    }

    server->sock = socket_open_udp_server(port);
    if(server->sock == INVALID_SOCKET) {
        free(server);
        return NULL;
    }

    server->plc = *plc;
    server->terminate = terminate;

    if(thread_create(&server->thread, udp_handler, 32 * 1024, server) != THREAD_STATUS_OK) {
        log_error("Unable to create the UDP thread!");
        socket_close(server->sock);
        free(server);
        return NULL;
    }

    log_info("Answering ListIdentity on UDP port %s.", port);

    return server;
}


void udp_server_destroy(udp_server_p server) {
    if(!server) { return; }

    thread_join(server->thread);
    thread_destroy(&server->thread);
    socket_close(server->sock);
    free(server);
}


static THREAD_FUNC(udp_handler) {
    udp_server_p server = (udp_server_p)arg;
    uint8_t in_data[UDP_BUFFER_SIZE];
    uint8_t out_data[UDP_BUFFER_SIZE];

    while(!*server->terminate) {
        socket_peer_s peer;
        slice_s request = socket_read_from(server->sock, slice_make(in_data, sizeof(in_data)), UDP_POLL_MS, &peer);
        slice_s response = {0};

        if(slice_has_err(request)) { continue; }

        /* only discovery travels over UDP here. */
        if(slice_len(request) < EIP_HEADER_SIZE || slice_get_uint16_le(request, 0) != EIP_LIST_IDENTITY_COMMAND) {
            log_info("Ignoring a UDP datagram that is not ListIdentity.");
            continue;
        }

        server->plc.local_ip = socket_local_ipv4_toward(&peer);
        response = eip_dispatch_request(request, slice_make(out_data, sizeof(out_data)), &server->plc);
        if(!slice_has_err(response)) { socket_write_to(server->sock, response, &peer); }
    }

    THREAD_RETURN(0);
}
