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

#include "tcp_server.h"
#include "err.h"
#include "plc.h"
#include <libplctag/api/libplctag.h>
#include <utils/byte_slice.h>
#include "socket.h"
#include "thread.h"
#include "utils.h"
#include "log.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


static THREAD_FUNC(conn_handler);


struct tcp_server {
    SOCKET sock_fd;
    tcp_server_handler_t handler;
    void *context;
    size_t context_size;
};

struct client_session {
    SOCKET client_fd;
    tcp_server_p server;
    void *server_context;
    bool *server_done;
    thread_p thread;
};
typedef struct client_session *client_session_p;


tcp_server_p tcp_server_create(const char *host, const char *port,
                               tcp_server_handler_t handler, void *context,
                               size_t context_size) {
    tcp_server_p server = calloc(1, sizeof(*server));

    (void)host;

    if(server) {
        SOCKET sock = socket_open_tcp_server(port);

        if(sock == INVALID_SOCKET) {
            /*
             * Fail here rather than carrying on. Storing a bad descriptor left the
             * accept loop polling something that was never opened: the process stayed
             * alive and looked healthy while never accepting anything, so a caller
             * waiting for the port to open (the test harness) waited out its whole
             * startup timeout and reported a misleading "did not start listening in
             * time" instead of the bind error -- with no chance to retry, because
             * nothing ever exited. socket_open_tcp_server() logged the real reason.
             */
            free(server);
            return NULL;
        }

        server->sock_fd = sock;
        server->handler = handler;
        server->context = context;
        server->context_size = context_size;
    }

    return server;
}

void tcp_server_start(tcp_server_p server, volatile sig_atomic_t *terminate) {
    static bool done; /* static so it doesn't go out of scope, since it's passed to sub-threads. */
    done = false;     /* initialised every invocation for logic sake, even though that's once. */

    log_info("Waiting for new client connection.");

    do {
        SOCKET client_fd = INVALID_SOCKET;
        int32_t accept_status = socket_accept(server->sock_fd, 1000, &client_fd); /* MAGIC */

        if(accept_status == PLCTAG_STATUS_OK) {
            /* The client thread is responsible for freeing these */
            struct client_session *session = calloc(1, sizeof(*session));
            void *context = calloc(1, server->context_size);

            /* B9: drop the connection rather than run without memory. */
            if(!session || !context) {
                log_error("Unable to allocate memory for the session!");
                free(session);
                free(context);
                socket_close(client_fd);
                continue;
            }

            /* Make a copy of the server context so the thread can use it without threading concerns. */
            memcpy(session->server_context = context, server->context, server->context_size);
            session->client_fd = client_fd; /* copy of a temporary value - no thread safety concerns */
            session->server = server;       /* reference to a long-lived struct, which has values and the original context */
            session->server_done = &done;   /* reference to a flag that any thread can raise (and all must monitor) */

            if(thread_create(&(session->thread), conn_handler, session) != PLCTAG_STATUS_OK) {
                log_error("ERROR: Unable to create connection handler thread!");
                free(context);
                free(session);
                socket_close(client_fd);
            }
        } else if(accept_status == PLCTAG_ERR_TIMEOUT) {
            log_info("Timed out waiting for new client connection.");
            continue;
        } else {
            log_error("ERROR: Received error, %d, accepting new client connection!", accept_status);
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
    uint8_t input_buf[65536 + 128];                      /* Rockwell supports up to 64k (Micro800) */
    uint8_t output_buf[65536 + 128];                     /* plus some extra for headers etc. */
    tcp_server_p server = (tcp_server_p)session->server; /* need to cast for C++ */
    byte_slice_t pending = byte_slice_make(input_buf, 0); /* unhandled input, always at the start of input_buf */
    bool need_read = true;
    plc_s *plc = (plc_s *)session->server_context;

    log_info("Got new client connection, going into processing loop.");
    log_info("server_to_client_max_packet = %zu", plc->server_to_client_max_packet);

    /* no one will join this thread, so clean ourselves up. */
    thread_detach();

    /* make sure another thread hasn't killed the server */
    while(*(session->server_done) != true) {
        byte_slice_t reply = {0};
        byte_slice_t rest = {0};

        if(need_read) {
            byte_slice_t space = byte_slice_skip(byte_slice_make(input_buf, (int32_t)sizeof(input_buf)), byte_slice_len(pending));
            byte_slice_t new_data = {0};

            if(byte_slice_len(space) == 0) {
                log_info("WARN: Request does not fit in %zu bytes!", sizeof(input_buf));
                break;
            }

            new_data = socket_read(session->client_fd, space, 1000); /* MAGIC */

            if(byte_slice_get_error(new_data) == PLCTAG_ERR_TIMEOUT) {
                log_info("Timed out waiting for client to send us a request.");
                continue;
            }

            if(byte_slice_has_error(new_data)) {
                log_info("Error, %d, reading data from the client!", byte_slice_get_error(new_data));
                break;
            }

            pending = byte_slice_make(input_buf, byte_slice_len(pending) + byte_slice_len(new_data));
        }

        log_info("Handler input:");
        log_info_byte_slice(pending);

        /* ponytail: handlers may rely on a zeroed output buffer; drop this once P2-P5 make every writer a chain. */
        memset(output_buf, 0, sizeof(output_buf));

        rest = server->handler(pending, byte_slice_make(output_buf, (int32_t)sizeof(output_buf)), &reply, session->server_context);

        if(byte_slice_has_error(rest)) {
            log_info("Closing connection: %d.", byte_slice_get_error(rest));
            break;
        }

        /* nothing used: the request is incomplete. */
        if(byte_slice_len(rest) == byte_slice_len(pending)) {
            need_read = true;
            continue;
        }

        if(byte_slice_len(reply) > 0) {
            byte_slice_t write_res = {0};

            log_info("Handler output:");
            log_info_byte_slice(reply);

            write_res = socket_write(session->client_fd, reply, 1000); /* MAGIC*/

            if(byte_slice_has_error(write_res)) {
                log_info("Error, %d, writing packet!", byte_slice_get_error(write_res));
                break;
            }
        } else {
            log_info("Deliberately not answering this request.");
        }

        /* keep pipelined bytes and try them before reading again. */
        pending = byte_slice_compress(rest);
        need_read = (byte_slice_len(pending) == 0);
    }

    socket_close(session->client_fd);

    /* see tcp_server_start() where these are malloc'ed for us */
    free(session->server_context);
    free(session);

    THREAD_RETURN(0);
}
