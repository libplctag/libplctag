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
#include "slice.h"
#include "socket.h"
#include "thread.h"
#include "utils.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>


static THREAD_FUNC(conn_handler);


struct tcp_server {
    int sock_fd;
    slice_s (*handler)(slice_s input, slice_s output, void *context);
    void *context;
    size_t context_size;
};

struct client_session {
    int client_fd;
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
        server->sock_fd = socket_open_tcp_server(port);

        if(server->sock_fd < 0) { error("ERROR: Unable to open TCP socket, error code %d!", server->sock_fd); }

        server->handler = handler;
        server->context = context;
        server->context_size = context_size;
    }

    return server;
}

void tcp_server_start(tcp_server_p server, volatile sig_atomic_t *terminate) {
    int client_fd;
    static bool done; /* static so it doesn't go out of scope, since it's passed to sub-threads. */
    done = false;     /* initialised every invocation for logic sake, even though that's once. */

    info("Waiting for new client connection.");

    do {
        client_fd = socket_accept(server->sock_fd);

        if(client_fd >= 0) {
            bool thread_created = false;

            /* The client thread is responsible for freeing these */
            /* TODO: A malloc'ed blob inside a malloc'ed blob is too much. Simplify. */
            // FIXME - combine the allocations and use calloc or memset to get zeroed memory
            struct client_session *session = malloc(sizeof(struct client_session));
            // NOLINTNEXTLINE
            memset(session, 0, sizeof(*session));

            session->server_context = malloc(server->context_size);
            if(session && session->server_context) {
                /* Make a copy of the server context so the thread can use it without threading concerns. */
                // NOLINTNEXTLINE
                memcpy(session->server_context, server->context, server->context_size);
                session->client_fd = client_fd; /* copy of a temporary value - no thread safety concerns */
                session->server = server;       /* reference to a long-lived struct, which has values and the original context */
                session->server_done = &done;   /* reference to a flag that any thread can raise (and all must monitor) */

                /* spawn new thread to handle the connection */

                // /* DEBUG */
                // fprintf(stderr, "tcp_server.c:112 Creating new thread for socket %d.\n", client_fd);
                // fflush(stderr);
                // /* DEBUG */


                if(thread_create(&(session->thread), conn_handler, 10 * 1024, session) == THREAD_STATUS_OK) {
                    thread_created = true;
                } else {
                    info("ERROR: Unable to create connection handler thread!");
                }
            } else {
                info("ERROR: Unable to alloc client session memory!");
            }
            if(!thread_created) /* then cleanup and abandon the connection */
            {
                socket_close(session->client_fd);
                if(session->server_context) { free(session->server_context); }
                if(session) { free(session); }
            }
        } else if(client_fd != SOCKET_STATUS_OK) {
            /* There was an error either opening or accepting! */
            info("WARN: error while trying to open/accept the client socket.");
        }

        /* wait a bit to give back the CPU. */
        util_sleep_ms(1);
    } while(!done && !*terminate);

    /* in case we were terminated by signal, raise the done flag for all the threads to exit */
    done = true;
}


void tcp_server_destroy(tcp_server_p server) {
    if(server) {
        if(server->sock_fd >= 0) {
            socket_close(server->sock_fd);
            server->sock_fd = INT_MIN;
        }
        free(server);
    }
}

THREAD_FUNC(conn_handler) {
    client_session_p session = arg;
    uint8_t buf[65536 + 128];                            /* Rockwell supports up to 64k (Micro800) */
    tcp_server_p server = (tcp_server_p)session->server; /* need to cast for C++ */
    slice_s tmp_input = {0};
    slice_s tmp_output = {0};
    int rc = TCP_SERVER_DONE;

    info("Got new client connection, going into processing loop.");

    /* no one will join this thread, so clean ourselves up. */
    thread_detach();

    session->buffer = slice_make(buf, sizeof(buf));
    tmp_input = session->buffer;

    do {
        /* get an incoming packet or a partial packet. */
        tmp_input = socket_read(session->client_fd, tmp_input);

        if(slice_has_err(tmp_input)) {
            info("WARN: error response reading socket! error %d", slice_get_err(tmp_input));
            break;
        }

        /* try to process the packet. */
        tmp_output = server->handler(tmp_input, session->buffer, session->server_context);

        /* check the response. */
        if(!slice_has_err(tmp_output)) {
            /* FIXME - this should be in a loop to make sure all data is pushed. */
            rc = socket_write(session->client_fd, tmp_output);

            /* error writing? */
            if(rc < 0) {
                info("ERROR: error writing output packet! Error: %d", rc);
                break;
            } else {
                /* all good. Reset the buffers etc. */
                tmp_input = session->buffer;
                rc = TCP_SERVER_PROCESSED;
            }
        } else {
            /* there was some sort of error or exceptional condition. */
            switch((rc = slice_get_err(tmp_output))) {
                case TCP_SERVER_DONE:
                    /* Note this is assumed atomic, which is not guaranteed. To be really sure it
                       should be mutex protected or changed to a stdatomic. The former is messy
                       and the latter requires C11. Since I think it might be actually a bug (why
                       would deregistering a session kill the server?) I've not bothered for now. */
                    *(session->server_done) = true;
                    break;

                case TCP_SERVER_INCOMPLETE:
                    tmp_input = slice_from_slice(session->buffer, slice_len(tmp_input),
                                                 slice_len(session->buffer) - slice_len(tmp_input));
                    break;

                case TCP_SERVER_PROCESSED: break;

                case TCP_SERVER_UNSUPPORTED:
                    info("WARN: Unsupported packet!");
                    slice_dump(tmp_input);
                    break;

                default: info("WARN: Unsupported return code %d!", rc); break;
            }
        }
    } while((rc == TCP_SERVER_INCOMPLETE || rc == TCP_SERVER_PROCESSED)
            && (*(session->server_done) != true)); /* make sure another thread hasn't killed the server */

    /* done with the socket */


    // /* DEBUG */
    // fprintf(stderr, "tcp_server.c:227 Closing client socket %d.\n", session->client_fd);
    // fflush(stderr);
    // /* DEBUG */

    socket_close(session->client_fd);

    /* see tcp_server_start() where these are malloc'ed for us */
    free(session->server_context);
    free(session);

    THREAD_RETURN(0);
}
