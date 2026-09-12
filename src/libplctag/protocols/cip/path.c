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
 * The whole grammar:
 *
 *     path      := [ item ( ',' item )* ]
 *     item      := port ( port_tail | separator node | <nothing> )
 *     port      := number | letter [ '2' ]
 *     node      := ipv4 | number
 *     separator := ',' | '/'
 *
 * One character of lookahead after the port picks between a device specific
 * tail and a node, so only the node itself ever backtracks -- and the cursor is
 * an index, so that costs one assignment.
 *
 * A port with nothing after it is the degenerate case.  It emits a single byte,
 * which is what the old segment-at-a-time encoder did for every number, and it
 * is kept so that a path of "1" still means what it used to.
 */

#include <libplctag/protocols/cip/path.h>

#include <ctype.h>
#include <libplctag/lib/libplctag.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <utils/debug.h>
#include <utils/str.h>


/* the port byte bit that says a length byte and a multi-byte link address follow. */
#define CIP_PORT_EXTENDED_LINK ((uint32_t)0x10)

/* channel + this = the port byte to use when the node is an IP address. */
#define CIP_CHANNEL_IP_OFFSET ((uint32_t)0x11)

/* a port is written as one byte, so this is the ceiling for a numeric one. */
#define CIP_PORT_MAX ((uint32_t)0xFF)

/* a link address length is written as one byte. */
#define CIP_NODE_MAX_LEN (255)

/* "255.255.255.255" */
#define CIP_MAX_IP_LEN (15)

#define CIP_IPV4_OCTETS (4)
#define CIP_OCTET_MAX (255)


/*********************************************************************
 ** Cursor
 *********************************************************************/

extern char cip_path_peek(const cip_path_cursor_t *cursor) {
    return (cursor->pos < cursor->len) ? cursor->str[cursor->pos] : '\0';
}


extern bool cip_path_at_end(const cip_path_cursor_t *cursor) { return cursor->pos >= cursor->len; }


extern void cip_path_skip_spaces(cip_path_cursor_t *cursor) {
    while(cip_path_peek(cursor) == ' ') { cursor->pos++; }
}


/* Consume ch if it is next, skipping spaces on either side of it. */
extern bool cip_path_accept(cip_path_cursor_t *cursor, char ch) {
    cip_path_skip_spaces(cursor);

    if(cip_path_peek(cursor) != ch) { return false; }

    cursor->pos++;
    cip_path_skip_spaces(cursor);

    return true;
}


/* Read a run of digits.  False if there is not at least one, or if it overflows. */
extern bool cip_path_read_uint(cip_path_cursor_t *cursor, uint32_t *val) {
    size_t start = cursor->pos;
    uint32_t acc = 0;

    while(isdigit((unsigned char)cip_path_peek(cursor))) {
        if(acc > (UINT32_MAX / 10)) { return false; }

        acc = (acc * 10) + (uint32_t)(cip_path_peek(cursor) - '0');
        cursor->pos++;
    }

    if(cursor->pos == start) { return false; }

    *val = acc;

    return true;
}


/*********************************************************************
 ** Ports
 *********************************************************************/

/*
 * A DH+/RIO channel letter: A or B, optionally followed by a 2.
 *
 * Channels are numbered A=1, B=2, A2=3, B2=4.  The trailing '2' is only taken
 * when it sits directly against the letter, so "A,2" is channel A with node 2
 * and "A2,42" is channel A2 with node 42.
 */
static bool parse_port_letter(cip_path_cursor_t *cursor, uint32_t *channel) {
    char letter = cip_path_peek(cursor);
    uint32_t base = 0;

    if(letter == 'A' || letter == 'a') {
        base = 1;
    } else if(letter == 'B' || letter == 'b') {
        base = 2;
    } else {
        return false;
    }

    cursor->pos++;

    /* the second channel pair shares the letters and adds a '2'. */
    if(cip_path_peek(cursor) == '2') {
        cursor->pos++;
        base += 2;
    }

    *channel = base;

    return true;
}


/*
 * A port, either spelled out as the encoded byte or named by channel letter.
 * port_from_letter tells the caller which, because only a letter port is
 * adjusted once the node turns out to be an IP address.
 */
static int parse_port(cip_path_cursor_t *cursor, uint32_t *port, bool *port_from_letter) {
    cip_path_skip_spaces(cursor);

    if(parse_port_letter(cursor, port)) {
        *port_from_letter = true;
        return PLCTAG_STATUS_OK;
    }

    *port_from_letter = false;

    if(!cip_path_read_uint(cursor, port)) { return PLCTAG_ERR_NOT_FOUND; }

    if(*port > CIP_PORT_MAX) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Port %u does not fit in a port byte (0 to %u).", (unsigned int)(*port),
               (unsigned int)CIP_PORT_MAX);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    return PLCTAG_STATUS_OK;
}


