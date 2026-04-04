/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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
 * Modbus TCP server using fiber_net for connection handling.
 *
 * Each listener runs in its own fiber; each accepted client runs in its own
 * fiber.  All buffers are arena-allocated per-connection (reset each request
 * cycle).  No buf_t dependency.
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "args.h"
#include "bytes.h"
#include "err.h"
#include "fiber_net.h"
#include "log.h"
#include "modbus_protocol3.h"
#include "register_storage.h"
#include "utils.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

#define CLIENT_ARENA_SIZE 8192U
#define ACCEPT_TIMEOUT_MS 1000U
#define IO_TIMEOUT_MS 30000U

/* Histogram bucket boundaries (microseconds) */
#define HIST_BUCKET_COUNT 8
static const int64_t HIST_BOUNDARIES[HIST_BUCKET_COUNT] = {
    100,      /* 0–100 us    */
    500,      /* 100–500 us  */
    1000,     /* 500 us–1 ms */
    2000,     /* 1–2 ms      */
    5000,     /* 2–5 ms      */
    10000,    /* 5–10 ms     */
    50000,    /* 10–50 ms    */
    INT64_MAX /* > 50 ms     */
};

/* ============================================================================
 * Statistics
 * ============================================================================ */

typedef struct {
    int64_t total_requests;
    /* "active" window: data-ready → send-complete (comparable to coroutine/threaded totals) */
    int64_t total_response_time_us;
    int64_t total_response_time_sq_us;
    int64_t min_response_time_us;
    int64_t max_response_time_us;
    /* per-phase accumulators */
    int64_t total_recv_time_us;    /* recv_start → both recvs done (includes wait for data) */
    int64_t total_process_time_us; /* modbus_process_request_bytes */
    int64_t total_encode_time_us;  /* build MBAP response header + bytes_join */
    int64_t total_send_time_us;    /* fiber_socket_send */
    /* full wall-clock roundtrip: recv_start → send_end */
    int64_t total_roundtrip_time_us;
    int64_t clients_connected;
    int64_t clients_disconnected;
    int64_t hist_buckets[HIST_BUCKET_COUNT];
} server_stats_t;

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

static void update_histogram(server_stats_t *stats, int64_t us) {
    for(int i = 0; i < HIST_BUCKET_COUNT; i++) {
        if(us <= HIST_BOUNDARIES[i]) {
            stats->hist_buckets[i]++;
            return;
        }
    }
}

/* ============================================================================
 * Server context (shared across all fibers)
 * ============================================================================ */

typedef struct {
    register_storage_t *storage;
    fiber_net_t *net;
    int64_t start_time_us;
    server_stats_t stats;
} server_ctx_t;

/* Global pointer for signal handler */
static server_ctx_t *g_server = NULL;

/* ============================================================================
 * Per-listener fiber context
 * ============================================================================ */

typedef struct {
    server_ctx_t *server;
    char bind_ip[64];
    uint16_t port;
} listener_ctx_t;

/* ============================================================================
 * Per-client fiber context
 * ============================================================================ */

typedef struct {
    server_ctx_t *server;
    fiber_socket_t sock;
} client_ctx_t;

/* ============================================================================
 * Client fiber
 * ============================================================================ */

