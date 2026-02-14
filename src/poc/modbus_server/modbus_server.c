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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <inttypes.h>
#include <math.h>

#ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
#else
#    include <sys/socket.h>
#    include <netinet/in.h>
#    include <arpa/inet.h>
#endif

#include "modbus_protocol.h"
#include "register_storage.h"
#include "coro_net.h"
#include "socket.h"
#include "log.h"
#include "err.h"
#include "buf.h"
#include "args.h"
#include "utils.h"

/* ============================================================================
 * Forward Declarations and Constants
 * ============================================================================ */

typedef struct server_ctx_s server_ctx_t;
typedef struct modbus_client_s modbus_client_t;

#define MODBUS_RECV_BUFFER_SIZE 512
#define MODBUS_SEND_BUFFER_SIZE 512

/* Histogram bucket boundaries (in microseconds) */
#define HIST_BUCKET_COUNT 8
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

/* Server statistics structure */
typedef struct {
    /* Total request count and timing */
    int64_t total_requests;
    int64_t total_response_time_us;
    int64_t total_response_time_sq_us; /* Sum of squares for std dev */
    int64_t min_response_time_us;
    int64_t max_response_time_us;

    /* Per-component timing breakdown */
    int64_t total_recv_time_us;      /* Total time from recv_start to recv_complete (includes polling) */
    int64_t total_poll_overhead_us;  /* Time spent waiting for first byte (polling overhead) */
    int64_t total_actual_io_time_us; /* Time for actual socket I/O after first byte arrives */
    int64_t total_process_time_us;
    int64_t total_send_time_us;
    int64_t total_overhead_time_us;

    int64_t clients_connected;
    int64_t clients_disconnected;

    /* Histogram buckets for response time distribution */
    int64_t hist_buckets[HIST_BUCKET_COUNT];
} server_stats_t;

/* Per-request timing breakdown */
typedef struct {
    int64_t request_start_us;      /* When we start attempting to receive (includes polling) */
    int64_t first_byte_us;         /* When first byte actually received (key timing point) */
    int64_t recv_start_us;         /* When recv started */
    int64_t recv_complete_us;      /* When full request received */
    int64_t process_start_us;      /* When processing started */
    int64_t process_complete_us;   /* When processing completed */
    int64_t send_start_us;         /* When send started */
    int64_t send_complete_us;      /* When send completed */
    int64_t total_recv_time_us;    /* Accumulated times for multi-recv scenarios */
    int64_t total_process_time_us; /* Accumulated times for multi-process scenarios */
    int64_t total_send_time_us;    /* Accumulated times for multi-send scenarios */
} request_timing_t;

static server_ctx_t *g_server = NULL;

/* ============================================================================
 * Server Context
 * ============================================================================ */

struct server_ctx_s {
    register_storage_t *storage;
    coro_net_t *coro_net;
    volatile int running;
    int64_t start_time_us;
    server_stats_t stats;
};

/* ============================================================================
 * Modbus Client Context (per connection)
 * ============================================================================ */

struct modbus_client_s {
    coro_task_handle_t handle;
    server_ctx_t *server;
    uint8_t recv_buffer[MODBUS_RECV_BUFFER_SIZE];
    uint8_t send_buffer[MODBUS_SEND_BUFFER_SIZE];
    buf_t recv_buf;
    buf_t send_buf;
    mbap_header_t mbap_header;
    uint16_t expected_pdu_length;
    request_timing_t timing;
};


/* ============================================================================
 * Modbus Listener Context (per endpoint)
 * ============================================================================ */

struct listener_info_s {
    coro_task_handle_t handle;
    server_ctx_t *server;
    char bind_address[256];
    uint16_t bind_port;
    size_t clients_created;
    size_t clients_closed;
};

typedef struct listener_info_s listener_info_t;

/* ============================================================================
 * Statistics Helpers
 * ============================================================================ */

static const char *hist_bucket_label(int bucket) {
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
    for(int i = 0; i < HIST_BUCKET_COUNT; i++) {
        if(response_time_us <= hist_boundaries[i]) {
            stats->hist_buckets[i]++;
            break;
        }
    }
}


