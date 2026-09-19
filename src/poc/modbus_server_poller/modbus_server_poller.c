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
 * A fork of src/tools/modbus_server, rewritten onto utils/poller.h and
 * utils/socket_fd.h.  The original is untouched and stays the one the test
 * suite runs; this exists to shape the poller against a real caller, which
 * is the condition docs/socket_layering_design.md sets on step 3.
 *
 * What is actually different:
 *
 *   - No coro_net.  Each connection is an explicit state machine in a
 *     switch, stepped by the event loop.  The coroutine version reads more
 *     like straight-line code, which is its selling point; the cost is that
 *     control flow lives in a macro-built switch nobody can breakpoint, and
 *     every yield point is a place the compiler cannot see a bug.
 *   - No per-socket wake channel.  The poller owns one for the thread.  The
 *     coro_net version builds its own self-connected TCP pair; the library's
 *     sock_p builds one per socket whether or not anything ever signals it.
 *   - No blocking anywhere.  socket_fd never sleeps, so the loop reaches
 *     poller_wait() with every connection's work already done.
 *
 * What is deliberately identical: the Modbus framing, the register storage,
 * the buffer and error types, the command line, and the statistics output.
 * Those are shared with the original, not copied, so that a difference in
 * behaviour between the two servers is a difference in the socket layer and
 * nothing else.
 */

#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libplctag/lib/libplctag.h>
#include <utils/poller.h>
#include <utils/socket_fd.h>

#include "args.h"
#include "buf.h"
#include "err.h"
#include "log.h"
#include "modbus_protocol.h"
#include "register_storage.h"
#include "utils.h"

#define MODBUS_RECV_BUFFER_SIZE (512)
#define MODBUS_SEND_BUFFER_SIZE (512)

#define MAX_LISTENERS (8)
#define MAX_CLIENTS (240)
#define MAX_POLLER_SOCKETS (MAX_LISTENERS + MAX_CLIENTS)
#define MAX_EVENTS_PER_WAIT (64)

#define POLL_TICK_MS (50)

#define HIST_BUCKET_COUNT (8)

static const int64_t hist_boundaries[HIST_BUCKET_COUNT] = {
    100,      /* 0-100us */
    500,      /* 100-500us */
    1000,     /* 500us-1ms */
    2000,     /* 1-2ms */
    5000,     /* 2-5ms */
    10000,    /* 5-10ms */
    50000,    /* 10-50ms */
    INT64_MAX /* >50ms */
};


typedef struct {
    int64_t total_requests;
    int64_t total_response_time_us;
    int64_t total_response_time_sq_us;
    int64_t min_response_time_us;
    int64_t max_response_time_us;

    int64_t total_recv_time_us;
    int64_t total_process_time_us;
    int64_t total_send_time_us;

    int64_t clients_connected;
    int64_t clients_disconnected;
    int64_t clients_rejected;

    int64_t hist_buckets[HIST_BUCKET_COUNT];
} server_stats_t;


/*
 * Every registered socket carries one of these as its poller context.  The
 * kind tag is what lets one event loop dispatch listeners and clients
 * without a second table: poller_wait() hands back the context, the loop
 * reads the first field, and there is no lookup at all.
 */
typedef enum { ENTRY_LISTENER, ENTRY_CLIENT } entry_kind_t;

/*
 * A connection is in exactly one of these.  The reason this is written out
 * rather than hidden in a coroutine is that a Modbus connection genuinely
 * has these states, and a stuck server is diagnosed by asking which one a
 * connection is stuck in.
 */
typedef enum {
    CLIENT_READING_REQUEST, /* accumulating bytes until a whole MBAP frame is present */
    CLIENT_SENDING_REPLY,   /* draining the response, which may take several writes */
    CLIENT_CLOSING          /* done, to be reaped at the end of this iteration */
} client_state_t;


struct server_ctx_s;

typedef struct {
    entry_kind_t kind;
    struct server_ctx_s *server;
    socket_fd_t fd;
} listener_ctx_t;

