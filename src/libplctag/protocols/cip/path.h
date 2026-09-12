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
 * CIP connection path encoding.
 *
 * A path is a comma separated list of items.  An ordinary item is a port and a
 * node address, which together encode as one CIP port segment:
 *
 *     1,0               port 1, node 0          -> 01 00
 *     18,10.206.1.39    port 18, extended node  -> 12 0B "10.206.1.39" 00
 *
 * An IP address is not a separate kind of segment.  It is a link address whose
 * length is greater than one, so the port byte carries the extended-link-address
 * bit and a length byte follows.  One encoder covers both forms.
 *
 * A port may be written as a number or as a DH+/RIO channel letter:
 *
 *     A, a  -> 1        A2, a2 -> 3
 *     B, b  -> 2        B2, b2 -> 4
 *
 * A NUMERIC PORT IS THE ENCODED PORT BYTE, not a logical port number.  That is
 * why 18 pairs with an IP address: 18 is 0x12, which is the extended-link-address
 * bit 0x10 plus port 2.  Nothing is added to a numeric port.
 *
 * A LETTER PORT names a channel, and the channel number alone goes on the wire
 * when the node is a plain number.  When the node is an IP address the port byte
 * becomes the channel plus 0x11 -- which sets that same 0x10 bit and adds one, so
 * A with an IP node is 18 and A2 is 20.  The offset is empirical, not derived
 * from the specification; do not "simplify" it to a plain bit set.
 *
 * Everything beyond that is device specific and arrives through
 * cip_path_hooks_t, so this file knows nothing about DH+ routing, message router
 * paths or PLC types.
 */

#include <libplctag/protocols/cip/defs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <utils/byte_buf.h>


/* Cursor over the path string.  The path is text, so this is not a byte_buf. */
typedef struct {
    const char *str;
    size_t len;
    size_t pos;
} cip_path_cursor_t;


/*
 * Device specific behaviour.  Either entry may be NULL, and hooks itself may be
 * NULL for a device that needs neither.
 *
 * parse_port_tail is called when an item's port is followed by something that is
 * not a separator.  Today the only such form is the legacy DH+ item
 * "A:src:dest".  Return PLCTAG_ERR_NOT_FOUND to decline, leaving the cursor
 * where it was.  Setting *terminal makes the item the last one the path may
 * hold.  The hook emits nothing itself; whatever it needs goes on in finish.
 *
 * finish is called once, after the last item and before the path is padded to a
 * 16-bit boundary.  It appends whatever the device's own routing needs, and
 * follows the byte_buf convention: it is handed the space that is left and
 * returns what is left after it.
 */
typedef struct {
    int (*parse_port_tail)(void *ctx, cip_path_cursor_t *cursor, uint32_t port, bool port_from_letter, bool *terminal);
    byte_buf (*finish)(void *ctx, byte_buf out);
    void *ctx;
} cip_path_hooks_t;


/* Cursor helpers, exported because a device's parse_port_tail needs them. */
extern char cip_path_peek(const cip_path_cursor_t *cursor);
extern bool cip_path_at_end(const cip_path_cursor_t *cursor);
extern void cip_path_skip_spaces(cip_path_cursor_t *cursor);
extern bool cip_path_accept(cip_path_cursor_t *cursor, char ch);
extern bool cip_path_read_uint(cip_path_cursor_t *cursor, uint32_t *val);

/*
 * Encode path into out.  Returns the space that is left, or an error buffer;
 * byte_buf_written(out, result) is the encoded length.
 */
extern byte_buf cip_path_encode(const char *path, byte_buf out, const cip_path_hooks_t *hooks);