static void *client_fiber(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    fiber_net_t *net = ctx->server->net;
    fiber_socket_t sock = ctx->sock;
    Arena arena;

    if(arena_init(&arena, CLIENT_ARENA_SIZE) != UTIL_OK) {
        pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_ERROR, "Failed to allocate client arena");
        fiber_socket_close(net, sock);
        free(ctx);
        return NULL;
    }

    ctx->server->stats.clients_connected++;
    pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_DETAIL, "Client fiber started");

    for(;;) {
        arena_reset(&arena);

        /* ---- Phase 1: Receive MBAP header (7 bytes) ---- */
        Bytes hdr_buf = bytes_alloc(&arena, MBAP_HEADER_SIZE);
        if(bytes_is_null(hdr_buf)) {
            pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_ERROR, "Arena exhausted allocating MBAP header buffer");
            break;
        }

        /* t0: start of recv phase — fiber may suspend here waiting for data */
        int64_t recv_start = util_time_us();
        util_err_t rc = fiber_socket_recv(net, sock, hdr_buf, IO_TIMEOUT_MS);
        if(rc != UTIL_OK) {
            if(rc != UTIL_ECLOSED && rc != UTIL_ECANCELLED) {
                pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_WARN, "MBAP header recv error: %s", util_err_str(rc));
            }
            break;
        }

        /* ---- Phase 2: Decode MBAP header ---- */
        uint16_t txn_id = 0, proto_id = 0, length = 0;
        uint8_t unit_id = 0;
        Bytes rest = bytes_unpack(hdr_buf, ">HHHB", &txn_id, &proto_id, &length, &unit_id);
        if(bytes_is_null(rest)) {
            pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_WARN, "Failed to decode MBAP header");
            break;
        }

        if(proto_id != 0) {
            pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_WARN, "Bad Modbus protocol ID: %u", proto_id);
            break;
        }
        /* length = unit_id byte + PDU; minimum is FC (2), max is PDU_SIZE+1 */
        if(length < 2 || length > (uint16_t)(MODBUS_MAX_PDU_SIZE + 1)) {
            pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_WARN, "Bad MBAP length field: %u", length);
            break;
        }

        /* ---- Phase 3: Receive PDU (length - 1 bytes; unit_id already consumed) ---- */
        size_t pdu_len = (size_t)(length - 1U);
        Bytes pdu_buf = bytes_alloc(&arena, pdu_len);
        if(bytes_is_null(pdu_buf)) {
            pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_ERROR, "Arena exhausted allocating PDU buffer");
            break;
        }

        rc = fiber_socket_recv(net, sock, pdu_buf, IO_TIMEOUT_MS);
        if(rc != UTIL_OK) {
            if(rc != UTIL_ECLOSED && rc != UTIL_ECANCELLED) {
                pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_WARN, "PDU recv error: %s", util_err_str(rc));
            }
            break;
        }

        /* ---- Phase 4: Process Modbus request ---- */
        uint8_t fc = pdu_buf.data[0];
        Bytes pdu_body = bytes_slice(pdu_buf, 1, pdu_len - 1);

        pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_DETAIL, "Processing FC=0x%02X unit=%u txn=%u", fc, unit_id, txn_id);

        int64_t process_start = util_time_us();
        Bytes response_pdu = modbus_process_request_bytes(&arena, fc, pdu_body, ctx->server->storage);
        int64_t process_end = util_time_us();

        if(bytes_is_null(response_pdu)) {
            pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_ERROR, "Arena exhausted building response PDU");
            break;
        }

        /* ---- Phase 5: Build MBAP response header + send ---- */
        /* MBAP length = unit_id (1) + response PDU */
        uint16_t resp_mbap_len = (uint16_t)(1U + response_pdu.len);
        Bytes resp_hdr = bytes_pack(&arena, ">HHHB", txn_id, (uint16_t)0, resp_mbap_len, unit_id);
        if(bytes_is_null(resp_hdr)) {
            pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_ERROR, "Arena exhausted building response MBAP header");
            break;
        }

        Bytes response = bytes_join(&arena, resp_hdr, response_pdu);
        if(bytes_is_null(response)) {
            pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_ERROR, "Arena exhausted joining response");
            break;
        }

        /* ---- Phase 6: Send response ---- */
        int64_t send_start = util_time_us();
        rc = fiber_socket_send(net, sock, response, IO_TIMEOUT_MS);
        int64_t send_end = util_time_us();

        if(rc != UTIL_OK) {
            if(rc != UTIL_ECANCELLED) {
                pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_WARN, "Response send error: %s", util_err_str(rc));
            }
            break;
        }

        /* ---- Update statistics ---- */
        /*
         * active_us:     data-ready → send-complete (comparable to coroutine/threaded totals:
         *                both exclude time blocking in recv waiting for data to arrive).
         * recv_us:       recv_start → data-ready    (includes blocking wait + actual socket read).
         * process_us:    modbus_process_request_bytes only.
         * encode_us:     build MBAP response header + bytes_join.
         * send_us:       fiber_socket_send.
         * roundtrip_us:  recv_start → send_end      (full wall-clock time per request).
         */
        int64_t active_us = send_end - process_start;
        int64_t recv_us = process_start - recv_start;
        int64_t process_us = process_end - process_start;
        int64_t encode_us = send_start - process_end;
        int64_t send_us = send_end - send_start;
        int64_t roundtrip_us = send_end - recv_start;

        server_stats_t *stats = &ctx->server->stats;
        stats->total_requests++;
        stats->total_response_time_us += active_us;
        stats->total_response_time_sq_us += active_us * active_us;
        stats->total_recv_time_us += recv_us;
        stats->total_process_time_us += process_us;
        stats->total_encode_time_us += encode_us;
        stats->total_send_time_us += send_us;
        stats->total_roundtrip_time_us += roundtrip_us;

        if(stats->min_response_time_us == 0 || active_us < stats->min_response_time_us) {
            stats->min_response_time_us = active_us;
        }
        if(active_us > stats->max_response_time_us) { stats->max_response_time_us = active_us; }
        update_histogram(stats, active_us);

        pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_DETAIL,
              "Request done: roundtrip=%" PRId64 "us recv=%" PRId64 "us process=%" PRId64 "us encode=%" PRId64 "us send=%" PRId64
              "us",
              roundtrip_us, recv_us, process_us, encode_us, send_us);
    }

    pdlog(LOG_MODULE_MODBUS3_CLIENT, LOG_LEVEL_DETAIL, "Client fiber closing");
    ctx->server->stats.clients_disconnected++;
    fiber_socket_close(net, sock);
    arena_free(&arena);
    free(ctx);
    return NULL;
}

