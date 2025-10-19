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

#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#define MAX_LISTEN_ENDPOINTS 10
#define DEFAULT_REGISTER_COUNT 100

/* Listening endpoint */
typedef struct {
    char host[64];
    int port;
} listen_endpoint_t;

/* Server configuration */
typedef struct {
    /* Listening endpoints */
    listen_endpoint_t listen_endpoints[MAX_LISTEN_ENDPOINTS];
    int num_listen_endpoints;
    
    /* Register counts */
    int num_coils;
    int num_discrete_inputs;
    int num_holding_registers;
    int num_input_registers;
    
    /* Debug mode */
    bool debug;
} server_config_t;

/* Initialize configuration with defaults */
void config_init(server_config_t *config);

/* Parse command line arguments */
bool config_parse_args(server_config_t *config, int argc, char **argv);

/* Print usage information */
void config_print_usage(const char *program_name);

/* Validate configuration */
bool config_validate(const server_config_t *config);

#endif /* CONFIG_H */
