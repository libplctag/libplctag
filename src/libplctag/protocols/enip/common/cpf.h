#pragma once

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

#include <stdint.h>

#include "utils/arena.h"
#include "utils/bytes.h"
#include <libplctag/protocols/enip/server/device.h>

/* ============================================================================
 * CPF item type codes
 * ============================================================================ */

#define CPF_ITEM_NULL_ADDR  ((uint16_t)0x0000)
#define CPF_ITEM_CONN_ADDR  ((uint16_t)0x00A1)
#define CPF_ITEM_CONN_DATA  ((uint16_t)0x00B1)
#define CPF_ITEM_UCONN_DATA ((uint16_t)0x00B2)

/* ============================================================================
 * CPF framing sizes
 * ============================================================================ */

#define CPF_HEADER_SIZE                ((size_t)8)
#define CPF_UNCONNECTED_ADDR_ITEM_SIZE ((size_t)4)
#define CPF_UNCONNECTED_DATA_ITEM_SIZE ((size_t)4)
#define CPF_CONNECTED_ADDR_ITEM_SIZE   ((size_t)8)
#define CPF_CONNECTED_DATA_ITEM_SIZE   ((size_t)4)
#define CPF_CONN_SEQ_NUM_SIZE          ((size_t)2)

/* ============================================================================
 * Public API
 * ============================================================================ */

extern Bytes cpf_handle_unconnected(Arena *a, Bytes payload, eip_session_t *sess, device_t *dev);
extern Bytes cpf_handle_connected(Arena *a, Bytes payload, eip_session_t *sess, device_t *dev);
