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
#include <libplctag/protocols/ab/defs.h>
#include <libplctag/protocols/ab/tag.h>
#include <platform.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <utils/debug.h>


static int skip_whitespace(const char *name, int *name_index);
static int parse_bit_segment(ab_tag_p tag, const char *name, int *name_index);
static int parse_symbolic_segment(ab_tag_p tag, const char *name, int *encoded_index, int *name_index);
static int parse_numeric_segment(ab_tag_p tag, const char *name, int *encoded_index, int *name_index);

static int match_numeric_segment(const char *path, size_t *path_index, uint8_t *conn_path, size_t *conn_path_index);
static int match_ip_addr_segment(const char *path, size_t *path_index, uint8_t *conn_path, size_t *conn_path_index);
static int match_dhp_addr_segment(const char *path, size_t *path_index, uint8_t *port, uint8_t *src_node, uint8_t *dest_node);

/* 255 less a pad byte and the 6-byte DH+ suffix. */
#define MAX_ROUTE_BYTES (248)


int cip_encode_path(const char *path, int *needs_connection, plc_type_t plc_type, uint8_t *tmp_conn_path, int *tmp_conn_path_size,
                    int *is_dhp, uint16_t *dhp_dest) {
    size_t path_len = 0;
    size_t conn_path_index = 0;
    size_t path_index = 0;
    uint8_t dhp_port = 0;
    uint8_t dhp_src_node = 0;
    uint8_t dhp_dest_node = 0;
    // uint8_t tmp_conn_path[MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN];
    size_t max_conn_path_size = (size_t)(*tmp_conn_path_size) - MAX_IP_ADDR_SEG_LEN;

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Starting");

    *is_dhp = 0;

    path_len = (size_t)(ssize_t)str_length(path);

    while(path && path[path_index] && path_index < path_len && conn_path_index < max_conn_path_size) {
        /* skip spaces before each segment */
        while(path[path_index] == ' ') { path_index++; }

        if(path[path_index] == ',') {
            /* skip separators. */
            pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Skipping separator character '%c'.", (char)path[path_index]);

            path_index++;
        } else if(match_numeric_segment(path, &path_index, tmp_conn_path, &conn_path_index) == PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Found numeric segment.");
        } else if(match_ip_addr_segment(path, &path_index, tmp_conn_path, &conn_path_index) == PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Found IP address segment.");
        } else if(match_dhp_addr_segment(path, &path_index, &dhp_port, &dhp_src_node, &dhp_dest_node) == PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Found DH+ address segment.");

            /* check if it is last. */
            if(path_index < path_len) {
                pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, 0, "DH+ address must be the last segment in a path! %d %d",
                       (int)(ssize_t)path_index, (int)(ssize_t)path_len);
                return PLCTAG_ERR_BAD_PARAM;
            }

            *is_dhp = 1;
        } else {
            /* unknown, cannot parse this! */
            pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, 0, "Unable to parse remaining path string from position %d, \"%s\".",
                   (int)(ssize_t)path_index, (char *)&path[path_index]);
            return PLCTAG_ERR_BAD_PARAM;
        }
    }

    if(conn_path_index >= max_conn_path_size) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, 0, "Encoded connection path is too long (%d >= %zu).",
               (int)(ssize_t)conn_path_index, max_conn_path_size);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /*
     * The encoded size is 8 bits (conn_path_size, route_path_size).  Leave room for a pad byte
     * and the 6-byte DH+ suffix, which also covers the 4-byte Message Router added here or
     * after identify.
     */
    if(conn_path_index > MAX_ROUTE_BYTES) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, 0, "Encoded route of %d bytes is over the maximum of %d bytes!",
               (int)(ssize_t)conn_path_index, MAX_ROUTE_BYTES);
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(*is_dhp && (plc_type == AB_PLC_PLC5 || plc_type == AB_PLC_SLC || plc_type == AB_PLC_MLGX)) {
        /* DH+ bridging always needs a connection. */
        *needs_connection = 1;

        /* add the special PCCC/DH+ routing on the end. */
        tmp_conn_path[conn_path_index + 0] = 0x20;
        tmp_conn_path[conn_path_index + 1] = 0xA6;
        tmp_conn_path[conn_path_index + 2] = 0x24;
        tmp_conn_path[conn_path_index + 3] = dhp_port;
        tmp_conn_path[conn_path_index + 4] = 0x2C;
        tmp_conn_path[conn_path_index + 5] = 0x01;
        conn_path_index += 6;

        *dhp_dest = (uint16_t)dhp_dest_node;
    } else if(!*is_dhp) {
        if(*needs_connection) {
            pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "PLC needs connection, adding path to the router object.");

            /*
             * we do a generic path to the router
             * object in the PLC.  But only if the PLC is
             * one that needs a connection.  For instance a
             * Micro850 needs to work in connected mode.
             */
            tmp_conn_path[conn_path_index + 0] = 0x20;
            tmp_conn_path[conn_path_index + 1] = 0x02;
            tmp_conn_path[conn_path_index + 2] = 0x24;
            tmp_conn_path[conn_path_index + 3] = 0x01;
            conn_path_index += 4;
        }

        *dhp_dest = 0;
    } else {
        /*
         *we had the special DH+ format and it was
         * either not last or not a PLC5/SLC.  That
         * is an error.
         */

        *dhp_dest = 0;

        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * zero pad the path to a multiple of 16-bit
     * words.
     */
    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "IOI size before %zu", conn_path_index);
    if(conn_path_index & 0x01) {
        tmp_conn_path[conn_path_index] = 0;
        conn_path_index++;
    }

    *tmp_conn_path_size = (uint8_t)conn_path_index;

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Done");

    return PLCTAG_STATUS_OK;
}


