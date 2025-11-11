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
#include "args.h"
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

/* ============================================================================
 * Debug/Logging Helpers
 * ============================================================================ */

/**
 * @brief Convert FSM state ID to human-readable name
 */
static const char* state_name(fsm_state_id_t state) {
    switch (state) {
        case APP_STATE_LISTENING: return "LISTENING";
        case APP_STATE_READING_HEADER: return "READING_HEADER";
        case APP_STATE_READING_PDU: return "READING_PDU";
        case APP_STATE_PROCESSING: return "PROCESSING";
        case APP_STATE_SENDING: return "SENDING";
        case APP_STATE_IDLE: return "IDLE";
        case APP_STATE_CLOSING: return "CLOSING";
        default: return "UNKNOWN";
    }
}

/**
 * @brief Convert event type ID to human-readable name
 */
static const char* event_name(event_type_t event) {
    switch (event) {
        case REACTOR_EVENT_ERROR: return "REACTOR_EVENT_ERROR";
        case REACTOR_EVENT_CAN_READ: return "REACTOR_EVENT_CAN_READ";
        case REACTOR_EVENT_CAN_WRITE: return "REACTOR_EVENT_CAN_WRITE";
        case REACTOR_EVENT_CLOSED: return "REACTOR_EVENT_CLOSED";
        case REACTOR_EVENT_CONNECTED: return "REACTOR_EVENT_CONNECTED";
        case REACTOR_EVENT_WRITTEN: return "REACTOR_EVENT_WRITTEN";
        case REACTOR_EVENT_TICK: return "REACTOR_EVENT_TICK";
        case REACTOR_EVENT_SHUTDOWN: return "REACTOR_EVENT_SHUTDOWN";
        default:
            if (event == APP_EVENT_PROCESS) return "APP_EVENT_PROCESS";
            if (event == APP_EVENT_IDLE) return "APP_EVENT_IDLE";
            return "UNKNOWN_EVENT";
    }
}

/**
 * @brief Dump event mask as readable bits with event names
 */
static void dump_event_mask(const char *label, bitarray_t mask) {
    log_detail("Event Mask [%s]: Enabled events:", label);

    /* Check reactor events */
    for (int i = 0; i < REACTOR_EVENT_MAX; i++) {
        if (bitarray_test(&mask, (event_type_t)i)) {
            log_detail("  - Bit %d: %s", i, event_name((event_type_t)i));
        }
    }

    /* Check application events */
    if (bitarray_test(&mask, (event_type_t)APP_EVENT_PROCESS)) {
        log_detail("  - Bit %d: %s", APP_EVENT_PROCESS, event_name((event_type_t)APP_EVENT_PROCESS));
    }
    if (bitarray_test(&mask, (event_type_t)APP_EVENT_IDLE)) {
        log_detail("  - Bit %d: %s", APP_EVENT_IDLE, event_name((event_type_t)APP_EVENT_IDLE));
    }
}

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
 * FSM State Change Callback
 * ============================================================================ */

