/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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

// #define MAX_IP_ADDR_SEG_LEN (16)


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

    pdebug(DEBUG_DETAIL, "Starting");

    *is_dhp = 0;

    path_len = (size_t)(ssize_t)str_length(path);

    while(path && path[path_index] && path_index < path_len && conn_path_index < max_conn_path_size) {
        /* skip spaces before each segment */
        while(path[path_index] == ' ') { path_index++; }

        if(path[path_index] == ',') {
            /* skip separators. */
            pdebug(DEBUG_DETAIL, "Skipping separator character '%c'.", (char)path[path_index]);

            path_index++;
        } else if(match_numeric_segment(path, &path_index, tmp_conn_path, &conn_path_index) == PLCTAG_STATUS_OK) {
            pdebug(DEBUG_DETAIL, "Found numeric segment.");
        } else if(match_ip_addr_segment(path, &path_index, tmp_conn_path, &conn_path_index) == PLCTAG_STATUS_OK) {
            pdebug(DEBUG_DETAIL, "Found IP address segment.");
        } else if(match_dhp_addr_segment(path, &path_index, &dhp_port, &dhp_src_node, &dhp_dest_node) == PLCTAG_STATUS_OK) {
            pdebug(DEBUG_DETAIL, "Found DH+ address segment.");

            /* check if it is last. */
            if(path_index < path_len) {
                pdebug(DEBUG_WARN, "DH+ address must be the last segment in a path! %d %d", (int)(ssize_t)path_index,
                       (int)(ssize_t)path_len);
                return PLCTAG_ERR_BAD_PARAM;
            }

            *is_dhp = 1;
        } else {
            /* unknown, cannot parse this! */
            pdebug(DEBUG_WARN, "Unable to parse remaining path string from position %d, \"%s\".", (int)(ssize_t)path_index,
                   (char *)&path[path_index]);
            return PLCTAG_ERR_BAD_PARAM;
        }
    }

    if(conn_path_index >= max_conn_path_size) {
        pdebug(DEBUG_WARN, "Encoded connection path is too long (%d >= %d).", (int)(ssize_t)conn_path_index, max_conn_path_size);
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
            pdebug(DEBUG_DETAIL, "PLC needs connection, adding path to the router object.");

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
    pdebug(DEBUG_DETAIL, "IOI size before %d", conn_path_index);
    if(conn_path_index & 0x01) {
        tmp_conn_path[conn_path_index] = 0;
        conn_path_index++;
    }

    *tmp_conn_path_size = (uint8_t)conn_path_index;

    pdebug(DEBUG_DETAIL, "Done");

    return PLCTAG_STATUS_OK;
}


