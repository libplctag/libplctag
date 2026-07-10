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
 * Tag Name Utilities Implementation
 *
 * STATUS: CORRECT, but enip_name_encode_route is currently never called.
 *         Wire it in during Phase 3.
 *
 * Phase 3 (Routing): call enip_name_encode_route from enip_connection_create
 *   after parsing the "path" attribute.  Store the result in conn->conn_path and
 *   conn->conn_path_size.  This is what enip_connection_forward_open uses as the
 *   connection path and what Unconnected_Send needs as the route_path (plan §3 K).
 *
 * No functional changes needed to this file.
 */

#include <libplctag/protocols/enip/client/enip.h>
#include <libplctag/protocols/enip/client/enip_cip.h>
#include <libplctag/protocols/enip/client/enip_name.h>
#include <utils/debug.h>
#include <string.h>

/* Phase 2: correct as-is.  Called during tag creation to extract the root name
 * (before '[' or '.') for Phase-1 metadata lookup. */
size_t enip_name_extract_root(const char *tag_path, char *buf, size_t buf_size) {
    if(!tag_path || !buf || buf_size == 0) { return 0; }

    /* Root ends at the first '[' or '.' or end of string */
    size_t len = 0;
    while(tag_path[len] != '\0' && tag_path[len] != '[' && tag_path[len] != '.') {
        len++;
    }

    if(len == 0 || len >= buf_size) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP name: root segment length %zu invalid (buf_size=%zu)", len, buf_size);
        return 0;
    }

    memcpy(buf, tag_path, len);
    buf[len] = '\0';

    return len;
}

/* Phase 2: correct as-is — thin wrapper around enip_cip_encode_tag_path.
 * No changes needed. */
size_t enip_name_encode_path(const char *tag_path, uint8_t *buf, size_t buf_size) {
    return enip_cip_encode_tag_path(tag_path, buf, buf_size);
}

/*
 * Route encoding
 *
 * CIP port segment format:
 *   Extended port segment (link address is a string, e.g. IP address):
 *     byte 0: 0x1F | (port_number & 0x0F) if port <= 14, else 0x1F (extended)
 *             Actually: 0x1? where low nibble is port; 0x1F signals extended (string) link
 *     byte 1: length of link address string
 *     bytes 2..(2+len-1): link address bytes (ASCII, not null-terminated)
 *     pad byte if (2+len) is odd
 *
 *   Simple port segment (link address is a single byte slot number):
 *     byte 0: 0x01 | ((port_number & 0x0F) << 4) -- no, that's wrong
 *
 * The CIP port segment encoding from the AB code (see defs.h path comments):
 *   0x01, slot  -- backplane port 1 (default for EN2T), slot = CPU slot number
 * For backplane routing:  {0x01, slot}  (2 bytes, port 1 = backplane)
 * For Ethernet routing (IP link address):
 *   {0x1F, len, ip_bytes..., [pad]}
 *   where 0x1F = extended port segment, port = 2 (Ethernet)
 *   Actually the correct encoding from the Logix manual:
 *     0x1? -- port segment with extended link address
 *     Low 4 bits of byte 0 = port number (2 for Ethernet/IP port)
 *     0x12 = port 2 with extended link; byte 1 = link address byte count
 *
 * Supported route string forms:
 *   "A,<slot>"  -- port 18 (0x12 in hex), backplane to slot
 *   "B,<slot>"  -- port 19 (0x13)
 *   "<port>,<slot>"  -- numeric port, slot in [0..15]
 *   "<port>,<ip>"    -- numeric port, IP dotted-decimal link address
 */

/* Phase 3: correct as-is — currently never called.
 * Phase 3 wiring: call this from enip_connection_create with the "path" attribute
 * (e.g. "1,4" or "1,5").  Store result in conn->conn_path / conn->conn_path_size.
 * Used both in enip_connection_forward_open (connection path) and in the
 * Unconnected_Send route path (plan §3 K). */
size_t enip_name_encode_route(const char *route, uint8_t *buf, size_t buf_size) {
    if(!route || !buf || buf_size == 0) { return 0; }

    /* Parse "<port_spec>,<link>" */
    const char *comma = strchr(route, ',');
    if(!comma) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
               "ENIP route: missing comma in '%s'", route);
        return 0;
    }

    /* Determine port number */
    uint8_t port_num = 0;
    if(route[0] == 'A' || route[0] == 'a') {
        port_num = 18;
    } else if(route[0] == 'B' || route[0] == 'b') {
        port_num = 19;
    } else {
        /* Numeric port */
        uint32_t p = 0;
        const char *s = route;
        while(s < comma && *s >= '0' && *s <= '9') {
            p = p * 10u + (uint32_t)(*s - '0');
            s++;
        }
        if(s != comma || p > 255u) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                   "ENIP route: invalid port spec in '%s'", route);
            return 0;
        }
        port_num = (uint8_t)p;
    }

    const char *link = comma + 1;

    /* Determine whether link address is a slot number (all digits) or IP string */
    bool is_ip = false;
    for(const char *c = link; *c != '\0'; c++) {
        if(*c == '.') { is_ip = true; break; }
    }

    if(!is_ip) {
        /* Simple port segment: byte0 = 0x01 (port segment, 1-byte link), byte1 = slot */
        uint32_t slot = 0;
        const char *s = link;
        while(*s >= '0' && *s <= '9') {
            slot = slot * 10u + (uint32_t)(*s - '0');
            s++;
        }
        if(*s != '\0' || slot > 255u) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                   "ENIP route: invalid slot number in '%s'", route);
            return 0;
        }
        if(buf_size < 2) { return 0; }
        buf[0] = port_num;
        buf[1] = (uint8_t)slot;
        return 2;
    } else {
        /* Extended port segment: byte0 = 0x10 | (port & 0x0F) -- lower nibble = port
         * but extended link flag is set by bit 4 of the segment type.
         * CIP spec: segment type 0x1? with extended link = 0x1F when port <= 15,
         * or the high nibble encodes port if port > 15.
         *
         * For Ethernet ports (port 2): byte0 = 0x12 (port 2, extended link).
         * General: byte0 = 0x10 | (port_num & 0x0F) when port_num fits in 4 bits,
         *          byte0 = 0x1F (always signals extended) for higher port numbers.
         */
        size_t link_len = strlen(link);
        uint8_t pad = (uint8_t)((2u + link_len) & 1u); /* pad to even total */
        size_t total = 2 + link_len + pad;

        if(buf_size < total) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0,
                   "ENIP route: buffer too small for IP link (%zu needed)", total);
            return 0;
        }

        buf[0] = (port_num <= 14u) ? (uint8_t)(0x10u | port_num) : 0x1Fu;
        buf[1] = (uint8_t)(link_len & 0xFFu);
        memcpy(&buf[2], link, link_len);
        if(pad) { buf[2 + link_len] = 0x00; }

        return total;
    }
}
