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
 * ab_server_fiber.c — fiber-based Allen-Bradley EIP/CIP+PCCC simulator.
 *
 * Each client connection runs in a dedicated fiber.  A single listener
 * fiber accepts TCP connections on port 44818 (or the --port= value).
 * All I/O goes through fiber_net, making the whole server single-threaded:
 * no mutexes, no locks, no threads.
 *
 * Command-line arguments:
 *   --plc=<type>        Required. ControlLogix, Micro800, Omron,
 *                                PLC/5, SLC500, or Micrologix.
 *   --path=<a,b>        Required for ControlLogix; ignored otherwise.
 *   --port=<port>       TCP port. Default: 44818.
 *   --tag=<tagdef>      Tag definition. At least one required.
 *                         CIP  format: <name>:<TYPE>[<d>[,<d>[,<d>]]]
 *                         PCCC format: <file>[<size>]
 *   --debug=<level>     ERROR, WARN, INFO, DETAIL, SPEW. Default: INFO.
 *   --reject_fo=<n>     Reject first n ForwardOpen requests. Default: 0.
 *   --delay=<ms>        Artificial response delay in ms. Default: 0.
 *   --help              Print usage.
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#    include <strings.h> /* strcasecmp */
#endif

#include "arena.h"
#include "args.h"
#include "bytes.h"
#include "eip.h"
#include "err.h"
#include "fiber_net.h"
#include "log.h"
#include "plc.h"
#include "utils.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

#define CLIENT_ARENA_SIZE (65536U)
#define ACCEPT_TIMEOUT_MS (1000U)
#define IO_TIMEOUT_MS (30000U)
#define DEFAULT_PORT "44818"

#ifdef _WIN32
#    define str_cmp_i _stricmp
#else
#    define str_cmp_i strcasecmp
#endif

/* ============================================================================
 * Structs
 * ============================================================================ */

typedef struct {
    int64_t clients_connected;
    int64_t clients_disconnected;
    int64_t total_requests;
    ArenaStats arena;
} server_stats_t;

typedef struct {
    fiber_net_t *net;
    plc_config_t *cfg;
    server_stats_t stats;
    int64_t start_time_us;
} server_ctx_t;

typedef struct {
    server_ctx_t *server;
    char bind_ip[64];
    uint16_t port;
} listener_ctx_t;

typedef struct {
    server_ctx_t *server;
    fiber_socket_t sock;
} client_ctx_t;

/* ============================================================================
 * Static global data
 * ============================================================================ */

static server_ctx_t *g_server = NULL;

/* ============================================================================
 * Forward declarations of static functions
 * ============================================================================ */

static void usage(void);
static bool setup_plc_type(const char *plc_str, plc_config_t *cfg);
static bool parse_path(const char *path_str, plc_config_t *cfg);
static bool parse_pccc_tag(const char *tag_str, plc_config_t *cfg);
static bool parse_cip_tag(const char *tag_str, plc_config_t *cfg);
static void free_tags(tag_def_t *tags);
static void print_statistics(server_ctx_t *server);
static void signal_handler(void);
static void *listener_fiber(void *arg);
static void *client_fiber(void *arg);

/* ============================================================================
 * main()
 * ============================================================================ */

