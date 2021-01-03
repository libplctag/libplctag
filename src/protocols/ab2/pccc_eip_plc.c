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




struct local_plc_context_s {
    atomic_int tsn;
};

#define PCCC_CIP_EIP_STACK "PCCC+CIP+EIP"


static int pccc_constructor(plc_p plc, attr attribs);


plc_p pccc_eip_plc_get(attr attribs)
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

    return result;
}


int pccc_eip_plc_get_tsn(plc_p plc, uint16_t *tsn)
{
    struct local_plc_context_s *context = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    *tsn = UINT16_MAX;

    context = plc_get_context(plc);

    if(context) {
        *tsn = (uint16_t)atomic_int_add_mask(&(context->tsn), 1, (unsigned int)0xFFFF);
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