static void print_statistics(server_ctx_t *server) {
    if(!server) { return; }

    int64_t end_time_us = util_time_us();
    double runtime_sec = (double)(end_time_us - server->start_time_us) / 1000000.0;

    server_stats_t *stats = &server->stats;
    int64_t total_reqs = stats->total_requests;
    int64_t total_time = stats->total_response_time_us;
    int64_t total_time_sq = stats->total_response_time_sq_us;
    int64_t min_time = stats->min_response_time_us;
    int64_t max_time = stats->max_response_time_us;

    int64_t total_recv = stats->total_recv_time_us;
    int64_t total_poll_overhead = stats->total_poll_overhead_us;
    int64_t total_actual_io = stats->total_actual_io_time_us;
    int64_t total_process = stats->total_process_time_us;
    int64_t total_send = stats->total_send_time_us;
    int64_t total_overhead = stats->total_overhead_time_us;

    /* flush the rest of the log out */
    fflush(stderr);

    fprintf(stderr, "\n");
    fprintf(stderr, "╔══════════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║        MODBUS SERVER (COROUTINE) PERFORMANCE STATISTICS          ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║ Runtime: %.2f seconds                                            \n", runtime_sec);
    fprintf(stderr, "║ Total requests: %" PRId64 "                                              \n", (long long)total_reqs);
    if(runtime_sec > 0) {
        fprintf(stderr, "║ Throughput: %.2f requests/sec                                    \n",
                (double)total_reqs / (double)runtime_sec);
    }
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║                       CLIENT STATISTICS                          ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║ Clients connected:    %" PRId64 "                                     \n",
            (long long)stats->clients_connected);
    fprintf(stderr, "║ Clients disconnected: %" PRId64 "                                     \n",
            (long long)stats->clients_disconnected);
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║                     RESPONSE TIME SUMMARY                        ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");

    if(total_reqs > 0) {
        double mean = (double)total_time / (double)total_reqs;
        double variance = ((double)total_time_sq / (double)total_reqs) - (mean * mean);
        double stddev = variance > 0 ? sqrt(variance) : 0.0;

        fprintf(stderr, "║ Average:  %8.2f us                                            \n", mean);
        fprintf(stderr, "║ Std Dev:  %8.2f us                                            \n", stddev);
        fprintf(stderr, "║ Minimum:  %8lld us                                            \n", (long long)min_time);
        fprintf(stderr, "║ Maximum:  %8lld us                                            \n", (long long)max_time);

        fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║                    LATENCY BREAKDOWN (avg)                       ║\n");
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");

        double avg_recv = (double)total_recv / (double)total_reqs;
        double avg_process = (double)total_process / (double)total_reqs;
        double avg_send = (double)total_send / (double)total_reqs;
        double avg_overhead = (double)total_overhead / (double)total_reqs;
        double total_avg = avg_recv + avg_process + avg_send + avg_overhead;

        double avg_poll_overhead = (double)total_poll_overhead / (double)total_reqs;
        double avg_actual_io = (double)total_actual_io / (double)total_reqs;

        fprintf(stderr, "║  Recv (socket):   %8.2f us (%5.1f%%)                           \n", avg_recv,
                total_avg > 0 ? (avg_recv / total_avg) * 100 : 0);
        fprintf(stderr, "║    - Poll overhead: %6.2f us (%5.1f%% of recv)                  \n", avg_poll_overhead,
                avg_recv > 0 ? (avg_poll_overhead / avg_recv) * 100 : 0);
        fprintf(stderr, "║    - Actual I/O:    %6.2f us (%5.1f%% of recv)                  \n", avg_actual_io,
                avg_recv > 0 ? (avg_actual_io / avg_recv) * 100 : 0);
        fprintf(stderr, "║  Process (modbus):%8.2f us (%5.1f%%)                           \n", avg_process,
                total_avg > 0 ? (avg_process / total_avg) * 100 : 0);
        fprintf(stderr, "║  Send (socket):   %8.2f us (%5.1f%%)                           \n", avg_send,
                total_avg > 0 ? (avg_send / total_avg) * 100 : 0);
        fprintf(stderr, "║  Overhead (coro): %8.2f us (%5.1f%%)                           \n", avg_overhead,
                total_avg > 0 ? (avg_overhead / total_avg) * 100 : 0);
    }

    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║                  RESPONSE TIME HISTOGRAM                         ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");

    int64_t max_bucket = 0;
    for(int i = 0; i < HIST_BUCKET_COUNT; i++) {
        int64_t count = stats->hist_buckets[i];
        if(count > max_bucket) { max_bucket = count; }
    }

    for(int i = 0; i < HIST_BUCKET_COUNT; i++) {
        int64_t count = stats->hist_buckets[i];
        double pct = total_reqs > 0 ? (double)count / (double)total_reqs * 100 : 0;
        int bar_len = max_bucket > 0 ? (int)((double)count / (double)max_bucket * 30) : 0;

        fprintf(stderr, "║  %-12s │", hist_bucket_label(i));
        for(int j = 0; j < bar_len; j++) { fprintf(stderr, "█"); }
        for(int j = bar_len; j < 30; j++) { fprintf(stderr, " "); }
        fprintf(stderr, "│ %6lld (%5.1f%%)\n", (long long)count, pct);
    }

    fprintf(stderr, "╚══════════════════════════════════════════════════════════════════╝\n");
    fflush(stderr);
}