int main(int argc, char *argv[]) {
    server_ctx_t server = {0};
    plc_config_t cfg = {0};
    args_result_t args = {0};
    util_err_t rc = UTIL_OK;
    size_t i = 0;

    g_server = &server;
    server.cfg = &cfg;

    log_set_all_modules(LOG_LEVEL_INFO);

    args_flag_def_t flags[] = {
        {"plc",
         ARGS_TYPE_STRING,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "ab.plc",
         "PLC type: ControlLogix, Micro800, Omron, PLC/5, SLC500, Micrologix",
         {.has_default = false}},
        {"path",
         ARGS_TYPE_STRING,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "ab.path",
         "CIP path (required for ControlLogix), e.g. \"1,0\"",
         {.has_default = false}},
        {"port",
         ARGS_TYPE_STRING,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "ab.port",
         "TCP port to listen on. Default: 44818",
         {.has_default = true, .value.string_val = DEFAULT_PORT}},
        {"tag",
         ARGS_TYPE_STRING,
         ARGS_OPTIONAL,
         ARGS_MULTIPLE,
         "ab.tag",
         "Tag definition, e.g. MyTag:DINT[10] or N7[100]",
         {.has_default = false}},
        {"debug",
         ARGS_TYPE_BOOL,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "logging.debug",
         "Enable DETAIL-level logging. Default: off (INFO level).",
         {.has_default = true, .value.bool_val = false}},
        {"reject_fo",
         ARGS_TYPE_INT,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "ab.reject_fo",
         "Reject the first N ForwardOpen requests. Default: 0",
         {.has_default = true, .value.int_val = 0}},
        {"delay",
         ARGS_TYPE_INT,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "ab.delay",
         "Artificial response delay in milliseconds. Default: 0",
         {.has_default = true, .value.int_val = 0}},
        {"help",
         ARGS_TYPE_BOOL,
         ARGS_OPTIONAL,
         ARGS_ONCE,
         "general.help",
         "Show this help message",
         {.has_default = true, .value.bool_val = false}},
    };
    size_t num_flags = sizeof(flags) / sizeof(flags[0]);

    rc = args_parse(argc, (const char **)argv, flags, num_flags, &args);
    if(rc != UTIL_OK) {
        fprintf(stderr, "Argument parse error: %s\n", args_get_error_detail(&args));
        args_print_help(argv[0], flags, num_flags);
        args_free(&args);
        return EXIT_FAILURE;
    }

    if(args_get_bool(&args, "help")) {
        usage();
        args_print_help(argv[0], flags, num_flags);
        args_free(&args);
        return EXIT_SUCCESS;
    }

    /* ----- Configure logging ----- */
    {
        log_level_t log_level = args_get_bool(&args, "debug") ? LOG_LEVEL_DETAIL : LOG_LEVEL_NONE;
        log_set_all_modules(log_level);
    }

    /* ----- PLC type ----- */
    {
        const char *plc_str = args_get_string(&args, "plc");
        if(!plc_str) {
            fprintf(stderr, "Error: --plc= is required.\n");
            usage();
            args_free(&args);
            return EXIT_FAILURE;
        }
        if(!setup_plc_type(plc_str, &cfg)) {
            fprintf(stderr, "Error: unknown PLC type \"%s\".\n", plc_str);
            usage();
            args_free(&args);
            return EXIT_FAILURE;
        }
    }

    /* ----- CIP path (ControlLogix only) ----- */
    if(cfg.plc_type == PLC_CONTROL_LOGIX) {
        const char *path_str = args_get_string(&args, "path");
        if(!path_str) {
            fprintf(stderr, "Error: --path= is required for ControlLogix.\n");
            usage();
            args_free(&args);
            return EXIT_FAILURE;
        }
        if(!parse_path(path_str, &cfg)) {
            fprintf(stderr, "Error: invalid path \"%s\". Expected two comma-separated integers, e.g. 1,0.\n", path_str);
            args_free(&args);
            return EXIT_FAILURE;
        }
    }

    /* ----- Port ----- */
    cfg.port_str = args_get_string(&args, "port");
    if(!cfg.port_str) { cfg.port_str = DEFAULT_PORT; }

    /* ----- reject_fo and delay ----- */
    cfg.reject_fo_count = (int32_t)args_get_int(&args, "reject_fo");
    cfg.response_delay_ms = (int32_t)args_get_int(&args, "delay");

    /* ----- Tags ----- */
    {
        size_t tag_count = args_get_count(&args, "tag");
        if(tag_count == 0) {
            fprintf(stderr, "Error: at least one --tag= is required.\n");
            usage();
            args_free(&args);
            return EXIT_FAILURE;
        }

        bool is_pccc = (cfg.plc_type == PLC_PLC5 || cfg.plc_type == PLC_SLC || cfg.plc_type == PLC_MICROLOGIX);

        for(i = 0; i < tag_count; i++) {
            args_value_t val = args_get_at(&args, "tag", i);
            if(!val.present || !val.value.string_val) { continue; }
            bool ok = is_pccc ? parse_pccc_tag(val.value.string_val, &cfg) : parse_cip_tag(val.value.string_val, &cfg);
            if(!ok) {
                fprintf(stderr, "Error: failed to parse tag \"%s\".\n", val.value.string_val);
                args_free(&args);
                free_tags(cfg.tags);
                return EXIT_FAILURE;
            }
        }
    }

    args_free(&args);

    server.start_time_us = util_time_us();

    pdlog(LOG_MODULE_AB_SERVER, LOG_LEVEL_INFO, "ab_server_fiber starting on port %s", cfg.port_str);

    /* ----- Create fiber_net event loop ----- */
    rc = fiber_net_create(&server.net, 256, 32 * 1024, YAFL_STACK_FLAGS_MALLOC | YAFL_STACK_FLAGS_WATERMARK);
    if(rc != UTIL_OK) {
        pdlog(LOG_MODULE_AB_SERVER, LOG_LEVEL_ERROR, "fiber_net_create failed: %s", util_err_str(rc));
        free_tags(cfg.tags);
        return EXIT_FAILURE;
    }

    /* ----- Signal handler ----- */
    util_set_interrupt_handler(signal_handler);

    /* ----- Listener fiber ----- */
    {
        listener_ctx_t *lctx = (listener_ctx_t *)malloc(sizeof(listener_ctx_t));
        if(!lctx) {
            pdlog(LOG_MODULE_AB_SERVER, LOG_LEVEL_ERROR, "Failed to allocate listener context");
            fiber_net_destroy(&server.net);
            free_tags(cfg.tags);
            return EXIT_FAILURE;
        }
        lctx->server = &server;
        strncpy(lctx->bind_ip, "0.0.0.0", sizeof(lctx->bind_ip) - 1);
        lctx->bind_ip[sizeof(lctx->bind_ip) - 1] = '\0';
        lctx->port = (uint16_t)(unsigned int)atoi(cfg.port_str);

        rc = fiber_net_add_task(server.net, listener_fiber, lctx);
        if(rc != UTIL_OK) {
            pdlog(LOG_MODULE_AB_SERVER, LOG_LEVEL_ERROR, "Failed to add listener fiber: %s", util_err_str(rc));
            free(lctx);
            fiber_net_destroy(&server.net);
            free_tags(cfg.tags);
            return EXIT_FAILURE;
        }
    }

    /* ----- Run event loop ----- */
    pdlog(LOG_MODULE_AB_SERVER, LOG_LEVEL_INFO, "Entering fiber_net event loop");
    fiber_net_run(server.net, 50);

    pdlog(LOG_MODULE_AB_SERVER, LOG_LEVEL_INFO, "ab_server_fiber shutting down");

    print_statistics(&server);

    fiber_net_destroy(&server.net);
    free_tags(cfg.tags);

    return EXIT_SUCCESS;
}