/*********************************************************************
 ** Nodes
 *********************************************************************/

/*
 * A dotted quad, stored as the ASCII text it was written as -- that is what a
 * CIP extended link address carries for an IP node, not four binary octets.
 */
static int parse_ipv4(cip_path_cursor_t *cursor, uint8_t *node, intptr_t capacity, intptr_t *node_len) {
    intptr_t len = 0;

    for(int octet = 0; octet < CIP_IPV4_OCTETS; octet++) {
        size_t digits_start = cursor->pos;
        uint32_t val = 0;

        if(octet > 0) {
            if(cip_path_peek(cursor) != '.') { return PLCTAG_ERR_NOT_FOUND; }

            if(len >= capacity) { return PLCTAG_ERR_TOO_LARGE; }

            node[len] = (uint8_t)'.';
            len++;
            cursor->pos++;
            digits_start = cursor->pos;
        }

        if(!cip_path_read_uint(cursor, &val)) { return PLCTAG_ERR_NOT_FOUND; }

        if(val > CIP_OCTET_MAX) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "IP address octet %u is out of bounds (0 to %u).", (unsigned int)val,
                   (unsigned int)CIP_OCTET_MAX);
            return PLCTAG_ERR_OUT_OF_BOUNDS;
        }

        if((len + (intptr_t)(cursor->pos - digits_start)) > capacity) { return PLCTAG_ERR_TOO_LARGE; }

        /* copy the digits through verbatim; the wire carries the text. */
        for(size_t i = digits_start; i < cursor->pos; i++) {
            node[len] = (uint8_t)cursor->str[i];
            len++;
        }
    }

    *node_len = len;

    return PLCTAG_STATUS_OK;
}


/*
 * A node is either a dotted quad or a single number.  Only the IP form needs
 * lookahead, and the cursor is an index, so the retry costs one assignment.
 */
static int parse_node(cip_path_cursor_t *cursor, uint8_t *node, intptr_t capacity, intptr_t *node_len, bool *node_is_ip) {
    size_t start = cursor->pos;
    uint32_t val = 0;

    cip_path_skip_spaces(cursor);

    if(parse_ipv4(cursor, node, capacity, node_len) == PLCTAG_STATUS_OK) {
        *node_is_ip = true;
        return PLCTAG_STATUS_OK;
    }

    cursor->pos = start;
    *node_is_ip = false;

    cip_path_skip_spaces(cursor);

    if(!cip_path_read_uint(cursor, &val)) { return PLCTAG_ERR_NOT_FOUND; }

    if(val > CIP_OCTET_MAX) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Node address %u is out of bounds (0 to %u).", (unsigned int)val,
               (unsigned int)CIP_OCTET_MAX);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(capacity < 1) { return PLCTAG_ERR_TOO_LARGE; }

    node[0] = (uint8_t)val;
    *node_len = 1;

    return PLCTAG_STATUS_OK;
}


/*********************************************************************
 ** Emitting
 *********************************************************************/

/*
 * A write that ran out of room means the encoded path does not fit, which is
 * what callers check for.  Relabel it here, at the only places a write can
 * fail, so that a genuine range error elsewhere keeps its own code.
 */
static byte_buf relabel_overflow(byte_buf rem) {
    if(byte_buf_has_err(rem) && byte_buf_get_err(rem) == PLCTAG_ERR_OUT_OF_BOUNDS) {
        return byte_buf_make_err("cip_path: encoded path does not fit the buffer", PLCTAG_ERR_TOO_LARGE);
    }

    return rem;
}


/*
 * One CIP port segment.  A single byte node is the short form; anything longer
 * sets the extended-link-address bit, adds a length byte and pads the address
 * out to a 16-bit boundary.
 */
static byte_buf emit_port_node(byte_buf out, uint32_t port, const uint8_t *node, intptr_t node_len) {
    bool extended_link = (node_len > 1);
    byte_buf rem = out;

    if(node_len > CIP_NODE_MAX_LEN) {
        return byte_buf_make_err("cip_path: node address is too long", PLCTAG_ERR_TOO_LARGE);
    }

    rem = byte_buf_encode_uint8(rem, (uint8_t)(extended_link ? (port | CIP_PORT_EXTENDED_LINK) : port));

    if(extended_link) { rem = byte_buf_encode_uint8(rem, (uint8_t)node_len); }

    rem = byte_buf_encode_bytes(rem, node, node_len);

    /* only the extended form pads; a short segment is already two bytes. */
    if(extended_link && (node_len & 1)) { rem = byte_buf_encode_uint8(rem, 0); }

    return relabel_overflow(rem);
}


/*********************************************************************
 ** Items
 *********************************************************************/

/* ',' and '/' both separate a port from its node. */
static bool accept_separator(cip_path_cursor_t *cursor) {
    return cip_path_accept(cursor, ',') || cip_path_accept(cursor, '/');
}


