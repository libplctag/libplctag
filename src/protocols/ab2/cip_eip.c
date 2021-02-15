/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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
#include <stdlib.h>
#include <lib/libplctag.h>
#include <ab2/cip_eip.h>
#include <ab2/eip.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/lock.h>
#include <util/mem.h>
#include <util/mutex.h>
#include <util/protocol.h>
#include <util/string.h>

struct cip_eip_s {
    struct protocol_s protocol;

    lock_t lock;
    uint16_t tsn;

    protocol_p eip;
    struct eip_request_s eip_request;

    char *host;
    char *path;
};

#define CIP_EIP_STACK "CIP/EIP"


static int cip_constructor(const char *protocol_key, attr attribs, protocol_p *protocol);
static void cip_rc_destroy(void *plc_arg);

/* calls to handle requests to this protocol layer. */
static int new_cip_request_callback(protocol_p protocol, protocol_request_p request);
static int cleanup_cip_request_callback(protocol_p protocol, protocol_request_p request);
static int build_cip_request_result_callback(protocol_p protocol, protocol_request_p request, int status, slice_t used_slice, slice_t *new_output_slice);
static int process_cip_response_result_callback(protocol_p protocol, protocol_request_p request, int status, slice_t used_slice, slice_t *new_input_slice);

/* calls to handle requests queued for the next protocol layer */
static int build_eip_request_callback(protocol_p protocol, void *client, slice_t output_buffer, slice_t *used_buffer);
static int handle_eip_response_callback(protocol_p protocol, void *client, slice_t input_buffer, slice_t *used_buffer);



protocol_p cip_eip_get(attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    const char *host = NULL;
    const char *path = NULL;
    const char *protocol_key = NULL;
    cip_eip_p result = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    host = attr_get_str(attribs, "gateway", "");
    path = attr_get_str(attribs, "path", "");

    /* if the host is empty, error. */
    if(!host || str_length(host) == 0) {
        pdebug(DEBUG_WARN, "Gateway must not be empty or null!");
        return NULL;
    }

    /* create the protocol key, a copy will be made, so destroy this after use. */
    protocol_key = str_concat(CIP_EIP_STACK, "/", host, "/", path);
    if(!protocol_key) {
        pdebug(DEBUG_WARN, "Unable to allocate protocol key string!");
        return PLCTAG_ERR_NO_MEM;
    }

    rc = protocol_get(protocol_key, attribs, (protocol_p *)&result, cip_constructor);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to get CIP/EIP protocol stack, error %s!", plc_tag_decode_error(rc));
        mem_free(protocol_key);
        rc_dec(result);
        return NULL;
    }

    /* we are done with the protocol key */
    mem_free(protocol_key);

    pdebug(DEBUG_INFO, "Done.");

    return (protocol_p)result;
}