/* ============================================================================
 * Static functions
 * ============================================================================ */

static void signal_handler(void) {
    if(g_server && g_server->net) { fiber_net_stop(g_server->net); }
}

/* ---- listener_fiber ---- */

static void *listener_fiber(void *arg) {
    listener_ctx_t *ctx = (listener_ctx_t *)arg;
    fiber_net_t *net = ctx->server->net;
    fiber_socket_t listen_fd = FIBER_INVALID_SOCKET;

    pdlog(LOG_MODULE_AB_LISTENER, LOG_LEVEL_INFO, "Listener starting on %s:%" PRIu16, ctx->bind_ip, ctx->port);

    util_err_t rc = fiber_socket_listen(net, &listen_fd, ctx->bind_ip, ctx->port, 128);
    if(rc != UTIL_OK) {
        pdlog(LOG_MODULE_AB_LISTENER, LOG_LEVEL_ERROR, "fiber_socket_listen failed: %s", util_err_str(rc));
        free(ctx);
        return NULL;
    }

    for(;;) {
        fiber_socket_t client_fd = FIBER_INVALID_SOCKET;
        rc = fiber_socket_accept(net, listen_fd, &client_fd, ACCEPT_TIMEOUT_MS);

        if(rc == UTIL_ETIMEOUT) { continue; }
        if(rc != UTIL_OK) {
            if(rc != UTIL_ECANCELLED) { pdlog(LOG_MODULE_AB_LISTENER, LOG_LEVEL_WARN, "Accept error: %s", util_err_str(rc)); }
            break;
        }

        pdlog(LOG_MODULE_AB_LISTENER, LOG_LEVEL_DETAIL, "Accepted client (fd %d)", (int)client_fd);

        client_ctx_t *cctx = (client_ctx_t *)malloc(sizeof(client_ctx_t));
        if(!cctx) {
            pdlog(LOG_MODULE_AB_LISTENER, LOG_LEVEL_ERROR, "Failed to allocate client context");
            fiber_socket_close(net, client_fd);
            continue;
        }
        cctx->server = ctx->server;
        cctx->sock = client_fd;

        rc = fiber_net_add_task(net, client_fiber, cctx);
        if(rc != UTIL_OK) {
            pdlog(LOG_MODULE_AB_LISTENER, LOG_LEVEL_ERROR, "Failed to add client fiber: %s", util_err_str(rc));
            fiber_socket_close(net, client_fd);
            free(cctx);
            continue;
        }
    }

    pdlog(LOG_MODULE_AB_LISTENER, LOG_LEVEL_INFO, "Listener stopping");
    fiber_socket_close(net, listen_fd);
    free(ctx);
    return NULL;
}

/* ---- client_fiber ---- */