typedef struct {
    entry_kind_t kind;
    struct server_ctx_s *server;
    socket_fd_t fd;

    client_state_t state;

    uint8_t recv_storage[MODBUS_RECV_BUFFER_SIZE];
    uint8_t send_storage[MODBUS_SEND_BUFFER_SIZE];
    buf_t recv_buf;
    buf_t send_buf;

    mbap_header_t mbap_header;

    int64_t first_byte_us;
    int64_t recv_complete_us;
    int64_t process_complete_us;
} client_ctx_t;

typedef struct server_ctx_s {
    register_storage_t *storage;
    poller_p poller;

    volatile sig_atomic_t running;

    int64_t start_time_us;

    listener_ctx_t *listeners[MAX_LISTENERS];
    int32_t listener_count;

    client_ctx_t *clients[MAX_CLIENTS];
    int32_t client_count;

    server_stats_t stats;
} server_ctx_t;


static server_ctx_t *g_server = NULL;


/* ============================================================================
 * Statistics
 * ============================================================================ */

static const char *hist_bucket_label(int32_t bucket) {
    switch(bucket) {
        case 0: return "0-100us";
        case 1: return "100-500us";
        case 2: return "500us-1ms";
        case 3: return "1-2ms";
        case 4: return "2-5ms";
        case 5: return "5-10ms";
        case 6: return "10-50ms";
        case 7: return ">50ms";
        default: return "unknown";
    }
}


static void update_histogram(server_stats_t *stats, int64_t response_time_us) {
    int32_t bucket = 0;

    for(bucket = 0; bucket < HIST_BUCKET_COUNT; bucket++) {
        if(response_time_us <= hist_boundaries[bucket]) {
            stats->hist_buckets[bucket]++;
            return;
        }
    }

    stats->hist_buckets[HIST_BUCKET_COUNT - 1]++;
}


static void print_statistics(server_ctx_t *server) {
    server_stats_t *stats = &(server->stats);
    int64_t elapsed_us = util_time_us() - server->start_time_us;
    int32_t bucket = 0;

    /* stderr, matching the original, so the two servers' output can be diffed */
    fprintf(stderr, "\n=== Modbus poller server statistics ===\n");
    fprintf(stderr, "Uptime:                %" PRId64 " ms\n", elapsed_us / 1000);
    fprintf(stderr, "Clients connected:     %" PRId64 "\n", stats->clients_connected);
    fprintf(stderr, "Clients disconnected:  %" PRId64 "\n", stats->clients_disconnected);
    fprintf(stderr, "Clients rejected:      %" PRId64 "\n", stats->clients_rejected);
    fprintf(stderr, "Requests:              %" PRId64 "\n", stats->total_requests);

    if(stats->total_requests > 0) {
        int64_t mean = stats->total_response_time_us / stats->total_requests;

        fprintf(stderr, "Response time mean:    %" PRId64 " us\n", mean);
        fprintf(stderr, "Response time min:     %" PRId64 " us\n", stats->min_response_time_us);
        fprintf(stderr, "Response time max:     %" PRId64 " us\n", stats->max_response_time_us);
        fprintf(stderr, "  receive:             %" PRId64 " us\n", stats->total_recv_time_us / stats->total_requests);
        fprintf(stderr, "  process:             %" PRId64 " us\n", stats->total_process_time_us / stats->total_requests);
        fprintf(stderr, "  send:                %" PRId64 " us\n", stats->total_send_time_us / stats->total_requests);

        fprintf(stderr, "Distribution:\n");

        for(bucket = 0; bucket < HIST_BUCKET_COUNT; bucket++) {
            if(stats->hist_buckets[bucket] > 0) {
                fprintf(stderr, "  %-12s %" PRId64 "\n", hist_bucket_label(bucket), stats->hist_buckets[bucket]);
            }
        }
    }

    fflush(stderr);
}


/* ============================================================================
 * Framing
 * ============================================================================ */

/*
 * Is there a whole Modbus frame in the buffer yet?  Same rule as the
 * original's modbus_frame_check(), which is the point: the framing is not
 * what this fork is changing.
 */
