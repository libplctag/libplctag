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

/*
 * The TCP listener + connection registry are now the protocol-agnostic
 * net/tcp_server.{c,h} (ENIP-UPDATES-PLAN.md items 3.4/3.5); this header
 * just exposes the EIP-specific listener entry point (eip_server.c) that
 * plugs EIP framing/dispatch into it. tcp_registry_t is what device_sim.c
 * and discovery.c share for the live-socket registry.
 */

#include "platform.h"
#include "net/tcp_server.h"
#include "device.h"

/* Allocate a tcp_server_config_t for EIP, start the listener thread on
 * device->bind_addr:device->port, and hand back its thread_p (join/destroy
 * it like any other thread_create() result). */
extern int32_t eip_server_start_listener(device_t *device, tcp_registry_t *registry, thread_p *out_thread);
