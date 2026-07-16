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
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/*
 * omron_listing.h — OMRON NJ/NX CIP tag and UDT listing dialect.
 *
 * Handles class 0x6A (Tag Name Server, service 0x5F GetInstanceListEx2) and
 * class 0x6C (Variable Type Object, service 0x01 GetAttributeAll) per
 * OMRON-SPECIFIC-DESIGN.md §5. Registered through the generic CIP object
 * registry so core cip.c is never touched to add vendor-specific behaviour.
 *
 * Call omron_listing_register() once per device, before device_sim_start().
 */

#include <stdint.h>
#include <libplctag/protocols/enip/server/device.h>
#include <libplctag/protocols/enip/server/device_sim.h>

extern int32_t omron_listing_register(device_sim_t *sim, device_t *dev);