/*
 * A port with no node behind it.  The old encoder emitted every number as its
 * own one byte segment, so a trailing lone port has to keep meaning that.
 */
static byte_buf emit_bare_port(byte_buf out, uint32_t port, bool port_from_letter) {
    if(port_from_letter) {
        return byte_buf_make_err("cip_path: a channel letter needs a node address", PLCTAG_ERR_BAD_PARAM);
    }

    return relabel_overflow(byte_buf_encode_uint8(out, (uint8_t)port));
}


/*
 * One item: a port, then whichever of the three tails follows it.
 *
 * terminal is set when the item may not be followed by anything else, which is
 * a device's decision -- the legacy DH+ form is the only one that says so.
 */
static byte_buf parse_item(cip_path_cursor_t *cursor, byte_buf out, const cip_path_hooks_t *hooks, bool *terminal) {
    uint8_t node[CIP_MAX_IP_LEN];
    intptr_t node_len = 0;
    uint32_t port = 0;
    bool port_from_letter = false;
    bool node_is_ip = false;
    int rc = PLCTAG_STATUS_OK;

    rc = parse_port(cursor, &port, &port_from_letter);

    if(rc != PLCTAG_STATUS_OK) {
        /*
         * NOT_FOUND is how the sub-parsers say "this is not one of mine".  By
         * the time it reaches here there is nothing else it could have been, so
         * it becomes a bad parameter rather than leaking out as not-found.
         */
        return byte_buf_make_err("cip_path: expected a port", (rc == PLCTAG_ERR_NOT_FOUND) ? PLCTAG_ERR_BAD_PARAM : rc);
    }

    /* a device specific tail, such as the legacy DH+ "A:src:dest" form. */
    if(hooks != NULL && hooks->parse_port_tail != NULL && !cip_path_at_end(cursor) && cip_path_peek(cursor) != ',') {
        rc = hooks->parse_port_tail(hooks->ctx, cursor, port, port_from_letter, terminal);

        if(rc == PLCTAG_STATUS_OK) { return out; }

        if(rc != PLCTAG_ERR_NOT_FOUND) { return byte_buf_make_err("cip_path: bad device specific segment", rc); }
    }

    if(!accept_separator(cursor)) { return emit_bare_port(out, port, port_from_letter); }

    rc = parse_node(cursor, node, (intptr_t)sizeof(node), &node_len, &node_is_ip);

    if(rc != PLCTAG_STATUS_OK) {
        return byte_buf_make_err("cip_path: expected a node address", (rc == PLCTAG_ERR_NOT_FOUND) ? PLCTAG_ERR_BAD_PARAM : rc);
    }

    /*
     * A channel letter names a channel, and only an IP node turns it into the
     * extended-address port byte.  A numeric port was already written as the
     * byte the caller wanted and is never adjusted.
     */
    if(port_from_letter && node_is_ip) { port += CIP_CHANNEL_IP_OFFSET; }

    return emit_port_node(out, port, node, node_len);
}


/*********************************************************************
 ** The path
 *********************************************************************/

extern byte_buf cip_path_encode(const char *path, byte_buf out, const cip_path_hooks_t *hooks) {
    cip_path_cursor_t cursor;
    byte_buf rem = out;
    bool terminal = false;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Starting for path \"%s\".", (path != NULL) ? path : "<null>");

    if(path == NULL) { return byte_buf_make_err("cip_path: null path", PLCTAG_ERR_NULL_PTR); }

    cursor.str = path;
    cursor.len = (size_t)(ptrdiff_t)str_length(path);
    cursor.pos = 0;

    cip_path_skip_spaces(&cursor);

    while(!cip_path_at_end(&cursor)) {
        rem = parse_item(&cursor, rem, hooks, &terminal);

        if(byte_buf_has_err(rem)) { break; }

        cip_path_skip_spaces(&cursor);

        if(cip_path_at_end(&cursor)) { break; }

        if(terminal) { return byte_buf_make_err("cip_path: segment must be the last one in the path", PLCTAG_ERR_BAD_PARAM); }

        if(!cip_path_accept(&cursor, ',')) {
            return byte_buf_make_err("cip_path: expected a comma between items", PLCTAG_ERR_BAD_PARAM);
        }
    }

    /*
     * finish only runs on a path that parsed.  byte_buf would pass an existing
     * error through untouched, but relabel_overflow() would then rewrite it,
     * turning a range error into a does-not-fit error.
     */
    if(!byte_buf_has_err(rem) && hooks != NULL && hooks->finish != NULL) {
        rem = relabel_overflow(hooks->finish(hooks->ctx, rem));
    }

    /* the whole path sits on a 16-bit boundary. */
    if(!byte_buf_has_err(rem) && (byte_buf_written(out, rem) & 1)) { rem = relabel_overflow(byte_buf_encode_uint8(rem, 0)); }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Done.");

    return rem;
}
