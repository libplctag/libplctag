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
 * logix_client.h — Logix/Micro800 client dialect (3.d, moved out of
 * enip_session.c). Reached through enip_dialect_t (enip_logix_dialect,
 * declared in client/enip_dialect.h). enip_logix_build/enip_logix_apply are
 * also reused directly by dialects/omron/omron_client.c's enip_omron_dialect
 * (identical path encoding and CIP Common Format reply framing; only the
 * listing pair and Forward Open size differ).
 */

#include <stdbool.h>
#include <utils/bytes.h>
#include <libplctag/protocols/enip/client/enip_connection_internal.h>

extern Bytes enip_logix_build(enip_connection_t *c, enip_tag_p t, Bytes dest);
extern int32_t enip_logix_apply(enip_connection_t *c, enip_tag_p t, Bytes cip_reply, bool *more);