static bool have_complete_frame(buf_t *buf, size_t *frame_length) {
    buf_t header_buf;
    uint16_t length = 0;
    size_t total = 0;

    if(frame_length) { *frame_length = 0; }

    if(buf_read_size(buf) < MBAP_HEADER_SIZE) { return false; }

    header_buf = *buf;

    buf_read_advance(&header_buf, 4); /* transaction ID and protocol ID */

    if(!buf_read_u16_be(&header_buf, "length", &length)) { return false; }

    /* the length field counts the unit ID, which the header size already includes */
    total = (size_t)(MBAP_HEADER_SIZE + length - 1);

    if(buf_read_size(buf) < total) { return false; }

    if(frame_length) { *frame_length = total; }

    return true;
}


/* ============================================================================
 * Clients
 * ============================================================================ */

static client_ctx_t *client_create(server_ctx_t *server, socket_fd_t fd) {
    client_ctx_t *client = (client_ctx_t *)calloc(1, sizeof(client_ctx_t));

    if(!client) { return NULL; }

    client->kind = ENTRY_CLIENT;
    client->server = server;
    client->fd = fd;
    client->state = CLIENT_READING_REQUEST;

    client->recv_buf = buf_init(client->recv_storage, MODBUS_RECV_BUFFER_SIZE);
    client->send_buf = buf_init(client->send_storage, MODBUS_SEND_BUFFER_SIZE);

    return client;
}


/*
 * Takes the connection out of the poller and off the server's list, closes
 * it and frees it.  Called only from the reap pass at the end of an
 * iteration, never from inside event dispatch -- freeing a context that the
 * event array still points at is exactly the use-after-free this structure
 * exists to make impossible.
 */
static void client_destroy(server_ctx_t *server, int32_t index) {
    client_ctx_t *client = server->clients[index];

    poller_remove(server->poller, client->fd);
    socket_fd_close(&(client->fd));

    server->client_count--;

    if(index != server->client_count) { server->clients[index] = server->clients[server->client_count]; }

    server->clients[server->client_count] = NULL;

    server->stats.clients_disconnected++;

    free(client);
}


static void client_record_timing(client_ctx_t *client, int64_t send_complete_us) {
    server_stats_t *stats = &(client->server->stats);
    int64_t total = send_complete_us - client->first_byte_us;
    int64_t recv_time = client->recv_complete_us - client->first_byte_us;
    int64_t process_time = client->process_complete_us - client->recv_complete_us;
    int64_t send_time = send_complete_us - client->process_complete_us;

    stats->total_requests++;
    stats->total_response_time_us += total;
    stats->total_response_time_sq_us += total * total;
    stats->total_recv_time_us += recv_time;
    stats->total_process_time_us += process_time;
    stats->total_send_time_us += send_time;

    if(stats->min_response_time_us == 0 || total < stats->min_response_time_us) { stats->min_response_time_us = total; }
    if(total > stats->max_response_time_us) { stats->max_response_time_us = total; }

    update_histogram(stats, total);

    pdlog(LOG_MODULE_MODBUS_POLLER_CLIENT, LOG_LEVEL_DETAIL,
          "Request completed: total=%" PRId64 "us, recv=%" PRId64 "us, process=%" PRId64 "us, send=%" PRId64 "us", total,
          recv_time, process_time, send_time);
}


