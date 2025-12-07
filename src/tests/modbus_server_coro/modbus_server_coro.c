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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>

#include "modbus_protocol.h"
#include "register_storage.h"
#include "coro_net.h"
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

static server_ctx_t *g_server = NULL;

/* ============================================================================
 * Server Context
 * ============================================================================ */

struct server_ctx_s {
    register_storage_t *storage;
    volatile int running;
};

/* ============================================================================
 * Modbus Client Context (per connection)
 * ============================================================================ */

struct modbus_client_s {
    Task base;
    server_ctx_t *server;
    uint8_t recv_buffer[MODBUS_RECV_BUFFER_SIZE];
    uint8_t send_buffer[MODBUS_SEND_BUFFER_SIZE];
    buf_t recv_buf;
    buf_t send_buf;
    mbap_header_t mbap_header;
    uint16_t expected_pdu_length;
};

/* ============================================================================
 * Signal Handling
 * ============================================================================ */

static void signal_handler(int signum) {
    (void)signum;
    if (g_server) {
        g_server->running = 0;
        coro_stop();
    }
}

/* ============================================================================
 * Client Coroutine Handler - SIMPLIFIED VERSION
 * ============================================================================ */

static void client_handler(Task *t) {
    modbus_client_t *client = (modbus_client_t *)t->context;
    ssize_t bytes_received;
    uint8_t function_code;
    util_err_t err;

    CR_START(t);

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_DETAIL, "Client handler started");

    /* Read data into buffer */
    while (1) {
        /* Attempt to read more data */
        ssize_t r = recv(t->fd, client->recv_buffer + client->recv_buf.write,
                        MODBUS_RECV_BUFFER_SIZE - client->recv_buf.write, 0);

        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                CR_YIELD(t, POLLIN);
                continue;
            } else {
                pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "recv() failed: %d", errno);
                break;
            }
        } else if (r == 0) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_DETAIL, "Client disconnected");
            break;
        }

        client->recv_buf.write += r;

        /* Check if we have at least the MBAP header */
        if (client->recv_buf.write < MBAP_HEADER_SIZE) {
            continue;
        }

        /* Parse MBAP header if not done yet */
        if (client->recv_buf.read == 0) {
            buf_t header_buf = client->recv_buf;
            header_buf.write = MBAP_HEADER_SIZE;

            err = modbus_parse_mbap_header(&header_buf, &client->mbap_header);
            if (err != UTIL_OK) {
                pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_WARN, "Failed to parse MBAP header");
                break;
            }

            client->expected_pdu_length = client->mbap_header.length - 1;
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_DETAIL, "MBAP header: txn_id=%u, length=%u",
                            client->mbap_header.transaction_id,
                            client->mbap_header.length);
        }

        /* Check if we have the complete PDU */
        size_t expected_total = MBAP_HEADER_SIZE + client->expected_pdu_length;
        if (client->recv_buf.write < expected_total) {
            continue;
        }

        /* We have a complete request, process it */
        buf_reset(&client->recv_buf);
        client->recv_buf.write = client->recv_buf.read + expected_total;

        /* Skip the MBAP header in the request buffer */
        buf_read_advance(&client->recv_buf, MBAP_HEADER_SIZE);

        /* Extract function code */
        if (!buf_read_u8(&client->recv_buf, "function_code", &function_code)) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_WARN, "Failed to read function code");
            break;
        }

        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_DETAIL, "Processing function code 0x%02x", function_code);

        /* Reset send buffer and generate response */
        buf_reset(&client->send_buf);
        err = modbus_process_request(
            function_code,
            &client->recv_buf,
            &client->send_buf,
            &client->mbap_header,
            client->server->storage
        );

        if (err != UTIL_OK) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_WARN, "Request processing failed");
            buf_reset(&client->send_buf);
            modbus_build_exception_response(
                &client->send_buf,
                &client->mbap_header,
                function_code,
                err
            );
        }

        /* Send response */
        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_DETAIL, "Sending response of %zu bytes", client->send_buf.write);

        while (client->send_buf.read < client->send_buf.write) {
            ssize_t s = send(t->fd, client->send_buf.data + client->send_buf.read,
                           client->send_buf.write - client->send_buf.read, 0);

            if (s < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    CR_YIELD(t, POLLOUT);
                    continue;
                } else {
                    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "send() failed: %d", errno);
                    break;
                }
            }

            client->send_buf.read += s;
        }

        /* Reset for next request */
        buf_reset(&client->recv_buf);
        buf_reset(&client->send_buf);
        client->expected_pdu_length = 0;
    }

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_DETAIL, "Client handler closing");
    free(client);
    CR_END(t);
}