/* ============================================================================
 * Listener fiber
 * ============================================================================ */

static void *listener_fiber(void *arg) {
    listener_ctx_t *ctx = (listener_ctx_t *)arg;
    fiber_net_t *net = ctx->server->net;
    fiber_socket_t listen_fd = FIBER_INVALID_SOCKET;

    pdlog(LOG_MODULE_MODBUS3_LISTENER, LOG_LEVEL_INFO, "Listener starting on %s:%" PRIu16, ctx->bind_ip, ctx->port);

    util_err_t rc = fiber_socket_listen(net, &listen_fd, ctx->bind_ip, ctx->port, 128);
    if(rc != UTIL_OK) {
        pdlog(LOG_MODULE_MODBUS3_LISTENER, LOG_LEVEL_ERROR, "fiber_socket_listen failed: %s", util_err_str(rc));
        free(ctx);
        return NULL;
    }

    for(;;) {
        fiber_socket_t client_fd = FIBER_INVALID_SOCKET;
        rc = fiber_socket_accept(net, listen_fd, &client_fd, ACCEPT_TIMEOUT_MS);

        if(rc == UTIL_ETIMEOUT) { continue; }
        if(rc != UTIL_OK) {
            /* UTIL_ECANCELLED on shutdown, or a real error */
            if(rc != UTIL_ECANCELLED) {
                pdlog(LOG_MODULE_MODBUS3_LISTENER, LOG_LEVEL_WARN, "Accept error: %s", util_err_str(rc));
            }
            break;
        }

        pdlog(LOG_MODULE_MODBUS3_LISTENER, LOG_LEVEL_DETAIL, "Accepted client connection (fd %d)", (int)client_fd);

        client_ctx_t *cctx = (client_ctx_t *)malloc(sizeof(client_ctx_t));
        if(!cctx) {
            pdlog(LOG_MODULE_MODBUS3_LISTENER, LOG_LEVEL_ERROR, "Failed to allocate client context");
            fiber_socket_close(net, client_fd);
            continue;
        }
        cctx->server = ctx->server;
        cctx->sock = client_fd;

        rc = fiber_net_add_task(net, client_fiber, cctx);
        if(rc != UTIL_OK) {
            pdlog(LOG_MODULE_MODBUS3_LISTENER, LOG_LEVEL_ERROR, "Failed to add client fiber: %s", util_err_str(rc));
            fiber_socket_close(net, client_fd);
            free(cctx);
            continue;
        }
    }

    pdlog(LOG_MODULE_MODBUS3_LISTENER, LOG_LEVEL_INFO, "Listener stopping");
    fiber_socket_close(net, listen_fd);
    free(ctx);
    return NULL;
}