/* Turns the frame sitting in recv_buf into a response in send_buf. */
static void client_build_response(client_ctx_t *client) {
    uint8_t function_code = 0;
    util_err_t err = UTIL_OK;

    modbus_parse_mbap_header(&client->recv_buf, &client->mbap_header);

    pdlog(LOG_MODULE_MODBUS_POLLER_CLIENT, LOG_LEVEL_DETAIL,
          "MBAP header - transaction %u, protocol %u, length %u, unit %u", client->mbap_header.transaction_id,
          client->mbap_header.protocol_id, client->mbap_header.length, client->mbap_header.unit_id);

    if(!buf_read_u8(&client->recv_buf, "function_code", &function_code)) {
        pdlog(LOG_MODULE_MODBUS_POLLER_CLIENT, LOG_LEVEL_WARN, "Unable to read the function code!");
        client->state = CLIENT_CLOSING;
        return;
    }

    buf_reset(&client->send_buf);

    err = modbus_process_request(function_code, &client->recv_buf, &client->send_buf, &client->mbap_header,
                                 client->server->storage);

    if(err != UTIL_OK) {
        pdlog(LOG_MODULE_MODBUS_POLLER_CLIENT, LOG_LEVEL_WARN, "Request processing failed: %s", util_err_str(err));
        modbus_build_exception_response(&client->send_buf, &client->mbap_header, function_code, err);
    }

    client->process_complete_us = util_time_us();
}


static void client_pump(client_ctx_t *client);


/*
 * One readable event.  Reads whatever is there once, then hands off to the
 * pump, which answers every frame that read completed.
 */
static void client_on_readable(client_ctx_t *client) {
    int32_t count = 0;
    int32_t rc = PLCTAG_STATUS_OK;

    buf_compact(&client->recv_buf);

    if(buf_write_size(&client->recv_buf) == 0) {
        pdlog(LOG_MODULE_MODBUS_POLLER_CLIENT, LOG_LEVEL_WARN, "Receive buffer is full; dropping the connection.");
        client->state = CLIENT_CLOSING;
        return;
    }

    rc = socket_fd_recv(client->fd, buf_write_ptr(&client->recv_buf), (int32_t)buf_write_size(&client->recv_buf), &count);

    if(rc == PLCTAG_STATUS_PENDING) { return; }

    if(rc != PLCTAG_STATUS_OK) {
        pdlog(LOG_MODULE_MODBUS_POLLER_CLIENT, LOG_LEVEL_WARN, "Read error; dropping the connection.");
        client->state = CLIENT_CLOSING;
        return;
    }

    if(count == 0) {
        /* orderly close by the peer */
        pdlog(LOG_MODULE_MODBUS_POLLER_CLIENT, LOG_LEVEL_DETAIL, "Client closed the connection.");
        client->state = CLIENT_CLOSING;
        return;
    }

    if(client->first_byte_us == 0) { client->first_byte_us = util_time_us(); }

    buf_write_advance(&client->recv_buf, (size_t)count);

    client_pump(client);
}


/*
 * Drains send_buf.  Returns having either finished the reply and gone back
 * to reading, or left the remainder for the next writable event.
 */
static void client_on_writable(client_ctx_t *client) {
    int32_t count = 0;
    int32_t rc = PLCTAG_STATUS_OK;
    size_t to_send = buf_read_size(&client->send_buf);

    if(to_send == 0) {
        client->state = CLIENT_READING_REQUEST;
        return;
    }

    rc = socket_fd_send(client->fd, buf_read_ptr(&client->send_buf), (int32_t)to_send, &count);

    if(rc == PLCTAG_STATUS_PENDING) { return; }

    if(rc != PLCTAG_STATUS_OK) {
        pdlog(LOG_MODULE_MODBUS_POLLER_CLIENT, LOG_LEVEL_WARN, "Write error; dropping the connection.");
        client->state = CLIENT_CLOSING;
        return;
    }

    buf_read_advance(&client->send_buf, (size_t)count);

    if(buf_read_size(&client->send_buf) > 0) {
        /* partial write; stay in this state and wait for room */
        return;
    }

    client_record_timing(client, util_time_us());

    client->first_byte_us = 0;
    client->recv_complete_us = 0;
    client->process_complete_us = 0;

    client->state = CLIENT_READING_REQUEST;
}


/*
 * Answers every frame that is already in the receive buffer, sending each
 * reply as it goes, and stops on the first thing that needs the kernel: no
 * complete frame left, a send that would block, or a connection to close.
 *
 * This loop is why a pipelined client works.  A client that puts two
 * requests in one segment leaves the second one sitting in recv_buf after
 * the first is answered -- and the socket then has no unread data, so poll()
 * never reports it readable again and that connection stalls until the
 * client happens to send something else.  Readiness is a fact about the
 * kernel's buffer, not about ours, so anything already buffered has to be
 * drained here rather than waited for.
 */