/* ============================================================================
 * Signal Handling
 * ============================================================================ */

static void signal_handler(void) {
    if(g_server) {
        g_server->running = 0;
        coro_stop(g_server->coro_net);
    }
}

/* ============================================================================
 * Client Coroutine Handler
 * ============================================================================ */

static util_err_t modbus_frame_check(buf_t *buf, void *context) {
    (void)context; /* unused */

    pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL, "Checking for complete Modbus frame");
    /* Check if we have enough data for the MBAP header */
    if(buf_read_size(buf) < MBAP_HEADER_SIZE) {
        pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL, "Not enough data for MBAP header");
        return UTIL_EAGAIN;
    }

    /* make local copy of buffer */
    buf_t header_buf = *buf;

    /* Peek at the length field in the MBAP header */
    uint16_t length = 0;
    buf_read_advance(&header_buf, 4); /* Skip Transaction ID and Protocol ID */

    if(!buf_read_u16_be(&header_buf, "length", &length)) {
        pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL, "Failed to read length field from MBAP header");
        return buf_get_error(&header_buf);
    }

    /* Total required size is MBAP header + length field - 1 for the unit byte */
    if(buf_read_size(buf) < (MBAP_HEADER_SIZE + length - 1)) {
        pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL, "Not enough data for complete Modbus frame");
        return UTIL_EAGAIN;
    }

    return UTIL_OK;
}