/* ============================================================================
 * Statistics output
 * ============================================================================ */

static void print_statistics(server_ctx_t *server) {
    if(!server) { return; }

    int64_t end_time_us = util_time_us();
    double runtime_sec = (double)(end_time_us - server->start_time_us) / 1000000.0;
    server_stats_t *stats = &server->stats;
    int64_t total_reqs = stats->total_requests;

    fflush(stderr);

    fprintf(stderr, "\n");
    fprintf(stderr, "╔══════════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║          MODBUS SERVER (FIBER) PERFORMANCE STATISTICS            ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║ Runtime: %.2f seconds\n", runtime_sec);
    fprintf(stderr, "║ Total requests: %" PRId64 "\n", (long long)total_reqs);
    if(runtime_sec > 0.0) { fprintf(stderr, "║ Throughput: %.2f requests/sec\n", (double)total_reqs / runtime_sec); }
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║ Clients connected:    %" PRId64 "\n", (long long)stats->clients_connected);
    fprintf(stderr, "║ Clients disconnected: %" PRId64 "\n", (long long)stats->clients_disconnected);
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");

    if(total_reqs > 0) {
        double mean = (double)stats->total_response_time_us / (double)total_reqs;
        double variance = ((double)stats->total_response_time_sq_us / (double)total_reqs) - (mean * mean);
        double stddev = variance > 0.0 ? sqrt(variance) : 0.0;

        double avg_roundtrip = (double)stats->total_roundtrip_time_us / (double)total_reqs;
        double avg_recv = (double)stats->total_recv_time_us / (double)total_reqs;
        double avg_process = (double)stats->total_process_time_us / (double)total_reqs;
        double avg_encode = (double)stats->total_encode_time_us / (double)total_reqs;
        double avg_send = (double)stats->total_send_time_us / (double)total_reqs;
        /* roundtrip = recv + active; active = process + encode + send */
        double avg_active = avg_process + avg_encode + avg_send;

        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║                     RESPONSE TIME SUMMARY                        ║\n");
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║ Average (active):  %8.2f us   (data-ready → send-complete)\n", mean);
        fprintf(stderr, "║ Std Dev:           %8.2f us\n", stddev);
        fprintf(stderr, "║ Minimum:           %8" PRId64 " us\n", (long long)stats->min_response_time_us);
        fprintf(stderr, "║ Maximum:           %8" PRId64 " us\n", (long long)stats->max_response_time_us);
        fprintf(stderr, "║ Average (roundtrip):%7.2f us   (recv-start → send-complete)\n", avg_roundtrip);

        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║                   LATENCY BREAKDOWN (avg)                        ║\n");
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");

        double rt = avg_roundtrip > 0.0 ? avg_roundtrip : 1.0;
        double at = avg_active > 0.0 ? avg_active : 1.0;

        fprintf(stderr, "║  Recv (socket):    %8.2f us (%5.1f%% of roundtrip)\n", avg_recv, avg_recv / rt * 100.0);
        fprintf(stderr, "║    incl. wait for data + actual I/O across both recv calls\n");
        fprintf(stderr, "║  Process (modbus): %8.2f us (%5.1f%% of roundtrip, %5.1f%% of active)\n", avg_process,
                avg_process / rt * 100.0, avg_process / at * 100.0);
        fprintf(stderr, "║  Encode (response):%8.2f us (%5.1f%% of roundtrip, %5.1f%% of active)\n", avg_encode,
                avg_encode / rt * 100.0, avg_encode / at * 100.0);
        fprintf(stderr, "║  Send (socket):    %8.2f us (%5.1f%% of roundtrip, %5.1f%% of active)\n", avg_send,
                avg_send / rt * 100.0, avg_send / at * 100.0);

        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║                  RESPONSE TIME HISTOGRAM\n");
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");

        int64_t max_bucket = 0;
        for(int i = 0; i < HIST_BUCKET_COUNT; i++) {
            if(stats->hist_buckets[i] > max_bucket) { max_bucket = stats->hist_buckets[i]; }
        }
        for(int i = 0; i < HIST_BUCKET_COUNT; i++) {
            int64_t cnt = stats->hist_buckets[i];
            double pct = (double)cnt / (double)total_reqs * 100.0;
            int bar = max_bucket > 0 ? (int)((double)cnt / (double)max_bucket * 30) : 0;

            fprintf(stderr, "║  %-12s │", hist_bucket_label(i));
            for(int j = 0; j < bar; j++) { fprintf(stderr, "█"); }
            for(int j = bar; j < 30; j++) { fprintf(stderr, " "); }
            fprintf(stderr, "│ %6lld (%5.1f%%)\n", (long long)cnt, pct);
        }
    }

    /* ---- fiber_net event loop instrumentation ---- */
    if(server->net) {
        fiber_net_loop_stats_t ls;
        fiber_net_get_loop_stats(server->net, &ls);

        double timeout_pct = ls.loop_iterations > 0 ? (double)ls.poll_timeout_iters / (double)ls.loop_iterations * 100.0 : 0.0;
        double wakeup_pct = ls.loop_iterations > 0 ? (double)ls.poll_wakeup_iters / (double)ls.loop_iterations * 100.0 : 0.0;

        /* total non-poll overhead per iteration (everything except the poll() block) */
        double avg_overhead_us =
            ls.avg_step1_us + ls.avg_step2_us + ls.avg_step4_us + ls.avg_step5_scan_us + ls.avg_step5_resume_us;

        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║              FIBER_NET EVENT LOOP INSTRUMENTATION                ║\n");
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║ Loop iterations:   %10" PRId64 "\n", (long long)ls.loop_iterations);
        fprintf(stderr, "║ Poll timeouts:     %10" PRId64 "  (%5.1f%% of iters)\n", (long long)ls.poll_timeout_iters,
                timeout_pct);
        fprintf(stderr, "║ Poll wakeups:      %10" PRId64 "  (%5.1f%% of iters)\n", (long long)ls.poll_wakeup_iters, wakeup_pct);
        fprintf(stderr, "║ Total resumes:     %10" PRId64 "\n", (long long)ls.total_resumes);
        if(total_reqs > 0) { fprintf(stderr, "║ Resumes/request:   %10.2f\n", (double)ls.total_resumes / (double)total_reqs); }
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║              LOOP PHASE BREAKDOWN (avg µs / iteration)           ║\n");
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║  Step 1 — pending-start scan + initial resumes: %8.3f us\n", ls.avg_step1_us);
        fprintf(stderr, "║  Step 2 — pollfd array build:                   %8.3f us\n", ls.avg_step2_us);
        fprintf(stderr, "║  Step 3 — poll() [includes idle blocking time]: %8.1f us\n", ls.avg_step3_us);
        fprintf(stderr, "║  Step 4 — wakeup pipe drain:                    %8.3f us\n", ls.avg_step4_us);
        fprintf(stderr, "║  Step 5 — ready-fd scan (no resumes):           %8.3f us\n", ls.avg_step5_scan_us);
        fprintf(stderr, "║  Step 5 — fiber resumes (exec + ctx switch):    %8.3f us\n", ls.avg_step5_resume_us);
        fprintf(stderr, "║  ──────────────────────────────────────────────────────────\n");
        fprintf(stderr, "║  Total non-poll overhead / iteration:           %8.3f us\n", avg_overhead_us);
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║              PER RESUME STATISTICS                               ║\n");
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║  Avg duration (exec + 2× ctx switch): %8.2f us\n", ls.avg_resume_us);
        fprintf(stderr, "║  Minimum:                             %8" PRId64 " us\n", (long long)ls.min_resume_us);
        fprintf(stderr, "║  Maximum:                             %8" PRId64 " us\n", (long long)ls.max_resume_us);
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║              CPU USAGE (getrusage / GetProcessTimes)             ║\n");
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║  User CPU time:    %10.1f ms\n", ls.cpu_user_ms);
        fprintf(stderr, "║  System CPU time:  %10.1f ms\n", ls.cpu_system_ms);
        fprintf(stderr, "║  Wall clock time:  %10.1f ms\n", ls.wall_time_ms);
        fprintf(stderr, "║  CPU load:         %10.1f %%\n", ls.cpu_load_pct);

        /* ---- Stack high-watermark ---- */
        if(ls.watermark_count > 0) {
            double sz = ls.stack_size_bytes > 0 ? (double)ls.stack_size_bytes : 1.0;
            fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
            fprintf(stderr, "║              STACK HIGH-WATERMARK                                ║\n");
            fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
            fprintf(stderr, "║  Stack size (configured): %7zu bytes (%zu KB)\n",
                    ls.stack_size_bytes, ls.stack_size_bytes / 1024u);
            fprintf(stderr, "║  Fibers measured:         %7" PRId64 "\n", (long long)ls.watermark_count);
            fprintf(stderr, "║  Avg stack used:          %7.0f bytes (%4.1f%%)\n",
                    ls.avg_stack_used_bytes, ls.avg_stack_used_bytes / sz * 100.0);
            fprintf(stderr, "║  Min stack used:          %7zu bytes (%4.1f%%)\n",
                    ls.min_stack_used_bytes, (double)ls.min_stack_used_bytes / sz * 100.0);
            fprintf(stderr, "║  Max stack used:          %7zu bytes (%4.1f%%)\n",
                    ls.max_stack_used_bytes, (double)ls.max_stack_used_bytes / sz * 100.0);
        }
    }

    fprintf(stderr, "╚══════════════════════════════════════════════════════════════════╝\n");
    fflush(stderr);
}