static void client_pump(client_ctx_t *client) {
    while(client->state != CLIENT_CLOSING) {
        size_t frame_length = 0;
        size_t frame_start = 0;
        size_t consumed = 0;

        if(client->state == CLIENT_SENDING_REPLY) {
            client_on_writable(client);

            /* still sending means the socket is full; wait to be told there is room */
            if(client->state == CLIENT_SENDING_REPLY) { return; }

            continue;
        }

        if(!have_complete_frame(&client->recv_buf, &frame_length)) { return; }

        client->recv_complete_us = util_time_us();

        pdlog(LOG_MODULE_MODBUS_POLLER_CLIENT, LOG_LEVEL_DETAIL, "Complete request of %" PRIu64 " bytes.",
              (uint64_t)frame_length);

        frame_start = buf_read_pos(&client->recv_buf);

        client_build_response(client);

        if(client->state == CLIENT_CLOSING) { return; }

        /*
         * Consume exactly one frame regardless of how much of it the handler
         * chose to read.  Without this, a handler that stops short leaves the
         * tail of its own request at the head of the buffer, where it is read
         * as the start of the next one -- which only shows up once more than
         * one frame is in flight, and then looks like corruption rather than
         * like an off-by-some.
         */
        consumed = buf_read_pos(&client->recv_buf) - frame_start;

        if(consumed < frame_length) {
            buf_read_advance(&client->recv_buf, frame_length - consumed);
        } else if(consumed > frame_length) {
            pdlog(LOG_MODULE_MODBUS_POLLER_CLIENT, LOG_LEVEL_WARN,
                  "Handler read %" PRIu64 " bytes of a %" PRIu64 " byte frame; dropping the connection.",
                  (uint64_t)consumed, (uint64_t)frame_length);
            client->state = CLIENT_CLOSING;
            return;
        }

        client->state = CLIENT_SENDING_REPLY;

        /*
         * Fall through to the send rather than waiting to be told the socket
         * is writable.  A socket with room in its send buffer -- the normal
         * case for a small Modbus reply -- finishes on the next pass of this
         * loop, and the request costs one readiness event instead of two.
         */
    }
}


/*
 * The state machine proper.  Each call does as much as it can without
 * blocking and then says what it wants to hear about next.
 */
static void client_step(client_ctx_t *client, int32_t events) {
    if(events & (POLLER_EVENT_ERROR | POLLER_EVENT_DISCONNECT)) {
        /*
         * Drain anything still buffered on a half-close before giving up;
         * a client that sends a request and immediately shuts down its
         * write side is well behaved, and dropping it here would lose the
         * last request.
         */
        if((events & POLLER_EVENT_CAN_READ) == 0) {
            client->state = CLIENT_CLOSING;
            return;
        }
    }

    switch(client->state) {
        case CLIENT_READING_REQUEST:
            /* client_on_readable() pumps, so a completed frame is answered here too */
            if(events & POLLER_EVENT_CAN_READ) { client_on_readable(client); }
            break;

        case CLIENT_SENDING_REPLY:
            /*
             * Pump rather than just write: draining this reply may uncover
             * another frame that was already buffered behind it.
             */
            if(events & POLLER_EVENT_CAN_WRITE) { client_pump(client); }
            break;

        case CLIENT_CLOSING: break;

        default: client->state = CLIENT_CLOSING; break;
    }
}


/* what this connection wants the poller to watch for next */
static int32_t client_interest(client_ctx_t *client) {
    switch(client->state) {
        case CLIENT_READING_REQUEST: return POLLER_EVENT_CAN_READ;
        case CLIENT_SENDING_REPLY: return POLLER_EVENT_CAN_WRITE;
        default: return POLLER_EVENT_NONE;
    }
}


/* ============================================================================
 * Listeners
 * ============================================================================ */