int match_numeric_segment(const char *path, size_t *path_index, uint8_t *conn_path, size_t *conn_path_index) {
    int val = 0;
    size_t p_index = *path_index;
    size_t c_index = *conn_path_index;

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Starting at position %d in string %s.", (int)(ssize_t)*path_index, path);

    while(isdigit(path[p_index])) {
        val = (val * 10) + (path[p_index] - '0');
        p_index++;
    }

    /* did we match anything? */
    if(p_index == *path_index) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Did not find numeric path segment at position %d.", (int)(ssize_t)p_index);
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* was the numeric segment valid? */
    if(val < 0 || val > 0x11) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, 0, "Numeric segment in path at position %d is out of bounds!",
               (int)(ssize_t)(*path_index));
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /* store the encoded segment data. */
    conn_path[c_index] = (uint8_t)(unsigned int)(val);
    c_index++;
    *conn_path_index = c_index;

    /* skip trailing spaces */
    while(path[p_index] == ' ') { p_index++; }

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Remaining path \"%s\".", &path[p_index]);

    /* bump past our last read character. */
    *path_index = p_index;

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Done. Found numeric segment %d.", val);

    return PLCTAG_STATUS_OK;
}


/*
 * match symbolic IP address segments.
 *  18,10.206.10.14 - port 2/A -> 10.206.10.14
 *  19,10.206.10.14 - port 3/B -> 10.206.10.14
 */

