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

#pragma once

#include <signal.h>
#include <stdint.h>

#include "platform.h"
#include "device.h"

/* ============================================================================
 * Global termination flag — defined in main.c, checked by server threads.
 * ============================================================================ */

extern volatile sig_atomic_t g_terminate;

/* ============================================================================
 * Live-socket registry — mutex-protected array of all active sockets.
 * The signal handler sets g_terminate; main then calls registry_wake_all()
 * with the mutex to unblock every blocking operation.
 * ============================================================================ */

#define REGISTRY_MAX 64

typedef struct {
    mutex_p mutex;
    sock_p  socks[REGISTRY_MAX];
} registry_t;

extern registry_t *registry_create(void);
extern void        registry_destroy(registry_t *reg);
extern void        registry_add(registry_t *reg, sock_p sock);
extern void        registry_remove(registry_t *reg, sock_p sock);
extern void        registry_wake_all(registry_t *reg);

/* ============================================================================
 * Listener thread context and entry point.
 * ============================================================================ */

typedef struct {
    device_t   *device;
    registry_t *registry;
} listener_ctx_t;

extern THREAD_FUNC(server_listener);