static void client_handler(coro_task_handle_t handle, socket_t fd, void *context) {
    modbus_client_t *client = (modbus_client_t *)context;
    uint8_t function_code;
    util_err_t err;

    (void)fd; /* We have the fd in the handle */

    CORO_START(handle);

    pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL, "Client handler started");

    client->server->stats.clients_connected++;

    while(1) {
        /* compact the receive buffer */
        buf_compact(&client->recv_buf);

        pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL, "%zu bytes space in receive buffer before read",
              buf_write_size(&client->recv_buf));

        /* Reset timestamps for new request */
        client->timing.first_byte_us = 0;
        client->timing.recv_complete_us = 0;

        /* Capture when we start trying to receive (may block waiting for data) */
        client->timing.recv_start_us = util_time_us();

        /* Read until we get a full frame */
        while((err = socket_recv_frame(fd, &client->recv_buf, modbus_frame_check, NULL)) == UTIL_EAGAIN) {
            coro_yield(handle, CORO_EVENT_READ);
        }

        /* Capture timestamps after we exit the loop (data arrived or error occurred) */
        client->timing.recv_complete_us = util_time_us();
        /* For simplicity, use same timestamp for first byte and complete in the refactored version
         * This is acceptable since socket_recv_frame() typically completes in one call for small Modbus frames */
        client->timing.first_byte_us = client->timing.recv_complete_us;

        /* Request processing start is when data actually arrived (not when we started blocking) */
        client->timing.request_start_us = client->timing.first_byte_us;

        if(err != UTIL_OK) {
            pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_WARN, "Client error %s during APU read", util_err_str(err));
            break;
        }

        /* we got at least enough data for a full packet */
        pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL,
              "Received complete Modbus request of %zu bytes:", buf_read_size(&client->recv_buf));
        pdlog_bytes(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL, &client->recv_buf);

        /* get the header info */
        modbus_parse_mbap_header(&client->recv_buf, &client->mbap_header);

        pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL,
              "MBAP Header - Transaction ID: %u, Protocol ID: %u, Length: %u, Unit ID: %u", client->mbap_header.transaction_id,
              client->mbap_header.protocol_id, client->mbap_header.length, client->mbap_header.unit_id);

        /* FIXME - we should check the unit ID here. */

        /* Extract function code */
        if(!buf_read_u8(&client->recv_buf, "function_code", &function_code)) {
            pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_WARN, "Failed to read function code");
            break;
        }

        pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL, "Processing function code 0x%02x", function_code);
        /* Capture process start time */
        client->timing.process_start_us = util_time_us();

        /* Reset send buffer and generate response */
        buf_reset(&client->send_buf);
        err = modbus_process_request(function_code, &client->recv_buf, &client->send_buf, &client->mbap_header,
                                     client->server->storage);

        /* Capture process complete time */
        client->timing.process_complete_us = util_time_us();

        if(err != UTIL_OK) {
            pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_WARN, "Request processing failed");

            modbus_build_exception_response(&client->send_buf, &client->mbap_header, function_code, err);
        } else {
            pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL, "Request processed successfully");
        }

        pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL,
              "Prepared response of %zu bytes:", buf_read_size(&client->send_buf));
        pdlog_bytes(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL, &client->send_buf);

        /* Capture send start time */
        client->timing.send_start_us = util_time_us();

        /* Send response */
        pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL, "Sending response of %zu bytes", buf_write_pos(&client->send_buf));
        while((err = socket_send_buf(fd, &client->send_buf)) == UTIL_EAGAIN) { coro_yield(handle, CORO_EVENT_WRITE); }
        if(err != UTIL_OK) {
            pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_WARN, "Failed to send response: %s", util_err_str(err));
            break;
        }

        /* Capture send complete time and calculate statistics */
        client->timing.send_complete_us = util_time_us();

        /* Calculate response time and component times
         * Note: total_response_time excludes poll_overhead (time blocked waiting for data)
         * It measures pure processing time from when data arrives to when response is sent
         */
        int64_t total_response_time = client->timing.send_complete_us - client->timing.request_start_us;
        int64_t poll_overhead = client->timing.first_byte_us - client->timing.recv_start_us;
        int64_t actual_io_time = client->timing.recv_complete_us - client->timing.first_byte_us;
        int64_t recv_time = poll_overhead + actual_io_time; /* Total receive time including blocking */
        int64_t process_time = client->timing.process_complete_us - client->timing.process_start_us;
        int64_t send_time = client->timing.send_complete_us - client->timing.send_start_us;
        int64_t overhead_time = total_response_time - actual_io_time - process_time - send_time;

        /* Update statistics */
        server_stats_t *stats = &client->server->stats;
        stats->total_requests++;
        stats->total_response_time_us += total_response_time;
        stats->total_response_time_sq_us += total_response_time * total_response_time;
        stats->total_recv_time_us += recv_time;
        stats->total_poll_overhead_us += poll_overhead;
        stats->total_actual_io_time_us += actual_io_time;
        stats->total_process_time_us += process_time;
        stats->total_send_time_us += send_time;
        stats->total_overhead_time_us += overhead_time;

        if(stats->min_response_time_us == 0 || total_response_time < stats->min_response_time_us) {
            stats->min_response_time_us = total_response_time;
        }
        if(total_response_time > stats->max_response_time_us) { stats->max_response_time_us = total_response_time; }

        update_histogram(stats, total_response_time);

        pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL,
              "Request completed: total=%" PRId64 "us, recv=%" PRId64 "us, process=%" PRId64 "us, send=%" PRId64
              "us, overhead=%" PRId64 "us",
              total_response_time, recv_time, process_time, send_time, overhead_time);

        memset(&client->timing, 0, sizeof(client->timing));
    }

    pdlog(LOG_MODULE_MODBUS_CORO_CLIENT, LOG_LEVEL_DETAIL, "Client handler closing");

    client->server->stats.clients_disconnected++;

    coro_remove_task(client->handle);
    socket_close(coro_get_fd(client->handle));
    free(client);

    CORO_END(handle);
}

/* ============================================================================
 * Listener Coroutine Handler
 * ============================================================================ */

