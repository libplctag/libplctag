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
#include <ab2/eip_layer.h>
#include <ab2/pccc_cip_eip.h>
#include <util/atomic_int.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/mutex.h>
#include <util/plc.h>
#include <util/string.h>


// struct pccc_cip_eip_s {
//     struct protocol_s protocol;

//     lock_t lock;
//     uint16_t tsn;

//     atomic_int requests_in_flight;

//     /* a reference to the CIP protocol layer. */
//     protocol_p cip;
//     struct pccc_cip_eip_request_s cip_request;

//     /* we might be pointed at a DH+ node. */
//     int dhp_dest_node;
// };


struct local_plc_context_s {
    atomic_int tsn;
};

#define PCCC_CIP_EIP_STACK "PCCC+CIP+EIP"


static int pccc_constructor(plc_p plc, attr attribs);
// static void pccc_rc_destroy(void *plc_arg);

// /* calls to handle requests to this protocol layer. */
// static int new_pccc_request_callback(protocol_p protocol, protocol_request_p request);
// static int cleanup_pccc_request_callback(protocol_p protocol, protocol_request_p request);
// static int build_pccc_request_result_callback(protocol_p protocol, protocol_request_p request, int status, slice_t used_slice, slice_t *new_output_slice);
// static int process_response_result_callback(protocol_p protocol, protocol_request_p request, int status, slice_t used_slice, slice_t *new_input_slice);

// /* calls to handle requests queued for the next protocol layer */
// static int build_cip_request_callback(protocol_p protocol, void *client, slice_t output_buffer, slice_t *used_buffer);
// static int handle_cip_response_callback(protocol_p protocol, void *client, slice_t input_buffer, slice_t *used_buffer);

/* tag name parsing. */
static int parse_pccc_file_type(const char **str, pccc_file_t *file_type);
static int parse_pccc_file_num(const char **str, int *file_num);
static int parse_pccc_elem_num(const char **str, int *elem_num);
static int parse_pccc_subelem_num(const char **str, pccc_file_t file_type, int *subelem_num);


plc_p pccc_cip_eip_plc_get(attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p result = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    rc = plc_get(PCCC_CIP_EIP_STACK, attribs, &result, pccc_constructor);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to get PCCC/CIP/EIP protocol stack, error %s!", plc_tag_decode_error(rc));
        return NULL;
    }

    pdebug(DEBUG_INFO, "Done.");

    return (protocol_p)result;
}


