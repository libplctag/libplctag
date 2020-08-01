/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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

#include <stdbool.h>
#include <stdlib.h>
#include "slice.h"
#include "socket.h"
#include "tcp_server.h"
#include "utils.h"

struct tcp_server {
    int sock_fd;
    slice_s buffer;
    slice_s (*handler)(slice_s input, slice_s output, void *context);
    void *context;
};


tcp_server_p tcp_server_create(const char *host, const char *port, slice_s buffer, slice_s (*handler)(slice_s input, slice_s output, void *context), void *context)
{
    tcp_server_p server = calloc(1, sizeof(*server));

    if(server) {
        server->sock_fd = socket_open(host, port);

        if(server->sock_fd < 0) {
            error("ERROR: Unable to open TCP socket, error code %d!", server->sock_fd);
        }

        server->buffer = buffer;
        server->handler = handler;
        server->context = context;
    }

    return server;
}

void tcp_server_start(tcp_server_p server, volatile sig_atomic_t *terminate)
{
    int client_fd;
    bool done = false;

    info("Waiting for new client connection.");

    do {
        client_fd = socket_accept(server->sock_fd);

        if(client_fd >= 0) {
            slice_s tmp_input = server->buffer;
            slice_s tmp_output;
            int rc;

            info("Got new client connection, going into processing loop.");

            do {
                rc = TCP_SERVER_PROCESSED;

                /* get an incoming packet or a partial packet. */
                tmp_input = socket_read(client_fd, tmp_input);

                if((rc = slice_has_err(tmp_input))) {
                    info("WARN: error response reading socket! error %d", rc);
                    rc = TCP_SERVER_DONE;
                    break;
                }

                /* try to process the packet. */
                tmp_output = server->handler(tmp_input, server->buffer, server->context);

                /* check the response. */
                if(!slice_has_err(tmp_output)) {
                    /* FIXME - this should be in a loop to make sure all data is pushed. */
                    rc = socket_write(client_fd, tmp_output);

                    /* error writing? */
                    if(rc < 0) {
                        info("ERROR: error writing output packet! Error: %d", rc);
                        rc = TCP_SERVER_DONE;
                        break;
                    } else {
                        /* all good. Reset the buffers etc. */
                        tmp_input = server->buffer;
                        rc = TCP_SERVER_PROCESSED;
                    }
                } else {
                    /* there was some sort of error or exceptional condition. */
                    switch((rc = slice_get_err(tmp_input))) {
                        case TCP_SERVER_DONE:
                            done = true;
                            break;

                        case TCP_SERVER_INCOMPLETE:
                            tmp_input = slice_from_slice(server->buffer, slice_len(tmp_input), slice_len(server->buffer) - slice_len(tmp_input));
                            break;

                        case TCP_SERVER_PROCESSED:
                            break;

                        case TCP_SERVER_UNSUPPORTED:
                            info("WARN: Unsupported packet!");
                            slice_dump(tmp_input);
                            break;

                        default:
                            info("WARN: Unsupported return code %d!", rc);
                            break;
                    }
                }
            } while(rc == TCP_SERVER_INCOMPLETE || rc == TCP_SERVER_PROCESSED);

            /* done with the socket */
            socket_close(client_fd);
        } else if (client_fd != SOCKET_STATUS_OK) {
            /* There was an error either opening or accepting! */
            info("WARN: error while trying to open/accept the client socket.");
        }

        /* wait a bit to give back the CPU. */
        util_sleep_ms(1);
    } while(!done && !*terminate);
}



void tcp_server_destroy(tcp_server_p server)
{
    if(server) {
        if(server->sock_fd >= 0) {
            socket_close(server->sock_fd);
            server->sock_fd = INT_MIN;
        }
        free(server);
    }
}
