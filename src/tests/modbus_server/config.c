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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_init(server_config_t *config) {
    memset(config, 0, sizeof(server_config_t));
    config->num_listen_endpoints = 0;
    config->num_coils = DEFAULT_REGISTER_COUNT;
    config->num_discrete_inputs = DEFAULT_REGISTER_COUNT;
    config->num_holding_registers = DEFAULT_REGISTER_COUNT;
    config->num_input_registers = DEFAULT_REGISTER_COUNT;
    config->debug = false;
}

static bool parse_endpoint(const char *arg, listen_endpoint_t *endpoint) {
    char buffer[128];
    char *colon;
    
    strncpy(buffer, arg, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    
    colon = strchr(buffer, ':');
    if (!colon) {
        log_error("Invalid endpoint format '%s'. Expected IP:PORT", arg);
        return false;
    }
    
    *colon = '\0';
    const char *ip = buffer;
    const char *port_str = colon + 1;
    
    /* Validate and copy IP */
    if (strlen(ip) >= sizeof(endpoint->host)) {
        log_error("IP address too long: %s", ip);
        return false;
    }
    strncpy(endpoint->host, ip, sizeof(endpoint->host) - 1);
    endpoint->host[sizeof(endpoint->host) - 1] = '\0';
    
    /* Parse port */
    char *endptr;
    long port = strtol(port_str, &endptr, 10);
    if (*endptr != '\0' || port < 1 || port > 65535) {
        log_error("Invalid port number: %s", port_str);
        return false;
    }
    endpoint->port = (int)port;
    
    return true;
}

void config_print_usage(const char *program_name) {
    printf("Modbus TCP Server\n");
    printf("\nUsage: %s [OPTIONS]\n\n", program_name);
    printf("Options:\n");
    printf("  --listen <ip:port>            Add a listening endpoint (up to %d)\n", 
           MAX_LISTEN_ENDPOINTS);
    printf("                                Example: --listen 0.0.0.0:502\n");
    printf("  --coils <count>               Number of coils (default: %d)\n", 
           DEFAULT_REGISTER_COUNT);
    printf("  --discrete-inputs <count>     Number of discrete inputs (default: %d)\n", 
           DEFAULT_REGISTER_COUNT);
    printf("  --holding-registers <count>   Number of holding registers (default: %d)\n", 
           DEFAULT_REGISTER_COUNT);
    printf("  --input-registers <count>     Number of input registers (default: %d)\n", 
           DEFAULT_REGISTER_COUNT);
    printf("  --debug                       Enable detailed debug logging\n");
    printf("  --help                        Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s --listen 127.0.0.1:502\n", program_name);
    printf("  %s --listen 0.0.0.0:502 --listen 0.0.0.0:5020 --debug\n", program_name);
    printf("  %s --listen 127.0.0.1:502 --coils 1000 --holding-registers 2000\n", 
           program_name);
    printf("\n");
}

bool config_parse_args(server_config_t *config, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            config_print_usage(argv[0]);
            return false;
        }
        else if (strcmp(argv[i], "--debug") == 0) {
            config->debug = true;
        }
        else if (strcmp(argv[i], "--listen") == 0) {
            if (i + 1 >= argc) {
                log_error("--listen requires an argument (ip:port)");
                return false;
            }
            if (config->num_listen_endpoints >= MAX_LISTEN_ENDPOINTS) {
                log_error("Maximum of %d listen endpoints exceeded", MAX_LISTEN_ENDPOINTS);
                return false;
            }
            if (!parse_endpoint(argv[++i], &config->listen_endpoints[config->num_listen_endpoints])) {
                return false;
            }
            config->num_listen_endpoints++;
        }
        else if (strcmp(argv[i], "--coils") == 0) {
            if (i + 1 >= argc) {
                log_error("--coils requires a number");
                return false;
            }
            config->num_coils = atoi(argv[++i]);
            if (config->num_coils < 1) {
                log_error("Invalid coil count: %d", config->num_coils);
                return false;
            }
        }
        else if (strcmp(argv[i], "--discrete-inputs") == 0) {
            if (i + 1 >= argc) {
                log_error("--discrete-inputs requires a number");
                return false;
            }
            config->num_discrete_inputs = atoi(argv[++i]);
            if (config->num_discrete_inputs < 1) {
                log_error("Invalid discrete input count: %d", config->num_discrete_inputs);
                return false;
            }
        }
        else if (strcmp(argv[i], "--holding-registers") == 0) {
            if (i + 1 >= argc) {
                log_error("--holding-registers requires a number");
                return false;
            }
            config->num_holding_registers = atoi(argv[++i]);
            if (config->num_holding_registers < 1) {
                log_error("Invalid holding register count: %d", 
                         config->num_holding_registers);
                return false;
            }
        }
        else if (strcmp(argv[i], "--input-registers") == 0) {
            if (i + 1 >= argc) {
                log_error("--input-registers requires a number");
                return false;
            }
            config->num_input_registers = atoi(argv[++i]);
            if (config->num_input_registers < 1) {
                log_error("Invalid input register count: %d", config->num_input_registers);
                return false;
            }
        }
        else {
            log_error("Unknown option: %s", argv[i]);
            config_print_usage(argv[0]);
            return false;
        }
    }
    
    return true;
}

bool config_validate(const server_config_t *config) {
    if (config->num_listen_endpoints == 0) {
        log_error("No listening endpoints specified. Use --listen <ip:port>");
        return false;
    }
    
    if (config->num_coils < 1 || config->num_coils > 65536) {
        log_error("Coil count out of range: %d (must be 1-65536)", config->num_coils);
        return false;
    }
    
    if (config->num_discrete_inputs < 1 || config->num_discrete_inputs > 65536) {
        log_error("Discrete input count out of range: %d (must be 1-65536)", 
                 config->num_discrete_inputs);
        return false;
    }
    
    if (config->num_holding_registers < 1 || config->num_holding_registers > 65536) {
        log_error("Holding register count out of range: %d (must be 1-65536)", 
                 config->num_holding_registers);
        return false;
    }
    
    if (config->num_input_registers < 1 || config->num_input_registers > 65536) {
        log_error("Input register count out of range: %d (must be 1-65536)", 
                 config->num_input_registers);
        return false;
    }
    
    return true;
}