static util_err_t on_client_state_change(fsm_t *fsm, fsm_state_id_t old_state, fsm_state_id_t new_state,
                                         bitarray_t new_event_mask, void *user_context) {
    (void)fsm;
    (void)old_state;

    client_ctx_t *client = (client_ctx_t *)user_context;

    /* Get transition info for better logging - look up an event in the new state to get its name */
    const char *old_name = state_name(old_state);
    const char *new_name = state_name(new_state);

    log_info("Client %s FSM state transition: %s -> %s", client->client_address, old_name, new_name);

    /* Dump the new event mask being applied */
    dump_event_mask("FSM state change", new_event_mask);

    /* Update the reactor with the new event mask for this socket */
    util_err_t rc = reactor_set_event_mask(client->server->reactor, client->socket, new_event_mask);
    if (rc != UTIL_OK) {
        log_error("Failed to set event mask for client %s: %d", client->client_address, rc);
        return rc;
    }

    return UTIL_OK;
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

    /* In IDLE state: queue a CAN_READ to process the data in READING_HEADER state */
    if (current_state == APP_STATE_IDLE) {
        /* The data is already in the buffer from socket_recv_buf.
         * Queue CAN_READ to trigger processing after transition to READING_HEADER. */
        fsm_queue_event(client->fsm, (event_type_t)REACTOR_EVENT_CAN_READ, UTIL_OK, client);
        return;
    }

    /* In READING_HEADER state: check if we have the MBAP header */
    if (current_state == APP_STATE_READING_HEADER) {
        if (buf_read_size(&client->recv_buf) >= MBAP_HEADER_SIZE) {
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

                /* Queue a CAN_READ event to trigger READING_PDU processing.
                 * This will cause the FSM to transition to READING_PDU and then
                 * process the CAN_READ event in that state. */
                fsm_queue_event(client->fsm, (event_type_t)REACTOR_EVENT_CAN_READ, UTIL_OK, client);
            } else {
                log_error("Invalid MBAP header from %s", client->client_address);
                fsm_queue_event(client->fsm, (event_type_t)REACTOR_EVENT_CLOSED, UTIL_OK, client);
            }
        }
        return;
    }

    /* In READING_PDU state: check if we have the complete message */
    if (current_state == APP_STATE_READING_PDU) {
        if (buf_read_size(&client->recv_buf) >= client->expected_length) {
            log_detail("Complete message received from %s (%zu bytes)",
                      client->client_address, buf_read_size(&client->recv_buf));
            fsm_queue_event(client->fsm, APP_EVENT_PROCESS, UTIL_OK, client);
        }
        /* No explicit re-enable needed; reactor event mask is controlled by FSM state transitions */
        return;
    }

    /* Shouldn't reach here - unexpected state */
    log_error("client_read_action called in unexpected state %u", current_state);
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

    /* Queue send event to transition to SENDING state.
     * The FSM state change callback will update the reactor event mask
     * to enable CAN_WRITE when transitioning to SENDING state. */
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
        /* Partial send; the reactor event mask will stay set for CAN_WRITE
         * since we're still in SENDING state */
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

    /* The FSM state change callback will update the reactor event mask
     * to enable CAN_READ when transitioning to IDLE state */
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

    /* Note: We cannot call fsm_destroy() or free(client) here because we are being
     * called from within fsm_process_events(). Destroying the FSM or client context
     * while the FSM is still processing would cause a use-after-free error.
     * The socket has been closed and removed from the reactor, so no more events
     * will be delivered to this client. The FSM and context will be cleaned up
     * by the reactor when it finishes processing all pending events. */

    log_detail("Client socket closed");
}

/* ============================================================================
 * FSM Transition Table
 * ============================================================================ */

