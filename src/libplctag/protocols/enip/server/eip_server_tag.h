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

#include <libplctag/lib/init.h>

/*
 * eip_server_tag.c — the (protocol,role=server) constructor for CIP server
 * tags (SERVER_TAGS.md).  plc_tag_create("protocol=ab-eip&role=server&...")
 * and protocol=enip-tcp both route here (see lib/init.c's tag_type_map).
 *
 * A server tag is a normal plc_tag whose own data buffer is read/written by
 * the plc_tag_get_ and plc_tag_set_ accessors; its read()/write() vtable
 * functions copy that buffer to/from a tag_def_t registered on a shared,
 * refcounted endpoint
 * (server/endpoint.c), which the listener thread (server/server.c ->
 * common/cip.c) serves to remote clients.
 */
extern plc_tag_p eip_server_tag_create(attr attribs,
                                       void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                       void *userdata, plc_tag_p src_tag);
