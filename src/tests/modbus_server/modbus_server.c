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

#include "modbus_protocol.h"
#include "register_storage.h"
#include "log.h"
#include "reactor.h"
#include "fsm.h"
#include "socket.h"
#include "err.h"
#include "buf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* Forward declarations */
typedef struct server_ctx_s server_ctx_t;
typedef struct client_ctx_s client_ctx_t;
typedef struct listener_ctx_s listener_ctx_t;

/* Server context - holds reactor and storage (accessed via signal handler) */
struct server_ctx_s {
    reactor_t *reactor;
    register_storage_t *storage;
};

/* Single static variable for signal handler - this is necessary because
 * signal handlers can only take an int parameter and cannot pass context */
static server_ctx_t *g_server = NULL;

/* FSM event type IDs (application-defined) */
enum {
    APP_EVENT_NONE = REACTOR_EVENT_MAX,
    APP_EVENT_PROCESS,
    APP_EVENT_IDLE,
};

/* FSM State IDs (application-defined) */
enum {
    APP_STATE_LISTENING = 1,
    APP_STATE_READING_HEADER,
    APP_STATE_READING_PDU,
    APP_STATE_PROCESSING,
    APP_STATE_SENDING,
    APP_STATE_IDLE,
    APP_STATE_CLOSING,
};

/* Listener context - one per listen endpoint */
struct listener_ctx_s {
    server_ctx_t *server;
    socket_t listener_socket;
    char bind_address[256];
    uint16_t bind_port;
};

/* Client context - one per connected client */
struct client_ctx_s {
    server_ctx_t *server;
    socket_t socket;
    char client_address[256];
    fsm_t *fsm;  /* FSM for handling this client connection */

    /* Buffers for request/response */
    uint8_t recv_buffer[MODBUS_MAX_ADU_SIZE];
    buf_t recv_buf;

    uint8_t send_buffer[MODBUS_MAX_ADU_SIZE];
    buf_t send_buf;

    /* Current MBAP header */
    mbap_header_t mbap_header;

    /* Expected total message length */
    size_t expected_length;
};

/* ============================================================================
 * Signal Handling
 * ============================================================================ */

static void signal_handler(int signum) {
    (void)signum;

    /* Stop the reactor if it's been created */
    if (g_server && g_server->reactor) {
        reactor_stop(g_server->reactor);
    }
}

static void setup_signal_handlers(void) {
#ifdef _WIN32
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#endif
}

/* ============================================================================
 * FSM Actions
 * ============================================================================ */

static void client_read_action(fsm_t *fsm, fsm_state_id_t current_state, event_type_t event,
                               util_err_t status, fsm_state_id_t next_state, void *user_data) {
    (void)current_state;
    (void)event;
    (void)next_state;
    (void)fsm;

    client_ctx_t *client = (client_ctx_t *)user_data;

    if (status != UTIL_OK) {
        log_warn("Read event with error for %s: %d", client->client_address, status);
        fsm_queue_event(client->fsm, (event_type_t)REACTOR_EVENT_CLOSED, UTIL_OK, client);
        return;
    }

    /* Read from socket using buf.h API */
    util_err_t rc = socket_recv_buf(client->socket, &client->recv_buf);

    if (rc == UTIL_ECLOSED) {
        log_info("Client %s disconnected", client->client_address);
        fsm_queue_event(client->fsm, (event_type_t)REACTOR_EVENT_CLOSED, UTIL_OK, client);
        return;
    }

    if (rc != UTIL_OK && rc != UTIL_EAGAIN) {
        log_error("Socket recv error from %s: %d", client->client_address, rc);
        fsm_queue_event(client->fsm, (event_type_t)REACTOR_EVENT_CLOSED, UTIL_OK, client);
        return;
    }

    log_detail("Received data from %s, buffer has %zu bytes", client->client_address, buf_read_size(&client->recv_buf));

    /* Check if we have the MBAP header yet */
    if (buf_read_size(&client->recv_buf) >= MBAP_HEADER_SIZE &&
        current_state == APP_STATE_READING_HEADER) {

        /* Parse MBAP header */
        buf_t header_buf = client->recv_buf;
        header_buf.write = MBAP_HEADER_SIZE;  /* Only read header */

        if (modbus_parse_mbap_header(&header_buf, &client->mbap_header) == UTIL_OK) {
            /* Calculate total expected message length */
            client->expected_length = MBAP_HEADER_SIZE + client->mbap_header.length - 1;

            if (client->expected_length > MODBUS_MAX_ADU_SIZE) {
                log_error("PDU too large from %s", client->client_address);
                fsm_queue_event(client->fsm, (event_type_t)REACTOR_EVENT_CLOSED, UTIL_OK, client);
                return;
            }

            log_detail("MBAP header parsed, expecting %zu bytes total", client->expected_length);
            fsm_queue_event(client->fsm, (event_type_t)REACTOR_EVENT_CAN_READ, UTIL_OK, client);
        } else {
            log_error("Invalid MBAP header from %s", client->client_address);
            fsm_queue_event(client->fsm, (event_type_t)REACTOR_EVENT_CLOSED, UTIL_OK, client);
        }
        return;
    }

    /* Check if we have the complete message */
    if (buf_read_size(&client->recv_buf) >= client->expected_length) {
        log_detail("Complete message received from %s (%zu bytes)",
                  client->client_address, buf_read_size(&client->recv_buf));
        fsm_queue_event(client->fsm, APP_EVENT_PROCESS, UTIL_OK, client);
    } else {
        /* Need more data, re-enable read and continue */
        reactor_set_event_enable_mask(client->server->reactor, client->socket, (event_type_t)REACTOR_EVENT_CAN_READ, true);
    }
}