int cip_constructor(const char *protocol_key, attr attribs, protocol_p *protocol)
{
    int rc = PLCTAG_STATUS_OK;
    cip_eip_p result = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    result = (cip_eip_p)rc_alloc(sizeof(*result), cip_rc_destroy);
    if(result) {
        rc = protocol_init(result, protocol_key, new_cip_request_callback, cleanup_cip_request_callback);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to initialize new protocol, error %s!", plc_tag_decode_error(rc));
            rc_dec(result);
            return rc;
        }

        /* set up the request for the next level down */
        rc = protocol_request_init((protocol_p)result, (protocol_request_p)&(result->eip_request));
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to initialize protocol request, error %s!", plc_tag_decode_error(rc));
            rc_dec(result);
            return rc;
        }

        /* TODO get the next protocol layer */
        result->eip = eip_get(attribs);
        if(!result->eip) {
            pdebug(DEBUG_WARN, "Unable to create next protocol layer for EIP!");
            rc_dec(result);
            return PLCTAG_ERR_BAD_CONNECTION;
        }

        *protocol = (protocol_p)result;
    } else {
        pdebug(DEBUG_WARN, "Unable to allocate PCCC/CIP/EIP stack!");
        *protocol = NULL;
        return PLCTAG_ERR_NO_MEM;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



void cip_rc_destroy(void *plc_arg)
{
    cip_eip_p plc = (cip_eip_p)plc_arg;

    if(!plc) {
        pdebug(DEBUG_WARN, "Destructor function called with null pointer!");
        return;
    }

    pdebug(DEBUG_INFO, "Starting for protocol layer %s.", plc->protocol.protocol_key);

    /* destroy PCCC specific features first, then destroy the generic protocol. */

    /* abort anything we have in flight to the layer below */
    protocol_stop_request(plc->eip, (protocol_request_p)&(plc->eip_request));

    /* release our reference on the EIP layer */
    plc->eip = rc_dec(plc->eip);

    /* destroy the generic protocol */
    protocol_cleanup((protocol_p)plc);

    pdebug(DEBUG_INFO, "Done.");
}


// static int skip_whitespace(const char *name, int *name_index);
// static int parse_bit_segment(ab_tag_p tag, const char *name, int *name_index);
// static int parse_symbolic_segment(ab_tag_p tag, const char *name, int *encoded_index, int *name_index);
// static int parse_numeric_segment(ab_tag_p tag, const char *name, int *encoded_index, int *name_index);

// static slice_t cip_encode_path(const char *path, bool is_connected, int *dhp_dest, slice_t buffer);
// static int match_numeric_segment(const char *path, size_t *path_index, uint8_t *conn_path, size_t *buffer_index);
// static int match_ip_addr_segment(const char *path, size_t *path_index, uint8_t *conn_path, size_t *buffer_index);
// static int match_dhp_addr_segment(const char *path, size_t *path_index, uint8_t *port, uint8_t *src_node, uint8_t *dest_node);

// #define MAX_IP_ADDR_SEG_LEN (16)



/*
 * cip_encode_path()
 *
 * This function takes a path string of comma separated components that are numbers or
 * colon-separated triples that designate a DHP connection.  It converts the path
 * into a path segment in the passed slice.
 *
 * If the protocol type is for a PCCC-PLC series and the last hop in the path is
 * DH+, then we need to set up a different message routing path.
 *
 * Note that it is possible that the path passed is null.  That is OK for Micro850,
 * for example.  In that case, we still need to put the message routing info at
 * the end.
 */

// slice_t cip_encode_path(const char *path, bool is_connected, int *dhp_dest, slice_t buffer)
// {
//     size_t path_len = 0;
//     size_t buffer_index = 0;
//     size_t path_index = 0;
//     int has_dhp = 0;
//     uint8_t dhp_port = 0;
//     uint8_t dhp_src_node = 0;
//     uint8_t dhp_dest_node = 0;

//     pdebug(DEBUG_DETAIL, "Starting");

//     path_len = (size_t)(ssize_t)str_length(path);

//     while(path_index < path_len && path[path_index] && buffer_index < slice_len(buffer)) {
//         if(path[path_index] == ',') {
//             /* skip separators. */
//             pdebug(DEBUG_DETAIL, "Skipping separator character '%c'.", (char)path[path_index]);

//             path_index++;
//         } else if(match_numeric_segment(path, &path_index, buffer, &buffer_index) == PLCTAG_STATUS_OK) {
//             pdebug(DEBUG_DETAIL, "Found numeric segment.");
//         } else if(match_ip_addr_segment(path, &path_index, buffer, &buffer_index) == PLCTAG_STATUS_OK) {
//             pdebug(DEBUG_DETAIL, "Found IP address segment.");
//         } else if(match_dhp_addr_segment(path, &path_index, &dhp_port, &dhp_src_node, &dhp_dest_node) == PLCTAG_STATUS_OK) {
//             pdebug(DEBUG_DETAIL, "Found DH+ address segment.");

//             /* check if it is last. */
//             if(path_index < path_len) {
//                 pdebug(DEBUG_WARN, "DH+ address must be the last segment in a path! %d %d", (int)(ssize_t)path_index, (int)(ssize_t)path_len);
//                 return slice_make_err(PLCTAG_ERR_BAD_PARAM);
//             }

//             has_dhp = 1;
//         } else {
//             /* unknown, cannot parse this! */
//             pdebug(DEBUG_WARN, "Unable to parse remaining path string from position %d, \"%s\".", (int)(ssize_t)path_index, (char*)&path[path_index]);
//             return slice_make_err(PLCTAG_ERR_BAD_PARAM);
//         }
//     }

//     if(buffer_index >= slice_len(buffer)) {
//         pdebug(DEBUG_WARN, "Encoded connection path is too long to fit in the buffer (%d >= %d).", (int)(ssize_t)buffer_index, slice_len(buffer));
//         return slice_make_err(PLCTAG_ERR_TOO_LARGE);
//     }

//     /* did we find DH+ in the path? */
//     if(has_dhp) {
//         /* do we support it? */
//         if(dhp_dest) {
//             *dhp_dest = (uint16_t)dhp_dest_node;

//             /* add the special PCCC/DH+ routing on the end. */
//             slice_set_byte(buffer, buffer_index + 0, 0x20);
//             slice_set_byte(buffer, buffer_index + 1, 0xA6);
//             slice_set_byte(buffer, buffer_index + 2, 0x24);
//             slice_set_byte(buffer, buffer_index + 3, dhp_port);
//             slice_set_byte(buffer, buffer_index + 4, 0x2C);
//             slice_set_byte(buffer, buffer_index + 5, 0x01);
//             buffer_index += 6;
//         } else {
//             /* error, we found it but do not support it! */
//             pdebug(DEBUG_WARN, "Found a DH+ address in the path unexpectedly!");
//             return slice_make_err(PLCTAG_ERR_BAD_PARAM);
//         }
//     } else {
//         if(dhp_dest) {
//             /* we could have had a DH+ address but there was not one in the path. */
//             *dhp_dest = PLCTAG_ERR_NOT_FOUND;
//         }

//         if(is_connected) {
//             pdebug(DEBUG_DETAIL, "PLC needs connection, adding path to the router object.");

//             /*
//              * we do a generic path to the router
//              * object in the PLC.  But only if the PLC is
//              * one that needs a connection.  For instance a
//              * Micro850 needs to work in connected mode.
//              */
//             slice_set_byte(buffer, buffer_index + 0, 0x20);
//             slice_set_byte(buffer, buffer_index + 1, 0x02);
//             slice_set_byte(buffer, buffer_index + 2, 0x24);
//             slice_set_byte(buffer, buffer_index + 3, 0x01);
//             buffer_index += 4;
//         }
//     }

//     /*
//      * zero pad the path to a multiple of 16-bit
//      * words.
//      */
//     pdebug(DEBUG_DETAIL,"IOI size before %d", buffer_index);
//     if(buffer_index & 0x01) {
//         slice_set_byte(buffer, buffer_index, 0);
//         buffer_index++;
//     }

//     pdebug(DEBUG_DETAIL, "Done");

//     return slice_from_slice(buffer, 0, buffer_index);
// }


// int match_numeric_segment(const char *path, int *path_index, slice_t buffer, int *buffer_index)
// {
//     int val = 0;
//     size_t p_index = *path_index;
//     size_t c_index = *buffer_index;

//     pdebug(DEBUG_DETAIL, "Starting at position %d in string %s.", *path_index, path);

//     while(isdigit(path[p_index])) {
//         val = (val * 10) + (path[p_index] - '0');
//         p_index++;
//     }

//     /* did we match anything? */
//     if(p_index == *path_index) {
//         pdebug(DEBUG_DETAIL,"Did not find numeric path segment at position %d.", p_index);
//         return PLCTAG_ERR_NOT_FOUND;
//     }

//     /* was the numeric segment valid? */
//     if(val < 0 || val > 0x0F) {
//         pdebug(DEBUG_WARN, "Numeric segment in path at position %d is out of bounds!", *path_index);
//         return PLCTAG_ERR_OUT_OF_BOUNDS;
//     }

//     /* store the encoded segment data. */
//     conn_path[c_index] = (uint8_t)(unsigned int)(val);
//     c_index++;
//     *buffer_index = c_index;

//     /* bump past our last read character. */
//     *path_index = p_index;

//     pdebug(DEBUG_DETAIL, "Done.   Found numeric segment %d.", val);

//     return PLCTAG_STATUS_OK;
// }

// /*
//  * match symbolic IP address segments.
//  *  18,10.206.10.14 - port 2/A -> 10.206.10.14
//  *  19,10.206.10.14 - port 3/B -> 10.206.10.14
//  */

// int match_ip_addr_segment(const char *path, size_t *path_index, uint8_t *conn_path, size_t *buffer_index)
// {
//     uint8_t *addr_seg_len = NULL;
//     int val = 0;
//     size_t p_index = *path_index;
//     size_t c_index = *buffer_index;

//     pdebug(DEBUG_DETAIL, "Starting at position %d in string %s.", (int)(ssize_t)*path_index, path);

//     /* first part, the extended address marker*/
//     val = 0;
//     while(isdigit(path[p_index])) {
//         val = (val * 10) + (path[p_index] - '0');
//         p_index++;
//     }

//     if(val != 18 && val != 19) {
//         pdebug(DEBUG_DETAIL, "Path segment at %d does not match IP address segment.", (int)(ssize_t)*path_index);
//         return PLCTAG_ERR_NOT_FOUND;
//     }

//     if(val == 18) {
//         pdebug(DEBUG_DETAIL, "Extended address on port A.");
//     } else {
//         pdebug(DEBUG_DETAIL, "Extended address on port B.");
//     }

//     /* is the next character a comma? */
//     if(path[p_index] != ',') {
//         pdebug(DEBUG_DETAIL, "Not an IP address segment starting at position %d of path.  Remaining: \"%s\".",(int)(ssize_t)p_index, &path[p_index]);
//         return PLCTAG_ERR_NOT_FOUND;
//     }

//     p_index++;

//     /* start building up the connection path. */
//     conn_path[c_index] = (uint8_t)(unsigned int)val;
//     c_index++;

//     /* point into the encoded path for the symbolic segment length. */
//     addr_seg_len = &conn_path[c_index];
//     *addr_seg_len = 0;
//     c_index++;

//     /* get the first IP address digit. */
//     val = 0;
//     while(isdigit(path[p_index]) && (int)(unsigned int)(*addr_seg_len) < (MAX_IP_ADDR_SEG_LEN - 1)) {
//         val = (val * 10) + (path[p_index] - '0');
//         conn_path[c_index] = (uint8_t)path[p_index];
//         c_index++;
//         p_index++;
//         (*addr_seg_len)++;
//     }

//     if(val < 0 || val > 255) {
//         pdebug(DEBUG_WARN, "First IP address part is out of bounds (0 <= %d < 256) for an IPv4 octet.", val);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     pdebug(DEBUG_DETAIL, "First IP segment: %d.", val);

//     /* is the next character a dot? */
//     if(path[p_index] != '.') {
//         pdebug(DEBUG_DETAIL, "Unexpected character '%c' found at position %d in first IP address part.", path[p_index], p_index);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     /* copy the dot. */
//     conn_path[c_index] = (uint8_t)path[p_index];
//     c_index++;
//     p_index++;
//     (*addr_seg_len)++;

//     /* get the second part. */
//     val = 0;
//     while(isdigit(path[p_index]) && (int)(unsigned int)(*addr_seg_len) < (MAX_IP_ADDR_SEG_LEN - 1)) {
//         val = (val * 10) + (path[p_index] - '0');
//         conn_path[c_index] = (uint8_t)path[p_index];
//         c_index++;
//         p_index++;
//         (*addr_seg_len)++;
//     }

//     if(val < 0 || val > 255) {
//         pdebug(DEBUG_WARN, "Second IP address part is out of bounds (0 <= %d < 256) for an IPv4 octet.", val);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     pdebug(DEBUG_DETAIL, "Second IP segment: %d.", val);

//     /* is the next character a dot? */
//     if(path[p_index] != '.') {
//         pdebug(DEBUG_DETAIL, "Unexpected character '%c' found at position %d in second IP address part.", path[p_index], p_index);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     /* copy the dot. */
//     conn_path[c_index] = (uint8_t)path[p_index];
//     c_index++;
//     p_index++;
//     (*addr_seg_len)++;

//     /* get the third part. */
//     val = 0;
//     while(isdigit(path[p_index]) && (int)(unsigned int)(*addr_seg_len) < (MAX_IP_ADDR_SEG_LEN - 1)) {
//         val = (val * 10) + (path[p_index] - '0');
//         conn_path[c_index] = (uint8_t)path[p_index];
//         c_index++;
//         p_index++;
//         (*addr_seg_len)++;
//     }

//     if(val < 0 || val > 255) {
//         pdebug(DEBUG_WARN, "Third IP address part is out of bounds (0 <= %d < 256) for an IPv4 octet.", val);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     pdebug(DEBUG_DETAIL, "Third IP segment: %d.", val);

//     /* is the next character a dot? */
//     if(path[p_index] != '.') {
//         pdebug(DEBUG_DETAIL, "Unexpected character '%c' found at position %d in third IP address part.", path[p_index], p_index);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     /* copy the dot. */
//     conn_path[c_index] = (uint8_t)path[p_index];
//     c_index++;
//     p_index++;
//     (*addr_seg_len)++;

//     /* get the fourth part. */
//     val = 0;
//     while(isdigit(path[p_index]) && (int)(unsigned int)(*addr_seg_len) < (MAX_IP_ADDR_SEG_LEN - 1)) {
//         val = (val * 10) + (path[p_index] - '0');
//         conn_path[c_index] = (uint8_t)path[p_index];
//         c_index++;
//         p_index++;
//         (*addr_seg_len)++;
//     }

//     if(val < 0 || val > 255) {
//         pdebug(DEBUG_WARN, "Fourth IP address part is out of bounds (0 <= %d < 256) for an IPv4 octet.", val);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     pdebug(DEBUG_DETAIL, "Fourth IP segment: %d.", val);

//     /* We need to zero pad if the length is not a multiple of two. */
//     if((*addr_seg_len) && (uint8_t)0x01) {
//         conn_path[c_index] = (uint8_t)0;
//         c_index++;
//     }

//     /* set the return values. */
//     *path_index = p_index;
//     *buffer_index = c_index;

//     pdebug(DEBUG_DETAIL, "Done.");

//     return PLCTAG_STATUS_OK;
// }


// /*
//  * match DH+ address segments.
//  *  A:1:2 - port 2/A -> DH+ node 2
//  *  B:1:2 - port 3/B -> DH+ node 2
//  *
//  * A and B can be lowercase or numeric.
//  */

// int match_dhp_addr_segment(const char *path, size_t *path_index, uint8_t *port, uint8_t *src_node, uint8_t *dest_node)
// {
//     int val = 0;
//     size_t p_index = *path_index;

//     pdebug(DEBUG_DETAIL, "Starting at position %d in string %s.", (int)(ssize_t)*path_index, path);

//     /* Get the port part. */
//     switch(path[p_index]) {
//         case 'A':
//             /* fall through */
//         case 'a':
//             /* fall through */
//         case '2':
//             *port = 1;
//             break;

//         case 'B':
//             /* fall through */
//         case 'b':
//             /* fall through */
//         case '3':
//             *port = 2;
//             break;

//         default:
//             pdebug(DEBUG_DETAIL, "Character '%c' at position %d does not match start of DH+ segment.", path[p_index], (int)(ssize_t)p_index);
//             return PLCTAG_ERR_NOT_FOUND;
//             break;
//     }

//     p_index++;

//     /* is the next character a colon? */
//     if(path[p_index] != ':') {
//         pdebug(DEBUG_DETAIL, "Character '%c' at position %d does not match first colon expected in DH+ segment.", path[p_index], (int)(ssize_t)p_index);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     p_index++;

//     /* get the source node */
//     val = 0;
//     while(isdigit(path[p_index])) {
//         val = (val * 10) + (path[p_index] - '0');
//         p_index++;
//     }

//     /* is the source node a valid number? */
//     if(val < 0 || val > 255) {
//         pdebug(DEBUG_WARN, "Source node DH+ address part is out of bounds (0 <= %d < 256).", val);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     *src_node = (uint8_t)(unsigned int)val;

//     /* is the next character a colon? */
//     if(path[p_index] != ':') {
//         pdebug(DEBUG_DETAIL, "Character '%c' at position %d does not match the second colon expected in DH+ segment.", path[p_index], (int)(ssize_t)p_index);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     p_index++;

//     /* get the destination node */
//     val = 0;
//     while(isdigit(path[p_index])) {
//         val = (val * 10) + (path[p_index] - '0');
//         p_index++;
//     }

//     /* is the destination node a valid number? */
//     if(val < 0 || val > 255) {
//         pdebug(DEBUG_WARN, "Destination node DH+ address part is out of bounds (0 <= %d < 256).", val);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     *dest_node = (uint8_t)(unsigned int)val;
//     *path_index = p_index;

//     pdebug(DEBUG_DETAIL, "Done.");

//     return PLCTAG_STATUS_OK;
// }

// /*
//  * The EBNF is:
//  *
//  * tag ::= SYMBOLIC_SEG ( tag_seg )* ( bit_seg )?
//  *
//  * tag_seg ::= '.' SYMBOLIC_SEG
//  *             '[' array_seg ']'
//  *
//  * bit_seg ::= '.' [0-9]+
//  *
//  * array_seg ::= NUMERIC_SEG ( ',' NUMERIC_SEG ) {0,2}
//  *
//  * SYMBOLIC_SEG ::= [a-zA-Z]([a-zA-Z0-9_]*)
//  *
//  * NUMERIC_SEG ::= [0-9]+
//  *
//  */


// int cip_encode_tag_name(const char *name, slice_t buffer)
// {
//     int rc = PLCTAG_STATUS_OK;
//     int encoded_index = 0;
//     int name_index = 0;
//     int name_len = str_length(name);

//     /* zero out the CIP encoded name size. Byte zero in the encoded name. */
//     tag->encoded_name[encoded_index] = 0;
//     encoded_index++;

//     /* names must start with a symbolic segment. */
//     if(parse_symbolic_segment(tag, name, &encoded_index, &name_index) != PLCTAG_STATUS_OK) {
//         pdebug(DEBUG_WARN,"Unable to parse initial symbolic segment in tag name %s!", name);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     while(name_index < name_len && encoded_index < MAX_TAG_NAME) {
//         /* try to parse the different parts of the name. */
//         if(name[name_index] == '.') {
//             name_index++;
//             /* could be a name segment or could be a bit identifier. */
//             if(parse_symbolic_segment(tag, name, &encoded_index, &name_index) != PLCTAG_STATUS_OK) {
//                 /* try a bit identifier. */
//                 if(parse_bit_segment(tag, name, &name_index) == PLCTAG_STATUS_OK) {
//                     pdebug(DEBUG_DETAIL, "Found bit identifier %u.", tag->bit);
//                     break;
//                 } else {
//                     pdebug(DEBUG_WARN, "Expected a symbolic segment or a bit identifier at position %d in tag name %s", name_index, name);
//                     return PLCTAG_ERR_BAD_PARAM;
//                 }
//             } else {
//                 pdebug(DEBUG_DETAIL, "Found symbolic segment ending at %d", name_index);
//             }
//         } else if (name[name_index] == '[') {
//             int num_dimensions = 0;
//             /* must be an array so look for comma separated numeric segments. */
//             do {
//                 name_index++;
//                 num_dimensions++;

//                 skip_whitespace(name, &name_index);
//                 rc = parse_numeric_segment(tag, name, &encoded_index, &name_index);
//                 skip_whitespace(name, &name_index);
//             } while(rc == PLCTAG_STATUS_OK && name[name_index] == ',' && num_dimensions < 3);

//             /* must terminate with a closing ']' */
//             if(name[name_index] != ']') {
//                 pdebug(DEBUG_WARN, "Bad tag name format, expected closing array bracket at %d in tag name %s!", name_index, name);
//                 return PLCTAG_ERR_BAD_PARAM;
//             }

//             /* step past the closing bracket. */
//             name_index++;
//         } else {
//             pdebug(DEBUG_WARN,"Unexpected character at position %d in name string %s!", name_index, name);
//             break;
//         }
//     }

//     if(name_index != name_len) {
//         pdebug(DEBUG_WARN, "Bad tag name format.  Tag must end with a bit identifier if one is present.");
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     /* set the word count. */
//     tag->encoded_name[0] = (uint8_t)((encoded_index -1)/2);
//     tag->encoded_name_size = encoded_index;

//     return PLCTAG_STATUS_OK;
// }

// int skip_whitespace(const char *name, int *name_index)
// {
//     while(name[*name_index] == ' ') {
//         (*name_index)++;
//     }

//     return PLCTAG_STATUS_OK;
// }


// /*
//  * A bit segment is simply an integer from 0 to 63 (inclusive). */
// int parse_bit_segment(ab_tag_p tag, const char *name, int *name_index)
// {
//     const char *p, *q;
//     long val;

//     pdebug(DEBUG_DETAIL, "Starting with name index=%d.", *name_index);

//     p = &name[*name_index];
//     q = p;

//     val = strtol((char *)p, (char **)&q, 10);

//     /* sanity checks. */
//     if(p == q) {
//         /* no number. */
//         pdebug(DEBUG_WARN,"Expected bit identifier or symbolic segment at position %d in tag name %s!", *name_index, name);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     if((val < 0) || (val >= 256)) {
//         pdebug(DEBUG_WARN,"Bit identifier must be between 0 and 255, inclusive, was %d!", (int)val);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     if(tag->elem_count != 1) {
//         pdebug(DEBUG_WARN, "Bit tags must have only one element!");
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     /* bump name_index. */
//     *name_index += (int)(q-p);
//     tag->is_bit = 1;
//     tag->bit = (uint8_t)val;

//     return PLCTAG_STATUS_OK;
// }


// int parse_symbolic_segment(ab_tag_p tag, const char *name, int *encoded_index, int *name_index)
// {
//     int encoded_i = *encoded_index;
//     int name_i = *name_index;
//     int name_start = name_i;
//     int seg_len_index = 0;
//     int seg_len = 0;

//     pdebug(DEBUG_DETAIL, "Starting with name index=%d and encoded name index=%d.", name_i, encoded_i);

//     /* a symbolic segment must start with an alphabetic character, then can have digits or underscores. */
//     if(!isalpha(name[name_i]) && name[name_i] != ':' && name[name_i] != '_') {
//         pdebug(DEBUG_DETAIL, "tag name at position %d is not the start of a symbolic segment.", name_i);
//         return PLCTAG_ERR_NO_MATCH;
//     }

//     /* start building the encoded symbolic segment. */
//     tag->encoded_name[encoded_i] = 0x91; /* start of symbolic segment. */
//     encoded_i++;
//     seg_len_index = encoded_i;
//     tag->encoded_name[seg_len_index]++;
//     encoded_i++;

//     /* store the first character of the name. */
//     tag->encoded_name[encoded_i] = (uint8_t)name[name_i];
//     encoded_i++;
//     name_i++;

//     /* get the rest of the name. */
//     while(isalnum(name[name_i]) || name[name_i] == ':' || name[name_i] == '_') {
//         tag->encoded_name[encoded_i] = (uint8_t)name[name_i];
//         encoded_i++;
//         tag->encoded_name[seg_len_index]++;
//         name_i++;
//     }

//     seg_len = tag->encoded_name[seg_len_index];

//     /* finish up the encoded name.   Space for the name must be a multiple of two bytes long. */
//     if(tag->encoded_name[seg_len_index] & 0x01) {
//         tag->encoded_name[encoded_i] = 0;
//         encoded_i++;
//     }

//     *encoded_index = encoded_i;
//     *name_index = name_i;

//     pdebug(DEBUG_DETAIL, "Parsed symbolic segment \"%.*s\" in tag name.", seg_len, &name[name_start]);

//     return PLCTAG_STATUS_OK;
// }


// int parse_numeric_segment(ab_tag_p tag, const char *name, int *encoded_index, int *name_index)
// {
//     const char *p, *q;
//     long val;

//     pdebug(DEBUG_DETAIL, "Starting with name index=%d and encoded name index=%d.", *name_index, *encoded_index);

//     p = &name[*name_index];
//     q = p;

//     val = strtol((char *)p, (char **)&q, 10);

//     /* sanity checks. */
//     if(p == q) {
//         /* no number. */
//         pdebug(DEBUG_WARN,"Expected numeric segment at position %d in tag name %s!", *name_index, name);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     if(val < 0) {
//         pdebug(DEBUG_WARN,"Numeric segment must be greater than or equal to zero, was %d!", (int)val);
//         return PLCTAG_ERR_BAD_PARAM;
//     }

//     /* bump name_index. */
//     *name_index += (int)(q-p);

//     /* encode the segment. */
//     if(val > 0xFFFF) {
//         tag->encoded_name[*encoded_index] = (uint8_t)0x2A; /* 4-byte segment value. */
//         (*encoded_index)++;

//         tag->encoded_name[*encoded_index] = (uint8_t)0; /* padding. */
//         (*encoded_index)++;

//         tag->encoded_name[*encoded_index] = (uint8_t)val & 0xFF;
//         (*encoded_index)++;
//         tag->encoded_name[*encoded_index] = (uint8_t)((val >> 8) & 0xFF);
//         (*encoded_index)++;
//         tag->encoded_name[*encoded_index] = (uint8_t)((val >> 16) & 0xFF);
//         (*encoded_index)++;
//         tag->encoded_name[*encoded_index] = (uint8_t)((val >> 24) & 0xFF);
//         (*encoded_index)++;

//         pdebug(DEBUG_DETAIL, "Parsed 4-byte numeric segment of value %u.", (uint32_t)val);
//     } else if(val > 0xFF) {
//         tag->encoded_name[*encoded_index] = (uint8_t)0x29; /* 2-byte segment value. */
//         (*encoded_index)++;

//         tag->encoded_name[*encoded_index] = (uint8_t)0; /* padding. */
//         (*encoded_index)++;

//         tag->encoded_name[*encoded_index] = (uint8_t)val & 0xFF;
//         (*encoded_index)++;
//         tag->encoded_name[*encoded_index] = (uint8_t)((val >> 8) & 0xFF);
//         (*encoded_index)++;

//         pdebug(DEBUG_DETAIL, "Parsed 2-byte numeric segment of value %u.", (uint32_t)val);
//     } else {
//         tag->encoded_name[*encoded_index] = (uint8_t)0x28; /* 1-byte segment value. */
//         (*encoded_index)++;

//         tag->encoded_name[*encoded_index] = (uint8_t)val & 0xFF;
//         (*encoded_index)++;

//         pdebug(DEBUG_DETAIL, "Parsed 1-byte numeric segment of value %u.", (uint32_t)val);
//     }

//     pdebug(DEBUG_DETAIL, "Done with name index=%d and encoded name index=%d.", *name_index, *encoded_index);

//     return PLCTAG_STATUS_OK;
// }





