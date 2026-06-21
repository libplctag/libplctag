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

#include "platform.h"
#include "utils/arena.h"
#include "utils/bytes.h"
#include "device.h"
#include "server.h"

/* ============================================================================
 * EIP discovery command codes (UDP 44818)
 * ============================================================================ */

#define EIP_CMD_LIST_SERVICES    ((uint16_t)0x0004)
#define EIP_CMD_LIST_IDENTITY    ((uint16_t)0x0063)
#define EIP_CMD_LIST_INTERFACES  ((uint16_t)0x0064)

/* ============================================================================
 * Discovery thread context
 * ============================================================================ */

typedef struct {
    device_t   *device;
    registry_t *registry;
} discovery_ctx_t;

/* UDP listener thread — started from main.c, runs until g_terminate. */
extern THREAD_FUNC(discovery_thread);

/*
 * Build the CPF body for a List Identity reply.
 * Called both from the UDP thread and from eip_dispatch() for TCP 0x0063.
 * local_ipv4 (host byte order) is the reply socket address the client should
 * connect back to; the caller computes it from the request's arrival path.
 */
extern Bytes discovery_list_identity_cpf(Arena *a, device_t *dev, uint32_t local_ipv4);

/* Build the CPF body for a List Services reply. */
extern Bytes discovery_list_services_cpf(Arena *a);

/* Build the CPF body for a List Interfaces reply (empty item list). */
extern Bytes discovery_list_interfaces_cpf(Arena *a);