static void client_process_action(fsm_t *fsm, fsm_state_id_t current_state, event_type_t event,
                                  util_err_t status, fsm_state_id_t next_state, void *user_data) {
    (void)current_state;
    (void)event;
    (void)next_state;
    (void)fsm;
    (void)status;

    client_ctx_t *client = (client_ctx_t *)user_data;

    /* Extract function code from received message */
    buf_t request_buf = client->recv_buf;
    request_buf.read = MBAP_HEADER_SIZE;  /* Skip MBAP header */

    uint8_t function_code;
    if (!buf_read_u8(&request_buf, "function_code", &function_code)) {
        log_error("Failed to read function code from %s", client->client_address);
        fsm_queue_event(client->fsm, (event_type_t)REACTOR_EVENT_CLOSED, UTIL_OK, client);
        return;
    }

    log_detail("Processing function code 0x%02X from %s", function_code, client->client_address);

    /* Process the request */
    util_err_t err = modbus_process_request(function_code, &request_buf,
                                            &client->send_buf, &client->mbap_header, client->server->storage);

    if (err != UTIL_OK && err != UTIL_ENOTSUPPORTED) {
        log_detail("Request processing returned error: %d", err);
    }

    /* Queue send event */
    fsm_queue_event(client->fsm, (event_type_t)REACTOR_EVENT_CAN_WRITE, UTIL_OK, client);
}

static void client_send_action(fsm_t *fsm, fsm_state_id_t current_state, event_type_t event,
                               util_err_t status, fsm_state_id_t next_state, void *user_data) {
    (void)current_state;
    (void)event;
    (void)next_state;
    (void)fsm;

    client_ctx_t *client = (client_ctx_t *)user_data;

    if (status != UTIL_OK) {
        log_warn("Write event with error for %s: %d", client->client_address, status);
        fsm_queue_event(client->fsm, (event_type_t)REACTOR_EVENT_CLOSED, UTIL_OK, client);
        return;
    }

    /* Send response using buf.h API */
    util_err_t rc = socket_send_buf(client->socket, &client->send_buf);

    if (rc == UTIL_ECLOSED) {
        log_info("Client %s connection closed during send", client->client_address);
        fsm_queue_event(client->fsm, (event_type_t)REACTOR_EVENT_CLOSED, UTIL_OK, client);
        return;
    }

    if (rc != UTIL_OK && rc != UTIL_EAGAIN) {
        log_error("Socket send error to %s: %d", client->client_address, rc);
        fsm_queue_event(client->fsm, (event_type_t)REACTOR_EVENT_CLOSED, UTIL_OK, client);
        return;
    }

    log_detail("Sent response to %s", client->client_address);

    /* Check if all data was sent */
    if (buf_read_size(&client->send_buf) > 0) {
        /* Partial send, re-enable write and continue */
        reactor_set_event_enable_mask(client->server->reactor, client->socket, (event_type_t)REACTOR_EVENT_CAN_WRITE, true);
    } else {
        /* All data sent, prepare for next request */
        fsm_queue_event(client->fsm, APP_EVENT_IDLE, UTIL_OK, client);
    }
}