static void *client_fiber(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    fiber_net_t *net = ctx->server->net;
    plc_config_t *cfg = ctx->server->cfg;
    fiber_socket_t sock = ctx->sock;
    eip_session_t sess = {0};
    Arena arena;

    if(arena_init(&arena, CLIENT_ARENA_SIZE) != UTIL_OK) {
        pdlog(LOG_MODULE_AB_CLIENT, LOG_LEVEL_ERROR, "Failed to allocate client arena");
        fiber_socket_close(net, sock);
        free(ctx);
        return NULL;
    }
    arena_set_stats(&arena, &ctx->server->stats.arena);

    sess.reject_fo_count = cfg->reject_fo_count;
    eip_session_set_unconnected_sizes(&sess, cfg->server_to_client_max_packet);

    ctx->server->stats.clients_connected++;
    pdlog(LOG_MODULE_AB_CLIENT, LOG_LEVEL_DETAIL, "Client fiber started");

    for(;;) {
        arena_reset(&arena);

        /* ---- Phase 1: Receive 24-byte EIP header ---- */
        Bytes hdr_buf = bytes_alloc(&arena, EIP_HEADER_SIZE);
        if(bytes_is_null(hdr_buf)) {
            pdlog(LOG_MODULE_AB_CLIENT, LOG_LEVEL_ERROR, "Arena exhausted allocating EIP header buffer");
            break;
        }

        util_err_t rc = fiber_socket_recv(net, sock, hdr_buf, IO_TIMEOUT_MS);
        if(rc != UTIL_OK) {
            if(rc != UTIL_ECLOSED && rc != UTIL_ECANCELLED) {
                pdlog(LOG_MODULE_AB_CLIENT, LOG_LEVEL_WARN, "EIP header recv error: %s", util_err_str(rc));
            }
            break;
        }

        /* ---- Phase 2: Extract payload length from EIP header ---- */
        uint16_t payload_len = 0;
        if(bytes_is_null(bytes_unpack(hdr_buf, BYTES_LE, BYTES_SKIP(2), &payload_len))) {
            pdlog(LOG_MODULE_AB_CLIENT, LOG_LEVEL_WARN, "Failed to read EIP payload length");
            break;
        }

        /* Protocol-level size guard: reject payloads that exceed the negotiated maximum.
         * This is the primary check — eip_dispatch repeats it on the parsed header. */
        if(sess.max_eip_packet_size > 0 && (size_t)payload_len > sess.max_eip_packet_size) {
            pdlog(LOG_MODULE_AB_CLIENT, LOG_LEVEL_WARN,
                  "EIP payload_len=%u exceeds negotiated max %zu — closing connection",
                  (unsigned)payload_len, sess.max_eip_packet_size);
            break;
        }

        /* Arena-level fallback: reject payload sizes that exceed remaining arena space. */
        if((size_t)payload_len > arena_remaining(&arena)) {
            pdlog(LOG_MODULE_AB_CLIENT, LOG_LEVEL_WARN,
                  "EIP payload_len=%u exceeds remaining arena space — closing connection", (unsigned)payload_len);
            break;
        }

        /* ---- Phase 3: Receive payload (may be zero bytes) ---- */
        Bytes payload_buf = {0};
        if(payload_len > 0) {
            payload_buf = bytes_alloc(&arena, payload_len);
            if(bytes_is_null(payload_buf)) {
                pdlog(LOG_MODULE_AB_CLIENT, LOG_LEVEL_ERROR, "Arena exhausted allocating payload buffer (len=%u)", payload_len);
                break;
            }

            rc = fiber_socket_recv(net, sock, payload_buf, IO_TIMEOUT_MS);
            if(rc != UTIL_OK) {
                if(rc != UTIL_ECLOSED && rc != UTIL_ECANCELLED) {
                    pdlog(LOG_MODULE_AB_CLIENT, LOG_LEVEL_WARN, "EIP payload recv error: %s", util_err_str(rc));
                }
                break;
            }
        }

        pdlog(LOG_MODULE_AB_CLIENT, LOG_LEVEL_DETAIL, "EIP request received, payload=%u bytes", (unsigned)payload_len);

        /* ---- Phase 4: Dispatch ---- */
        Bytes response = eip_dispatch(&arena, hdr_buf, payload_buf, &sess, cfg);

        /* ---- Phase 5: Optional artificial delay ---- */
        if(cfg->response_delay_ms > 0) { fiber_net_sleep_ms(net, (uint32_t)cfg->response_delay_ms); }

        /* null response means UnregisterSession or fatal parse error */
        if(bytes_is_null(response)) {
            pdlog(LOG_MODULE_AB_CLIENT, LOG_LEVEL_DETAIL, "EIP dispatch returned null — closing connection");
            break;
        }

        /* ---- Phase 7: Send response ---- */
        rc = fiber_socket_send(net, sock, response, IO_TIMEOUT_MS);
        if(rc != UTIL_OK) {
            if(rc != UTIL_ECANCELLED) {
                pdlog(LOG_MODULE_AB_CLIENT, LOG_LEVEL_WARN, "EIP response send error: %s", util_err_str(rc));
            }
            break;
        }

        ctx->server->stats.total_requests++;
    }

    pdlog(LOG_MODULE_AB_CLIENT, LOG_LEVEL_DETAIL, "Client fiber closing");
    ctx->server->stats.clients_disconnected++;
    fiber_socket_close(net, sock);
    arena_free(&arena);
    free(ctx);
    return NULL;
}

/* ---- Statistics ---- */

