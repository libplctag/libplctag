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

#include "tcp_server.h"
#include "err.h"
#include "slice.h"
#include "socket.h"
#include "thread.h"
#include "utils.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


static THREAD_FUNC(conn_handler);


struct tcp_server {
    SOCKET sock_fd;
    slice_s (*handler)(slice_s input, slice_s output, void *context);
    void *context;
    size_t context_size;
};

struct client_session {
    SOCKET client_fd;
    tcp_server_p server;
    void *server_context;
    bool *server_done;
    slice_s buffer;
    thread_p thread;
};
typedef struct client_session *client_session_p;


tcp_server_p tcp_server_create(const char *host, const char *port,
                               slice_s (*handler)(slice_s input, slice_s output, void *context), void *context,
                               size_t context_size) {
    tcp_server_p server = calloc(1, sizeof(*server));

    (void)host;

    if(server) {
        SOCKET sock = socket_open_tcp_server(port);

        if(sock >= 0) {
            server->sock_fd = sock;
        } else {
            error("ERROR: Unable to open TCP socket, error: %s", err_to_string((int)sock));
        }

        server->handler = handler;
        server->context = context;
        server->context_size = context_size;
    }

    return server;
}

void tcp_server_start(tcp_server_p server, volatile sig_atomic_t *terminate) {
    static bool done; /* static so it doesn't go out of scope, since it's passed to sub-threads. */
    done = false;     /* initialised every invocation for logic sake, even though that's once. */

    info("Waiting for new client connection.");

    do {
        SOCKET client_fd = socket_accept(server->sock_fd, 1000); /* MAGIC */

        if(client_fd >= 0) {
            struct client_session *session = NULL;

            /* The client thread is responsible for freeing these */
            /* TODO: A malloc'ed blob inside a malloc'ed blob is too much. Simplify. */
            // FIXME - combine the allocations and use calloc or memset to get zeroed memory
            session = malloc(sizeof(struct client_session));

            if(!session) { error("Unable to allocate memory for the session!"); }

            // NOLINTNEXTLINE
            memset(session, 0, sizeof(*session));

            session->server_context = malloc(server->context_size);

            if(!session->server_context) { error("Unable to allocate memory for the server context!"); }

            /* Make a copy of the server context so the thread can use it without threading concerns. */
            // NOLINTNEXTLINE
            memcpy(session->server_context, server->context, server->context_size);
            session->client_fd = client_fd; /* copy of a temporary value - no thread safety concerns */
            session->server = server;       /* reference to a long-lived struct, which has values and the original context */
            session->server_done = &done;   /* reference to a flag that any thread can raise (and all must monitor) */

            if(thread_create(&(session->thread), conn_handler, 10 * 1024, session) != THREAD_STATUS_OK) {
                error("ERROR: Unable to create connection handler thread!");
            }
        } else if(client_fd == ERR_SOCKET_TIMEOUT) {
            info("Timed out waiting for new client connection.");
            continue;
        } else {
            error("ERROR: Received error, %s, accepting new client connection!", err_to_string((int)client_fd));
            done = true;
        }

        /* give back the CPU. */
        system_yield();
    } while(!done && !*terminate);

    /* in case we were terminated by signal, raise the done flag for all the threads to exit */
    done = true;
}


void tcp_server_destroy(tcp_server_p server) {
    if(server) {
        if(server->sock_fd != INVALID_SOCKET) {
            socket_close(server->sock_fd);
            server->sock_fd = INVALID_SOCKET;
        }
        free(server);
    }
}


THREAD_FUNC(conn_handler) {
    client_session_p session = arg;
    uint8_t input_buf[65536 + 128];                            /* Rockwell supports up to 64k (Micro800) */
    uint8_t output_buf[65536 + 128];                           /* plus some extra for headers etc. */
    tcp_server_p server = (tcp_server_p)session->server; /* need to cast for C++ */
    slice_s accumulated_data = {0};  /* slice representing all data received so far */
    slice_s read_target = {0};       /* slice representing where to read next data */
    slice_s tmp_output = {0};
    int rc = TCP_SERVER_DONE;

    info("Got new client connection, going into processing loop.");

    /* no one will join this thread, so clean ourselves up. */
    thread_detach();

    accumulated_data = slice_make(input_buf, 0);  /* start with zero accumulated data */
    read_target = slice_make(input_buf, sizeof(input_buf));  /* read into entire buffer initially */
    tmp_output = slice_make(output_buf, sizeof(output_buf));

    do {
        /* get data from the socket into the input buffer */
        slice_s new_data = socket_read(session->client_fd, read_target, 1000); /* MAGIC */

        /* check for errors */
        if(slice_has_err(new_data)) {
            int err = slice_get_err(new_data);
            if(err == ERR_SOCKET_TIMEOUT) {
                info("Timed out waiting for client to send us a request.");
                continue;
            } else {
                info("Error, %s, reading data from the client!", err_to_string(err));
                break;
            }
        }

        /* update accumulated_data to include the newly read bytes */
        accumulated_data = slice_make(input_buf, slice_len(accumulated_data) + slice_len(new_data));

        /* try to process the packet. */
        tmp_output = server->handler(accumulated_data, tmp_output, session->server_context);

        /* check the response. */
        if(!slice_has_err(tmp_output)) {
            slice_s write_res = socket_write(session->client_fd, tmp_output, 1000); /* MAGIC*/

            if(slice_has_err(write_res)) {
                info("Error, %s, writing packet!", err_to_string(slice_get_err(write_res)));
                break;
            }

            /* all good. Reset the buffers etc. */
            accumulated_data = slice_make(input_buf, 0);
            read_target = slice_make(input_buf, sizeof(input_buf));
            rc = ERR_TCP_PROCESSED;
        } else {
            /* there was some sort of error or exceptional condition. */
            switch((rc = slice_get_err(tmp_output))) {
                case ERR_TCP_DONE:
                    /* Note this is assumed atomic, which is not guaranteed. To be really sure it
                       should be mutex protected or changed to a stdatomic. The former is messy
                       and the latter requires C11. Since I think it might be actually a bug (why
                       would deregistering a session kill the server?) I've not bothered for now. */
                    *(session->server_done) = true;
                    break;

                case ERR_TCP_INCOMPLETE:
                    /* next read should go after the accumulated data */
                    read_target = slice_from_slice(slice_make(input_buf, sizeof(input_buf)),
                                                   slice_len(accumulated_data),
                                                   sizeof(input_buf) - slice_len(accumulated_data));
                    break;

                case ERR_TCP_PROCESSED: break;

                case ERR_TCP_BAD_REQUEST:
                    info("WARN: Bad request!");
                    slice_dump(accumulated_data);
                    break;

                default: info("WARN: Unsupported return code %d!", rc); break;
            }
        }
    } while((rc == ERR_TCP_INCOMPLETE || rc == ERR_TCP_PROCESSED)
            && (*(session->server_done) != true)); /* make sure another thread hasn't killed the server */

    socket_close(session->client_fd);

    /* see tcp_server_start() where these are malloc'ed for us */
    free(session->server_context);
    free(session);

    THREAD_RETURN(0);
}
