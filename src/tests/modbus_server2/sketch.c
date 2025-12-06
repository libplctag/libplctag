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

#include "../pt_net/pt_net.h"
#include "../utils/buf.h"
#include "../utils/err.h"


static util_err_t modbus_framer(buf_t *input, void *context);
static PT_NET_FUNC(lifetime_handler);
static PT_NET_FUNC(listener_handler);
static PT_NET_FUNC(modbus_server2_client_handler);


util_err_t modbus_framer(buf_t *input, void *context) {
    size_t initial_start = buf_read_pos(input);

    /* we have enough for the MBAP header */
    uint16_t transaction_id = 0;
    uint16_t protocol_id = 0;
    uint16_t length_field = 0;
    uint8_t unit_id = 0;
    bool ok = buf_read_u16_be(input, "transaction_id", &transaction_id)
           && buf_read_u16_be(input, "protocol_id", &protocol_id)
           && buf_read_u16_be(input, "length_field", &length_field);

    if(!ok && buf_get_error(input) == UTIL_EBOUNDS) {
        buf_clear_error(input);
        buf_set_read_pos(input, initial_start);
        return UTIL_EBOUNDS;
    }

    if(buf_size(input) < length_field) {
        buf_clear_error(input);
        buf_set_read_pos(input, initial_start);
        return UTIL_EBOUNDS;
    }

    buf_set_read_pos(input, initial_start);

    return UTIL_OK;
}


PT_NET_FUNC(listener_handler) {
    util_err_t err = UTIL_OK;

    server_ctx_t *ctx = (server_ctx_t *)context;
    net_pt_core_t *core = ctx->core;

    PT_NET_FUNC_BODY_START

    /* first open a listener socket */
    err = pt_net_open_tcp_listener(&ctx->listener_sock, core, ctx->listener_addr, ctx->listener_port, modbus_framer, ctx);
    if(err != UTIL_OK) {
        pdlog(MODBUS_MODULE_SERVER2, LOG_LEVEL_ERROR,
              "Failed to open listener socket on %s:%d: %s",
              ctx->listener_addr, ctx->listener_port, util_err_str(err));
        return err;
    }

    do {
        net_pt_socket_t client_sock;
        err = pt_net_accept_tcp_connection(&client_sock, core, ctx->listener_sock, ctx);
        if(err != UTIL_OK) {
            pdlog(MODBUS_MODULE_SERVER2, LOG_LEVEL_ERROR,
                  "Failed to accept incoming Modbus TCP connection: %s",
                  util_err_str(err));
            continue;
        }

        pdlog(MODBUS_MODULE_SERVER2, LOG_LEVEL_INFO,
              "Accepted new Modbus TCP connection.");


        client_ctx_t *client_ctx = calloc(1, sizeof(client_ctx_t));
        if(!client_ctx) {
            pdlog(MODBUS_MODULE_SERVER2, LOG_LEVEL_ERROR,
                  "Failed to allocate memory for Modbus TCP client context.");
            pt_net_close_tcp_socket(core, client_sock);
            return UTIL_ERESOURCE;
        }

        client_ctx->core = core;
        client_ctx->server_ctx = ctx;
        client_ctx->client_sock = client_sock;

        err = pt_net_spawn_thread(NULL, core, client_ctx, modbus_server2_client_handler, "modbus_server2_client");
        if(err != UTIL_OK) {
            pdlog(MODBUS_MODULE_SERVER2, LOG_LEVEL_ERROR,
                  "Failed to spawn Modbus TCP client handler thread: %s",
                  util_err_str(err));
            pt_net_close_tcp_socket(core, client_sock);
            free(client_ctx);
            return err;
        }
    } while(true);

    PT_NET_FUNC_BODY_END
}


PT_NET_FUNC(lifetime_handler) {
    util_err_t err = UTIL_OK;

    server_ctx_t *ctx = (server_ctx_t *)context;
    net_pt_core_t *core = ctx->core;

    PT_NET_FUNC_BODY_START

    /* set up the listener thread */
    err = pt_net_spawn_thread(&ctx->listener_thread, core, context, listener_handler, "modbus_server2_listener");
    if(err != UTIL_OK) {
        pdlog(MODBUS_MODULE_SERVER2, LOG_LEVEL_ERROR,
              "Failed to spawn listener thread: %s", util_err_str(err));
        return err;
    }

    /* wait for wakeup to shut down */
    net_pt_suspend_thread(this_pt);

    pdlog(MODBUS_MODULE_SERVER2, LOG_LEVEL_INFO,
          "Shutting down Modbus TCP server.");

    err = pt_net_join_thread(core, ctx->listener_thread);
    if(err != UTIL_OK) {
        pdlog(MODBUS_MODULE_SERVER2, LOG_LEVEL_ERROR,
              "Failed to join listener thread: %s", util_err_str(err));
        return err;
    }

    PT_NET_FUNC_BODY_END
}