int match_numeric_segment(const char *path, size_t *path_index, uint8_t *conn_path, size_t *conn_path_index) {
    int val = 0;
    size_t p_index = *path_index;
    size_t c_index = *conn_path_index;

    pdebug(DEBUG_DETAIL, "Starting at position %d in string %s.", (int)(ssize_t)*path_index, path);

    while(isdigit(path[p_index])) {
        val = (val * 10) + (path[p_index] - '0');
        p_index++;
    }

    /* did we match anything? */
    if(p_index == *path_index) {
        pdebug(DEBUG_DETAIL, "Did not find numeric path segment at position %d.", (int)(ssize_t)p_index);
        return PLCTAG_ERR_NOT_FOUND;
    }

    /* was the numeric segment valid? */
    if(val < 0 || val > 0x0F) {
        pdebug(DEBUG_WARN, "Numeric segment in path at position %d is out of bounds!", (int)(ssize_t)(*path_index));
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /* store the encoded segment data. */
    conn_path[c_index] = (uint8_t)(unsigned int)(val);
    c_index++;
    *conn_path_index = c_index;

    /* skip trailing spaces */
    while(path[p_index] == ' ') { p_index++; }

    pdebug(DEBUG_DETAIL, "Remaining path \"%s\".", &path[p_index]);

    /* bump past our last read character. */
    *path_index = p_index;

    pdebug(DEBUG_DETAIL, "Done. Found numeric segment %d.", val);

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

    pdebug(DEBUG_DETAIL, "Starting at position %d in string %s.", (int)(ssize_t)*path_index, path);

    /* first part, the extended address marker*/
    val = 0;
    while(isdigit(path[p_index])) {
        val = (val * 10) + (path[p_index] - '0');
        p_index++;
    }

    if(val != 18 && val != 19) {
        pdebug(DEBUG_DETAIL, "Path segment at %d does not match IP address segment.", (int)(ssize_t)*path_index);
        return PLCTAG_ERR_NOT_FOUND;
    }

    if(val == 18) {
        pdebug(DEBUG_DETAIL, "Extended address on port A.");
    } else {
        pdebug(DEBUG_DETAIL, "Extended address on port B.");
    }

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* is the next character a comma? */
    if(path[p_index] != ',') {
        pdebug(DEBUG_DETAIL, "Not an IP address segment starting at position %d of path.  Remaining: \"%s\".",
               (int)(ssize_t)p_index, &path[p_index]);
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

    /* get the first IP address digit. */
    val = 0;
    while(isdigit(path[p_index]) && (int)(unsigned int)(*addr_seg_len) < (MAX_IP_ADDR_SEG_LEN - 1)) {
        val = (val * 10) + (path[p_index] - '0');
        conn_path[c_index] = (uint8_t)path[p_index];
        c_index++;
        p_index++;
        (*addr_seg_len)++;
    }

    if(val < 0 || val > 255) {
        pdebug(DEBUG_WARN, "First IP address part is out of bounds (0 <= %d < 256) for an IPv4 octet.", val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    pdebug(DEBUG_DETAIL, "First IP segment: %d.", val);

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* is the next character a dot? */
    if(path[p_index] != '.') {
        pdebug(DEBUG_DETAIL, "Unexpected character '%c' found at position %d in first IP address part.", path[p_index], p_index);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* copy the dot. */
    conn_path[c_index] = (uint8_t)path[p_index];
    c_index++;
    p_index++;
    (*addr_seg_len)++;

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* get the second part. */
    val = 0;
    while(isdigit(path[p_index]) && (int)(unsigned int)(*addr_seg_len) < (MAX_IP_ADDR_SEG_LEN - 1)) {
        val = (val * 10) + (path[p_index] - '0');
        conn_path[c_index] = (uint8_t)path[p_index];
        c_index++;
        p_index++;
        (*addr_seg_len)++;
    }

    if(val < 0 || val > 255) {
        pdebug(DEBUG_WARN, "Second IP address part is out of bounds (0 <= %d < 256) for an IPv4 octet.", val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    pdebug(DEBUG_DETAIL, "Second IP segment: %d.", val);

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* is the next character a dot? */
    if(path[p_index] != '.') {
        pdebug(DEBUG_DETAIL, "Unexpected character '%c' found at position %d in second IP address part.", path[p_index], p_index);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* copy the dot. */
    conn_path[c_index] = (uint8_t)path[p_index];
    c_index++;
    p_index++;
    (*addr_seg_len)++;

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* get the third part. */
    val = 0;
    while(isdigit(path[p_index]) && (int)(unsigned int)(*addr_seg_len) < (MAX_IP_ADDR_SEG_LEN - 1)) {
        val = (val * 10) + (path[p_index] - '0');
        conn_path[c_index] = (uint8_t)path[p_index];
        c_index++;
        p_index++;
        (*addr_seg_len)++;
    }

    if(val < 0 || val > 255) {
        pdebug(DEBUG_WARN, "Third IP address part is out of bounds (0 <= %d < 256) for an IPv4 octet.", val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    pdebug(DEBUG_DETAIL, "Third IP segment: %d.", val);

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* is the next character a dot? */
    if(path[p_index] != '.') {
        pdebug(DEBUG_DETAIL, "Unexpected character '%c' found at position %d in third IP address part.", path[p_index], p_index);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* copy the dot. */
    conn_path[c_index] = (uint8_t)path[p_index];
    c_index++;
    p_index++;
    (*addr_seg_len)++;

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* get the fourth part. */
    val = 0;
    while(isdigit(path[p_index]) && (int)(unsigned int)(*addr_seg_len) < (MAX_IP_ADDR_SEG_LEN - 1)) {
        val = (val * 10) + (path[p_index] - '0');
        conn_path[c_index] = (uint8_t)path[p_index];
        c_index++;
        p_index++;
        (*addr_seg_len)++;
    }

    if(val < 0 || val > 255) {
        pdebug(DEBUG_WARN, "Fourth IP address part is out of bounds (0 <= %d < 256) for an IPv4 octet.", val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    pdebug(DEBUG_DETAIL, "Fourth IP segment: %d.", val);

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

    pdebug(DEBUG_DETAIL, "Done.");

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

    pdebug(DEBUG_DETAIL, "Starting at position %d in string %s.", (int)(ssize_t)*path_index, path);

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
            pdebug(DEBUG_DETAIL, "Character '%c' at position %d does not match start of DH+ segment.", path[p_index],
                   (int)(ssize_t)p_index);
            return PLCTAG_ERR_NOT_FOUND;
            break;
    }

    p_index++;

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* is the next character a colon? */
    if(path[p_index] != ':') {
        pdebug(DEBUG_DETAIL, "Character '%c' at position %d does not match first colon expected in DH+ segment.", path[p_index],
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
        pdebug(DEBUG_WARN, "Source node DH+ address part is out of bounds (0 <= %d < 256).", val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    *src_node = (uint8_t)(unsigned int)val;

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    /* is the next character a colon? */
    if(path[p_index] != ':') {
        pdebug(DEBUG_DETAIL, "Character '%c' at position %d does not match the second colon expected in DH+ segment.",
               path[p_index], (int)(ssize_t)p_index);
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
        pdebug(DEBUG_WARN, "Destination node DH+ address part is out of bounds (0 <= %d < 256).", val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* skip spaces */
    while(path[p_index] == ' ') { p_index++; }

    *dest_node = (uint8_t)(unsigned int)val;
    *path_index = p_index;

    pdebug(DEBUG_DETAIL, "Found DH+ path port:%d, source node:%d, destination node:%d.", (int)(unsigned int)*port,
           (int)(unsigned int)*src_node, (int)(unsigned int)*dest_node);

    pdebug(DEBUG_DETAIL, "Done.");

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
        pdebug(DEBUG_WARN, "Unable to parse initial symbolic segment in tag name %s!", name);
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
                    pdebug(DEBUG_DETAIL, "Found bit identifier %u.", tag->bit);
                    break;
                } else {
                    pdebug(DEBUG_WARN, "Expected a symbolic segment or a bit identifier at position %d in tag name %s",
                           name_index, name);
                    return PLCTAG_ERR_BAD_PARAM;
                }
            } else {
                pdebug(DEBUG_DETAIL, "Found symbolic segment ending at %d", name_index);
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

            /* must terminate with a closing ']' */
            if(name[name_index] != ']') {
                pdebug(DEBUG_WARN, "Bad tag name format, expected closing array bracket at %d in tag name %s!", name_index, name);
                return PLCTAG_ERR_BAD_PARAM;
            }

            /* step past the closing bracket. */
            name_index++;
        } else {
            pdebug(DEBUG_WARN, "Unexpected character at position %d in name string %s!", name_index, name);
            break;
        }
    }

    if(name_index != name_len) {
        pdebug(DEBUG_WARN, "Bad tag name format.  Tag must end with a bit identifier if one is present.");
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

    pdebug(DEBUG_DETAIL, "Starting with name index=%d.", *name_index);

    p = &name[*name_index];
    q = p;

    val = strtol((char *)p, (char **)&q, 10);

    /* sanity checks. */
    if(p == q) {
        /* no number. */
        pdebug(DEBUG_WARN, "Expected bit identifier or symbolic segment at position %d in tag name %s!", *name_index, name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if((val < 0) || (val >= 65536)) {
        pdebug(DEBUG_WARN, "Bit identifier must be between 0 and 255, inclusive, was %d!", (int)val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(tag->elem_count != 1) {
        pdebug(DEBUG_WARN, "Bit tags must have only one element!");
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

    pdebug(DEBUG_DETAIL, "Starting with name index=%d and encoded name index=%d.", name_i, encoded_i);

    /* a symbolic segment must start with an alphabetic character or @, then can have digits or underscores. */
    if(!isalpha(name[name_i]) && name[name_i] != ':' && name[name_i] != '_' && name[name_i] != '@') {
        pdebug(DEBUG_DETAIL, "tag name at position %d is not the start of a symbolic segment.", name_i);
        return PLCTAG_ERR_NO_MATCH;
    }

    /* start building the encoded symbolic segment. */
    tag->encoded_name[encoded_i] = 0x91; /* start of symbolic segment. */
    encoded_i++;
    seg_len_index = encoded_i;
    tag->encoded_name[seg_len_index]++;
    encoded_i++;

    /* store the first character of the name. */
    tag->encoded_name[encoded_i] = (uint8_t)name[name_i];
    encoded_i++;
    name_i++;

    /* get the rest of the name. */
    while((isalnum(name[name_i]) || name[name_i] == ':' || name[name_i] == '_') && (encoded_i < (MAX_TAG_NAME - 1))) {
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

    pdebug(DEBUG_DETAIL, "Parsed symbolic segment \"%.*s\" in tag name.", seg_len, &name[name_start]);

    return PLCTAG_STATUS_OK;
}


int parse_numeric_segment(ab_tag_p tag, const char *name, int *encoded_index, int *name_index) {
    const char *p, *q;
    long val;

    pdebug(DEBUG_DETAIL, "Starting with name index=%d and encoded name index=%d.", *name_index, *encoded_index);

    p = &name[*name_index];
    q = p;

    val = strtol((char *)p, (char **)&q, 10);

    /* sanity checks. */
    if(p == q) {
        /* no number. */
        pdebug(DEBUG_WARN, "Expected numeric segment at position %d in tag name %s!", *name_index, name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(val < 0) {
        pdebug(DEBUG_WARN, "Numeric segment must be greater than or equal to zero, was %d!", (int)val);
        return PLCTAG_ERR_BAD_PARAM;
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

        pdebug(DEBUG_DETAIL, "Parsed 4-byte numeric segment of value %u.", (uint32_t)val);
    } else if(val > 0xFF) {
        tag->encoded_name[*encoded_index] = (uint8_t)0x29; /* 2-byte segment value. */
        (*encoded_index)++;

        tag->encoded_name[*encoded_index] = (uint8_t)0; /* padding. */
        (*encoded_index)++;

        tag->encoded_name[*encoded_index] = (uint8_t)val & 0xFF;
        (*encoded_index)++;
        tag->encoded_name[*encoded_index] = (uint8_t)((val >> 8) & 0xFF);
        (*encoded_index)++;

        pdebug(DEBUG_DETAIL, "Parsed 2-byte numeric segment of value %u.", (uint32_t)val);
    } else {
        tag->encoded_name[*encoded_index] = (uint8_t)0x28; /* 1-byte segment value. */
        (*encoded_index)++;

        tag->encoded_name[*encoded_index] = (uint8_t)val & 0xFF;
        (*encoded_index)++;

        pdebug(DEBUG_DETAIL, "Parsed 1-byte numeric segment of value %u.", (uint32_t)val);
    }

    pdebug(DEBUG_DETAIL, "Done with name index=%d and encoded name index=%d.", *name_index, *encoded_index);

    return PLCTAG_STATUS_OK;
}


struct cip_type_lookup_entry_t {
    int is_found;
    int type_data_length;
    int instance_data_length;
};

static struct cip_type_lookup_entry_t cip_type_lookup[] = {
    /* 0x00 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x01 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x02 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x03 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x04 */ {PLCTAG_STATUS_OK, 2, 2},    /* UINT_BCD: OMRON-specific */
    /* 0x05 */ {PLCTAG_STATUS_OK, 2, 4},    /* UDINT_BCD: OMRON-specific */
    /* 0x06 */ {PLCTAG_STATUS_OK, 2, 8},    /* ULINT_BCD: OMRON-specific */
    /* 0x07 */ {PLCTAG_STATUS_OK, 2, 4},    /* ENUM: OMRON-specific */
    /* 0x08 */ {PLCTAG_STATUS_OK, 2, 8},    /* DATE_NSEC: OMRON-specific */
    /* 0x09 */ {PLCTAG_STATUS_OK, 2, 8},    /* TIME_NSEC: OMRON-specific, Time in nanoseconds */
    /* 0x0a */ {PLCTAG_STATUS_OK, 2, 8},    /* DATE_AND_TIME_NSEC: OMRON-specific, Date/Time in nanoseconds*/
    /* 0x0b */ {PLCTAG_STATUS_OK, 2, 8},    /* TIME_OF_DAY_NSEC: OMRON-specific */
    /* 0x0c */ {PLCTAG_ERR_NO_MATCH, 0, 0}, /* ???? UNION: Omron-specific */
    /* 0x0d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x0e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x0f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x10 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x11 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x12 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x13 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x14 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x15 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x16 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x17 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x18 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x19 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x1a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x1b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x1c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x1d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x1e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x1f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x20 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x21 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x22 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x23 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x24 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x25 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x26 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x27 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x28 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x29 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x2a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x2b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x2c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x2d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x2e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x2f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x30 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x31 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x32 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x33 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x34 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x35 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x36 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x37 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x38 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x39 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x3a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x3b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x3c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x3d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x3e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x3f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x40 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x41 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x42 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x43 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x44 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x45 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x46 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x47 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x48 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x49 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x4a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x4b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x4c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x4d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x4e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x4f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x50 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x51 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x52 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x53 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x54 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x55 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x56 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x57 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x58 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x59 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x5a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x5b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x5c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x5d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x5e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x5f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x60 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x61 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x62 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x63 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x64 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x65 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x66 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x67 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x68 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x69 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x6a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x6b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x6c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x6d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x6e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x6f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x70 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x71 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x72 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x73 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x74 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x75 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x76 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x77 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x78 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x79 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x7a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x7b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x7c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x7d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x7e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x7f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x80 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x81 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x82 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x83 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x84 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x85 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x86 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x87 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x88 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x89 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x8a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x8b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x8c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x8d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x8e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x8f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x90 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x91 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x92 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x93 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x94 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x95 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x96 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x97 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x98 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x99 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x9a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x9b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x9c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x9d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x9e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x9f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xa0 */ {PLCTAG_STATUS_OK, 4, 0},    /* Data is an abbreviated struct type, i.e. a CRC of the actual type descriptor */
    /* 0xa1 */ {PLCTAG_STATUS_OK, 4, 0},    /* Data is an abbreviated array type. The limits are left off */
    /* 0xa2 */ {PLCTAG_ERR_NO_MATCH, 0, 0}, /* Data is a struct type descriptor, marked no match because we do not know how to
                                               parse it */
    /* 0xa3 */ {PLCTAG_ERR_NO_MATCH, 0, 0}, /* Data is an array type descriptor, marked no match because we do not know how to
                                               parse it */
    /* 0xa4 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xa5 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xa6 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xa7 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xa8 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xa9 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xaa */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xab */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xac */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xad */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xae */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xaf */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb0 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb1 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb2 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb3 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb4 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb5 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb6 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb7 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb8 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb9 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xba */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xbb */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xbc */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xbd */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xbe */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xbf */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xc0 */ {PLCTAG_STATUS_OK, 2, 8},  /* DT: DT value, 64 bit */
    /* 0xc1 */ {PLCTAG_STATUS_OK, 2, 1},  /* BOOL: Boolean value, 1 bit */
    /* 0xc2 */ {PLCTAG_STATUS_OK, 2, 1},  /* SINT: Signed 8–bit integer value */
    /* 0xc3 */ {PLCTAG_STATUS_OK, 2, 2},  /* INT: Signed 16–bit integer value */
    /* 0xc4 */ {PLCTAG_STATUS_OK, 2, 4},  /* DINT: Signed 32–bit integer value */
    /* 0xc5 */ {PLCTAG_STATUS_OK, 2, 8},  /* LINT: Signed 64–bit integer value */
    /* 0xc6 */ {PLCTAG_STATUS_OK, 2, 1},  /* USINT: Unsigned 8–bit integer value */
    /* 0xc7 */ {PLCTAG_STATUS_OK, 2, 2},  /* UINT: Unsigned 16–bit integer value */
    /* 0xc8 */ {PLCTAG_STATUS_OK, 2, 4},  /* UDINT: Unsigned 32–bit integer value */
    /* 0xc9 */ {PLCTAG_STATUS_OK, 2, 8},  /* ULINT: Unsigned 64–bit integer value */
    /* 0xca */ {PLCTAG_STATUS_OK, 2, 4},  /* REAL: 32–bit floating point value, IEEE format */
    /* 0xcb */ {PLCTAG_STATUS_OK, 2, 8},  /* LREAL: 64–bit floating point value, IEEE format */
    /* 0xcc */ {PLCTAG_STATUS_OK, 2, 4},  /* STIME: System Time Synchronous time value */
    /* 0xcd */ {PLCTAG_STATUS_OK, 2, 2},  /* DATE: Date value */
    /* 0xce */ {PLCTAG_STATUS_OK, 2, 4},  /* TIME_OF_DAY: Time of day value */
    /* 0xcf */ {PLCTAG_STATUS_OK, 2, 8},  /* DATE_AND_TIME: Date and time of day value */
    /* 0xd0 */ {PLCTAG_STATUS_OK, 2, 84}, /* STRING: Character string, 2 byte count word, 1 byte per character */
    /* 0xd1 */ {PLCTAG_STATUS_OK, 2, 1},  /* BYTE: 8-bit bit string */
    /* 0xd2 */ {PLCTAG_STATUS_OK, 2, 2},  /* WORD: 16-bit bit string */
    /* 0xd3 */ {PLCTAG_STATUS_OK, 2, 4},  /* DWORD: 32-bit bit string */
    /* 0xd4 */ {PLCTAG_STATUS_OK, 2, 8},  /* LWORD: 64-bit bit string */
    /* 0xd5 */ {PLCTAG_STATUS_OK, 2, 0},  /* STRING2: Wide string, 2-byte count, 2 bytes per character, utf-16-le */
    /* 0xd6 */ {PLCTAG_STATUS_OK, 2, 4},  /* FTIME: High resolution duration value */
    /* 0xd7 */ {PLCTAG_STATUS_OK, 2, 8},  /* TIME: Medium resolution duration value */
    /* 0xd8 */ {PLCTAG_STATUS_OK, 2, 2},  /* ITIME: Low resolution duration value */
    /* 0xd9 */ {PLCTAG_STATUS_OK, 2, 0},  /* STRINGN: N-byte per char character string */
    /* 0xda */ {PLCTAG_STATUS_OK, 2, 0},  /* SHORT_STRING: 1 byte per character and 1 byte length */
    /* 0xdb */ {PLCTAG_STATUS_OK, 2, 4},  /* TIME: Duration in milliseconds */
    /* 0xdc */ {PLCTAG_STATUS_OK, 2, 0},  /* EPATH: CIP path segment(s) */
    /* 0xdd */ {PLCTAG_STATUS_OK, 2, 2},  /* ENGUNIT: Engineering units */
    /* 0xde */ {PLCTAG_STATUS_OK, 2, 0},  /* STRINGI: International character string (encoding?) */
    /* 0xdf */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe0 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe1 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe2 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe3 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe4 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe5 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe6 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe7 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe8 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe9 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xea */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xeb */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xec */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xed */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xee */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xef */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf0 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf1 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf2 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf3 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf4 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf5 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf6 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf7 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf8 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf9 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xfa */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xfb */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xfc */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xfd */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xfe */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xff */ {PLCTAG_ERR_NO_MATCH, 0, 0},
};


int cip_lookup_encoded_type_size(uint8_t type_byte, int *type_size) {
    *type_size = cip_type_lookup[type_byte].type_data_length;
    return cip_type_lookup[type_byte].is_found;
}


int cip_lookup_data_element_size(uint8_t type_byte, int *element_size) {
    *element_size = cip_type_lookup[type_byte].instance_data_length;
    return cip_type_lookup[type_byte].is_found;
}
