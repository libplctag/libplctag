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

#include "config.h"
#include "logger.h"
#include "socket_utils.h"
#include "register_storage.h"
#include "modbus_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifndef _WIN32
#include <poll.h>
#else
#include <winsock2.h>
/* Define nfds_t for Windows if not already defined */
#ifndef nfds_t
typedef unsigned int nfds_t;
#endif
#endif

/* Client connection states */
typedef enum {
    CLIENT_STATE_READING_HEADER,
    CLIENT_STATE_READING_BODY,
    CLIENT_STATE_PROCESSING,
    CLIENT_STATE_WRITING_RESPONSE
} client_state_t;

/* Client connection structure */
typedef struct {
    socket_t socket;
    client_state_t state;
    uint8_t recv_buffer[MODBUS_MAX_ADU_SIZE];
    int recv_offset;
    int expected_length;
    uint8_t send_buffer[MODBUS_MAX_ADU_SIZE];
    int send_length;
    int send_offset;
    char client_info[64];
} client_connection_t;

#define MAX_CLIENTS 100

/* Global variables for signal handling */
static volatile sig_atomic_t g_running = 1;

static void signal_handler(int signum) {
    (void)signum;
    g_running = 0;
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

static void close_client(client_connection_t *client) {
    if (client->socket != INVALID_SOCKET_VALUE) {
        log_info("Closing connection to %s", client->client_info);
        socket_close(client->socket);
        client->socket = INVALID_SOCKET_VALUE;
    }
}

static bool accept_new_client(socket_t listener_socket,
                              client_connection_t *clients,
                              int max_clients) {
    char client_info[64];
    socket_t client_socket = socket_accept(listener_socket, client_info, 
                                           sizeof(client_info));
    
    if (client_socket == INVALID_SOCKET_VALUE) {
        return false;
    }
    
    /* Find empty slot */
    for (int i = 0; i < max_clients; i++) {
        if (clients[i].socket == INVALID_SOCKET_VALUE) {
            clients[i].socket = client_socket;
            clients[i].state = CLIENT_STATE_READING_HEADER;
            clients[i].recv_offset = 0;
            clients[i].expected_length = MBAP_HEADER_SIZE;
            clients[i].send_offset = 0;
            clients[i].send_length = 0;
            strncpy(clients[i].client_info, client_info, sizeof(clients[i].client_info) - 1);
            clients[i].client_info[sizeof(clients[i].client_info) - 1] = '\0';
            
            log_info("Accepted connection from %s", client_info);
            return true;
        }
    }
    
    log_warn("Max clients reached, rejecting connection from %s", client_info);
    socket_close(client_socket);
    return false;
}

static void handle_client_read(client_connection_t *client,
                               register_storage_t *storage) {
    int to_read = client->expected_length - client->recv_offset;
    int n = socket_recv(client->socket, 
                       &client->recv_buffer[client->recv_offset],
                       to_read);
    
    if (n <= 0) {
        if (n < 0) {
            log_debug("Receive error from %s", client->client_info);
        }
        close_client(client);
        return;
    }
    
    client->recv_offset += n;
    
    /* Check if we completed this phase */
    if (client->recv_offset >= client->expected_length) {
        if (client->state == CLIENT_STATE_READING_HEADER) {
            /* Parse header to determine body length */
            mbap_header_t header;
            if (!modbus_parse_mbap_header(client->recv_buffer, client->recv_offset, &header)) {
                log_debug("Invalid MBAP header from %s", client->client_info);
                close_client(client);
                return;
            }
            
            /* Now read the PDU (length field includes unit_id which is already in header) */
            client->expected_length = MBAP_HEADER_SIZE + header.length - 1;
            
            if (client->expected_length > MODBUS_MAX_ADU_SIZE) {
                log_debug("PDU too large (%d bytes) from %s", 
                         client->expected_length, client->client_info);
                close_client(client);
                return;
            }
            
            if (client->recv_offset < client->expected_length) {
                client->state = CLIENT_STATE_READING_BODY;
            } else {
                /* Header included the complete request */
                client->state = CLIENT_STATE_PROCESSING;
            }
        } else if (client->state == CLIENT_STATE_READING_BODY) {
            client->state = CLIENT_STATE_PROCESSING;
        }
    }
    
    /* Process complete request */
    if (client->state == CLIENT_STATE_PROCESSING) {
        modbus_message_t request;
        if (!modbus_parse_request(client->recv_buffer, client->recv_offset, &request)) {
            log_debug("Failed to parse request from %s", client->client_info);
            close_client(client);
            return;
        }
        
        log_dump_bytes("Request", client->recv_buffer, (size_t)client->recv_offset);
        
        client->send_length = modbus_process_request(&request, storage,
                                                     client->send_buffer,
                                                     MODBUS_MAX_ADU_SIZE);
        
        if (client->send_length < 0) {
            log_error("Failed to process request from %s", client->client_info);
            close_client(client);
            return;
        }

        log_dump_bytes("Response", client->send_buffer, (size_t)client->send_length);

        client->send_offset = 0;
        client->state = CLIENT_STATE_WRITING_RESPONSE;
    }
}

static void handle_client_write(client_connection_t *client) {
    int to_write = client->send_length - client->send_offset;
    int n = socket_send(client->socket,
                       &client->send_buffer[client->send_offset],
                       to_write);
    
    if (n <= 0) {
        if (n < 0) {
            log_debug("Send error to %s", client->client_info);
        }
        close_client(client);
        return;
    }
    
    client->send_offset += n;
    
    if (client->send_offset >= client->send_length) {
        /* Response sent, prepare for next request */
        client->state = CLIENT_STATE_READING_HEADER;
        client->recv_offset = 0;
        client->expected_length = MBAP_HEADER_SIZE;
        client->send_offset = 0;
        client->send_length = 0;
    }
}

int main(int argc, char **argv) {
    server_config_t config;
    register_storage_t storage;
    socket_t listener_sockets[MAX_LISTEN_ENDPOINTS];
    client_connection_t clients[MAX_CLIENTS];
    int num_listeners = 0;
    int rc = EXIT_FAILURE;
    
    /* Initialize */
    memset(listener_sockets, 0, sizeof(listener_sockets));
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].socket = INVALID_SOCKET_VALUE;
    }
    
    /* Initialize configuration with defaults */
    config_init(&config);
    
    /* Parse configuration */
    if (!config_parse_args(&config, argc, argv)) {
        config_print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    
    if (config.num_listen_endpoints == 0) {
        log_error("No listen endpoints specified");
        config_print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    
    /* Initialize logger with debug flag from config */
    logger_set_debug(config.debug);
    
    /* Initialize sockets */
    if (!socket_init()) {
        log_error("Failed to initialize sockets");
        return EXIT_FAILURE;
    }
    
    /* Setup signal handlers */
    setup_signal_handlers();
    
    /* Initialize register storage */
    if (!register_storage_init(&storage,
                               config.num_coils,
                               config.num_discrete_inputs,
                               config.num_holding_registers,
                               config.num_input_registers)) {
        log_error("Failed to initialize register storage");
        goto cleanup;
    }
    
    /* Create listener sockets */
    for (int i = 0; i < config.num_listen_endpoints; i++) {
        listener_sockets[num_listeners] = socket_create_listener(
            config.listen_endpoints[i].host,
            config.listen_endpoints[i].port
        );
        
        if (listener_sockets[num_listeners] == INVALID_SOCKET_VALUE) {
            log_error("Failed to create listener on %s:%d",
                     config.listen_endpoints[i].host,
                     config.listen_endpoints[i].port);
            goto cleanup;
        }
        
        num_listeners++;
    }
    
    log_info("Modbus TCP server started");
    
    /* Main event loop */
    while (g_running) {
        /* Build poll array with parallel client pointer array */
        struct pollfd fds[MAX_LISTEN_ENDPOINTS + MAX_CLIENTS];
        client_connection_t *client_ptrs[MAX_LISTEN_ENDPOINTS + MAX_CLIENTS];
        nfds_t nfds = 0;
        
        /* Add listener sockets */
        for (int i = 0; i < num_listeners; i++) {
            fds[nfds].fd = listener_sockets[i];
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            client_ptrs[nfds] = NULL;  /* NULL for listener sockets */
            nfds++;
        }
        
        /* Add client sockets */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].socket != INVALID_SOCKET_VALUE) {
                fds[nfds].fd = clients[i].socket;
                fds[nfds].events = 0;
                
                if (clients[i].state == CLIENT_STATE_READING_HEADER ||
                    clients[i].state == CLIENT_STATE_READING_BODY) {
                    fds[nfds].events |= POLLIN;
                } else if (clients[i].state == CLIENT_STATE_WRITING_RESPONSE) {
                    fds[nfds].events |= POLLOUT;
                }
                
                fds[nfds].revents = 0;
                client_ptrs[nfds] = &clients[i];  /* Store pointer to this client */
                nfds++;
            }
        }
        
        /* Wait for events */
        int poll_rc = poll_wrapper(fds, nfds, 1000);
        
        if (poll_rc < 0) {
            if (!g_running) break;
            log_error("poll() failed");
            goto cleanup;
        }
        
        if (poll_rc == 0) {
            /* Timeout, check running flag */
            continue;
        }
        
        /* Process listener sockets */
        for (int i = 0; i < num_listeners; i++) {
            if (fds[i].revents & POLLIN) {
                accept_new_client(listener_sockets[i], clients, MAX_CLIENTS);
            }
        }
        
        /* Process client sockets - use parallel array for direct access */
        {
            nfds_t j;
            for (j = num_listeners; j < nfds; j++) {
                if (client_ptrs[j] != NULL) {
                    if (fds[j].revents & (POLLIN | POLLERR | POLLHUP)) {
                        handle_client_read(client_ptrs[j], &storage);
                    } else if (fds[j].revents & POLLOUT) {
                        handle_client_write(client_ptrs[j]);
                    }
                }
            }
        }
    }
    
    log_info("Shutting down...");
    rc = EXIT_SUCCESS;
    
cleanup:
    /* Close all client connections */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].socket != INVALID_SOCKET_VALUE) {
            close_client(&clients[i]);
        }
    }
    
    /* Close listener sockets */
    for (int i = 0; i < num_listeners; i++) {
        socket_close(listener_sockets[i]);
    }
    
    /* Cleanup storage */
    register_storage_cleanup(&storage);
    
    /* Cleanup sockets */
    socket_cleanup();
    
    log_info("Server stopped");
    return rc;
}