int match_ip_addr_segment(const char *path, size_t *path_index, uint8_t *conn_path, size_t *conn_path_index) {
    uint8_t *addr_seg_len = NULL;
    int val = 0;
    size_t p_index = *path_index;
    size_t c_index = *conn_path_index;

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Starting at position %d in string %s.", (int)(ssize_t)*path_index, path);

    /* first part, the extended address marker*/
    val = 0;
    while(isdigit(path[p_index])) {
        val = (val * 10) + (path[p_index] - '0');
        p_index++;
    }

    if(val != 18 && val != 19) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Path segment at %d does not match IP address segment.",
               (int)(ssize_t)*path_index);
        return PLCTAG_ERR_NOT_FOUND;
    }

    if(val == 18) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Extended address on port A.");
    } else {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Extended address on port B.");
    }

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* is the next character a comma? */
    if(path[p_index] != ',') {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0,
               "Not an IP address segment starting at position %d of path.  Remaining: \"%s\".", (int)(ssize_t)p_index,
               &path[p_index]);
        return PLCTAG_ERR_NOT_FOUND;
    }

    p_index++;

    /* start building up the connection path. */
    conn_path[c_index] = (uint8_t)(unsigned int)val;
    c_index++;

    /* point into the encoded path for the symbolic segment length. */
    addr_seg_len = &conn_path[c_index];
    *addr_seg_len = 0;
    c_index++;

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* four octets, separated by dots, copied as text into the segment. */
    for(int32_t octet = 0; octet < 4; octet++) {
        int32_t digits = 0;

        if(octet > 0) {
            if(path[p_index] != '.') {
                pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Unexpected character '%c' found at position %d after IP address part %d.",
                       path[p_index], (int)(ssize_t)p_index, (int)octet);
                return PLCTAG_ERR_BAD_PARAM;
            }

            conn_path[c_index] = (uint8_t)path[p_index];
            c_index++;
            p_index++;
            (*addr_seg_len)++;

            while(path[p_index] == ' ') { p_index++; }
        }

        val = 0;
        while(isdigit((unsigned char)path[p_index]) && (int)(unsigned int)(*addr_seg_len) < (MAX_IP_ADDR_SEG_LEN - 1)) {
            /* stop counting once it is out of range, so a long run of digits cannot overflow. */
            if(val <= 255) { val = (val * 10) + (path[p_index] - '0'); }
            conn_path[c_index] = (uint8_t)path[p_index];
            c_index++;
            p_index++;
            (*addr_seg_len)++;
            digits++;
        }

        if(digits == 0 || val > 255) {
            pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, 0, "IP address part %d is missing or out of bounds (0 <= %d < 256).",
                   (int)octet + 1, val);
            return PLCTAG_ERR_BAD_PARAM;
        }

        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "IP address part %d: %d.", (int)octet + 1, val);

        while(path[p_index] == ' ') { p_index++; }
    }

    /* We need to zero pad if the length is not a multiple of two. */
    if((*addr_seg_len) & (uint8_t)0x01) {
        conn_path[c_index] = (uint8_t)0;
        c_index++;
    }

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* set the return values. */
    *path_index = p_index;
    *conn_path_index = c_index;

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * match DH+ address segments.
 *  A:1:2 - port 2/A -> DH+ node 2
 *  B:1:2 - port 3/B -> DH+ node 2
 *
 * A and B can be lowercase or numeric.
 */

int match_dhp_addr_segment(const char *path, size_t *path_index, uint8_t *port, uint8_t *src_node, uint8_t *dest_node) {
    int val = 0;
    size_t p_index = *path_index;

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Starting at position %d in string %s.", (int)(ssize_t)*path_index, path);

    /* Get the port part. */
    switch(path[p_index]) {
        case 'A':
            /* fall through */
        case 'a':
            /* fall through */
        case '2': *port = 1; break;

        case 'B':
            /* fall through */
        case 'b':
            /* fall through */
        case '3': *port = 2; break;

        default:
            pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Character '%c' at position %d does not match start of DH+ segment.",
                   path[p_index], (int)(ssize_t)p_index);
            return PLCTAG_ERR_NOT_FOUND;
            break;
    }

    p_index++;

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* is the next character a colon? */
    if(path[p_index] != ':') {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0,
               "Character '%c' at position %d does not match first colon expected in DH+ segment.", path[p_index],
               (int)(ssize_t)p_index);
        return PLCTAG_ERR_BAD_PARAM;
    }

    p_index++;

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* get the source node */
    val = 0;
    while(isdigit(path[p_index])) {
        val = (val * 10) + (path[p_index] - '0');
        p_index++;
    }

    /* is the source node a valid number? */
    if(val < 0 || val > 255) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, 0, "Source node DH+ address part is out of bounds (0 <= %d < 256).", val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    *src_node = (uint8_t)(unsigned int)val;

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* is the next character a colon? */
    if(path[p_index] != ':') {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0,
               "Character '%c' at position %d does not match the second colon expected in DH+ segment.", path[p_index],
               (int)(ssize_t)p_index);
        return PLCTAG_ERR_BAD_PARAM;
    }

    p_index++;

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* get the destination node */
    val = 0;
    while(isdigit(path[p_index])) {
        val = (val * 10) + (path[p_index] - '0');
        p_index++;
    }

    /* is the destination node a valid number? */
    if(val < 0 || val > 255) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, 0, "Destination node DH+ address part is out of bounds (0 <= %d < 256).", val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    *dest_node = (uint8_t)(unsigned int)val;
    *path_index = p_index;

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Found DH+ path port:%d, source node:%d, destination node:%d.",
           (int)(unsigned int)*port, (int)(unsigned int)*src_node, (int)(unsigned int)*dest_node);

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * The EBNF is:
 *
 * tag ::= SYMBOLIC_SEG ( tag_seg )* ( bit_seg )?
 *
 * tag_seg ::= '.' SYMBOLIC_SEG
 *             '[' array_seg ']'
 *
 * bit_seg ::= '.' [0-9]+
 *
 * array_seg ::= NUMERIC_SEG ( ',' NUMERIC_SEG )*
 *
 * SYMBOLIC_SEG ::= [a-zA-Z]([a-zA-Z0-9_]*)
 *
 * NUMERIC_SEG ::= [0-9]+
 *
 */


