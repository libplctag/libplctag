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
#include <libplctag/protocols/omron/cip.h>
#include <libplctag/protocols/omron/defs.h>
#include <libplctag/protocols/omron/omron_common.h>
#include <libplctag/protocols/omron/tag.h>
#include <platform.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <utils/debug.h>


static int encode_path(const char *path, int *needs_connection, uint8_t *tmp_conn_path, int *tmp_conn_path_size, int *is_dhp,
                       uint16_t *dhp_dest);
static int encode_tag_name(omron_tag_p tag, const char *name);
static int lookup_encoded_type_size(uint8_t type_byte, int *type_size);
static int lookup_data_element_size(uint8_t type_byte, int *element_size);

static const char *decode_cip_error_short(uint8_t *data);
static const char *decode_cip_error_long(uint8_t *data);
static int decode_cip_error_code(uint8_t *data);

/* public access point */
cip_generic_t CIP = {
    .encode_path = encode_path,
    .encode_tag_name = encode_tag_name,
    .lookup_data_element_size = lookup_data_element_size,
    .lookup_encoded_type_size = lookup_encoded_type_size,
    .decode_cip_error_code = decode_cip_error_code,
    .decode_cip_error_long = decode_cip_error_long,
    .decode_cip_error_short = decode_cip_error_short,
};

static int skip_whitespace(const char *name, int *name_index);
static int parse_bit_segment(omron_tag_p tag, const char *name, int *name_index);
static int parse_symbolic_segment(omron_tag_p tag, const char *name, int *encoded_index, int *name_index);
static int parse_numeric_segment(omron_tag_p tag, const char *name, int *encoded_index, int *name_index);

static int match_numeric_segment(const char *path, size_t *path_index, uint8_t *conn_path, size_t *conn_path_index);
static int match_ip_addr_segment(const char *path, size_t *path_index, uint8_t *conn_path, size_t *conn_path_index);
static int match_dhp_addr_segment(const char *path, size_t *path_index, uint8_t *port, uint8_t *src_node, uint8_t *dest_node);

// #define MAX_IP_ADDR_SEG_LEN (16)


