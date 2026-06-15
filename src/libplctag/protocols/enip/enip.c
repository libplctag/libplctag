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
 * Module entry points (design doc §14.1).  Thin forwarders to
 * enip_session.c / enip_tag.c.
 */

#include <libplctag/protocols/enip/enip.h>
#include <libplctag/protocols/enip/enip_session.h>
#include <libplctag/protocols/enip/enip_tag.h>
#include <utils/debug.h>

int enip_init(void) {
    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "Starting.");

    return enip_session_module_init();
}

void enip_teardown(void) {
    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "Starting.");

    enip_session_module_teardown();
}

plc_tag_p enip_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                           void *userdata, plc_tag_p src_tag) {
    return enip_tag_create_impl(attribs, tag_callback_func, userdata, src_tag);
}
