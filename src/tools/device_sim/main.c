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

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <libplctag/lib/libplctag.h>
#include "platform.h"
#include "utils/debug.h"
#include "args.h"
#include "device.h"
#include "discovery.h"
#include "server.h"

/* ============================================================================
 * Globals — g_terminate defined here; declared extern in server.h.
 * ============================================================================ */

volatile sig_atomic_t g_terminate = 0;

#define DEBUG_MOD DEBUG_MODULE_UTILS

/* ============================================================================
 * Signal handler — async-signal-safe: sets flag only, no mutex.
 * ============================================================================ */

static void sigint_handler(int sig) {
    (void)sig;
    g_terminate = 1;
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(int argc, char **argv) {
    device_t device;
    int32_t  dlvl = PLCTAG_DEBUG_WARN;

    int32_t rc = args_parse(argc, argv, &device, &dlvl);
    if(rc == 1) { return 0; }          /* --help printed */
    if(rc != PLCTAG_STATUS_OK) {
        args_print_usage(argv[0]);
        return 1;
    }

    set_debug_level((int)dlvl);

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0,
           "device_sim starting on port %u.", (unsigned)device.port);

    /* Build registry. */
    registry_t *registry = registry_create();
    if(!registry) {
        fprintf(stderr, "device_sim: failed to create socket registry.\n");
        args_free_tags(device.tags);
        return 1;
    }

    /* Install signal handlers. */
    struct sigaction sa;
    mem_set(&sa, 0, (int)sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Launch TCP listener thread. */
    listener_ctx_t lctx;
    lctx.device   = &device;
    lctx.registry = registry;

    thread_p listener_thread = NULL;
    if(thread_create(&listener_thread, server_listener, 131072, &lctx) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "device_sim: failed to start listener thread.\n");
        registry_destroy(registry);
        args_free_tags(device.tags);
        return 1;
    }

    /* Launch UDP discovery thread. */
    discovery_ctx_t dctx;
    dctx.device   = &device;
    dctx.registry = registry;

    thread_p discovery_thread_handle = NULL;
    if(thread_create(&discovery_thread_handle, discovery_thread, 65536, &dctx) != PLCTAG_STATUS_OK) {
        fprintf(stderr, "device_sim: failed to start discovery thread.\n");
        registry_wake_all(registry);
        thread_join(listener_thread);
        thread_destroy(&listener_thread);
        registry_destroy(registry);
        args_free_tags(device.tags);
        return 1;
    }

    /* Poll until signal. */
    while(!g_terminate) {
        sleep_ms(50);
    }

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0,
           "Shutdown signal received — waking all connections.");

    registry_wake_all(registry);

    thread_join(listener_thread);
    thread_destroy(&listener_thread);

    thread_join(discovery_thread_handle);
    thread_destroy(&discovery_thread_handle);

    registry_destroy(registry);
    args_free_tags(device.tags);

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_INFO, 0, "device_sim stopped cleanly.");
    return 0;
}