static fsm_transition_t client_transitions[] = {
    /* State: READING_HEADER */
    { APP_STATE_READING_HEADER, REACTOR_EVENT_CAN_READ, client_read_action, APP_STATE_READING_PDU, "READING_HEADER", "CAN_READ" },
    { APP_STATE_READING_HEADER, REACTOR_EVENT_CLOSED, client_close_action, APP_STATE_CLOSING, "READING_HEADER", "CLOSED" },
    { APP_STATE_READING_HEADER, REACTOR_EVENT_ERROR, client_close_action, APP_STATE_CLOSING, "READING_HEADER", "ERROR" },

    /* State: READING_PDU */
    { APP_STATE_READING_PDU, REACTOR_EVENT_CAN_READ, client_read_action, APP_STATE_READING_PDU, "READING_PDU", "CAN_READ" },
    { APP_STATE_READING_PDU, APP_EVENT_PROCESS, client_process_action, APP_STATE_PROCESSING, "READING_PDU", "PROCESS" },
    { APP_STATE_READING_PDU, REACTOR_EVENT_CLOSED, client_close_action, APP_STATE_CLOSING, "READING_PDU", "CLOSED" },
    { APP_STATE_READING_PDU, REACTOR_EVENT_ERROR, client_close_action, APP_STATE_CLOSING, "READING_PDU", "ERROR" },

    /* State: PROCESSING */
    { APP_STATE_PROCESSING, REACTOR_EVENT_CAN_WRITE, client_send_action, APP_STATE_SENDING, "PROCESSING", "CAN_WRITE" },
    { APP_STATE_PROCESSING, REACTOR_EVENT_CLOSED, client_close_action, APP_STATE_CLOSING, "PROCESSING", "CLOSED" },
    { APP_STATE_PROCESSING, REACTOR_EVENT_ERROR, client_close_action, APP_STATE_CLOSING, "PROCESSING", "ERROR" },

    /* State: SENDING */
    { APP_STATE_SENDING, APP_EVENT_IDLE, client_idle_action, APP_STATE_IDLE, "SENDING", "IDLE" },
    { APP_STATE_SENDING, REACTOR_EVENT_CLOSED, client_close_action, APP_STATE_CLOSING, "SENDING", "CLOSED" },
    { APP_STATE_SENDING, REACTOR_EVENT_ERROR, client_close_action, APP_STATE_CLOSING, "SENDING", "ERROR" },

    /* State: IDLE */
    { APP_STATE_IDLE, REACTOR_EVENT_CAN_READ, client_read_action, APP_STATE_READING_HEADER, "IDLE", "CAN_READ" },
    { APP_STATE_IDLE, REACTOR_EVENT_CLOSED, client_close_action, APP_STATE_CLOSING, "IDLE", "CLOSED" },
    { APP_STATE_IDLE, REACTOR_EVENT_ERROR, client_close_action, APP_STATE_CLOSING, "IDLE", "ERROR" },

    /* Wildcard: CLOSING accepts anything and stays in CLOSING */
    { APP_STATE_CLOSING, FSM_STATE_ID_ANY, NULL, APP_STATE_CLOSING, "CLOSING", "ANY" },
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

    /* Log with transition table information */
    fsm_state_id_t current_state = fsm_get_state(client->fsm);
    const fsm_transition_t *trans = fsm_get_transition(client->fsm, current_state, event);
    if (trans && trans->state_name && trans->event_name) {
        log_detail("Socket event for %s: State=%s, Event=%s, Status=%d",
                   client->client_address, trans->state_name, trans->event_name, status);
    } else {
        log_detail("Socket event for %s: State=%u, Event=%u, Status=%d",
                   client->client_address, current_state, event, status);
    }

    /* Queue event to FSM */
    fsm_queue_event(client->fsm, event, status, client);

    /* Process events immediately.
     * Note: After processing, the client context may be marked for cleanup
     * if the socket was closed, but we don't clean it up here to avoid
     * use-after-free. The cleanup will happen when the client_ctx_t is
     * no longer referenced. */
    fsm_process_events(client->fsm);

    /* After processing events, we need to check if the socket is still valid.
     * If the client close action was triggered, the socket will be INVALID_SOCKET,
     * and we should not attempt any more operations on the client. */
    if (client->socket == INVALID_SOCKET) {
        log_detail("Client socket is closed, cleaning up client context");
        if (client->fsm) {
            fsm_destroy(client->fsm);
            client->fsm = NULL;
        }
        free(client);
    }
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
        return;
    }

    if (client_socket == INVALID_SOCKET) {
        log_warn("socket_accept returned INVALID_SOCKET on %s:%u", listener->bind_address, listener->bind_port);
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

    /* Create FSM for client with state change callback */
    client->fsm = fsm_create(client_transitions, num_client_transitions,
                            APP_STATE_READING_HEADER, 8, on_client_state_change, client);
    if (!client->fsm) {
        log_error("Failed to create FSM for client");
        free(client);
        socket_close(client_socket);
        return;
    }

    /* Register client socket with reactor, enabling only events for current FSM state (READING_HEADER) */
    bitarray_t initial_event_mask = fsm_get_event_mask(client->fsm);
    rc = reactor_add_socket(listener->server->reactor, client_socket, socket_event_callback, client, &initial_event_mask);
    if (rc != UTIL_OK) {
        log_error("Failed to register client socket with reactor: %d", rc);
        fsm_destroy(client->fsm);
        free(client);
        socket_close(client_socket);
        return;
    }
}

/* ============================================================================
 * Listener Creation
 * ============================================================================ */

/**
 * @brief Parse "address:port" format into address and port components
 * @param listen_str String in format "address:port"
 * @param address OUT: Parsed address (must be at least 256 bytes)
 * @param port OUT: Parsed port number
 * @return true on success, false on parse error
 */