static void client_idle_action(fsm_t *fsm, fsm_state_id_t current_state, event_type_t event,
                               util_err_t status, fsm_state_id_t next_state, void *user_data) {
    (void)current_state;
    (void)event;
    (void)next_state;
    (void)fsm;
    (void)status;

    client_ctx_t *client = (client_ctx_t *)user_data;

    /* Reset for next request */
    buf_reset(&client->recv_buf);
    buf_reset(&client->send_buf);
    client->expected_length = MBAP_HEADER_SIZE;

    log_detail("Client %s ready for next request", client->client_address);

    /* Re-enable read on socket */
    reactor_set_event_enable_mask(client->server->reactor, client->socket, (event_type_t)REACTOR_EVENT_CAN_READ, true);
}

static void client_close_action(fsm_t *fsm, fsm_state_id_t current_state, event_type_t event,
                                util_err_t status, fsm_state_id_t next_state, void *user_data) {
    (void)current_state;
    (void)event;
    (void)next_state;
    (void)fsm;
    (void)status;

    client_ctx_t *client = (client_ctx_t *)user_data;

    if (client->socket != INVALID_SOCKET) {
        reactor_remove_socket(client->server->reactor, client->socket);
        socket_close(client->socket);
        client->socket = INVALID_SOCKET;
    }

    if (client->fsm) {
        fsm_destroy(client->fsm);
        client->fsm = NULL;
    }

    free(client);
    log_detail("Client context freed");
}

/* ============================================================================
 * FSM Transition Table
 * ============================================================================ */

static fsm_transition_t client_transitions[] = {
    /* State: READING_HEADER */
    { APP_STATE_READING_HEADER, REACTOR_EVENT_CAN_READ, client_read_action, APP_STATE_READING_PDU },
    { APP_STATE_READING_HEADER, REACTOR_EVENT_CLOSED, client_close_action, APP_STATE_CLOSING },
    { APP_STATE_READING_HEADER, REACTOR_EVENT_ERROR, client_close_action, APP_STATE_CLOSING },

    /* State: READING_PDU */
    { APP_STATE_READING_PDU, REACTOR_EVENT_CAN_READ, client_read_action, APP_STATE_READING_PDU },
    { APP_STATE_READING_PDU, APP_EVENT_PROCESS, client_process_action, APP_STATE_PROCESSING },
    { APP_STATE_READING_PDU, REACTOR_EVENT_CLOSED, client_close_action, APP_STATE_CLOSING },
    { APP_STATE_READING_PDU, REACTOR_EVENT_ERROR, client_close_action, APP_STATE_CLOSING },

    /* State: PROCESSING */
    { APP_STATE_PROCESSING, REACTOR_EVENT_CAN_WRITE, client_send_action, APP_STATE_SENDING },
    { APP_STATE_PROCESSING, REACTOR_EVENT_CLOSED, client_close_action, APP_STATE_CLOSING },
    { APP_STATE_PROCESSING, REACTOR_EVENT_ERROR, client_close_action, APP_STATE_CLOSING },

    /* State: SENDING */
    { APP_STATE_SENDING, APP_EVENT_IDLE, client_idle_action, APP_STATE_IDLE },
    { APP_STATE_SENDING, REACTOR_EVENT_CLOSED, client_close_action, APP_STATE_CLOSING },
    { APP_STATE_SENDING, REACTOR_EVENT_ERROR, client_close_action, APP_STATE_CLOSING },

    /* State: IDLE */
    { APP_STATE_IDLE, REACTOR_EVENT_CAN_READ, client_read_action, APP_STATE_READING_HEADER },
    { APP_STATE_IDLE, REACTOR_EVENT_CLOSED, client_close_action, APP_STATE_CLOSING },
    { APP_STATE_IDLE, REACTOR_EVENT_ERROR, client_close_action, APP_STATE_CLOSING },

    /* Wildcard: CLOSING accepts anything and stays in CLOSING */
    { APP_STATE_CLOSING, FSM_STATE_ID_ANY, NULL, APP_STATE_CLOSING },
};

static const size_t num_client_transitions = sizeof(client_transitions) / sizeof(client_transitions[0]);

/* ============================================================================
 * Socket Event Callback
 * ============================================================================ */

static void socket_event_callback(reactor_t *reactor, socket_t socket,
                                  event_type_t event, util_err_t status, void *context) {
    (void)reactor;
    (void)socket;

    /* Context is client_ctx_t* */
    client_ctx_t *client = (client_ctx_t *)context;
    if (!client || !client->fsm) {
        log_error("Invalid client context in socket event callback");
        return;
    }

    log_detail("Socket event for %s: event=%u, status=%d", client->client_address, event, status);

    /* Queue event to FSM */
    fsm_queue_event(client->fsm, event, status, client);

    /* Process events immediately */
    fsm_process_events(client->fsm);
}