/* ============================================================================
 * Listener Coroutine Handler
 * ============================================================================ */

typedef struct {
    server_ctx_t *server;
    char bind_address[256];
    uint16_t bind_port;
} listener_info_t;

static void listener_handler(Task *t) {
    listener_info_t *listener = (listener_info_t *)t->context;
    CSOCKET client_fd;

    CR_START(t);

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Listener started on %s:%u", listener->bind_address, listener->bind_port);

    while (listener->server->running) {
        CR_ACCEPT(t, client_fd);

        if (client_fd == (CSOCKET)-1) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Failed to accept connection");
            break;
        }

        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_DETAIL, "Accepted client connection");

        /* Create client context */
        modbus_client_t *client = (modbus_client_t *)malloc(sizeof(modbus_client_t));
        if (!client) {
            pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_ERROR, "Failed to allocate client context");
            CS_CLOSE(client_fd);
            continue;
        }

        memset(client, 0, sizeof(modbus_client_t));
        client->base.fd = client_fd;
        client->base.line = 0;
        client->base.handler = client_handler;
        client->base.events = POLLIN;

        client->recv_buf = buf_init(client->recv_buffer, MODBUS_RECV_BUFFER_SIZE);
        client->send_buf = buf_init(client->send_buffer, MODBUS_SEND_BUFFER_SIZE);
        client->server = listener->server;
        client->expected_pdu_length = 0;

        /* Register with event loop - client is stored in the Task, not separate context */
        coro_add(client_fd, client_handler, (void *)client);
    }

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Listener stopping");
    free(t->context);
    CR_END(t);
}

/* ============================================================================
 * Main Entry Point
 * ============================================================================ */