int pccc_cip_eip_get_tsn(plc_p plc, uint16_t *tsn)
{
    struct local_plc_context_s *context = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    *tsn = UINT16_MAX;

    context = plc_get_context(plc);

    if(context) {
        *tsn = atomic_int_add_mask(&(context->tsn), 1, (unsigned int)0xFFFF);
    } else {
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}



int pccc_constructor(plc_p plc, attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    struct local_plc_context_s *context = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    /* allocate and set up our local data. */
    context = mem_alloc(sizeof(*context));
    if(!context) {
        pdebug(DEBUG_WARN, "Unable to allocate local PLC context!");
        return PLCTAG_ERR_NO_MEM;
    }

    atomic_int_set(&(context->tsn), rand() & 0xFFFF);

    /* start building up the layers. */
    rc = plc_init(plc, 1, context, NULL); /* 3 layers */
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to initialize the PLC with the layer count and context, error %s!", plc_tag_decode_error(rc));
        mem_free(context);
        return rc;
    }

    /* first layer, EIP */
    rc = eip_layer_setup(plc, 0, attribs);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to initialize the EIP layer, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



// void pccc_rc_destroy(void *plc_arg)
// {
//     pccc_cip_eip_p plc = (pccc_cip_eip_p)plc_arg;

//     pdebug(DEBUG_INFO, "Starting.");

//     if(!plc) {
//         pdebug(DEBUG_WARN, "Destructor function called with null pointer!");
//         return;
//     }

//     /* destroy PCCC specific features first, then destroy the generic protocol. */

//     /* abort anything we have in flight to the layer below */
//     protocol_stop_request(plc->cip, (protocol_request_p)&(plc->cip_request));

//     /* release our reference on the CIP layer */
//     plc->cip = rc_dec(plc->cip);

//     /* destroy the generic protocol */
//     protocol_cleanup((protocol_p)plc);

//     pdebug(DEBUG_INFO, "Done.");
// }



// /*
//  * Called when a new request is added to the this protocol layer's queue.
//  *
//  * This is called within this protocol layer's request list mutex.
//  *
//  * So it is safe to look at various protocol elements.
//  */
// int new_pccc_request_callback(protocol_p protocol, protocol_request_p pccc_request)
// {
//     int rc = PLCTAG_STATUS_OK;
//     pccc_cip_eip_p plc = (pccc_cip_eip_p)protocol;
//     int old_requests_in_flight = 0;

//     pdebug(DEBUG_DETAIL, "Starting.");

//     old_requests_in_flight = atomic_int_add(&(plc->requests_in_flight), 1);

//     /*
//      * if there were no other requests in flight, we need to
//      * register a new request with the next protocol layer.
//      */
//     if(!old_requests_in_flight) {
//         rc = protocol_request_start(plc->cip, &(plc->cip_request), plc, build_cip_request_callback, handle_cip_response_callback);
//         if(rc != PLCTAG_STATUS_OK) {
//             pdebug(DEBUG_WARN, "Unable to start request with CIP protocol layer, error %s!", plc_tag_decode_error(rc));
//             return rc;
//         }
//     }

//     pdebug(DEBUG_DETAIL, "Done.");

//     return rc;
// }


// /*
//  * Called when a request is removed from the queue.  _Only_
//  * called if the request was in the queue.
//  *
//  * Called within the protocol-specific request mutex.
//  */
// int cleanup_pccc_request_callback(protocol_p protocol, protocol_request_p pccc_request)
// {
//     int rc = PLCTAG_STATUS_OK;
//     pccc_cip_eip_p plc = (pccc_cip_eip_p)protocol;
//     int old_requests_in_flight = 0;

//     pdebug(DEBUG_DETAIL, "Starting.");

//     old_requests_in_flight = atomic_int_add(&(plc->requests_in_flight), -1);

//     /* was that the last one? */
//     if(old_requests_in_flight == 1) {
//         /* abort anything in flight below. */
//         rc = protocol_stop_request(plc->cip, (protocol_request_p)&(plc->cip_request));
//         if(rc != PLCTAG_STATUS_OK) {
//             pdebug(DEBUG_WARN, "Unable to abort CIP layer request, error %s!", plc_tag_decode_error(rc));
//             return rc;
//         }
//     }

//     pdebug(DEBUG_DETAIL, "Done.");

//     return rc;
// }

// #define TRY_SET_BYTE(out, off, val) rc = slice_set_byte(out, off, (uint8_t)(val)); if(rc != PLCTAG_STATUS_OK) break; else (off)++

// #define CIP_PCCC_CMD_EXECUTE ((uint8_t)0x4B)

// int build_cip_request_callback(protocol_p protocol, void *client, slice_t output_buffer, slice_t *used_buffer)
// {
//     int rc = PLCTAG_STATUS_OK;
//     pccc_cip_eip_p plc = (pccc_cip_eip_p)client;
//     int offset = 0;

//     (void)protocol;

//     pdebug(DEBUG_DETAIL, "Starting.");

//     do {
        // /* tell where to send this message. */
        // TRY_SET_BYTE(output_buffer, offset, CIP_PCCC_CMD_EXECUTE);
        // TRY_SET_BYTE(output_buffer, offset, 2); /* path length of path to PCCC object */
        // TRY_SET_BYTE(output_buffer, offset, 0x20); /* class */
        // TRY_SET_BYTE(output_buffer, offset, 0x67); /* PCCC object class */
        // TRY_SET_BYTE(output_buffer, offset, 0x24); /* instance */
        // TRY_SET_BYTE(output_buffer, offset, 0x01); /* instance 1 */

        // /* PCCC vendor and serial number info. */
        // TRY_SET_BYTE(output_buffer, offset, 7);   /* 7 bytes counting this one. */
        // TRY_SET_BYTE(output_buffer, offset, (LIBPLCTAG_VENDOR_ID & 0xFF));
        // TRY_SET_BYTE(output_buffer, offset, ((LIBPLCTAG_VENDOR_ID >> 8) & 0xFF));
        // TRY_SET_BYTE(output_buffer, offset, (LIBPLCTAG_VENDOR_SN & 0xFF));
        // TRY_SET_BYTE(output_buffer, offset, ((LIBPLCTAG_VENDOR_SN >> 8) & 0xFF));
        // TRY_SET_BYTE(output_buffer, offset, ((LIBPLCTAG_VENDOR_SN >> 16) & 0xFF));
        // TRY_SET_BYTE(output_buffer, offset, ((LIBPLCTAG_VENDOR_SN >> 24) & 0xFF));

        /* if we are sending DH+, we need additional fields. */
//         if(plc->dhp_dest_node >= 0) {
//             TRY_SET_BYTE(output_buffer, offset, 0); /* dest link low byte. */
//             TRY_SET_BYTE(output_buffer, offset, 0); /* dest link high byte. */

//             TRY_SET_BYTE(output_buffer, offset, (plc->dhp_dest_node & 0xFF)); /* dest node low byte */
//             TRY_SET_BYTE(output_buffer, offset, ((plc->dhp_dest_node >> 8) & 0xFF)); /* dest node high byte */

//             TRY_SET_BYTE(output_buffer, offset, 0); /* source link low byte. */
//             TRY_SET_BYTE(output_buffer, offset, 0); /* source link high byte. */

//             TRY_SET_BYTE(output_buffer, offset, 0); /* source node low byte. */
//             TRY_SET_BYTE(output_buffer, offset, 0); /* source node high byte. */
//         }

//         rc = protocol_build_request(plc, slice_from_slice(output_buffer, offset, slice_len(output_buffer)), used_buffer, NULL);
//         if(rc == PLCTAG_STATUS_OK) {
//             *used_buffer = slice_from_slice(output_buffer, 0, offset + slice_len(*used_buffer));
//         } else {
//             pdebug(DEBUG_DETAIL, "Error, %s, received while building request packet!", plc_tag_decode_error(rc));
//             *used_buffer = slice_make_err(rc);
//         }
//     } while(0);

//     pdebug(DEBUG_DETAIL, "Done.");

//     return rc;
// }


// int handle_cip_response_callback(protocol_p protocol, void *client, slice_t input_buffer, slice_t *used_buffer)
// {
//     int rc = PLCTAG_STATUS_OK;
//     pccc_cip_eip_p plc = (pccc_cip_eip_p)client;
//     int offset = 0;

//     (void)protocol;

//     pdebug(DEBUG_DETAIL, "Starting.");

//     do {
//         /* if we are sending DH+, we have additional fields. */
//         if(plc->dhp_dest_node >= 0) {
//             /* FIXME - at least check that the node numbers are sane. */
//             offset = 8; /* 8 bytes of destination and source link and node. */
//         }

//         rc = protocol_process_response(plc, slice_from_slice(input_buffer, offset, slice_len(input_buffer)), used_buffer, NULL);
//         if(rc == PLCTAG_STATUS_OK) {
//             *used_buffer = slice_from_slice(input_buffer, 0, offset + slice_len(*used_buffer));
//         } else {
//             pdebug(DEBUG_DETAIL, "Error, %s, received while processing response packet!", plc_tag_decode_error(rc));
//             *used_buffer = slice_make_err(rc);
//         }
//     } while(0);

//     pdebug(DEBUG_DETAIL, "Done.");

//     return rc;
// }



int pccc_parse_logical_address(const char *name, pccc_file_t *file_type, int *file_num, int *elem_num, int *subelem_num)
{
    int rc = PLCTAG_STATUS_OK;
    const char *p = name;

    pdebug(DEBUG_DETAIL, "Starting.");

    do {
        if((rc = parse_pccc_file_type(&p, file_type)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for data-table type! Error %s!", plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_pccc_file_num(&p, file_num)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for file number! Error %s!", plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_pccc_elem_num(&p, elem_num)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for element number! Error %s!", plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_pccc_subelem_num(&p, *file_type, subelem_num)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for subelement number! Error %s!", plc_tag_decode_error(rc));
            break;
        }
    } while(0);

    pdebug(DEBUG_DETAIL, "Starting.");

    return rc;
}




int parse_pccc_file_type(const char **str, pccc_file_t *file_type)
{
    int rc = PLCTAG_STATUS_OK;

    switch((*str)[0]) {
    case 'A':
    case 'a': /* ASCII */
        *file_type = PCCC_FILE_ASCII;
        (*str)++;
        break;

    case 'B':
    case 'b': /* Bit or block transfer */
        if(isdigit((*str)[1])) {
            /* Bit */
            *file_type = PCCC_FILE_BIT;
            (*str)++;
            break;
        } else {
            if((*str)[1] == 'T' || (*str)[1] == 't') {
                /* block transfer */
                *file_type = PCCC_FILE_BLOCK_TRANSFER;
                (*str) += 2;  /* skip past both characters */
            } else {
                *file_type = PCCC_FILE_UNKNOWN;
                pdebug(DEBUG_WARN, "Bad format or unsupported logical address, expected B or BT!");
                rc = PLCTAG_ERR_BAD_PARAM;
            }
        }

        break;

    case 'C':
    case 'c': /* Counter */
        *file_type = PCCC_FILE_COUNTER;
        (*str)++;
        break;

    case 'D':
    case 'd': /* BCD number */
        *file_type = PCCC_FILE_BCD;
        (*str)++;
        break;

    case 'F':
    case 'f': /* Floating point Number */
        *file_type = PCCC_FILE_FLOAT;
        (*str)++;
        break;

    case 'I':
    case 'i': /* Input */
        *file_type = PCCC_FILE_INPUT;
        (*str)++;
        break;

    case 'L':
    case 'l':
        *file_type = PCCC_FILE_LONG_INT;
        (*str)++;
        break;

    case 'M':
    case 'm': /* Message */
        if((*str)[1] == 'G' || (*str)[1] == 'g') {
            *file_type = PCCC_FILE_MESSAGE;
            (*str) += 2;  /* skip past both characters */
        } else {
            *file_type = PCCC_FILE_UNKNOWN;
            pdebug(DEBUG_WARN, "Bad format or unsupported logical address, expected MG!");
            rc = PLCTAG_ERR_BAD_PARAM;
        }

        break;

    case 'N':
    case 'n': /* INT */
        *file_type = PCCC_FILE_INT;
        (*str)++;
        break;

    case 'O':
    case 'o': /* Output */
        *file_type = PCCC_FILE_OUTPUT;
        (*str)++;
        break;

    case 'P':
    case 'p': /* PID */
        if((*str)[1] == 'D' || (*str)[1] == 'd') {
            *file_type = PCCC_FILE_PID;
            (*str) += 2;  /* skip past both characters */
        } else {
            *file_type = PCCC_FILE_UNKNOWN;
            pdebug(DEBUG_WARN, "Bad format or unsupported logical address, expected PD!");
            rc = PLCTAG_ERR_BAD_PARAM;
        }

        break;

    case 'R':
    case 'r': /* Control */
        *file_type = PCCC_FILE_CONTROL;
        (*str)++;
        break;

    case 'S':
    case 's': /* Status, SFC or String */
        if(isdigit((*str)[1])) {
            /* Status */
            *file_type = PCCC_FILE_STATUS;
            (*str)++;
            break;
        } else {
            if((*str)[1] == 'C' || (*str)[1] == 'c') {
                /* SFC */
                *file_type = PCCC_FILE_SFC;
                (*str) += 2;  /* skip past both characters */
            } else if((*str)[1] == 'T' || (*str)[1] == 't') {
                /* String */
                *file_type = PCCC_FILE_STRING;
                (*str) += 2;  /* skip past both characters */
            } else {
                *file_type = PCCC_FILE_UNKNOWN;
                pdebug(DEBUG_WARN, "Bad format or unsupported logical address, expected string, SFC or status!");
                rc = PLCTAG_ERR_BAD_PARAM;
            }
        }

        break;

    case 'T':
    case 't': /* Timer */
        *file_type = PCCC_FILE_TIMER;
        (*str)++;
        break;

    default:
        pdebug(DEBUG_WARN, "Bad format or unsupported logical address %s!", *str);
        *file_type = PCCC_FILE_UNKNOWN;
        rc = PLCTAG_ERR_BAD_PARAM;
        break;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



int parse_pccc_file_num(const char **str, int *file_num)
{
    int tmp = 0;

    pdebug(DEBUG_DETAIL,"Starting.");

    if(!str || !*str || !isdigit(**str)) {
        pdebug(DEBUG_WARN,"Expected data-table file number!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    while(**str && isdigit(**str) && tmp < 65535) {
        tmp *= 10;
        tmp += (int)((**str) - '0');
        (*str)++;
    }

    *file_num = tmp;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}



int parse_pccc_elem_num(const char **str, int *elem_num)
{
    int tmp = 0;

    pdebug(DEBUG_DETAIL,"Starting.");

    if(!str || !*str || **str != ':') {
        pdebug(DEBUG_WARN,"Expected data-table element number!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* step past the : character */
    (*str)++;

    while(**str && isdigit(**str) && tmp < 65535) {
        tmp *= 10;
        tmp += (int)((**str) - '0');
        (*str)++;
    }

    *elem_num = tmp;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}



int parse_pccc_subelem_num(const char **str, pccc_file_t file_type, int *subelem_num)
{
    int tmp = 0;

    pdebug(DEBUG_DETAIL,"Starting.");

    if(!str || !*str) {
        pdebug(DEBUG_WARN,"Called with bad string pointer!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * if we have a null character we are at the end of the name
     * and the subelement is not there.  That is not an error.
     */

    if( (**str) == 0) {
        pdebug(DEBUG_DETAIL, "No subelement in this name.");
        *subelem_num = -1;
        return PLCTAG_STATUS_OK;
    }

    /*
     * We do have a character.  It must be . or / to be valid.
     * The . character is valid before a mnemonic for a field in a structured type.
     * The / character is valid before a bit number.
     */

    /* make sure the next character is either / or . and nothing else. */
    if((**str) != '/' && (**str) != '.') {
        pdebug(DEBUG_WARN, "Bad subelement field in logical address.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if((**str) == '/') {
        /* bit number. */

        /* step past the / character */
        (*str)++;

        /* FIXME - we do this a lot, should be a small routine. */
        while(**str && isdigit(**str) && tmp < 65535) {
            tmp *= 10;
            tmp += (int)((**str) - '0');
            (*str)++;
        }

        *subelem_num = tmp;

        pdebug(DEBUG_DETAIL, "Done.");

        return PLCTAG_STATUS_OK;
    } else {
        /* mnemonic. */

        /* step past the . character */
        (*str)++;

        /* this depends on the data-table file type. */
        switch(file_type) {
        case PCCC_FILE_BLOCK_TRANSFER:
            if(str_cmp_i(*str,"con") == 0) {
                *subelem_num = 0;
            } else if(str_cmp_i(*str,"rlen") == 0) {
                *subelem_num = 1;
            } else if(str_cmp_i(*str,"dlen") == 0) {
                *subelem_num = 2;
            } else if(str_cmp_i(*str,"df") == 0) {
                *subelem_num = 3;
            } else if(str_cmp_i(*str,"elem") == 0) {
                *subelem_num = 4;
            } else if(str_cmp_i(*str,"rgs") == 0) {
                *subelem_num = 5;
            } else {
                pdebug(DEBUG_WARN,"Unsupported block transfer mnemonic!");
                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        case PCCC_FILE_COUNTER:
        case PCCC_FILE_TIMER:
            if(str_cmp_i(*str,"con") == 0) {
                *subelem_num = 0;
            } else if(str_cmp_i(*str,"pre") == 0) {
                *subelem_num = 1;
            } else if(str_cmp_i(*str,"acc") == 0) {
                *subelem_num = 2;
            } else {
                pdebug(DEBUG_WARN,"Unsupported %s mnemonic!", (file_type == PCCC_FILE_COUNTER ? "counter" : "timer"));
                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        case PCCC_FILE_CONTROL:
            if(str_cmp_i(*str,"con") == 0) {
                *subelem_num = 0;
            } else if(str_cmp_i(*str,"len") == 0) {
                *subelem_num = 1;
            } else if(str_cmp_i(*str,"pos") == 0) {
                *subelem_num = 2;
            } else {
                pdebug(DEBUG_WARN,"Unsupported control mnemonic!");
                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        case PCCC_FILE_PID:
            if(str_cmp_i(*str,"con") == 0) {
                *subelem_num = 0;
            } else if(str_cmp_i(*str,"sp") == 0) {
                *subelem_num = 2;
            } else if(str_cmp_i(*str,"kp") == 0) {
                *subelem_num = 4;
            } else if(str_cmp_i(*str,"ki") == 0) {
                *subelem_num = 6;
            } else if(str_cmp_i(*str,"kd") == 0) {
                *subelem_num = 8;
            } else if(str_cmp_i(*str,"pv") == 0) {
                *subelem_num = 26;
            } else {
                pdebug(DEBUG_WARN,"Unsupported PID mnemonic!");
                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        case PCCC_FILE_MESSAGE:
            if(str_cmp_i(*str,"con") == 0) {
                *subelem_num = 0;
            } else if(str_cmp_i(*str,"err") == 0) {
                *subelem_num = 1;
            } else if(str_cmp_i(*str,"rlen") == 0) {
                *subelem_num = 2;
            } else if(str_cmp_i(*str,"dlen") == 0) {
                *subelem_num = 3;
            } else {
                pdebug(DEBUG_WARN,"Unsupported message mnemonic!");
                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        case PCCC_FILE_STRING:
            if(str_cmp_i(*str,"len") == 0) {
                *subelem_num = 0;
            } else if(str_cmp_i(*str,"data") == 0) {
                *subelem_num = 1;
            } else {
                pdebug(DEBUG_WARN,"Unsupported string mnemonic!");
                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        default:
            pdebug(DEBUG_WARN, "Unsupported mnemonic %s!", *str);
            return PLCTAG_ERR_BAD_PARAM;
            break;
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}



const char *pccc_decode_error(uint8_t *data, int capacity)
{
    unsigned int error = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    error = data[0];

    /* extended error? */
    if(error == 0xF0) {
        if(capacity < 5) {
            pdebug(DEBUG_WARN, "Unable to get additional error status, no capacity!");
            return "Error translating PCCC error!";
        }

        error = (unsigned int)data[3] | (data[4] << 8);
    }

    switch(error) {
    case 1:
        return "Error converting block address.";
        break;

    case 2:
        return "Less levels specified in address than minimum for any address.";
        break;

    case 3:
        return "More levels specified in address than system supports";
        break;

    case 4:
        return "Symbol not found.";
        break;

    case 5:
        return "Symbol is of improper format.";
        break;

    case 6:
        return "Address doesn't point to something usable.";
        break;

    case 7:
        return "File is wrong size.";
        break;

    case 8:
        return "Cannot complete request, situation has changed since the start of the command.";
        break;

    case 9:
        return "File is too large.";
        break;

    case 0x0A:
        return "Transaction size plus word address is too large.";
        break;

    case 0x0B:
        return "Access denied, improper privilege.";
        break;

    case 0x0C:
        return "Condition cannot be generated - resource is not available (some has upload active)";
        break;

    case 0x0D:
        return "Condition already exists - resource is already available.";
        break;

    case 0x0E:
        return "Command could not be executed PCCC decode error.";
        break;

    case 0x0F:
        return "Requester does not have upload or download access - no privilege.";
        break;

    case 0x10:
        return "Illegal command or format.";
        break;

    case 0x20:
        return "Host has a problem and will not communicate.";
        break;

    case 0x30:
        return "Remote node host is missing, disconnected, or shut down.";
        break;

    case 0x40:
        return "Host could not complete function due to hardware fault.";
        break;

    case 0x50:
        return "Addressing problem or memory protect rungs.";
        break;

    case 0x60:
        return "Function not allowed due to command protection selection.";
        break;

    case 0x70:
        return "Processor is in Program mode.";
        break;

    case 0x80:
        return "Compatibility mode file missing or communication zone problem.";
        break;

    case 0x90:
        return "Remote node cannot buffer command.";
        break;

    case 0xA0:
        return "Wait ACK (1775-KA buffer full).";
        break;

    case 0xB0:
        return "Remote node problem due to download.";
        break;

    case 0xC0:
        return "Wait ACK (1775-KA buffer full).";  /* why is this duplicate? */
        break;

    default:
        return "Unknown error response.";
        break;
    }


    return "Unknown error response.";
}