static void listener_on_readable(listener_ctx_t *listener) {
    server_ctx_t *server = listener->server;

    /*
     * Accept until the queue is empty rather than one per event.  With many
     * clients arriving at once, one-per-event means one poll syscall per
     * connection, and the backlog drains at the rate of the event loop
     * instead of the rate of accept().
     */
    while(true) {
        socket_fd_t client_fd = SOCKET_FD_INVALID;
        socket_fd_addr_t client_addr;
        client_ctx_t *client = NULL;
        char addr_str[64] = {0};
        int32_t rc = PLCTAG_STATUS_OK;

        rc = socket_fd_accept(listener->fd, &client_fd, &client_addr);

        if(rc == PLCTAG_STATUS_PENDING) { return; }

        if(rc != PLCTAG_STATUS_OK) {
            pdlog(LOG_MODULE_MODBUS_POLLER_LISTENER, LOG_LEVEL_WARN, "Unable to accept a connection.");
            return;
        }

        if(server->client_count >= MAX_CLIENTS) {
            pdlog(LOG_MODULE_MODBUS_POLLER_LISTENER, LOG_LEVEL_WARN, "At the %d client limit; refusing the connection.",
                  MAX_CLIENTS);
            socket_fd_close(&client_fd);
            server->stats.clients_rejected++;
            continue;
        }

        client = client_create(server, client_fd);
        if(!client) {
            pdlog(LOG_MODULE_MODBUS_POLLER_LISTENER, LOG_LEVEL_ERROR, "Unable to allocate a client!");
            socket_fd_close(&client_fd);
            server->stats.clients_rejected++;
            continue;
        }

        if(poller_add(server->poller, client_fd, POLLER_EVENT_CAN_READ, client) != PLCTAG_STATUS_OK) {
            pdlog(LOG_MODULE_MODBUS_POLLER_LISTENER, LOG_LEVEL_ERROR, "Unable to register the client!");
            socket_fd_close(&client_fd);
            free(client);
            server->stats.clients_rejected++;
            continue;
        }

        server->clients[server->client_count] = client;
        server->client_count++;
        server->stats.clients_connected++;

        socket_fd_addr_str(&client_addr, addr_str, (int32_t)sizeof(addr_str));

        pdlog(LOG_MODULE_MODBUS_POLLER_LISTENER, LOG_LEVEL_DETAIL, "Accepted a connection from %s:%" PRId32 ".", addr_str,
              socket_fd_addr_port(&client_addr));
    }
}


/* ============================================================================
 * Shutdown
 * ============================================================================ */

static void signal_handler(void) {
    if(g_server) {
        g_server->running = 0;

        /*
         * The loop may be parked in poller_wait() with a long timeout.  This
         * is the one call in the whole program that is legal from another
         * context, and it is why the poller owns a wake channel at all.
         */
        poller_wake(g_server->poller);
    }
}


/* ============================================================================
 * Event loop
 * ============================================================================ */

static void server_run(server_ctx_t *server) {
    poller_event_t events[MAX_EVENTS_PER_WAIT];

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Starting the poller event loop.");

    while(server->running) {
        int32_t event_count = 0;
        int32_t index = 0;
        int32_t rc = PLCTAG_STATUS_OK;

        rc = poller_wait(server->poller, events, MAX_EVENTS_PER_WAIT, POLL_TICK_MS, &event_count);
        if(rc != PLCTAG_STATUS_OK) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Poller failed; shutting down.");
            break;
        }

        for(index = 0; index < event_count; index++) {
            entry_kind_t *kind = (entry_kind_t *)events[index].context;

            /* the wake event has no context; it exists only to end the wait */
            if(!kind) { continue; }

            if(*kind == ENTRY_LISTENER) {
                if(events[index].events & POLLER_EVENT_CAN_READ) { listener_on_readable((listener_ctx_t *)kind); }
            } else {
                client_step((client_ctx_t *)kind, events[index].events);
            }
        }

        /*
         * Re-arm and reap in one pass, backwards so that the swap-with-last
         * removal in client_destroy() cannot skip an entry.
         */
        for(index = server->client_count - 1; index >= 0; index--) {
            client_ctx_t *client = server->clients[index];

            if(client->state == CLIENT_CLOSING) {
                client_destroy(server, index);
                continue;
            }

            poller_modify(server->poller, client->fd, client_interest(client));
        }
    }

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Event loop finished.");
}