static void print_statistics(server_ctx_t *server) {
    if(!server) { return; }

    int64_t end_us = util_time_us();
    double runtime_sec = (double)(end_us - server->start_time_us) / 1000000.0;
    server_stats_t *stats = &server->stats;

    fflush(stderr);

    fprintf(stderr, "\n");
    fprintf(stderr, "╔══════════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║          AB SERVER (FIBER) PERFORMANCE STATISTICS                ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║ Runtime: %.2f seconds\n", runtime_sec);
    fprintf(stderr, "║ Total EIP requests: %" PRId64 "\n", (int64_t)stats->total_requests);
    if(runtime_sec > 0.0) { fprintf(stderr, "║ Throughput: %.2f requests/sec\n", (double)stats->total_requests / runtime_sec); }
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║ Clients connected:    %" PRId64 "\n", (int64_t)stats->clients_connected);
    fprintf(stderr, "║ Clients disconnected: %" PRId64 "\n", (int64_t)stats->clients_disconnected);

    /* ----- Arena statistics ----- */
    if(stats->arena.reset_count > 0) {
        double avg_bytes = (double)stats->arena.use_total / (double)stats->arena.reset_count;
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║              ARENA USAGE PER REQUEST                             ║\n");
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║  Requests measured: %7zu\n", stats->arena.reset_count);
        fprintf(stderr, "║  Avg usage:         %7.0f bytes\n", avg_bytes);
        fprintf(stderr, "║  Min usage:         %7zu bytes\n", stats->arena.use_min);
        fprintf(stderr, "║  Max usage:         %7zu bytes\n", stats->arena.use_max);
        fprintf(stderr, "║  Arena capacity:    %7u bytes\n", CLIENT_ARENA_SIZE);
    }

    /* ----- Per-tag statistics ----- */
    if(server->cfg && server->cfg->tags) {
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║ PER-TAG STATISTICS\n");
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        tag_def_t *tag = server->cfg->tags;
        while(tag) {
            if(tag->request_count > 0) {
                double avg_us = (double)tag->total_latency_us / (double)tag->request_count;
                fprintf(stderr, "║  %-20s  requests: %8" PRId64 "  avg: %8.2f us  min: %8" PRId64 " us  max: %8" PRId64 " us\n",
                        tag->name ? tag->name : "(pccc)", (int64_t)tag->request_count, avg_us, (int64_t)tag->min_latency_us,
                        (int64_t)tag->max_latency_us);
            }
            tag = tag->next_tag;
        }
    }

    /* ----- fiber_net event loop instrumentation ----- */
    if(server->net) {
        fiber_net_loop_stats_t ls;
        fiber_net_get_loop_stats(server->net, &ls);

        double timeout_pct = ls.loop_iterations > 0 ? (double)ls.poll_timeout_iters / (double)ls.loop_iterations * 100.0 : 0.0;
        double wakeup_pct = ls.loop_iterations > 0 ? (double)ls.poll_wakeup_iters / (double)ls.loop_iterations * 100.0 : 0.0;
        double avg_overhead_us =
            ls.avg_step1_us + ls.avg_step2_us + ls.avg_step4_us + ls.avg_step5_scan_us + ls.avg_step5_resume_us;

        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║              FIBER_NET EVENT LOOP INSTRUMENTATION                ║\n");
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║ Loop iterations:   %10" PRId64 "\n", (int64_t)ls.loop_iterations);
        fprintf(stderr, "║ Poll timeouts:     %10" PRId64 "  (%5.1f%% of iters)\n", (int64_t)ls.poll_timeout_iters,
                timeout_pct);
        fprintf(stderr, "║ Poll wakeups:      %10" PRId64 "  (%5.1f%% of iters)\n", (int64_t)ls.poll_wakeup_iters, wakeup_pct);
        fprintf(stderr, "║ Total resumes:     %10" PRId64 "\n", (int64_t)ls.total_resumes);
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║              LOOP PHASE BREAKDOWN (avg µs / iteration)           ║\n");
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║  Step 1 — pending-start scan + initial resumes: %8.3f us\n", ls.avg_step1_us);
        fprintf(stderr, "║  Step 2 — pollfd array build:                   %8.3f us\n", ls.avg_step2_us);
        fprintf(stderr, "║  Step 3 — poll() [includes idle blocking time]: %8.1f us\n", ls.avg_step3_us);
        fprintf(stderr, "║  Step 4 — wakeup pipe drain:                    %8.3f us\n", ls.avg_step4_us);
        fprintf(stderr, "║  Step 5 — ready-fd scan (no resumes):           %8.3f us\n", ls.avg_step5_scan_us);
        fprintf(stderr, "║  Step 5 — fiber resumes (exec + ctx switch):    %8.3f us\n", ls.avg_step5_resume_us);
        fprintf(stderr, "║  ─────────────────────────────────────────────────────────\n");
        fprintf(stderr, "║  Total non-poll overhead / iteration:           %8.3f us\n", avg_overhead_us);

        if(ls.watermark_count > 0) {
            double sz = ls.stack_size_bytes > 0 ? (double)ls.stack_size_bytes : 1.0;
            fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
            fprintf(stderr, "║              STACK HIGH-WATERMARK                                ║\n");
            fprintf(stderr, "╠══════════════════════════════════════════════════════════════════╣\n");
            fprintf(stderr, "║  Stack size (configured): %7zu bytes (%zu KB)\n", ls.stack_size_bytes,
                    ls.stack_size_bytes / 1024u);
            fprintf(stderr, "║  Fibers measured:         %7" PRId64 "\n", (int64_t)ls.watermark_count);
            fprintf(stderr, "║  Avg stack used:          %7.0f bytes (%4.1f%%)\n", ls.avg_stack_used_bytes,
                    ls.avg_stack_used_bytes / sz * 100.0);
            fprintf(stderr, "║  Min stack used:          %7zu bytes (%4.1f%%)\n", ls.min_stack_used_bytes,
                    (double)ls.min_stack_used_bytes / sz * 100.0);
            fprintf(stderr, "║  Max stack used:          %7zu bytes (%4.1f%%)\n", ls.max_stack_used_bytes,
                    (double)ls.max_stack_used_bytes / sz * 100.0);
        }
    }

    fprintf(stderr, "╚══════════════════════════════════════════════════════════════════╝\n");
    fflush(stderr);
}

/* ---- Argument helpers ---- */

