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

/*
 * pccc_defs.h — PCCC (Execute-PCCC, CIP service 0x4B) wire constants shared
 * by both halves of the dialect: dialects/pccc/pccc.c (server) and
 * dialects/pccc/pccc_client.c (client, PLC-5/SLC500/MicroLogix). Previously
 * duplicated as two independently-named copies (client/enip_session.c's
 * PCCC_PLC5_READ_FNC etc. vs this file's PLC5_CMD_READ etc.) with identical
 * values (3.d).
 */

#include <stddef.h>
#include <stdint.h>

#define PCCC_EXECUTE_SVC ((uint8_t)0x4B)
#define PCCC_TYPED_CMD   ((uint8_t)0x0F)

#define PCCC_PLC5_READ_FNC  ((uint8_t)0x01)
#define PCCC_PLC5_WRITE_FNC ((uint8_t)0x00)
#define PCCC_PLC5_RMW_FNC   ((uint8_t)0x26)
#define PCCC_SLC_READ_FNC   ((uint8_t)0xA2)
#define PCCC_SLC_WRITE_FNC  ((uint8_t)0xAA)
#define PCCC_SLC_RMW_FNC    ((uint8_t)0xAB)

#define PCCC_VENDOR_ID ((uint16_t)0xF33D)     /* matches ab/defs.h AB_EIP_VENDOR_ID */
#define PCCC_VENDOR_SN ((uint32_t)0x21504345) /* matches ab/defs.h AB_EIP_VENDOR_SN */

/* PCCC has no fragmentation status (unlike Logix's CIP_STATUS_FRAG), so every
 * read/write/@tags-listing round trip on both sides is capped at a fixed
 * page that fits comfortably under the single-byte SLC transfer-size field
 * (max 255) and typical DF1/EtherNet-IP embedded-packet limits. */
#define PCCC_MAX_TRANSFER_BYTES ((size_t)240)
#define PCCC_MAX_TRANSFER_WORDS ((uint16_t)(PCCC_MAX_TRANSFER_BYTES / 2))