static void server_cleanup(server_ctx_t *server) {
    int32_t index = 0;

    for(index = server->client_count - 1; index >= 0; index--) { client_destroy(server, index); }

    for(index = 0; index < server->listener_count; index++) {
        listener_ctx_t *listener = server->listeners[index];

        poller_remove(server->poller, listener->fd);
        socket_fd_close(&(listener->fd));

        free(listener);

        server->listeners[index] = NULL;
    }

    server->listener_count = 0;

    poller_destroy(&(server->poller));
}


static bool add_listener(server_ctx_t *server, const char *host, int32_t port) {
    listener_ctx_t *listener = NULL;
    socket_fd_t fd = SOCKET_FD_INVALID;

    if(server->listener_count >= MAX_LISTENERS) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "At the %d listener limit.", MAX_LISTENERS);
        return false;
    }

    if(socket_fd_open_tcp(&fd) != PLCTAG_STATUS_OK) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Unable to create a listening socket.");
        return false;
    }

    if(socket_fd_bind_listen(fd, host, port, 128) != PLCTAG_STATUS_OK) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Unable to listen on %s:%" PRId32 ".", host, port);
        socket_fd_close(&fd);
        return false;
    }

    listener = (listener_ctx_t *)calloc(1, sizeof(listener_ctx_t));
    if(!listener) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Unable to allocate a listener!");
        socket_fd_close(&fd);
        return false;
    }

    listener->kind = ENTRY_LISTENER;
    listener->server = server;
    listener->fd = fd;

    if(poller_add(server->poller, fd, POLLER_EVENT_CAN_READ, listener) != PLCTAG_STATUS_OK) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Unable to register the listener!");
        socket_fd_close(&fd);
        free(listener);
        return false;
    }

    server->listeners[server->listener_count] = listener;
    server->listener_count++;

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Listening on %s:%" PRId32 ".", host, port);

    return true;
}