static bool parse_listen_address(const char *listen_str, char *address, uint16_t *port) {
    if (!listen_str || !address || !port) {
        return false;
    }

    /* Find the colon separating address and port */
    const char *colon = strchr(listen_str, ':');
    if (!colon) {
        log_error("Invalid listen format (expected address:port): %s", listen_str);
        return false;
    }

    /* Extract address */
    size_t addr_len = (size_t)(colon - listen_str);
    if (addr_len == 0 || addr_len >= 256) {
        log_error("Invalid address in listen format: %s", listen_str);
        return false;
    }
    strncpy(address, listen_str, addr_len);
    address[addr_len] = '\0';

    /* Parse port */
    int port_num = atoi(colon + 1);
    if (port_num <= 0 || port_num > 65535) {
        log_error("Invalid port in listen format (must be 1-65535): %s", listen_str);
        return false;
    }
    *port = (uint16_t)port_num;

    return true;
}

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

    /* Register listener socket with reactor, enabling only CAN_ACCEPT initially */
    bitarray_t listener_events = BITARRAY_ZERO();
    bitarray_set(&listener_events, REACTOR_EVENT_CAN_ACCEPT);
    rc = reactor_add_socket(server->reactor, listener->listener_socket, listener_event_callback, listener, &listener_events);
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
    listener_ctx_t *listeners[10] = {NULL};  /* Support up to 10 listeners */
    size_t num_listeners = 0;
    args_result_t args_result;

    /* Set log level for startup messages */
    log_set_level(LOG_LEVEL_INFO);

    /* Define command-line arguments
     * Note: args.h uses --flag=value format
     * Examples:
     *   modbus_server
     *   modbus_server --listen=127.0.0.1:502
     *   modbus_server --listen=127.0.0.1:1502 --listen=127.0.0.1:2502
     *   modbus_server --listen=0.0.0.0:502 --debug=INFO
     *   modbus_server --coils=2000 --holding-registers=5000
     *   modbus_server --help
     */
    args_flag_def_t flags[] = {
        {
            "listen",
            ARGS_TYPE_STRING,
            ARGS_OPTIONAL,
            ARGS_MULTIPLE,
            "server.listen",
            "Address and port to listen on (address:port, can be specified multiple times)",
            { .has_default = false }
        },
        {
            "debug",
            ARGS_TYPE_STRING,
            ARGS_OPTIONAL,
            ARGS_ONCE,
            "logging.debug",
            "Debug/log level (ERROR, WARN, INFO, DETAIL, SPEW)",
            { .has_default = true, .value.string_val = "INFO" }
        },
        {
            "coils",
            ARGS_TYPE_INT,
            ARGS_OPTIONAL,
            ARGS_ONCE,
            "modbus.coils",
            "Number of coils (read/write bits, 0-65535)",
            { .has_default = true, .value.int_val = 1000 }
        },
        {
            "discrete-inputs",
            ARGS_TYPE_INT,
            ARGS_OPTIONAL,
            ARGS_ONCE,
            "modbus.discrete_inputs",
            "Number of discrete inputs (read-only bits, 0-65535)",
            { .has_default = true, .value.int_val = 1000 }
        },
        {
            "holding-registers",
            ARGS_TYPE_INT,
            ARGS_OPTIONAL,
            ARGS_ONCE,
            "modbus.holding_registers",
            "Number of holding registers (read/write 16-bit values, 0-65535)",
            { .has_default = true, .value.int_val = 1000 }
        },
        {
            "input-registers",
            ARGS_TYPE_INT,
            ARGS_OPTIONAL,
            ARGS_ONCE,
            "modbus.input_registers",
            "Number of input registers (read-only 16-bit values, 0-65535)",
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

    /* Parse command-line arguments */
    util_err_t parse_rc = args_parse(argc, (const char **)argv, flags, num_flags, &args_result);
    if (parse_rc != UTIL_OK) {
        printf("Error: %s\n", args_get_error_detail(&args_result));
        args_print_help(argv[0], flags, num_flags);
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    /* Check for help flag */
    if (args_get_bool(&args_result, "help")) {
        args_print_help(argv[0], flags, num_flags);
        args_free(&args_result);
        return EXIT_SUCCESS;
    }

    /* Parse debug/log level */
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
        } else {
            printf("Error: Invalid debug level: %s\n", debug_level_str);
            args_free(&args_result);
            return EXIT_FAILURE;
        }
    }
    log_set_level(log_level);

    /* Extract listen addresses - if none specified, use default */
    size_t listen_count = args_get_count(&args_result, "listen");
    if (listen_count == 0) {
        /* Default: listen on 127.0.0.1:502 */
        listen_count = 1;
    }

    /* Extract register storage sizes from arguments */
    int64_t coils_val = args_get_int(&args_result, "coils");
    int64_t di_val = args_get_int(&args_result, "discrete-inputs");
    int64_t hr_val = args_get_int(&args_result, "holding-registers");
    int64_t ir_val = args_get_int(&args_result, "input-registers");

    /* Validate register counts */
    if (coils_val < 0 || coils_val > 65535) {
        printf("Error: Invalid coils count: %lld (must be 0-65535)\n", (long long)coils_val);
        args_free(&args_result);
        return EXIT_FAILURE;
    }
    if (di_val < 0 || di_val > 65535) {
        printf("Error: Invalid discrete-inputs count: %lld (must be 0-65535)\n", (long long)di_val);
        args_free(&args_result);
        return EXIT_FAILURE;
    }
    if (hr_val < 0 || hr_val > 65535) {
        printf("Error: Invalid holding-registers count: %lld (must be 0-65535)\n", (long long)hr_val);
        args_free(&args_result);
        return EXIT_FAILURE;
    }
    if (ir_val < 0 || ir_val > 65535) {
        printf("Error: Invalid input-registers count: %lld (must be 0-65535)\n", (long long)ir_val);
        args_free(&args_result);
        return EXIT_FAILURE;
    }

    /* Set register storage sizes */
    size_t num_coils = (size_t)coils_val;
    size_t num_discrete_inputs = (size_t)di_val;
    size_t num_holding_registers = (size_t)hr_val;
    size_t num_input_registers = (size_t)ir_val;

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

    /* Create listeners for each listen address */
    if (listen_count > 0) {
        for (size_t i = 0; i < listen_count && i < 10; i++) {
            char listen_address[256];
            uint16_t listen_port;

            /* Get listen address from arguments */
            if (i == 0 && args_get_count(&args_result, "listen") == 0) {
                /* No listen addresses specified, use default */
                strcpy(listen_address, "127.0.0.1");
                listen_port = 502;
            } else {
                /* Parse listen address from arguments */
                args_value_t listen_val = args_get_at(&args_result, "listen", i);
                if (!listen_val.present) {
                    break;
                }
                if (!parse_listen_address(listen_val.value.string_val, listen_address, &listen_port)) {
                    log_error("Failed to parse listen address: %s", listen_val.value.string_val);
                    goto cleanup;
                }
            }

            /* Create listener for this address */
            listener_ctx_t *listener = create_listener(server, listen_address, listen_port);
            if (!listener) {
                log_error("Failed to create listener for %s:%u", listen_address, listen_port);
                goto cleanup;
            }

            listeners[num_listeners++] = listener;
            log_info("Modbus TCP Server listening on %s:%u", listen_address, listen_port);
        }
    } else {
        /* No listen addresses specified, use default */
        listener_ctx_t *listener = create_listener(server, "127.0.0.1", 502);
        if (!listener) {
            log_error("Failed to create listener");
            goto cleanup;
        }
        listeners[num_listeners++] = listener;
        log_info("Modbus TCP Server listening on 127.0.0.1:502");
    }

    /* Main event loop - reactor_run() blocks until reactor_stop() is called */
    util_err_t reactor_rc = reactor_run(server->reactor, 1000);  /* 1 second TICK period */
    if (reactor_rc != UTIL_OK) {
        log_error("Reactor error: %d", reactor_rc);
        goto cleanup;
    }

    rc = EXIT_SUCCESS;

cleanup:
    log_info("Cleaning up server");

    /* Destroy all listeners */
    for (size_t i = 0; i < num_listeners; i++) {
        if (listeners[i]) {
            destroy_listener(listeners[i]);
        }
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
    args_free(&args_result);

    log_info("Server stopped");
    return rc;
}