/* ============================================================================
 * Listener Socket Callback
 * ============================================================================ */

static void listener_event_callback(reactor_t *reactor, socket_t socket,
                                    event_type_t event, util_err_t status, void *context) {
    (void)reactor;
    (void)socket;

    listener_ctx_t *listener = (listener_ctx_t *)context;

    if (event != REACTOR_EVENT_CAN_ACCEPT) {
        log_warn("Unexpected event on listener socket: %u", event);
        return;
    }

    if (status != UTIL_OK) {
        log_warn("Listener socket error: %d", status);
        return;
    }

    /* Accept new client connection */
    socket_t client_socket = INVALID_SOCKET;
    socket_address_t client_addr;
    util_err_t rc = socket_accept(listener->listener_socket, &client_socket, &client_addr);

    if (rc != UTIL_OK) {
        log_warn("Failed to accept connection on %s:%u: %d", listener->bind_address, listener->bind_port, rc);
        reactor_set_event_enable_mask(listener->server->reactor, listener->listener_socket, (event_type_t)REACTOR_EVENT_CAN_ACCEPT, true);
        return;
    }

    if (client_socket == INVALID_SOCKET) {
        log_warn("socket_accept returned INVALID_SOCKET on %s:%u", listener->bind_address, listener->bind_port);
        reactor_set_event_enable_mask(listener->server->reactor, listener->listener_socket, (event_type_t)REACTOR_EVENT_CAN_ACCEPT, true);
        return;
    }

    /* Get client address string */
    char client_addr_str[256];
    socket_address_get_addr_str(&client_addr, client_addr_str, sizeof(client_addr_str));
    uint16_t client_port = socket_address_get_port(&client_addr);
    log_info("Accepted connection from %s:%u", client_addr_str, client_port);

    /* Create client context */
    client_ctx_t *client = calloc(1, sizeof(*client));
    if (!client) {
        log_error("Failed to allocate client context");
        socket_close(client_socket);
        reactor_set_event_enable_mask(listener->server->reactor, listener->listener_socket, (event_type_t)REACTOR_EVENT_CAN_ACCEPT, true);
        return;
    }

    /* Initialize client context */
    client->server = listener->server;
    client->socket = client_socket;
    snprintf(client->client_address, sizeof(client->client_address), "%s:%u", client_addr_str, client_port);

    /* Initialize buffers */
    client->recv_buf = buf_init(client->recv_buffer, sizeof(client->recv_buffer));
    client->send_buf = buf_init(client->send_buffer, sizeof(client->send_buffer));
    client->expected_length = MBAP_HEADER_SIZE;

    /* Create FSM for client */
    client->fsm = fsm_create(client_transitions, num_client_transitions,
                            APP_STATE_READING_HEADER, 8, client);
    if (!client->fsm) {
        log_error("Failed to create FSM for client");
        free(client);
        socket_close(client_socket);
        reactor_set_event_enable_mask(listener->server->reactor, listener->listener_socket, (event_type_t)REACTOR_EVENT_CAN_ACCEPT, true);
        return;
    }

    /* Register client socket with reactor */
    rc = reactor_add_socket(listener->server->reactor, client_socket, socket_event_callback, client);
    if (rc != UTIL_OK) {
        log_error("Failed to register client socket with reactor: %d", rc);
        fsm_destroy(client->fsm);
        free(client);
        socket_close(client_socket);
        reactor_set_event_enable_mask(listener->server->reactor, listener->listener_socket, (event_type_t)REACTOR_EVENT_CAN_ACCEPT, true);
        return;
    }

    /* Re-enable listener accept */
    reactor_set_event_enable_mask(listener->server->reactor, listener->listener_socket, (event_type_t)REACTOR_EVENT_CAN_ACCEPT, true);
}

/* ============================================================================
 * Listener Creation
 * ============================================================================ */