/* ============================================================================
 * Signal handling
 * ============================================================================ */

static void signal_handler(void) {
    if(g_server) { fiber_net_stop(g_server->net); }
}

/* ============================================================================
 * main()
 * ============================================================================ */

int main(int argc, char *argv[]) {
    server_ctx_t server = {0};
    g_server = &server;

    /* ----- Argument definitions (same as modbus_server) ----- */
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

    args_result_t args_result = {0};
    util_err_t rc = args_parse(argc, (const char **)argv, flags, num_flags, &args_result);
    if(rc != UTIL_OK) {
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

    /* ----- Configure logging ----- */
    const char *debug_str = args_get_string(&args_result, "debug");
    log_level_t log_level = LOG_LEVEL_INFO;
    if(debug_str) {
        if(strcmp(debug_str, "ERROR") == 0) {
            log_level = LOG_LEVEL_ERROR;
        } else if(strcmp(debug_str, "WARN") == 0) {
            log_level = LOG_LEVEL_WARN;
        } else if(strcmp(debug_str, "INFO") == 0) {
            log_level = LOG_LEVEL_INFO;
        } else if(strcmp(debug_str, "DETAIL") == 0) {
            log_level = LOG_LEVEL_DETAIL;
        } else if(strcmp(debug_str, "SPEW") == 0) {
            log_level = LOG_LEVEL_SPEW;
        }
    }
    log_set_all_modules(log_level);

    server.start_time_us = util_time_us();

    /* ----- Register storage ----- */
    int64_t coils_val = args_get_int(&args_result, "coils");
    int64_t di_val = args_get_int(&args_result, "discrete-inputs");
    int64_t hr_val = args_get_int(&args_result, "holding-registers");
    int64_t ir_val = args_get_int(&args_result, "input-registers");

    if(coils_val < 0 || coils_val > 65535 || di_val < 0 || di_val > 65535 || hr_val < 0 || hr_val > 65535 || ir_val < 0
       || ir_val > 65535) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Register counts must be in range 0-65535");
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    server.storage = register_storage_create((size_t)coils_val, (size_t)di_val, (size_t)hr_val, (size_t)ir_val);
    if(!server.storage) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Failed to create register storage");
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "modbus_server3 starting");
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Coils:             %zu", (size_t)coils_val);
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Discrete Inputs:   %zu", (size_t)di_val);
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Holding Registers: %zu", (size_t)hr_val);
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Input Registers:   %zu", (size_t)ir_val);

    /* ----- Create fiber_net event loop ----- */
    /* 256 max tasks: 1 listener per endpoint + up to N clients */
    rc = fiber_net_create(&server.net, 256, 24 * 1024, /* 24 KB per fiber stack */
                          YAFL_STACK_FLAGS_MALLOC | YAFL_STACK_FLAGS_WATERMARK);
    if(rc != UTIL_OK) {
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Failed to create fiber_net: %s", util_err_str(rc));
        register_storage_destroy(server.storage);
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    /* ----- Signal handler ----- */
    util_set_interrupt_handler(signal_handler);

    /* ----- Register listener fibers ----- */
    size_t listen_count = args_get_count(&args_result, "listen");
    if(listen_count == 0) { listen_count = 1; }

    for(size_t i = 0; i < listen_count; i++) {
        const char *listen_addr = NULL;

        args_value_t val = args_get_at(&args_result, "listen", i);
        if(val.present) { listen_addr = val.value.string_val; }
        if(!listen_addr) { listen_addr = "127.0.0.1:502"; }

        /* Parse "addr:port" */
        char addr_copy[128];
        strncpy(addr_copy, listen_addr, sizeof(addr_copy) - 1);
        addr_copy[sizeof(addr_copy) - 1] = '\0';

        char *colon = strchr(addr_copy, ':');
        if(!colon) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Invalid address format (expected addr:port): %s", listen_addr);
            fiber_net_destroy(&server.net);
            register_storage_destroy(server.storage);
            args_free(&args_result);
            return EXIT_FAILURE;
        }
        *colon = '\0';
        uint16_t port = (uint16_t)(unsigned int)atoi(colon + 1);

        listener_ctx_t *lctx = (listener_ctx_t *)malloc(sizeof(listener_ctx_t));
        if(!lctx) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Failed to allocate listener context");
            fiber_net_destroy(&server.net);
            register_storage_destroy(server.storage);
            args_free(&args_result);
            return EXIT_FAILURE;
        }
        lctx->server = &server;
        strncpy(lctx->bind_ip, addr_copy, sizeof(lctx->bind_ip) - 1);
        lctx->bind_ip[sizeof(lctx->bind_ip) - 1] = '\0';
        lctx->port = port;

        rc = fiber_net_add_task(server.net, listener_fiber, lctx);
        if(rc != UTIL_OK) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Failed to add listener fiber: %s", util_err_str(rc));
            free(lctx);
            fiber_net_destroy(&server.net);
            register_storage_destroy(server.storage);
            args_free(&args_result);
            return EXIT_FAILURE;
        }

        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Listener queued on %s:%" PRIu16, addr_copy, port);
    }

    /* ----- Run event loop (blocks until fiber_net_stop()) ----- */
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Entering fiber_net event loop");
    fiber_net_run(server.net, 50);

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "modbus_server3 shutting down");

    print_statistics(&server);

    fiber_net_destroy(&server.net);
    args_free(&args_result);
    register_storage_destroy(server.storage);

    return EXIT_SUCCESS;
}