int cip_encode_tag_name(ab_tag_p tag, const char *name) {
    int rc = PLCTAG_STATUS_OK;
    int encoded_index = 0;
    int name_index = 0;
    int name_len = str_length(name);

    /* zero out the CIP encoded name size. Byte zero in the encoded name. */
    tag->encoded_name[encoded_index] = 0;
    encoded_index++;

    /* names must start with a symbolic segment. */
    if(parse_symbolic_segment(tag, name, &encoded_index, &name_index) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id, "Unable to parse initial symbolic segment in tag name %s!", name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    while(name_index < name_len && encoded_index < MAX_TAG_NAME) {
        /* try to parse the different parts of the name. */
        if(name[name_index] == '.') {
            name_index++;
            /* could be a name segment or could be a bit identifier. */
            if(parse_symbolic_segment(tag, name, &encoded_index, &name_index) != PLCTAG_STATUS_OK) {
                /* try a bit identifier. */
                if(parse_bit_segment(tag, name, &name_index) == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, tag->tag_id, "Found bit identifier %u.", tag->bit);
                    break;
                } else {
                    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id,
                           "Expected a symbolic segment or a bit identifier at position %d in tag name %s", name_index, name);
                    return PLCTAG_ERR_BAD_PARAM;
                }
            } else {
                pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, tag->tag_id, "Found symbolic segment ending at %d", name_index);
            }
        } else if(name[name_index] == '[') {
            int num_dimensions = 0;
            /* must be an array so look for comma separated numeric segments. */
            do {
                name_index++;
                num_dimensions++;

                skip_whitespace(name, &name_index);
                rc = parse_numeric_segment(tag, name, &encoded_index, &name_index);
                skip_whitespace(name, &name_index);
            } while(rc == PLCTAG_STATUS_OK && name[name_index] == ',' && num_dimensions < 3);

            /* pass up the real reason rather than reporting it as a bad bracket below. */
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id, "Unable to parse numeric segment at %d in tag name %s!",
                       name_index, name);
                return rc;
            }

            /* must terminate with a closing ']' */
            if(name[name_index] != ']') {
                pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id,
                       "Bad tag name format, expected closing array bracket at %d in tag name %s!", name_index, name);
                return PLCTAG_ERR_BAD_PARAM;
            }

            /* step past the closing bracket. */
            name_index++;
        } else {
            pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id, "Unexpected character at position %d in name string %s!",
                   name_index, name);
            break;
        }
    }

    /*
     * The loop above stops as soon as the encoded name fills the buffer, which leaves the rest of
     * the name unparsed and looks exactly like a malformed name to the check below.  Say what
     * actually happened instead, and with the error code that fits it.
     */
    if(name_index < name_len && encoded_index >= MAX_TAG_NAME) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id, "Encoded tag name is too long at position %d in the tag name!",
               name_index);
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(name_index != name_len) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id,
               "Bad tag name format.  Tag must end with a bit identifier if one is present.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* set the word count. */
    tag->encoded_name[0] = (uint8_t)((encoded_index - 1) / 2);
    tag->encoded_name_size = encoded_index;

    return PLCTAG_STATUS_OK;
}

int skip_whitespace(const char *name, int *name_index) {
    while(name[*name_index] == ' ') { (*name_index)++; }

    return PLCTAG_STATUS_OK;
}


/*
 * A bit segment is simply an integer from 0 to 63 (inclusive). */