static listener_ctx_t* create_listener(server_ctx_t *server, const char *bind_address, uint16_t bind_port) {
    listener_ctx_t *listener = calloc(1, sizeof(*listener));
    if (!listener) {
        log_error("Failed to allocate listener context");
        return NULL;
    }

    listener->server = server;
    strncpy(listener->bind_address, bind_address, sizeof(listener->bind_address) - 1);
    listener->bind_address[sizeof(listener->bind_address) - 1] = '\0';
    listener->bind_port = bind_port;

    /* Create socket address */
    socket_address_t addr;
    util_err_t rc = socket_address_init(&addr, bind_address, bind_port);
    if (rc != UTIL_OK) {
        log_error("Failed to initialize socket address: %d", rc);
        free(listener);
        return NULL;
    }

    /* Create listener socket */
    listener->listener_socket = socket_create_tcp_server(&addr, 5);
    if (listener->listener_socket == INVALID_SOCKET) {
        log_error("Failed to create listener socket on %s:%u", bind_address, bind_port);
        free(listener);
        return NULL;
    }

    log_info("Listener socket created on %s:%u", bind_address, bind_port);

    /* Register listener socket with reactor */
    rc = reactor_add_socket(server->reactor, listener->listener_socket, listener_event_callback, listener);
    if (rc != UTIL_OK) {
        log_error("Failed to register listener socket with reactor: %d", rc);
        socket_close(listener->listener_socket);
        free(listener);
        return NULL;
    }

    return listener;
}

static void destroy_listener(listener_ctx_t *listener) {
    if (!listener) {
        return;
    }

    if (listener->listener_socket != INVALID_SOCKET) {
        reactor_remove_socket(listener->server->reactor, listener->listener_socket);
        socket_close(listener->listener_socket);
    }

    free(listener);
}

/* ============================================================================
 * Main Function
 * ============================================================================ */

int main(int argc, char **argv) {
    int rc = EXIT_FAILURE;
    listener_ctx_t *listener = NULL;

    /* Set log level for startup messages */
    log_set_level(LOG_LEVEL_INFO);

    /* Parse command line arguments */
    const char *bind_address = "127.0.0.1";
    uint16_t bind_port = 502;
    size_t num_coils = 1000;
    size_t num_discrete_inputs = 1000;
    size_t num_holding_registers = 1000;
    size_t num_input_registers = 1000;

    /* Simple argument parsing: --port <port_number> */
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--port") == 0) {
            bind_port = (uint16_t)atoi(argv[i + 1]);
            if (bind_port == 0) {
                log_error("Invalid port number: %s", argv[i + 1]);
                return EXIT_FAILURE;
            }
            i++;
        } else if (strcmp(argv[i], "--address") == 0) {
            bind_address = argv[i + 1];
            i++;
        }
    }

    log_info("Modbus TCP Server starting...");

    /* Create server context */
    server_ctx_t *server = calloc(1, sizeof(*server));
    if (!server) {
        log_error("Failed to allocate server context");
        return EXIT_FAILURE;
    }

    /* Set global server pointer for signal handler */
    g_server = server;

    /* Setup signal handlers */
    setup_signal_handlers();

    /* Initialize socket layer */
    util_err_t sock_rc = socket_init();
    if (sock_rc != UTIL_OK) {
        log_error("Failed to initialize socket layer: %d", sock_rc);
        goto cleanup;
    }

    /* Create reactor with max 100 sockets */
    server->reactor = reactor_create(100);
    if (!server->reactor) {
        log_error("Failed to create reactor");
        goto cleanup;
    }

    log_detail("Reactor created");

    /* Create register storage */
    server->storage = register_storage_create(num_coils, num_discrete_inputs,
                                       num_holding_registers, num_input_registers);
    if (!server->storage) {
        log_error("Failed to create register storage");
        goto cleanup;
    }

    log_detail("Register storage created: coils=%zu, di=%zu, hr=%zu, ir=%zu",
              num_coils, num_discrete_inputs, num_holding_registers, num_input_registers);

    /* Create listener */
    listener = create_listener(server, bind_address, bind_port);
    if (!listener) {
        log_error("Failed to create listener");
        goto cleanup;
    }

    log_info("Modbus TCP Server listening on %s:%u", bind_address, bind_port);

    /* Main event loop - reactor_run() blocks until reactor_stop() is called */
    util_err_t reactor_rc = reactor_run(server->reactor, 1000);  /* 1 second TICK period */
    if (reactor_rc != UTIL_OK) {
        log_error("Reactor error: %d", reactor_rc);
        goto cleanup;
    }

    rc = EXIT_SUCCESS;

cleanup:
    log_info("Cleaning up server");

    if (listener) {
        destroy_listener(listener);
    }

    if (server) {
        if (server->storage) {
            register_storage_destroy(server->storage);
            server->storage = NULL;
        }

        if (server->reactor) {
            reactor_destroy(server->reactor);
            server->reactor = NULL;
        }

        free(server);
    }

    g_server = NULL;
    socket_cleanup();

    log_info("Server stopped");
    return rc;
}