static void listener_handler(coro_task_handle_t handle, socket_t fd, void *context) {
    listener_info_t *listener = (listener_info_t *)context;
    socket_t client_fd = 0;
    socket_address_t client_addr = {0};
    util_err_t err = UTIL_OK;

    (void)fd; /* We have the fd in the handle */

    CORO_START(handle);

    pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_INFO, "Listener started on %s:%u", listener->bind_address,
          listener->bind_port);

    while(listener->server->running) {
        pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_DETAIL, "Waiting for incoming connections...");
        while((err = socket_accept(fd, &client_fd, &client_addr)) == UTIL_EAGAIN) { coro_yield(handle, CORO_EVENT_READ); }
        if(err != UTIL_OK) {
            pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_ERROR, "Failed to accept connection");
            break;
        }

        char client_ip[INET6_ADDRSTRLEN];
        socket_address_get_addr_str(&client_addr, client_ip, sizeof(client_ip));
        uint16_t client_port = socket_address_get_port(&client_addr);
        pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_DETAIL, "Accepted client connection from %s:%u (fd %d)", client_ip,
              client_port, client_fd);

        /* Create client context */
        modbus_client_t *client = (modbus_client_t *)calloc(1, sizeof(modbus_client_t));
        if(!client) {
            pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_ERROR, "Failed to allocate client context");
            CS_CLOSE(client_fd);
            continue;
        }

        client->recv_buf = buf_init(client->recv_buffer, MODBUS_RECV_BUFFER_SIZE);
        client->send_buf = buf_init(client->send_buffer, MODBUS_SEND_BUFFER_SIZE);
        client->server = listener->server;
        client->expected_pdu_length = 0;

        /* Register with event loop */
        coro_task_handle_t client_handle;
        err = coro_add_task(&client_handle, client->server->coro_net, client_fd, client_handler, (void *)client);
        if(err != UTIL_OK) {
            pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_ERROR, "Failed to add client socket: %s", util_err_str(err));
            CS_CLOSE(client_fd);
            free(client);
            continue;
        }

        client->handle = client_handle;
    }

    pdlog(LOG_MODULE_MODBUS_CORO_LISTENER, LOG_LEVEL_INFO, "Listener stopping");

    coro_remove_task(listener->handle);
    socket_close(coro_get_fd(listener->handle));
    free(listener);

    CORO_END(handle);
}


/* ============================================================================
 * Main Entry Point
 * ============================================================================ */

