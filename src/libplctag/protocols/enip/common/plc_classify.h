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
#include "utils/bytes.h"
#include "plc_type.h"

/*
 * Classify a CIP Identity Get_Attributes_All reply into a specific PLC
 * family, by vendor id + product-name catalog-family prefix. Shared by both
 * directions of the ENIP protocol: the client uses this to auto-classify a
 * live connection; the server (device_sim/eip_server_tag) uses the
 * resulting enip_plc_type_t to select which family to emulate (plc_type.h).
 *
 * Deliberately centralized here rather than split into per-vendor dialect
 * directories (dialects/rockwell, dialects/omron): List Identity /
 * Get_Attributes_All may be answered by *either* the PLC's own CPU or a
 * separate Ethernet bridge/adapter module sitting in the same chassis --
 * e.g. a ControlLogix rack's 1756-ENBT/EN2T/EN4TR, an SLC 500's
 * 1747-AENTR remote I/O adapter, a CompactLogix/1769 I/O system's
 * 1769-AENTR. Either one's product name shares its family's catalog prefix
 * ("1756-", "1747-", "1769-", ...) even though CPU vs adapter modules
 * report different CIP device_type values -- so device_type is NOT part of
 * the match, only vendor id + catalog-family prefix. The prefix table lives
 * in one place (plc_classify.c) because the vendor ids, device families,
 * and their real-world product-name collisions (adapters/bridges sharing a
 * CPU's catalog prefix) all have to be reasoned about together to get this
 * right -- splitting it across per-vendor files made it too easy for one
 * vendor's table to drift out of sync with what's actually been verified.
 *
 * reply_data is the full Get_Attributes_All payload (fixed 14-byte prefix +
 * product-name SHORT_STRING). Returns ENIP_PLC_UNKNOWN for anything that
 * doesn't match a known family -- never defaults to a specific family.
 */
extern enip_plc_type_t enip_classify_plc(uint16_t vendor_id, Bytes reply_data);

/*
 * Classify by catalog-prefix match alone (no vendor id, no wire reply) --
 * used for the client's model= attribute (ENIP-SESSION-DESIGN.md), which
 * lets a caller override a connection's auto-detected family, e.g. when
 * talking through a bridge/adapter whose own identity does not reflect the
 * end device, or against a device_sim endpoint. Matches the same table as
 * enip_classify_plc(), ignoring vendor id (an explicit override is trusted
 * as given). Returns ENIP_PLC_UNKNOWN if name matches no known prefix.
 */
extern enip_plc_type_t enip_classify_plc_by_name(const char *name);
