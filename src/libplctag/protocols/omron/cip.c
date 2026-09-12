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

#include <ctype.h>
#include <errno.h>
#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/cip/cip.h>
#include <libplctag/protocols/cip/path.h>
#include <libplctag/protocols/cip/cip.h>
#include <libplctag/protocols/cip/error_codes.h>
#include <libplctag/protocols/omron/cip.h>
#include <libplctag/protocols/omron/defs.h>
#include <libplctag/protocols/omron/omron_common.h>
#include <libplctag/protocols/omron/tag.h>
#include <utils/str.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <utils/debug.h>


// #define MAX_IP_ADDR_SEG_LEN (16)


/*
 * Omron NJ/NX speaks plain CIP over EtherNet/IP: port and node pairs, and a path
 * to the message router when the connection needs one.  There is no DH+
 * bridging, so it supplies no parse_port_tail hook and a DH+ path is rejected
 * rather than parsed.
 *
 * NOTE: the previous encoder DID parse the DH+ form, set is_dhp from it, and
 * then ignore it -- the routing that AB appends for such a path was never
 * emitted here.  A DH+ path that AB rejects therefore produced a silently wrong
 * connection path on Omron.  It is an error now.
 */

static byte_buf omron_path_finish(void *ctx_arg, byte_buf out) {
    int *needs_connection = (int *)ctx_arg;
    byte_buf rem = out;

    if(!*needs_connection) { return rem; }

    pdebug(DEBUG_MODULE_OMRON_CIP, DEBUG_DETAIL, 0, "Connection needed, adding the path to the router object.");

    /* the message router object: class 2, instance 1. */
    rem = byte_buf_encode_uint8(rem, 0x20);
    rem = byte_buf_encode_uint8(rem, 0x02);
    rem = byte_buf_encode_uint8(rem, 0x24);
    rem = byte_buf_encode_uint8(rem, 0x01);

    return rem;
}


extern int omron_encode_path(const char *path, int *needs_connection, uint8_t *tmp_conn_path, int *tmp_conn_path_size) {
    cip_path_hooks_t hooks;
    byte_buf out;
    byte_buf rem;

    hooks.parse_port_tail = NULL;
    hooks.finish = omron_path_finish;
    hooks.ctx = needs_connection;

    out = byte_buf_make(tmp_conn_path, (intptr_t)(*tmp_conn_path_size));

    rem = cip_path_encode(path, out, &hooks);

    if(byte_buf_has_err(rem)) {
        pdebug(DEBUG_MODULE_OMRON_CIP, DEBUG_WARN, 0, "Unable to encode path \"%s\": %s", path, byte_buf_get_err_msg(rem));
        return (int)byte_buf_get_err(rem);
    }

    *tmp_conn_path_size = (int)byte_buf_written(out, rem);

    return PLCTAG_STATUS_OK;
}