static void usage(void) {
    fprintf(stderr, "Usage: ab_server_fiber --plc=<type> [--path=<path>] [--port=<port>]\n"
                    "                       --tag=<tag> [--tag=<tag> ...]\n"
                    "                       [--debug=<level>] [--reject_fo=<n>] [--delay=<ms>]\n"
                    "\n"
                    "  <type>  = ControlLogix, Micro800, Omron, PLC/5, SLC500, Micrologix\n"
                    "  <path>  = (required for ControlLogix) e.g. \"1,0\"\n"
                    "  <port>  = TCP port, default 44818\n"
                    "  <level> = ERROR, WARN, INFO, DETAIL, SPEW (default INFO)\n"
                    "\n"
                    "  PCCC tag format:  <file>[<size>]\n"
                    "    B3[n]   1-bit boolean file (stored as uint16)\n"
                    "    N7[n]   2-byte signed integer\n"
                    "    F8[n]   4-byte floating point\n"
                    "    ST18[n] 82-byte ASCII string\n"
                    "    L19[n]  4-byte signed long integer\n"
                    "\n"
                    "  CIP tag format:  <name>:<TYPE>[<d>[,<d>[,<d>]]]\n"
                    "    SINT   1-byte signed integer\n"
                    "    INT    2-byte signed integer\n"
                    "    DINT   4-byte signed integer\n"
                    "    LINT   8-byte signed integer\n"
                    "    REAL   4-byte floating point\n"
                    "    LREAL  8-byte floating point\n"
                    "    STRING 88-byte string (4-byte count + 82 data + 2 pad)\n"
                    "    BOOL   1-byte boolean\n"
                    "\n"
                    "Examples:\n"
                    "  ab_server_fiber --plc=ControlLogix --path=1,0 --tag=MyTag:DINT[10,10]\n"
                    "  ab_server_fiber --plc=Micrologix   --tag=B3[10] --tag=N7[10]\n");
}

static bool setup_plc_type(const char *plc_str, plc_config_t *cfg) {
    if(str_cmp_i(plc_str, "ControlLogix") == 0) {
        cfg->plc_type = PLC_CONTROL_LOGIX;
        cfg->path[0] = 0x00; /* filled by parse_path */
        cfg->path[1] = 0x00;
        cfg->path[2] = 0x20;
        cfg->path[3] = 0x02;
        cfg->path[4] = 0x24;
        cfg->path[5] = 0x01;
        cfg->path_len = 6;
        cfg->client_to_server_max_packet = 504;
        cfg->server_to_client_max_packet = 504;
        return true;
    }
    if(str_cmp_i(plc_str, "Micro800") == 0) {
        cfg->plc_type = PLC_MICRO800;
        cfg->path[0] = 0x20;
        cfg->path[1] = 0x02;
        cfg->path[2] = 0x24;
        cfg->path[3] = 0x01;
        cfg->path_len = 4;
        cfg->client_to_server_max_packet = 504;
        cfg->server_to_client_max_packet = 504;
        return true;
    }
    if(str_cmp_i(plc_str, "Omron") == 0) {
        cfg->plc_type = PLC_OMRON;
        cfg->path[0] = 0x12;
        cfg->path[1] = 0x09;
        cfg->path[2] = 0x31;
        cfg->path[3] = 0x32;
        cfg->path[4] = 0x37;
        cfg->path[5] = 0x2e;
        cfg->path[6] = 0x30;
        cfg->path[7] = 0x2e;
        cfg->path[8] = 0x30;
        cfg->path[9] = 0x2e;
        cfg->path[10] = 0x31;
        cfg->path[11] = 0x00;
        cfg->path[12] = 0x20;
        cfg->path[13] = 0x02;
        cfg->path[14] = 0x24;
        cfg->path[15] = 0x01;
        cfg->path_len = 16;
        cfg->client_to_server_max_packet = 504;
        cfg->server_to_client_max_packet = 504;
        return true;
    }
    if(str_cmp_i(plc_str, "PLC/5") == 0) {
        cfg->plc_type = PLC_PLC5;
        cfg->path[0] = 0x20;
        cfg->path[1] = 0x02;
        cfg->path[2] = 0x24;
        cfg->path[3] = 0x01;
        cfg->path_len = 4;
        cfg->client_to_server_max_packet = 244;
        cfg->server_to_client_max_packet = 244;
        return true;
    }
    if(str_cmp_i(plc_str, "SLC500") == 0) {
        cfg->plc_type = PLC_SLC;
        cfg->path[0] = 0x20;
        cfg->path[1] = 0x02;
        cfg->path[2] = 0x24;
        cfg->path[3] = 0x01;
        cfg->path_len = 4;
        cfg->client_to_server_max_packet = 244;
        cfg->server_to_client_max_packet = 244;
        return true;
    }
    if(str_cmp_i(plc_str, "Micrologix") == 0) {
        cfg->plc_type = PLC_MICROLOGIX;
        cfg->path[0] = 0x20;
        cfg->path[1] = 0x02;
        cfg->path[2] = 0x24;
        cfg->path[3] = 0x01;
        cfg->path_len = 4;
        cfg->client_to_server_max_packet = 244;
        cfg->server_to_client_max_packet = 244;
        return true;
    }
    return false;
}

static bool parse_path(const char *path_str, plc_config_t *cfg) {
    int a = 0;
    int b = 0;
    if(sscanf(path_str, "%d,%d", &a, &b) != 2) { return false; }
    cfg->path[0] = (uint8_t)a;
    cfg->path[1] = (uint8_t)b;
    pdlog(LOG_MODULE_AB_SERVER, LOG_LEVEL_INFO, "Path: %d,%d", a, b);
    return true;
}

/*
 * PCCC tags: <file>[<size>]
 * e.g. B3[100], N7[50], F8[20], ST18[10], L19[30]
 */
