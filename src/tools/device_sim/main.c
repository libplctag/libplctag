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
#include "device_sim.h"
#include "args.h"

/* ============================================================================
 * Signal flag — set by handler, consumed by the main loop.
 * The handler only writes a flag; device_sim_stop (which takes a mutex) is
 * called from the main loop, not from the handler itself.
 * ============================================================================ */

static volatile sig_atomic_t g_signal = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_signal = 1;
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(int argc, char **argv) {
    device_sim_t *sim    = NULL;
    int32_t       dlvl   = PLCTAG_DEBUG_WARN;

    int32_t rc = args_parse(argc, argv, &sim, &dlvl);
    if(rc == 1) { return 0; }          /* --help printed */
    if(rc != PLCTAG_STATUS_OK) {
        args_print_usage(argv[0]);
        return 1;
    }

    set_debug_level((int)dlvl);

    /* Install signal handlers. */
    struct sigaction sa;
    mem_set(&sa, 0, (int)sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    rc = device_sim_start(sim);
    if(rc != PLCTAG_STATUS_OK) {
        fprintf(stderr, "device_sim: failed to start (%d).\n", (int)rc);
        device_sim_destroy(sim);
        return 1;
    }

    /* Wait for signal. */
    while(!g_signal) {
        sleep_ms(50);
    }

    device_sim_stop(sim);
    device_sim_destroy(sim);
    return 0;
}