int main(int argc, char *argv[]) {
    args_result_t args_result = {0};
    server_ctx_t server = {0};
    g_server = &server;
    server.running = 1;

    /* Initialize socket subsystem */
    util_err_t socket_err = socket_init();
    if(socket_err != UTIL_OK) {
        fprintf(stderr, "Failed to initialize socket subsystem: %s\n", util_err_str(socket_err));
        return EXIT_FAILURE;
    }

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
        {"coils",
         ARGS_TYPE_INT,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "modbus.coils",
         "Number of coils",
         {.has_default = true, .value.int_val = 1000}},
        {"discrete-inputs",
         ARGS_TYPE_INT,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "modbus.discrete_inputs",
         "Number of discrete inputs",
         {.has_default = true, .value.int_val = 1000}},
        {"holding-registers",
         ARGS_TYPE_INT,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "modbus.holding_registers",
         "Number of holding registers",
         {.has_default = true, .value.int_val = 1000}},
        {"input-registers",
         ARGS_TYPE_INT,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "modbus.input_registers",
         "Number of input registers",
         {.has_default = true, .value.int_val = 1000}},
        {"help",
         ARGS_TYPE_BOOL,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "general.help",
         "Show this help message",
         {.has_default = true, .value.bool_val = false}},
    };
    const size_t num_flags = sizeof(flags) / sizeof(flags[0]);

    util_err_t parse_rc = args_parse(argc, (const char **)argv, flags, num_flags, &args_result);
    if(parse_rc != UTIL_OK) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Failed to parse arguments: %s", args_get_error_detail(&args_result));
        args_print_help(argv[0], flags, num_flags);
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    if(args_get_bool(&args_result, "help")) {
        args_print_help(argv[0], flags, num_flags);
        args_free(&args_result);
        return EXIT_SUCCESS;
    }

    const char *debug_level_str = args_get_string(&args_result, "debug");
    log_level_t log_level = LOG_LEVEL_INFO;
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

    int64_t coils_val = args_get_int(&args_result, "coils");
    int64_t di_val = args_get_int(&args_result, "discrete-inputs");
    int64_t hr_val = args_get_int(&args_result, "holding-registers");
    int64_t ir_val = args_get_int(&args_result, "input-registers");

    if(coils_val < 0 || coils_val > 65535 || di_val < 0 || di_val > 65535 || hr_val < 0 || hr_val > 65535 || ir_val < 0
       || ir_val > 65535) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Register counts must be 0-65535");
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    register_storage_t *temp_storage = register_storage_create((size_t)coils_val, (size_t)di_val, (size_t)hr_val, (size_t)ir_val);
    if(!temp_storage) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Failed to create storage");
        args_free(&args_result);
        return EXIT_FAILURE;
    }
    server.storage = temp_storage;

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Modbus server starting");
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Coils: %zu", (size_t)coils_val);
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Discrete Inputs: %zu", (size_t)di_val);
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Holding Registers: %zu", (size_t)hr_val);
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Input Registers: %zu", (size_t)ir_val);

    /* Initialize the coroutine system */
    util_err_t err = coro_create(&g_server->coro_net, 256);
    if(err != UTIL_OK) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Failed to create coro_net instance: %s", util_err_str(err));
        return EXIT_FAILURE;
    }

    /* Set up signal handler for graceful shutdown */
    util_set_interrupt_handler(signal_handler);

    size_t listen_count = args_get_count(&args_result, "listen");
    if(listen_count == 0) { listen_count = 1; }

    for(size_t i = 0; i < listen_count; i++) {
        const char *listen_addr = NULL;

        /* Since listen is ARGS_MULTIPLE, always use args_get_at() */
        args_value_t val = args_get_at(&args_result, "listen", i);
        if(val.present) { listen_addr = val.value.string_val; }

        if(!listen_addr) { listen_addr = "127.0.0.1:502"; }

        char addr_copy[256];
        strncpy(addr_copy, listen_addr, sizeof(addr_copy) - 1);
        addr_copy[sizeof(addr_copy) - 1] = '\0';

        char *colon = strchr(addr_copy, ':');
        if(!colon) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Invalid address format: %s", listen_addr);
            args_free(&args_result);
            register_storage_destroy(temp_storage);
            return EXIT_FAILURE;
        }

        *colon = '\0';
        char *addr_str = addr_copy;
        uint16_t port = (uint16_t)atoi(colon + 1);

        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Creating listener on %s:%u", addr_str, port);

        /* Initialize socket address */
        socket_address_t listen_address;
        util_err_t addr_err = socket_address_init(&listen_address, addr_str, port);
        if(addr_err != UTIL_OK) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Failed to initialize socket address: %s", util_err_str(addr_err));
            args_free(&args_result);
            register_storage_destroy(temp_storage);
            return EXIT_FAILURE;
        }

        /* Create TCP server socket */
        socket_t listen_fd = socket_create_tcp_server(&listen_address, 128);
        if(listen_fd == INVALID_SOCKET) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Failed to create TCP server on %s:%u: %s", addr_str, port,
                  util_err_str(socket_get_err()));
            args_free(&args_result);
            register_storage_destroy(temp_storage);
            return EXIT_FAILURE;
        }

        /* Set socket options */
        socket_set_reuseaddr(listen_fd, true);

        listener_info_t *listener_info = (listener_info_t *)malloc(sizeof(listener_info_t));
        if(!listener_info) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Failed to allocate listener info");
            CS_CLOSE(listen_fd);
            args_free(&args_result);
            register_storage_destroy(temp_storage);
            return EXIT_FAILURE;
        }

        memset(listener_info, 0, sizeof(listener_info_t));
        listener_info->server = &server;
        strncpy(listener_info->bind_address, addr_str, sizeof(listener_info->bind_address) - 1);
        listener_info->bind_port = port;

        coro_task_handle_t listener_handle;
        err = coro_add_task(&listener_handle, g_server->coro_net, listen_fd, listener_handler, listener_info);
        if(err != UTIL_OK) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Failed to add listener socket: %s", util_err_str(err));
            CS_CLOSE(listen_fd);
            free(listener_info);
            args_free(&args_result);
            register_storage_destroy(temp_storage);
            return EXIT_FAILURE;
        }

        listener_info->handle = listener_handle;
    }


    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Starting coroutine event loop");
    coro_run(&(g_server->coro_net), 50); /* 50ms tick interval */

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Modbus server shutting down");
    coro_destroy(&(g_server->coro_net));

    g_server->coro_net = NULL;

    fflush(stderr);

    print_statistics(&server);

    fflush(stderr);

    args_free(&args_result);
    register_storage_destroy(temp_storage);
    socket_cleanup();

    return EXIT_SUCCESS;
}