int main(int argc, char *argv[]) {
    args_result_t args_result = {0};
    server_ctx_t server = {0};
    g_server = &server;
    server.running = 1;

    args_flag_def_t flags[] = {
        {
            "listen",
            ARGS_TYPE_STRING,
            ARGS_OPTIONAL,
            ARGS_MULTIPLE,
            "server.listen",
            "Address and port to listen on (address:port)",
            { .has_default = false }
        },
        {
            "debug",
            ARGS_TYPE_STRING,
            ARGS_OPTIONAL,
            ARGS_ONCE,
            "logging.debug",
            "Debug level (ERROR, WARN, INFO, DETAIL, SPEW)",
            { .has_default = true, .value.string_val = "INFO" }
        },
        {
            "coils",
            ARGS_TYPE_INT,
            ARGS_OPTIONAL,
            ARGS_ONCE,
            "modbus.coils",
            "Number of coils",
            { .has_default = true, .value.int_val = 1000 }
        },
        {
            "discrete-inputs",
            ARGS_TYPE_INT,
            ARGS_OPTIONAL,
            ARGS_ONCE,
            "modbus.discrete_inputs",
            "Number of discrete inputs",
            { .has_default = true, .value.int_val = 1000 }
        },
        {
            "holding-registers",
            ARGS_TYPE_INT,
            ARGS_OPTIONAL,
            ARGS_ONCE,
            "modbus.holding_registers",
            "Number of holding registers",
            { .has_default = true, .value.int_val = 1000 }
        },
        {
            "input-registers",
            ARGS_TYPE_INT,
            ARGS_OPTIONAL,
            ARGS_ONCE,
            "modbus.input_registers",
            "Number of input registers",
            { .has_default = true, .value.int_val = 1000 }
        },
        {
            "help",
            ARGS_TYPE_BOOL,
            ARGS_OPTIONAL,
            ARGS_ONCE,
            "general.help",
            "Show this help message",
            { .has_default = true, .value.bool_val = false }
        },
    };
    const size_t num_flags = sizeof(flags) / sizeof(flags[0]);

    util_err_t parse_rc = args_parse(argc, (const char **)argv, flags, num_flags, &args_result);
    if (parse_rc != UTIL_OK) {
        printf("Error: %s\n", args_get_error_detail(&args_result));
        args_print_help(argv[0], flags, num_flags);
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    if (args_get_bool(&args_result, "help")) {
        args_print_help(argv[0], flags, num_flags);
        args_free(&args_result);
        return EXIT_SUCCESS;
    }

    const char *debug_level_str = args_get_string(&args_result, "debug");
    log_level_t log_level = LOG_LEVEL_INFO;
    if (debug_level_str) {
        if (strcmp(debug_level_str, "ERROR") == 0) {
            log_level = LOG_LEVEL_ERROR;
        } else if (strcmp(debug_level_str, "WARN") == 0) {
            log_level = LOG_LEVEL_WARN;
        } else if (strcmp(debug_level_str, "INFO") == 0) {
            log_level = LOG_LEVEL_INFO;
        } else if (strcmp(debug_level_str, "DETAIL") == 0) {
            log_level = LOG_LEVEL_DETAIL;
        } else if (strcmp(debug_level_str, "SPEW") == 0) {
            log_level = LOG_LEVEL_SPEW;
        }
    }
    log_set_all_modules(log_level);

    int64_t coils_val = args_get_int(&args_result, "coils");
    int64_t di_val = args_get_int(&args_result, "discrete-inputs");
    int64_t hr_val = args_get_int(&args_result, "holding-registers");
    int64_t ir_val = args_get_int(&args_result, "input-registers");

    if (coils_val < 0 || coils_val > 65535 ||
        di_val < 0 || di_val > 65535 ||
        hr_val < 0 || hr_val > 65535 ||
        ir_val < 0 || ir_val > 65535) {
        printf("Error: Register counts must be 0-65535\n");
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    register_storage_t *temp_storage = register_storage_create(
        (size_t)coils_val,
        (size_t)di_val,
        (size_t)hr_val,
        (size_t)ir_val
    );
    if (!temp_storage) {
        printf("Error: Failed to create storage\n");
        args_free(&args_result);
        return EXIT_FAILURE;
    }
    server.storage = temp_storage;

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Modbus server starting");
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Coils: %zu", (size_t)coils_val);
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Discrete Inputs: %zu", (size_t)di_val);
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Holding Registers: %zu", (size_t)hr_val);
    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "  Input Registers: %zu", (size_t)ir_val);

    coro_init();
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    size_t listen_count = args_get_count(&args_result, "listen");
    if (listen_count == 0) {
        listen_count = 1;
    }

    for (size_t i = 0; i < listen_count; i++) {
        const char *listen_addr = NULL;

        /* Since listen is ARGS_MULTIPLE, always use args_get_at() */
        args_value_t val = args_get_at(&args_result, "listen", i);
        if (val.present) {
            listen_addr = val.value.string_val;
        }

        if (!listen_addr) {
            listen_addr = "127.0.0.1:502";
        }

        char addr_copy[256];
        strncpy(addr_copy, listen_addr, sizeof(addr_copy) - 1);
        addr_copy[sizeof(addr_copy) - 1] = '\0';

        char *colon = strchr(addr_copy, ':');
        if (!colon) {
            printf("Error: Invalid address format: %s\n", listen_addr);
            args_free(&args_result);
            register_storage_destroy(temp_storage);
            return EXIT_FAILURE;
        }

        *colon = '\0';
        char *addr_str = addr_copy;
        uint16_t port = (uint16_t)atoi(colon + 1);

        pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Creating listener on %s:%u", addr_str, port);

        CSOCKET listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd == (CSOCKET)-1) {
            perror("socket");
            args_free(&args_result);
            register_storage_destroy(temp_storage);
            return EXIT_FAILURE;
        }

        int reuse = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, addr_str, &addr.sin_addr);

        if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind");
            CS_CLOSE(listen_fd);
            args_free(&args_result);
            register_storage_destroy(temp_storage);
            return EXIT_FAILURE;
        }

        if (listen(listen_fd, 128) < 0) {
            perror("listen");
            CS_CLOSE(listen_fd);
            args_free(&args_result);
            register_storage_destroy(temp_storage);
            return EXIT_FAILURE;
        }

        listener_info_t *listener_info = (listener_info_t *)malloc(sizeof(listener_info_t));
        if (!listener_info) {
            printf("Error: Failed to allocate listener info\n");
            CS_CLOSE(listen_fd);
            args_free(&args_result);
            register_storage_destroy(temp_storage);
            return EXIT_FAILURE;
        }

        memset(listener_info, 0, sizeof(listener_info_t));
        listener_info->server = &server;
        strncpy(listener_info->bind_address, addr_str, sizeof(listener_info->bind_address) - 1);
        listener_info->bind_port = port;

        coro_add(listen_fd, listener_handler, listener_info);
    }

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Modbus server running");
    coro_run();

    pdlog(LOG_MODULE_MODBUS_SERVER, LOG_LEVEL_INFO, "Modbus server shutting down");
    args_free(&args_result);
    register_storage_destroy(temp_storage);

    return EXIT_SUCCESS;
}