int parse_bit_segment(ab_tag_p tag, const char *name, int *name_index) {
    const char *p, *q;
    long val;

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, tag->tag_id, "Starting with name index=%d.", *name_index);

    p = &name[*name_index];
    q = p;

    val = strtol((char *)p, (char **)&q, 10);

    /* sanity checks. */
    if(p == q) {
        /* no number. */
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id,
               "Expected bit identifier or symbolic segment at position %d in tag name %s!", *name_index, name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if((val < 0) || (val > 255)) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id, "Bit identifier must be between 0 and 255, inclusive, was %d!",
               (int)val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(tag->elem_count != 1) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id, "Bit tags must have only one element!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* bump name_index. */
    *name_index += (int)(q - p);
    tag->is_bit = 1;
    tag->bit = (int)val;

    return PLCTAG_STATUS_OK;
}


int parse_symbolic_segment(ab_tag_p tag, const char *name, int *encoded_index, int *name_index) {
    int encoded_i = *encoded_index;
    int name_i = *name_index;
    int name_start = name_i;
    int seg_len_index = 0;
    int seg_len = 0;

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, tag->tag_id, "Starting with name index=%d and encoded name index=%d.", name_i,
           encoded_i);

    /* a symbolic segment must start with an alphabetic character or @, then can have digits or underscores. */
    if(!isalpha(name[name_i]) && name[name_i] != ':' && name[name_i] != '_' && name[name_i] != '@') {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, tag->tag_id, "tag name at position %d is not the start of a symbolic segment.",
               name_i);
        return PLCTAG_ERR_NO_MATCH;
    }

    /*
     * The segment type, the length byte and the first character are written before the
     * bounded loop below, so make sure all three fit.  The caller only checks the encoded
     * index between segments, not within one.
     */
    if(encoded_i > MAX_TAG_NAME - 3) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id, "Encoded tag name is too long at position %d in the tag name!",
               name_i);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* start building the encoded symbolic segment. */
    tag->encoded_name[encoded_i] = 0x91; /* start of symbolic segment. */
    encoded_i++;
    seg_len_index = encoded_i;
    tag->encoded_name[seg_len_index] = 0; /* the loop below counts up from zero, do not trust what was here. */
    tag->encoded_name[seg_len_index]++;
    encoded_i++;

    /* store the first character of the name. */
    tag->encoded_name[encoded_i] = (uint8_t)name[name_i];
    encoded_i++;
    name_i++;

    /* get the rest of the name. */
    while((isalnum(name[name_i]) || name[name_i] == ':' || name[name_i] == '_') && (encoded_i < (MAX_TAG_NAME - 1))) {
        /*
         * The segment length is a single byte, so a symbolic segment cannot hold more than 255
         * characters.  Without this the counter below wraps back to zero and the PLC gets a
         * zero-length segment followed by the rest of the name as stray path bytes.
         */
        if(tag->encoded_name[seg_len_index] == 0xFF) {
            pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id,
                   "Symbolic segment starting at position %d in the tag name is longer than the maximum of 255 characters!",
                   name_start);
            return PLCTAG_ERR_TOO_LARGE;
        }

        tag->encoded_name[encoded_i] = (uint8_t)name[name_i];
        encoded_i++;
        tag->encoded_name[seg_len_index]++;
        name_i++;
    }

    seg_len = tag->encoded_name[seg_len_index];

    /* finish up the encoded name.   Space for the name must be a multiple of two bytes long. */
    if((tag->encoded_name[seg_len_index] & 0x01) && (encoded_i < MAX_TAG_NAME)) {
        tag->encoded_name[encoded_i] = 0;
        encoded_i++;
    }

    *encoded_index = encoded_i;
    *name_index = name_i;

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, tag->tag_id, "Parsed symbolic segment \"%.*s\" in tag name.", seg_len,
           &name[name_start]);

    return PLCTAG_STATUS_OK;
}