int encode_path(const char *path, int *needs_connection, uint8_t *tmp_conn_path, int *tmp_conn_path_size, int *is_dhp,
                uint16_t *dhp_dest) {
    size_t path_len = 0;
    size_t conn_path_index = 0;
    size_t path_index = 0;
    uint8_t dhp_port = 0;
    uint8_t dhp_src_node = 0;
    uint8_t dhp_dest_node = 0;
    // uint8_t tmp_conn_path[MAX_CONN_PATH + MAX_IP_ADDR_SEG_LEN];
    size_t max_conn_path_size = (size_t)(*tmp_conn_path_size) - (size_t)MAX_IP_ADDR_SEG_LEN;

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


int encode_tag_name(omron_tag_p tag, const char *name) {
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
int parse_bit_segment(omron_tag_p tag, const char *name, int *name_index) {
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


int parse_symbolic_segment(omron_tag_p tag, const char *name, int *encoded_index, int *name_index) {
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


int parse_numeric_segment(omron_tag_p tag, const char *name, int *encoded_index, int *name_index) {
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


int lookup_encoded_type_size(uint8_t type_byte, int *type_size) {
    *type_size = cip_type_lookup[type_byte].type_data_length;
    return cip_type_lookup[type_byte].is_found;
}


int lookup_data_element_size(uint8_t type_byte, int *element_size) {
    *element_size = cip_type_lookup[type_byte].instance_data_length;
    return cip_type_lookup[type_byte].is_found;
}


struct error_code_entry {
    int primary_code;
    int secondary_code;
    int translated_code;
    const char *short_desc;
    const char *long_desc;
};


/*
 * This information was constructed after finding a few online resources.  Most of it comes from publically published manuals for
 * other products. Sources include: Kepware aboutplcs.com (Productivity 3000 manual) Allen-Bradley and others I have long since
 * lost track of.
 *
 * Most probably comes from aboutplcs.com.
 *
 * The copyright on these entries that of their respective owners.  Used here under assumption of Fair Use.
 */


static struct error_code_entry error_code_table
    [] = {{0x01, 0x0100, PLCTAG_ERR_DUPLICATE, "Connection In Use/Duplicate Forward Open",
           "A connection is already established from the target device sending a Forward Open request or the target device has sent multiple forward open request. This could be caused by poor network traffic. Check the cabling, switches and connections."},
          {0x01, 0x0103, PLCTAG_ERR_UNSUPPORTED, "Transport Class/Trigger Combination not supported",
           "The Transport class and trigger combination is not supported. The Productivity Suite CPU only supports Class 1 and Class 3 transports and triggers: Change of State and Cyclic."},
          {0x01, 0x0106, PLCTAG_ERR_NOT_ALLOWED, "Owner Conflict",
           "An existing exclusive owner has already configured a connection to this Connection Point. Check to see if other Scanner devices are connected to this adapter or verify that Multicast is supported by adapter device if Multicast is selected for Forward Open. This could be caused by poor network traffic. Check the cabling, switches and connections."},
          {0x01, 0x0107, PLCTAG_ERR_NOT_FOUND, "Target Connection Not Found",
           "This occurs if a device sends a Forward Close on a connection and the device can't find this connection. This could occur if one of these devices has powered down or if the connection timed out on a bad connection. This could be caused by poor network traffic. Check the cabling, switches and connections."},
          {0x01, 0x0108, PLCTAG_ERR_BAD_PARAM, "Invalid Network Connection Parameter",
           "This error occurs when one of the parameters specified in the Forward Open message is not supported such as Connection Point, Connection type, Connection priority, redundant owner or exclusive owner. The Productivity Suite CPU does not return this error and will instead use errors 0x0120, 0x0121, 0x0122, 0x0123, 0x0124, 0x0125 or 0x0132 instead."},
          {0x01, 0x0109, PLCTAG_ERR_BAD_PARAM, "Invalid Connection Size",
           "This error occurs when the target device doesn't support the requested connection size. Check the documentation of the manufacturer's device to verify the correct Connection size required by the device. Note that most devices specify this value in terms of bytes. The Productivity Suite CPU does not return this error and will instead use errors 0x0126, 0x0127 and 0x0128."},
          {0x01, 0x0110, PLCTAG_ERR_NOT_FOUND, "Target for Connection Not Configured",
           "This error occurs when a message is received with a connection number that does not exist in the target device. This could occur if the target device has powered down or if the connection timed out. This could be caused by poor network traffic. Check the cabling, switches and connections."},
          {0x01, 0x0111, PLCTAG_ERR_UNSUPPORTED,
           "RPI Not Supported", "This error occurs if the Originator is specifying an RPI that is not supported. The Productivity Suite CPU will accept a minimum value of 10ms on a CIP Forward Open request. However, the CPU will produce at the specified rate up to the scan time of the installed project. The CPU cannot product any faster than the scan time of the running project."},
          {0x01, 0x0112, PLCTAG_ERR_BAD_PARAM, "RPI Value not acceptable",
           "This error can be returned if the Originator is specifying an RPI value that is not acceptable. There may be six additional values following the extended error code with the acceptable values. An array can be defined for this field in order to view the extended error code attributes. If the Target device supports extended status, the format of the values will be as shown below:\nUnsigned Integer 16, Value = 0x0112, Explanation: Extended Status code,\nUnsigned Integer 8, Value = variable, Explanation: Acceptable Originator to Target RPI type, values: 0 = The RPI specified in the forward open was acceptable (O -> T value is ignored), 1 = unspecified (use a different RPI), 2 = minimum acceptable RPI (too fast), 3 = maximum acceptable RPI (too slow), 4 = required RPI to corrected mismatch (data is already being consumed at a different RPI), 5 to 255 = reserved.\nUnsigned Integer 32, Value = variable, Explanation: Value of O -> T RPI that is within the acceptable range for the application.\nUnsigned Integer 32, Value = variable, Explanation: Value of T -> O RPI that is within the acceptable range for the application."},
          {0x01, 0x0113, PLCTAG_ERR_NO_RESOURCES, "Out of Connections",
           "The Productivity Suite EtherNet/IP Adapter connection limit of 4 when doing Class 3 connections has been reached. An existing connection must be dropped in order for a new one to be generated."},
          {0x01, 0x0114, PLCTAG_ERR_NOT_FOUND, "Vendor ID or Product Code Mismatch",
           "The compatibility bit was set in the Forward Open message but the Vendor ID or Product Code did not match."},
          {0x01, 0x0115, PLCTAG_ERR_NOT_FOUND, "Device Type Mismatch",
           "The compatibility bit was set in the Forward Open message but the Device Type did not match."},
          {0x01, 0x0116, PLCTAG_ERR_NO_MATCH, "Revision Mismatch",
           "The compatibility bit was set in the Forward Open message but the major and minor revision numbers were not a valid revision."},
          {0x01, 0x0117,
           PLCTAG_ERR_BAD_PARAM, "Invalid Produced or Consumed Application Path", "This error is returned from the Target device when the Connection Point parameters specified for the O -> T (Output) or T -> O (Input) connection is incorrect or not supported. The Productivity Suite CPU does not return this error and uses the following error codes instead: 0x012A, 0x012B or 0x012F."},
          {0x01, 0x0118, PLCTAG_ERR_BAD_PARAM, "Invalid or Inconsistent Configuration Application Path",
           "This error is returned from the Target device when the Connection Point parameter specified for the Configuration data is incorrect or not supported. The Productivity Suite CPU does not return this error and uses the following error codes instead: 0x0129 or 0x012F."},
          {0x01, 0x0119, PLCTAG_ERR_OPEN, "Non-listen Only Connection Not Opened",
           "This error code is returned when an Originator device attempts to establish a listen only connection and there is no non-listen only connection established. The Productivity Suite CPU does not support listen only connections as Scanner or Adapter."},
          {0x01, 0x011A, PLCTAG_ERR_NO_RESOURCES, "Target Object Out of Connections",
           "The maximum number of connections supported by this instance of the object has been exceeded."},
          {0x01, 0x011B, PLCTAG_ERR_TOO_SMALL, "RPI is smaller than the Production Inhibit Time",
           "The Target to Originator RPI is smaller than the Target to Originator Production Inhibit Time. Consult the manufacturer's documentation as to the minimum rate that data can be produced and adjust the RPI to greater than this value."},
          {0x01, 0x011C, PLCTAG_ERR_UNSUPPORTED, "Transport Class Not Supported",
           "The Transport Class requested in the Forward Open is not supported. Only Class 1 and Class 3 classes are supported in the Productivity Suite CPU."},
          {0x01, 0x011D, PLCTAG_ERR_UNSUPPORTED, "Production Trigger Not Supported",
           "The Production Trigger requested in the Forward Open is not supported. In Class 1, only Cyclic and Change of state are supported in the Productivity Suite CPU. In Class 3, Application object is supported."},
          {0x01, 0x011E, PLCTAG_ERR_UNSUPPORTED, "Direction Not Supported",
           "The Direction requested in the Forward Open is not supported."},
          {0x01, 0x011F, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Fixed/Variable Flag",
           "The Originator to Target fixed/variable flag specified in the Forward Open is not supported . Only Fixed is supported in the Productivity Suite CPU."},
          {0x01, 0x0120, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Network Connection Fixed/Variable Flag",
           "The Target to Originator fixed/variable flag specified in the Forward Open is not supported. Only Fixed is supported in the Productivity Suite CPU."},
          {0x01, 0x0121, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Priority",
           "The Originator to Target Network Connection Priority specified in the Forward Open is not supported. Low, High, Scheduled and Urgent are supported in the Productivity Suite CPU."},
          {0x01, 0x0122, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Network Connection Priority",
           "The Target to Originator Network Connection Priority specified in the Forward Open is not supported. Low, High, Scheduled and Urgent are supported in the Productivity Suite CPU."},
          {0x01, 0x0123, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Type",
           "The Originator to Target Network Connection Type specified in the Forward Open is not supported. Only Unicast is supported for O -> T (Output) data in the Productivity Suite CPU."},
          {0x01, 0x0124, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Network Connection Type",
           "The Target to Originator Network Connection Type specified in the Forward Open is not supported. Multicast and Unicast is supported in the Productivity Suite CPU. Some devices may not support one or the other so if this error is encountered try the other method."},
          {0x01, 0x0125, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Redundant_Owner",
           "The Originator to Target Network Connection Redundant_Owner flag specified in the Forward Open is not supported. Only Exclusive owner connections are supported in the Productivity Suite CPU."},
          {0x01, 0x0126, PLCTAG_ERR_BAD_PARAM, "Invalid Configuration Size",
           "This error is returned when the Configuration data sent in the Forward Open does not match the size specified or is not supported by the Adapter. The Target device may return an additional Unsigned Integer 16 value that specifies the maximum size allowed for this data. An array can be defined for this field in order to view the extended error code attributes."},
          {0x01, 0x0127, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Size",
           "This error is returned when the Originator to Target (Output data) size specified in the Forward Open does not match what is in the Target. Consult the documentation of the Adapter device to verify the required size. Note that if the Run/Idle header is requested, it will add 4 additional bytes and must be accounted for in the Forward Open calculation. The Productivity Suite CPU always requires the Run/Idle header so if the option doesn't exist in the Scanner device, you must add an additional 4 bytes to the O -> T (Output) setup. Some devices may publish the size that they are looking for as an additional attribute (Unsigned Integer 16 value) of the Extended Error Code. An array can be defined for this field in order to view the extended error code attributes.\nNote: This error may also be generated when a Connection Point value that is invalid for IO Messaging (but valid for other cases such as Explicit Messaging) is specified, such as 0. Please verify if the Connection Point value is valid for IO Messaging in the target device."},
          {0x01, 0x0128, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Size",
           "This error is returned when the Target to Originator (Input data) size specified in the Forward Open does not match what is in Target. Consult the documentation of the Adapter device to verify the required size. Note that if the Run/Idle header is requested, it will add 4 additional bytes and must be accounted for in the Forward Open calculation. The Productivity Suite CPU does not support a Run/Idle header for the T -> O (Input) data. Some devices may publish the size that they are looking for as an additional attribute (Unsigned Integer 16 value) of the Extended Error Code. An array can be defined for this field in order to view the extended error code attributes.\nNote: This error may also be generated when a Connection Point value that is invalid for IO Messaging (but valid for other cases such as Explicit Messaging) is specified, such as 0. Please verify if the Connection Point value is valid for IO Messaging in the target device."},
          {0x01, 0x0129, PLCTAG_ERR_BAD_PARAM, "Invalid Configuration Application Path",
           "This error will be returned by the Productivity Suite CPU if a Configuration Connection with a size other than 0 is sent to the CPU. The Configuration Connection size must always be zero if it this path is present in the Forward Open message coming from the Scanner device."},
          {0x01, 0x012A, PLCTAG_ERR_BAD_PARAM, "Invalid Consuming Application Path",
           "This error will be returned by the Productivity Suite CPU if the Consuming (O -> T) Application Path is not present in the Forward Open message coming from the Scanner device or if the specified Connection Point is incorrect."},
          {0x01, 0x012B, PLCTAG_ERR_BAD_PARAM, "Invalid Producing Application Path",
           "This error will be returned by the Productivity Suite CPU if the Producing (T -> O) Application Path is not present in the Forward Open message coming from the Scanner device or if the specified Connection Point is incorrect."},
          {0x01, 0x012C, PLCTAG_ERR_NOT_FOUND, "Configuration Symbol Does not Exist",
           "The Originator attempted to connect to a configuration tag name that is not supported in the Target."},
          {0x01, 0x012D, PLCTAG_ERR_NOT_FOUND, "Consuming Symbol Does not Exist",
           "The Originator attempted to connect to a consuming tag name that is not supported in the Target."},
          {0x01, 0x012E, PLCTAG_ERR_NOT_FOUND, "Producing Symbol Does not Exist",
           "The Originator attempted to connect to a producing tag name that is not supported in the Target."},
          {0x01, 0x012F, PLCTAG_ERR_BAD_DATA, "Inconsistent Application Path Combination",
           "The combination of Configuration, Consuming and Producing application paths specified are inconsistent."},
          {0x01, 0x0130, PLCTAG_ERR_BAD_DATA, "Inconsistent Consume data format",
           "Information in the data segment not consistent with the format of the data in the consumed data."},
          {0x01, 0x0131, PLCTAG_ERR_BAD_DATA, "Inconsistent Product data format",
           "Information in the data segment not consistent with the format of the data in the produced data."},
          {0x01, 0x0132, PLCTAG_ERR_UNSUPPORTED, "Null Forward Open function not supported",
           "The target device does not support the function requested in the NULL Forward Open request. The request could be such items as Ping device, Configure device application, etc."},
          {0x01, 0x0133, PLCTAG_ERR_BAD_PARAM, "Connection Timeout Multiplier not acceptable",
           "The Connection Multiplier specified in the Forward Open request not acceptable by the Target device (once multiplied in conjunction with the specified timeout value). Consult the manufacturer device's documentation on what the acceptable timeout and multiplier are for this device."},
          {0x01, 0x0203, PLCTAG_ERR_TIMEOUT, "Connection Timed Out",
           "This error will be returned by the Productivity Suite CPU if a message is sent to the CPU on a connection that has already timed out. Connections time out if no message is sent to the CPU in the time period specified by the RPI rate X Connection multiplier specified in the Forward Open message."},
          {0x01, 0x0204, PLCTAG_ERR_TIMEOUT, "Unconnected Request Timed Out",
           "This time out occurs when the device sends an Unconnected Request and no response is received within the specified time out period. In the Productivity Suite CPU, this value may be found in the hardware configuration under the Ethernet port settings for the P3-550 or P3-530."},
          {0x01, 0x0205, PLCTAG_ERR_BAD_PARAM, "Parameter Error in Unconnected Request Service",
           "This error occurs when Connection Tick Time/Connection time-out combination is specified in the Forward Open or Forward Close message is not supported by the device."},
          {0x01, 0x0206, PLCTAG_ERR_TOO_LARGE, "Message Too Large for Unconnected_Send Service",
           "Occurs when Unconnected_Send message is too large to be sent to the network."},
          {0x01, 0x0207, PLCTAG_ERR_BAD_REPLY, "Unconnected Acknowledge without Reply",
           "This error occurs if an Acknowledge was received but no data response occurred. Verify that the message that was sent is supported by the Target device using the device manufacturer's documentation."},
          {0x01, 0x0301, PLCTAG_ERR_NO_MEM, "No Buffer Memory Available", "This error occurs if the Connection memory buffer in the target device is full. Correct this by reducing the frequency of the messages being sent to the device and/or reducing the number of connections to the device. Consult the manufacturer's documentation for other means of correcting this."},
          {0x01, 0x0302, PLCTAG_ERR_NO_RESOURCES, "Network Bandwidth not Available for Data",
           "This error occurs if the Producer device cannot support the specified RPI rate when the connection has been configured with schedule priority. Reduce the RPI rate or consult the manufacturer's documentation for other means to correct this."},
          {0x01, 0x0303, PLCTAG_ERR_NO_RESOURCES, "No Consumed Connection ID Filter Available",
           "This error occurs if a Consumer device doesn't have an available consumed_connection_id filter."},
          {0x01, 0x0304, PLCTAG_ERR_BAD_CONFIG, "Not Configured to Send Scheduled Priority Data",
           "This error occurs if a device has been configured for a scheduled priority message and it cannot send the data at the scheduled time slot."},
          {0x01, 0x0305, PLCTAG_ERR_NO_MATCH, "Schedule Signature Mismatch",
           "This error occurs if the schedule priority information does not match between the Target and the Originator."},
          {0x01, 0x0306, PLCTAG_ERR_UNSUPPORTED, "Schedule Signature Validation not Possible",
           "This error occurs when the schedule priority information sent to the device is not validated."},
          {0x01, 0x0311, PLCTAG_ERR_BAD_DEVICE, "Port Not Available",
           "This error occurs when a port number specified in a port segment is not available. Consult the documentation of the device to verify the correct port number."},
          {0x01, 0x0312, PLCTAG_ERR_BAD_PARAM, "Link Address Not Valid",
           "The Link address specified in the port segment is not correct. Consult the documentation of the device to verify the correct port number."},
          {0x01, 0x0315, PLCTAG_ERR_BAD_PARAM, "Invalid Segment in Connection Path",
           "This error occurs when the target device cannot understand the segment type or segment value in the Connection Path. Consult the documentation of the device to verify the correct segment type and value. If a Connection Point greater than 255 is specified this error could occur."},
          {0x01, 0x0316, PLCTAG_ERR_NO_MATCH, "Forward Close Service Connection Path Mismatch",
           "This error occurs when the Connection path in the Forward Close message does not match the Connection Path configured in the connection. Contact Tech Support if this error persists."},
          {0x01, 0x0317, PLCTAG_ERR_BAD_PARAM, "Scheduling Not Specified",
           "This error can occur if the Schedule network segment or value is invalid."},
          {0x01, 0x0318, PLCTAG_ERR_BAD_PARAM, "Link Address to Self Invalid",
           "If the Link address points back to the originator device, this error will occur."},
          {0x01, 0x0319, PLCTAG_ERR_NO_RESOURCES, "Secondary Resource Unavailable",
           "This occurs in a redundant system when the secondary connection request is unable to duplicate the primary connection request."},
          {0x01, 0x031A, PLCTAG_ERR_DUPLICATE, "Rack Connection Already established",
           "The connection to a module is refused because part or all of the data requested is already part of an existing rack connection."},
          {0x01, 0x031B, PLCTAG_ERR_DUPLICATE, "Module Connection Already established",
           "The connection to a rack is refused because part or all of the data requested is already part of an existing module connection."},
          {0x01, 0x031C, PLCTAG_ERR_REMOTE_ERR, "Miscellaneous",
           "This error is returned when there is no other applicable code for the error condition. Consult the manufacturer's documentation or contact Tech support if this error persist."},
          {0x01, 0x031D, PLCTAG_ERR_NO_MATCH, "Redundant Connection Mismatch",
           "This error occurs when these parameters don't match when establishing a redundant owner connection: O -> T RPI, O -> T Connection Parameters, T -> O RPI, T -> O Connection Parameters and Transport Type and Trigger."},
          {0x01, 0x031E, PLCTAG_ERR_NO_RESOURCES, "No more User Configurable Link Resources Available in the Producing Module",
           "This error is returned from the Target device when no more available Consumer connections available for a Producer."},
          {0x01, 0x031F, PLCTAG_ERR_NO_RESOURCES,
           "No User Configurable Link Consumer Resources Configured in the Producing Module",
           "This error is returned from the Target device when no Consumer connections have been configured for a Producer connection."},
          {0x01, 0x0800, PLCTAG_ERR_BAD_DEVICE, "Network Link Offline", "The Link path is invalid or not available."},
          {0x01, 0x0810, PLCTAG_ERR_NO_DATA, "No Target Application Data Available",
           "This error is returned from the Target device when the application has no valid data to produce."},
          {0x01, 0x0811, PLCTAG_ERR_NO_DATA, "No Originator Application Data Available",
           "This error is returned from the Originator device when the application has no valid data to produce."},
          {0x01, 0x0812, PLCTAG_ERR_UNSUPPORTED, "Node Address has changed since the Network was scheduled",
           "This specifies that the router has changed node addresses since the value configured in the original connection."},
          {0x01, 0x0813, PLCTAG_ERR_UNSUPPORTED, "Not Configured for Off-subnet Multicast",
           "The producer has been requested to support a Multicast connection for a consumer on a different subnet and does not support this functionality."},
          {0x01, 0x0814, PLCTAG_ERR_BAD_DATA, "Invalid Produce/Consume Data format",
           "Information in the data segment not consistent with the format of the data in the consumed or produced data. Errors 0x0130 and 0x0131 are typically used for this situation in most devices now."},
          {0x02, -1, PLCTAG_ERR_NO_RESOURCES, "Resource Unavailable for Unconnected Send",
           "The Target device does not have the resources to process the Unconnected Send request."},
          {0x03, -1, PLCTAG_ERR_BAD_PARAM, "Parameter value invalid.", ""},
          {0x04, -1, PLCTAG_ERR_NOT_FOUND, "IOI could not be deciphered or tag does not exist.",
           "The path segment identifier or the segment syntax was not understood by the target device."},
          {0x05, -1, PLCTAG_ERR_BAD_PARAM, "Path Destination Error",
           "The Class, Instance or Attribute value specified in the Unconnected Explicit Message request is incorrect or not supported in the Target device. Check the manufacturer's documentation for the correct codes to use."},
          {0x06, -1, PLCTAG_ERR_TOO_LARGE, "Data requested would not fit in response packet.",
           "The data to be read/written needs to be broken up into multiple packets.0x070000 Connection lost: The messaging connection was lost."},
          {0x07, -1, PLCTAG_ERR_BAD_CONNECTION, "Connection lost", "The messaging connection was lost."},
          {0x08, -1, PLCTAG_ERR_UNSUPPORTED, "Unsupported service.", ""},
          {0x09, -1, PLCTAG_ERR_BAD_DATA, "Error in Data Segment",
           "This error code is returned when an error is encountered in the Data segment portion of a Forward Open message. The Extended Status value is the offset in the Data segment where the error was encountered."},
          {0x0A, -1, PLCTAG_ERR_BAD_STATUS, "Attribute list error",
           "An attribute in the Get_Attribute_List or Set_Attribute_List response has a non-zero status."},
          {0x0B, -1, PLCTAG_ERR_DUPLICATE, "Already in requested mode/state",
           "The object is already in the mode/state being requested by the service."},
          {0x0C, -1, PLCTAG_ERR_BAD_STATUS, "Object State Error",
           "This error is returned from the Target device when the current state of the Object requested does not allow it to be returned. The current state can be specified in the Optional Extended Error status field."},
          {0x0D, -1, PLCTAG_ERR_DUPLICATE, "Object already exists.",
           "The requested instance of object to be created already exists."},
          {0x0E, -1, PLCTAG_ERR_NOT_ALLOWED, "Attribute not settable",
           "A request to modify non-modifiable attribute was received."},
          {0x0F, -1, PLCTAG_ERR_NOT_ALLOWED, "Permission denied.", ""},
          {0x10, -1, PLCTAG_ERR_BAD_STATUS, "Device State Error",
           "This error is returned from the Target device when the current state of the Device requested does not allow it to be returned. The current state can be specified in the Optional Extended Error status field. Check your configured connections points for other Client devices using this same connection."},
          {0x11, -1, PLCTAG_ERR_TOO_LARGE, "Reply data too large",
           "The data to be transmitted in the response buffer is larger than the allocated response buffer."},
          {0x12, -1, PLCTAG_ERR_NOT_ALLOWED, "Fragmentation of a primitive value",
           "The service specified an operation that is going to fragment a primitive data value. For example, trying to send a 2 byte value to a REAL data type (4 byte)."},
          {0x13, -1, PLCTAG_ERR_TOO_SMALL, "Not Enough Data", "Not enough data was supplied in the service request specified."},
          {0x14, -1, PLCTAG_ERR_UNSUPPORTED, "Attribute not supported.",
           "The attribute specified in the request is not supported."},
          {0x15, -1, PLCTAG_ERR_TOO_LARGE, "Too Much Data", "Too much data was supplied in the service request specified."},
          {0x16, -1, PLCTAG_ERR_NOT_FOUND, "Object does not exist.", "The object specified does not exist in the device."},
          {0x17, -1, PLCTAG_ERR_NOT_ALLOWED, "Service fragmentation sequence not in progress.",
           "The fragmentation sequence for this service is not currently active for this data."},
          {0x18, -1, PLCTAG_ERR_NO_DATA, "No stored attribute data.",
           "The attribute data of this object was not saved prior to the requested service."},
          {0x19, -1, PLCTAG_ERR_REMOTE_ERR, "Store operation failure.",
           "The attribute data of this object was not saved due to a failure during the attempt."},
          {0x1A, -1, PLCTAG_ERR_TOO_LARGE, "Routing failure, request packet too large.",
           "The service request packet was too large for transmission on a network in the path to the destination."},
          {0x1B, -1, PLCTAG_ERR_TOO_LARGE, "Routing failure, response packet too large.",
           "The service reponse packet was too large for transmission on a network in the path from the destination."},
          {0x1C, -1, PLCTAG_ERR_NO_DATA, "Missing attribute list entry data.",
           "The service did not supply an attribute in a list of attributes that was needed by the service to perform the requested behavior."},
          {0x1E, -1, PLCTAG_ERR_PARTIAL, "One or more bundled requests failed..",
           "One or more of the bundled requests has an error."},
          {0x1D, -1, PLCTAG_ERR_BAD_DATA, "Invalid attribute value list.",
           "The service is returning the list of attributes supplied with status information for those attributes that were invalid."},
          {0x20, -1, PLCTAG_ERR_BAD_PARAM, "Invalid parameter.",
           "A parameter associated with the request was invalid. This code is used when a parameter does meet the requirements defined in an Application Object specification."},
          {0x21, -1, PLCTAG_ERR_DUPLICATE, "Write-once value or medium already written.",
           "An attempt was made to write to a write-once-medium that has already been written or to modify a value that cannot be change once established."},
          {0x22, -1, PLCTAG_ERR_BAD_REPLY, "Invalid Reply Received",
           "An invalid reply is received (example: service code sent doesn't match service code received.)."},
          {0x25, -1, PLCTAG_ERR_BAD_PARAM, "Key failure in path",
           "The key segment was included as the first segment in the path does not match the destination module."},
          {0x26, -1, PLCTAG_ERR_BAD_PARAM, "The number of IOI words specified does not match IOI word count.",
           "Check the tag length against what was sent."},
          {0x27, -1, PLCTAG_ERR_BAD_PARAM, "Unexpected attribute in list",
           "An attempt was made to set an attribute that is not able to be set at this time."},
          {0x28, -1, PLCTAG_ERR_BAD_PARAM, "Invalid Member ID.",
           "The Member ID specified in the request does not exist in the specified Class/Instance/Attribute."},
          {0x29, -1, PLCTAG_ERR_NOT_ALLOWED, "Member not writable.", "A request to modify a non-modifiable member was received."},
          {0xFF, 0x2104, PLCTAG_ERR_OUT_OF_BOUNDS, "Address is out of range.", ""},
          {0xFF, 0x2105, PLCTAG_ERR_OUT_OF_BOUNDS, "Attempt to access beyond the end of the data object.", ""},
          {0xFF, 0x2107, PLCTAG_ERR_BAD_PARAM, "The data type is invalid or not supported.", ""},
          {-1, -1, PLCTAG_ERR_REMOTE_ERR, "Unknown error code.", "Unknown error code."}};


static int lookup_error_code(uint8_t *data) {
    int index = 0;
    int primary_code = 0;
    int secondary_code = 0;

    /* build the error status */
    primary_code = (int)*data;

    if(primary_code != 0) {
        int num_status_words = 0;

        data++;
        num_status_words = (int)*data;

        if(num_status_words > 0) {
            data++;
            secondary_code = (int)data[0] + (int)(data[1] << 8);
        }
    }

    while(error_code_table[index].primary_code != -1) {
        if(error_code_table[index].primary_code == primary_code) {
            if(error_code_table[index].secondary_code == secondary_code || error_code_table[index].secondary_code == -1) {
                break;
            }
        }

        index++;
    }

    return index;
}


const char *decode_cip_error_short(uint8_t *data) {
    int index = lookup_error_code(data);

    return error_code_table[index].short_desc;
}


const char *decode_cip_error_long(uint8_t *data) {
    int index = lookup_error_code(data);

    return error_code_table[index].long_desc;
}


int decode_cip_error_code(uint8_t *data) {
    int index = lookup_error_code(data);

    return error_code_table[index].translated_code;
}