static bool parse_pccc_tag(const char *tag_str, plc_config_t *cfg) {
    char file_name[32] = {0};
    size_t name_len = 0;
    size_t elem_size = 0;
    size_t data_file_num = 0;
    tag_type_t tag_type = 0;
    size_t count = 0;
    size_t pos = 0;
    char size_buf[32] = {0};
    size_t size_len = 0;

    /* Extract data file name (alpha+digits). */
    name_len = strspn(tag_str, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
    if(name_len == 0 || name_len >= sizeof(file_name)) {
        fprintf(stderr, "PCCC tag: cannot parse file name from \"%s\"\n", tag_str);
        return false;
    }
    memcpy(file_name, tag_str, name_len);
    file_name[name_len] = '\0';
    pos = name_len;

    /* Match data file type. */
    if(str_cmp_i(file_name, "B3") == 0) {
        tag_type = TAG_PCCC_TYPE_BIT;
        elem_size = 2;
        data_file_num = 3;
    } else if(str_cmp_i(file_name, "N7") == 0) {
        tag_type = TAG_PCCC_TYPE_INT;
        elem_size = 2;
        data_file_num = 7;
    } else if(str_cmp_i(file_name, "F8") == 0) {
        tag_type = TAG_PCCC_TYPE_REAL;
        elem_size = 4;
        data_file_num = 8;
    } else if(str_cmp_i(file_name, "ST18") == 0) {
        tag_type = TAG_PCCC_TYPE_STRING;
        elem_size = 84;
        data_file_num = 18;
    } else if(str_cmp_i(file_name, "L19") == 0) {
        tag_type = TAG_PCCC_TYPE_DINT;
        elem_size = 4;
        data_file_num = 19;
    } else {
        fprintf(stderr, "PCCC tag: unknown data file \"%s\"\n", file_name);
        return false;
    }

    /* Expect '['. */
    if(tag_str[pos] != '[') {
        fprintf(stderr, "PCCC tag: expected '[' after file name in \"%s\"\n", tag_str);
        return false;
    }
    pos++;

    /* Extract size digits. */
    size_len = strspn(tag_str + pos, "0123456789");
    if(size_len == 0 || size_len >= sizeof(size_buf)) {
        fprintf(stderr, "PCCC tag: cannot parse size in \"%s\"\n", tag_str);
        return false;
    }
    memcpy(size_buf, tag_str + pos, size_len);
    size_buf[size_len] = '\0';
    pos += size_len;

    if(tag_str[pos] != ']') {
        fprintf(stderr, "PCCC tag: expected ']' after size in \"%s\"\n", tag_str);
        return false;
    }

    if(sscanf(size_buf, "%zu", &count) != 1 || count == 0) {
        fprintf(stderr, "PCCC tag: invalid size in \"%s\"\n", tag_str);
        return false;
    }

    tag_def_t *tag = (tag_def_t *)calloc(1, sizeof(tag_def_t));
    if(!tag) {
        pdlog(LOG_MODULE_AB_SERVER, LOG_LEVEL_WARN, "parse_pccc_tag: out of memory allocating tag_def_t");
        return false;
    }

    tag->tag_type = tag_type;
    tag->elem_size = elem_size;
    tag->data_file_num = data_file_num;
    tag->elem_count = count;
    tag->num_dimensions = 1;
    tag->dimensions[0] = count;
    tag->dimensions[1] = 1;
    tag->dimensions[2] = 1;
    tag->name = strdup(file_name);
    if(!tag->name) {
        pdlog(LOG_MODULE_AB_SERVER, LOG_LEVEL_WARN, "parse_pccc_tag: out of memory duplicating tag name");
        free(tag);
        return false;
    }

    tag->data = (uint8_t *)calloc(count, elem_size);
    if(!tag->data) {
        pdlog(LOG_MODULE_AB_SERVER, LOG_LEVEL_WARN, "parse_pccc_tag: out of memory allocating %zu bytes of tag data", count * elem_size);
        free(tag->name);
        free(tag);
        return false;
    }

    pdlog(LOG_MODULE_AB_SERVER, LOG_LEVEL_INFO, "PCCC tag %s: file=%zu type=0x%04x count=%zu elem_size=%zu", file_name,
          data_file_num, (unsigned)tag_type, count, elem_size);

    tag->next_tag = cfg->tags;
    cfg->tags = tag;
    return true;
}

/*
 * CIP tags: <name>:<TYPE>[<d>[,<d>[,<d>]]]
 * e.g. MyTag:DINT[10,10]
 */
static bool parse_cip_tag(const char *tag_str, plc_config_t *cfg) {
    char tag_name[200] = {0};
    char type_str[64] = {0};
    char dim_str[64] = {0};
    size_t pos = 0;
    size_t name_len = 0;
    size_t type_len = 0;
    size_t dim_len = 0;
    size_t elem_size = 0;
    tag_type_t tag_type = 0;
    size_t dims[3] = {0, 0, 0};
    int num_dims_found = 0;

    /* Tag name: alphanumeric + underscore. */
    name_len = strspn(tag_str, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_");
    if(name_len == 0 || name_len >= sizeof(tag_name)) {
        fprintf(stderr, "CIP tag: cannot parse name from \"%s\"\n", tag_str);
        return false;
    }
    memcpy(tag_name, tag_str, name_len);
    tag_name[name_len] = '\0';
    pos = name_len;

    if(tag_str[pos] != ':') {
        fprintf(stderr, "CIP tag: expected ':' after name in \"%s\"\n", tag_str);
        return false;
    }
    pos++;

    /* Type: alpha only. */
    type_len = strspn(tag_str + pos, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
    if(type_len == 0 || type_len >= sizeof(type_str)) {
        fprintf(stderr, "CIP tag: cannot parse type from \"%s\"\n", tag_str);
        return false;
    }
    memcpy(type_str, tag_str + pos, type_len);
    type_str[type_len] = '\0';
    pos += type_len;

    if(tag_str[pos] != '[') {
        fprintf(stderr, "CIP tag: expected '[' after type in \"%s\"\n", tag_str);
        return false;
    }
    pos++;

    /* Dimensions: digits and commas. */
    dim_len = strspn(tag_str + pos, "0123456789,");
    if(dim_len == 0 || dim_len >= sizeof(dim_str)) {
        fprintf(stderr, "CIP tag: cannot parse dimensions from \"%s\"\n", tag_str);
        return false;
    }
    memcpy(dim_str, tag_str + pos, dim_len);
    dim_str[dim_len] = '\0';
    pos += dim_len;

    if(tag_str[pos] != ']') {
        fprintf(stderr, "CIP tag: expected ']' after dimensions in \"%s\"\n", tag_str);
        return false;
    }

    /* Match type. */
    if(str_cmp_i(type_str, "SINT") == 0) {
        tag_type = TAG_CIP_TYPE_SINT;
        elem_size = 1;
    } else if(str_cmp_i(type_str, "INT") == 0) {
        tag_type = TAG_CIP_TYPE_INT;
        elem_size = 2;
    } else if(str_cmp_i(type_str, "DINT") == 0) {
        tag_type = TAG_CIP_TYPE_DINT;
        elem_size = 4;
    } else if(str_cmp_i(type_str, "LINT") == 0) {
        tag_type = TAG_CIP_TYPE_LINT;
        elem_size = 8;
    } else if(str_cmp_i(type_str, "REAL") == 0) {
        tag_type = TAG_CIP_TYPE_REAL;
        elem_size = 4;
    } else if(str_cmp_i(type_str, "LREAL") == 0) {
        tag_type = TAG_CIP_TYPE_LREAL;
        elem_size = 8;
    } else if(str_cmp_i(type_str, "STRING") == 0) {
        tag_type = TAG_CIP_TYPE_STRING;
        elem_size = 88;
    } else if(str_cmp_i(type_str, "BOOL") == 0) {
        tag_type = TAG_CIP_TYPE_BOOL;
        elem_size = 1;
    } else {
        fprintf(stderr, "CIP tag: unknown type \"%s\" in \"%s\"\n", type_str, tag_str);
        return false;
    }

    /* Parse dimensions. */
    num_dims_found = sscanf(dim_str, "%zu,%zu,%zu", &dims[0], &dims[1], &dims[2]);
    if(num_dims_found < 1 || dims[0] == 0) {
        fprintf(stderr, "CIP tag: invalid dimensions in \"%s\"\n", tag_str);
        return false;
    }

    tag_def_t *tag = (tag_def_t *)calloc(1, sizeof(tag_def_t));
    if(!tag) {
        pdlog(LOG_MODULE_AB_SERVER, LOG_LEVEL_WARN, "parse_cip_tag: out of memory allocating tag_def_t");
        return false;
    }

    tag->tag_type = tag_type;
    tag->elem_size = elem_size;
    tag->dimensions[0] = dims[0];
    tag->dimensions[1] = (dims[1] > 0) ? dims[1] : 1;
    tag->dimensions[2] = (dims[2] > 0) ? dims[2] : 1;

    tag->num_dimensions = (size_t)num_dims_found;
    tag->elem_count = tag->dimensions[0] * tag->dimensions[1] * tag->dimensions[2];

    tag->name = strdup(tag_name);
    if(!tag->name) {
        pdlog(LOG_MODULE_AB_SERVER, LOG_LEVEL_WARN, "parse_cip_tag: out of memory duplicating tag name");
        free(tag);
        return false;
    }

    tag->data = (uint8_t *)calloc(tag->elem_count, elem_size);
    if(!tag->data) {
        pdlog(LOG_MODULE_AB_SERVER, LOG_LEVEL_WARN, "parse_cip_tag: out of memory allocating %zu bytes of tag data", tag->elem_count * elem_size);
        free(tag->name);
        free(tag);
        return false;
    }

    pdlog(LOG_MODULE_AB_SERVER, LOG_LEVEL_INFO, "CIP tag %s: type=0x%04x dims=[%zu,%zu,%zu] elem_size=%zu", tag_name,
          (unsigned)tag_type, tag->dimensions[0], tag->dimensions[1], tag->dimensions[2], elem_size);

    tag->next_tag = cfg->tags;
    cfg->tags = tag;
    return true;
}

static void free_tags(tag_def_t *tags) {
    tag_def_t *t = tags;
    while(t) {
        tag_def_t *next = t->next_tag;
        if(t->name) { free(t->name); }
        if(t->data) { free(t->data); }
        free(t);
        t = next;
    }
}