/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    server_ctx_t server;
    args_result_t args_result = {0};
    args_flag_def_t flags[] = {
        {"listen",
         ARGS_TYPE_STRING,
         ARGS_OPTIONAL,
         ARGS_MULTIPLE,
         "server.listen",
         "Address and port to listen on (address:port)",
         {.has_default = false}},
        {"debug",
         ARGS_TYPE_STRING,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "logging.debug",
         "Debug level (ERROR, WARN, INFO, DETAIL, SPEW)",
         {.has_default = true, .value.string_val = "INFO"}},
        {"coils", ARGS_TYPE_INT, ARGS_OPTIONAL, ARGS_ONCE, "modbus.coils", "Number of coils",
         {.has_default = true, .value.int_val = 1000}},
        {"discrete-inputs", ARGS_TYPE_INT, ARGS_OPTIONAL, ARGS_ONCE, "modbus.discrete_inputs", "Number of discrete inputs",
         {.has_default = true, .value.int_val = 1000}},
        {"holding-registers", ARGS_TYPE_INT, ARGS_OPTIONAL, ARGS_ONCE, "modbus.holding_registers", "Number of holding registers",
         {.has_default = true, .value.int_val = 1000}},
        {"input-registers", ARGS_TYPE_INT, ARGS_OPTIONAL, ARGS_ONCE, "modbus.input_registers", "Number of input registers",
         {.has_default = true, .value.int_val = 1000}},
        {"help", ARGS_TYPE_BOOL, ARGS_OPTIONAL, ARGS_ONCE, "general.help", "Show this help message",
         {.has_default = true, .value.bool_val = false}},
    };
    const size_t num_flags = sizeof(flags) / sizeof(flags[0]);
    const char *debug_level_str = NULL;
    log_level_t log_level = LOG_LEVEL_INFO;
    int64_t coils_val = 0;
    int64_t di_val = 0;
    int64_t hr_val = 0;
    int64_t ir_val = 0;
    size_t listen_count = 0;
    size_t listen_index = 0;

    memset(&server, 0, sizeof(server));
    server.running = 1;

    g_server = &server;

    if(socket_fd_lib_startup() != PLCTAG_STATUS_OK) {
        fprintf(stderr, "Unable to start the socket library!\n");
        return EXIT_FAILURE;
    }

    if(args_parse(argc, (const char **)argv, flags, num_flags, &args_result) != UTIL_OK) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Unable to parse arguments: %s",
              args_get_error_detail(&args_result));
        args_print_help(argv[0], flags, num_flags);
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    if(args_get_bool(&args_result, "help")) {
        args_print_help(argv[0], flags, num_flags);
        args_free(&args_result);
        return EXIT_SUCCESS;
    }

    debug_level_str = args_get_string(&args_result, "debug");
    if(debug_level_str) {
        if(strcmp(debug_level_str, "ERROR") == 0) {
            log_level = LOG_LEVEL_ERROR;
        } else if(strcmp(debug_level_str, "WARN") == 0) {
            log_level = LOG_LEVEL_WARN;
        } else if(strcmp(debug_level_str, "INFO") == 0) {
            log_level = LOG_LEVEL_INFO;
        } else if(strcmp(debug_level_str, "DETAIL") == 0) {
            log_level = LOG_LEVEL_DETAIL;
        } else if(strcmp(debug_level_str, "SPEW") == 0) {
            log_level = LOG_LEVEL_SPEW;
        }
    }

    log_set_all_modules(log_level);

    server.start_time_us = util_time_us();

    coils_val = args_get_int(&args_result, "coils");
    di_val = args_get_int(&args_result, "discrete-inputs");
    hr_val = args_get_int(&args_result, "holding-registers");
    ir_val = args_get_int(&args_result, "input-registers");

    if(coils_val < 0 || coils_val > 65535 || di_val < 0 || di_val > 65535 || hr_val < 0 || hr_val > 65535 || ir_val < 0
       || ir_val > 65535) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Register counts must be 0-65535.");
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    server.storage = register_storage_create((size_t)coils_val, (size_t)di_val, (size_t)hr_val, (size_t)ir_val);
    if(!server.storage) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Unable to create the register storage.");
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Modbus poller server starting.");

    if(poller_create(&(server.poller), MAX_POLLER_SOCKETS) != PLCTAG_STATUS_OK) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Unable to create the poller.");
        register_storage_destroy(server.storage);
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    util_set_interrupt_handler(signal_handler);

    listen_count = args_get_count(&args_result, "listen");
    if(listen_count == 0) { listen_count = 1; }

    for(listen_index = 0; listen_index < listen_count; listen_index++) {
        const char *listen_addr = NULL;
        args_value_t val = args_get_at(&args_result, "listen", listen_index);
        char addr_copy[256] = {0};
        char *colon = NULL;

        if(val.present) { listen_addr = val.value.string_val; }

        if(!listen_addr) { listen_addr = "127.0.0.1:502"; }

        strncpy(addr_copy, listen_addr, sizeof(addr_copy) - 1);

        colon = strchr(addr_copy, ':');
        if(!colon) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Invalid address format: %s", listen_addr);
            server_cleanup(&server);
            register_storage_destroy(server.storage);
            args_free(&args_result);
            return EXIT_FAILURE;
        }

        *colon = '\0';

        if(!add_listener(&server, addr_copy, (int32_t)atoi(colon + 1))) {
            server_cleanup(&server);
            register_storage_destroy(server.storage);
            args_free(&args_result);
            return EXIT_FAILURE;
        }
    }

    server_run(&server);

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Modbus poller server shutting down.");

    server_cleanup(&server);

    print_statistics(&server);

    register_storage_destroy(server.storage);
    args_free(&args_result);

    socket_fd_lib_shutdown();

    return EXIT_SUCCESS;
}
