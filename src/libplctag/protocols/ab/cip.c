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
#include <libplctag/protocols/ab/ab_common.h>
#include <libplctag/protocols/ab/cip.h>
#include <libplctag/protocols/cip/cip.h>
#include <libplctag/protocols/cip/path.h>
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/ab/tag.h>
#include <utils/str.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <utils/debug.h>


// #define MAX_IP_ADDR_SEG_LEN (16)


/*
 * AB's path encoding is the generic port/node encoder plus two things no other
 * device has: the legacy DH+ item "A:src:dest", and the decision about whether
 * a connection is needed and what routing to append because of it.
 */

typedef struct {
    plc_type_t plc_type;
    int *needs_connection;
    int *is_dhp;
    uint16_t *dhp_dest;
    uint32_t dhp_port;
    uint32_t dhp_dest_node;
    bool saw_dhp;
} ab_path_ctx_t;


/* DH+ bridging only exists for the PCCC PLC families. */
static bool plc_type_speaks_dhp(plc_type_t plc_type) {
    return (plc_type == AB_PLC_PLC5) || (plc_type == AB_PLC_SLC) || (plc_type == AB_PLC_MLGX);
}


/*
 * The legacy DH+ item, "A:src:dest".  It is one comma-list item that emits no
 * port segment of its own; ab_path_finish() appends the routing for it.
 *
 * NOTE: the source node is parsed and range checked and then never used.  The
 * wire always carries zero for it -- see the four h2le16(0) assignments in
 * pccc.c.  It is still required here so that existing paths keep parsing.
 */
static int ab_parse_port_tail(void *ctx_arg, cip_path_cursor_t *cursor, uint32_t port, bool port_from_letter, bool *terminal) {
    ab_path_ctx_t *ctx = (ab_path_ctx_t *)ctx_arg;
    uint32_t src_node = 0;
    uint32_t dest_node = 0;

    /* only a channel letter introduces a DH+ item. */
    if(!port_from_letter) { return PLCTAG_ERR_NOT_FOUND; }

    if(!cip_path_accept(cursor, ':')) { return PLCTAG_ERR_NOT_FOUND; }

    if(!cip_path_read_uint(cursor, &src_node) || src_node > 255) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, 0, "DH+ source node is missing or out of bounds (0 to 255).");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(!cip_path_accept(cursor, ':')) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, 0, "DH+ segment needs a second colon before the destination node.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(!cip_path_read_uint(cursor, &dest_node) || dest_node > 255) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, 0, "DH+ destination node is missing or out of bounds (0 to 255).");
        return PLCTAG_ERR_BAD_PARAM;
    }

    ctx->saw_dhp = true;
    ctx->dhp_port = port;
    ctx->dhp_dest_node = dest_node;

    /* nothing may follow a DH+ item. */
    *terminal = true;

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Found DH+ segment, port %u, destination node %u.", (unsigned int)port,
           (unsigned int)dest_node);

    return PLCTAG_STATUS_OK;
}


/*
 * Append whatever routing the path implies, now that the whole path is known.
 * This is also where needs_connection is decided, which is why the generic
 * encoder never sees it.
 */
static byte_buf ab_path_finish(void *ctx_arg, byte_buf out) {
    ab_path_ctx_t *ctx = (ab_path_ctx_t *)ctx_arg;
    byte_buf rem = out;

    *(ctx->is_dhp) = ctx->saw_dhp ? 1 : 0;
    *(ctx->dhp_dest) = 0;

    if(ctx->saw_dhp) {
        if(!plc_type_speaks_dhp(ctx->plc_type)) {
            pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, 0, "A DH+ path is only valid for a PLC5, SLC or MicroLogix.");
            return byte_buf_make_err("ab path: DH+ segment on a PLC that does not bridge DH+", PLCTAG_ERR_BAD_PARAM);
        }

        /* DH+ bridging always needs a connection. */
        *(ctx->needs_connection) = 1;
        *(ctx->dhp_dest) = (uint16_t)ctx->dhp_dest_node;

        /* the DH+ routing: class 0xA6, instance = channel, connection point 1. */
        rem = byte_buf_encode_uint8(rem, 0x20);
        rem = byte_buf_encode_uint8(rem, 0xA6);
        rem = byte_buf_encode_uint8(rem, 0x24);
        rem = byte_buf_encode_uint8(rem, (uint8_t)ctx->dhp_port);
        rem = byte_buf_encode_uint8(rem, 0x2C);
        rem = byte_buf_encode_uint8(rem, 0x01);

        return rem;
    }

    if(*(ctx->needs_connection)) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "PLC needs a connection, adding the path to the router object.");

        /* the message router object: class 2, instance 1. */
        rem = byte_buf_encode_uint8(rem, 0x20);
        rem = byte_buf_encode_uint8(rem, 0x02);
        rem = byte_buf_encode_uint8(rem, 0x24);
        rem = byte_buf_encode_uint8(rem, 0x01);
    }

    return rem;
}


extern int cip_encode_path(const char *path, int *needs_connection, plc_type_t plc_type, uint8_t *tmp_conn_path,
                           int *tmp_conn_path_size, int *is_dhp, uint16_t *dhp_dest) {
    ab_path_ctx_t ctx;
    cip_path_hooks_t hooks;
    byte_buf out;
    byte_buf rem;
    intptr_t written = 0;

    ctx.plc_type = plc_type;
    ctx.needs_connection = needs_connection;
    ctx.is_dhp = is_dhp;
    ctx.dhp_dest = dhp_dest;
    ctx.dhp_port = 0;
    ctx.dhp_dest_node = 0;
    ctx.saw_dhp = false;

    hooks.parse_port_tail = ab_parse_port_tail;
    hooks.finish = ab_path_finish;
    hooks.ctx = &ctx;

    *is_dhp = 0;

    /*
     * The old encoder kept MAX_IP_ADDR_SEG_LEN of slack past its own limit so
     * that a trailing address could not run off the end.  byte_buf refuses the
     * overrun itself, so the limit is just the caller's buffer now.
     */
    out = byte_buf_make(tmp_conn_path, (intptr_t)(*tmp_conn_path_size));

    rem = cip_path_encode(path, out, &hooks);

    if(byte_buf_has_err(rem)) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, 0, "Unable to encode path \"%s\": %s", path, byte_buf_get_err_msg(rem));
        return (int)byte_buf_get_err(rem);
    }

    written = byte_buf_written(out, rem);

    *tmp_conn_path_size = (int)written;

    return PLCTAG_STATUS_OK;
}