int parse_numeric_segment(ab_tag_p tag, const char *name, int *encoded_index, int *name_index) {
    const char *p, *q;
    long val;

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, tag->tag_id, "Starting with name index=%d and encoded name index=%d.", *name_index,
           *encoded_index);

    p = &name[*name_index];
    q = p;

    errno = 0;
    val = strtol((char *)p, (char **)&q, 10);

    /* sanity checks. */
    if(p == q) {
        /* no number. */
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id, "Expected numeric segment at position %d in tag name %s!",
               *name_index, name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(val < 0) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id, "Numeric segment must be greater than or equal to zero, was %d!",
               (int)val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * strtol() returns a long, which is wider than the 32 bits the largest segment encoding
     * holds on many platforms.  Reject anything that would be silently truncated rather than
     * quietly addressing a different array element than the caller asked for.
     *
     * Where long is only 32 bits the range check alone is not enough: strtol() saturates at
     * LONG_MAX == INT32_MAX and sets ERANGE, so an overflowing value slips through as the
     * largest legal segment.
     */
    if(errno == ERANGE || val > (long)INT32_MAX) {
        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id, "Numeric segment must be less than or equal to %ld!",
               (long)INT32_MAX);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * Work out how much room the encoding needs and check it before writing anything.  The
     * caller only checks the encoded index once per segment group, but an array reference
     * such as "[a,b,c]" calls this three times, so the index can run well past the end of
     * the buffer between those checks.
     */
    {
        int segment_size = (val > 0xFFFF ? 6 : (val > 0xFF ? 4 : 2));

        if(*encoded_index > MAX_TAG_NAME - segment_size) {
            pdebug(DEBUG_MODULE_AB_CIP, DEBUG_WARN, tag->tag_id,
                   "Encoded tag name is too long to hold a numeric segment at position %d in the tag name!", *name_index);
            return PLCTAG_ERR_TOO_LARGE;
        }
    }

    /* bump name_index. */
    *name_index += (int)(q - p);

    /* encode the segment. */
    if(val > 0xFFFF) {
        tag->encoded_name[*encoded_index] = (uint8_t)0x2A; /* 4-byte segment value. */
        (*encoded_index)++;

        tag->encoded_name[*encoded_index] = (uint8_t)0; /* padding. */
        (*encoded_index)++;

        tag->encoded_name[*encoded_index] = (uint8_t)val & 0xFF;
        (*encoded_index)++;
        tag->encoded_name[*encoded_index] = (uint8_t)((val >> 8) & 0xFF);
        (*encoded_index)++;
        tag->encoded_name[*encoded_index] = (uint8_t)((val >> 16) & 0xFF);
        (*encoded_index)++;
        tag->encoded_name[*encoded_index] = (uint8_t)((val >> 24) & 0xFF);
        (*encoded_index)++;

        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, tag->tag_id, "Parsed 4-byte numeric segment of value %u.", (uint32_t)val);
    } else if(val > 0xFF) {
        tag->encoded_name[*encoded_index] = (uint8_t)0x29; /* 2-byte segment value. */
        (*encoded_index)++;

        tag->encoded_name[*encoded_index] = (uint8_t)0; /* padding. */
        (*encoded_index)++;

        tag->encoded_name[*encoded_index] = (uint8_t)val & 0xFF;
        (*encoded_index)++;
        tag->encoded_name[*encoded_index] = (uint8_t)((val >> 8) & 0xFF);
        (*encoded_index)++;

        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, tag->tag_id, "Parsed 2-byte numeric segment of value %u.", (uint32_t)val);
    } else {
        tag->encoded_name[*encoded_index] = (uint8_t)0x28; /* 1-byte segment value. */
        (*encoded_index)++;

        tag->encoded_name[*encoded_index] = (uint8_t)val & 0xFF;
        (*encoded_index)++;

        pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, tag->tag_id, "Parsed 1-byte numeric segment of value %u.", (uint32_t)val);
    }

    pdebug(DEBUG_MODULE_AB_CIP, DEBUG_DETAIL, tag->tag_id, "Done with name index=%d and encoded name index=%d.", *name_index,
           *encoded_index);

    return PLCTAG_STATUS_OK;
}


/*
 * How many bytes encode a CIP data type in a read reply: 2 for an elementary type (the type
 * byte and a pad), 4 for an abbreviated struct or array (the type byte, a pad and a 16-bit
 * handle).  The full struct and array descriptors (0xA2, 0xA3) are not parsed.
 */
int cip_lookup_encoded_type_size(uint8_t type_byte, int *type_size) {
    /* OMRON BCD, enum and nanosecond time types, then the elementary types. */
    if((type_byte >= 0x04 && type_byte <= 0x0B) || (type_byte >= 0xC0 && type_byte <= 0xDF)) {
        *type_size = 2;
        return PLCTAG_STATUS_OK;
    }

    /* abbreviated struct or array. */
    if(type_byte == 0xA0 || type_byte == 0xA1) {
        *type_size = 4;
        return PLCTAG_STATUS_OK;
    }

    *type_size = 0;
    return PLCTAG_ERR_NO_MATCH;
}


