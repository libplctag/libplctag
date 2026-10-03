#pragma once

/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
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

/*
 * The library's connection idle timeout, in milliseconds.
 *
 * This is the value the library promises, and everything else is derived from it.  A
 * protocol that can tell the device how long to hold a connection open rounds *up* from
 * this, so the device always grants at least this much; a protocol with no such mechanism
 * -- Modbus, whose devices have no built-in idle disconnect -- simply uses it.  Either way
 * the library closes first, by CONN_CLOSE_FIRST_MARGIN_MS.
 *
 * These are plain integer literals on purpose: cip/conn.h picks its EtherNet/IP timeout
 * multiplier with a preprocessor ladder, which cannot evaluate anything else.
 */
#define CONN_INACTIVITY_TIMEOUT_MAX_MS (31000)
#define CONN_INACTIVITY_TIMEOUT_MIN_MS (100)
#define CONN_CLOSE_FIRST_MARGIN_MS (1000)
