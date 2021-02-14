/***************************************************************************
 *   Copyright (C) 2021 by Kyle Hayes                                      *
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
#include <ab2/cip.h>
#include <ab2/cip_layer.h>
#include <ab2/eip_layer.h>
#include <ab2/cip_plc.h>
#include <util/atomic_int.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/mutex.h>
#include <util/plc.h>
#include <util/string.h>


#define RAW_CIP_STACK "RAW/CIP/EIP"

#define CIP_OVERHEAD (64)
#define CIP_PLC_MAX_PAYLOAD (508)

struct cip_plc_context_s {
    /* is there anything here? */
    int dummy;
};


static int cip_constructor(plc_p plc, attr attribs);


plc_p cip_plc_get(attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p result = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    rc = plc_get(RAW_CIP_STACK, attribs, &result, cip_constructor);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to get RAW/CIP/EIP protocol stack, error %s!", plc_tag_decode_error(rc));
        return NULL;
    }

    pdebug(DEBUG_INFO, "Done.");

    return result;
}



int cip_constructor(plc_p plc, attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    struct cip_plc_context_s *context = NULL;
    int max_packet = CIP_STD_PAYLOAD + CIP_OVERHEAD;

    pdebug(DEBUG_INFO, "Starting.");

    /* set up the PLC buffer */
    rc = plc_set_buffer_size(plc, max_packet);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error %s while trying to set CIP PLC buffer size.", plc_tag_decode_error(rc));
        return rc;
    }

    /* set special attribute for the regular CIP payload. */
    rc = attr_set_int(attribs, "cip_payload", attr_get_int(attribs, "cip_payload", CIP_STD_PAYLOAD));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error %s while trying to set max CIP payload size.", plc_tag_decode_error(rc));
        return rc;
    }

    /* allocate and set up our local data. */
    context = mem_alloc(sizeof(*context));
    if(!context) {
        pdebug(DEBUG_WARN, "Unable to allocate local PLC context!");
        return PLCTAG_ERR_NO_MEM;
    }

    /* set the context for this type of PLC. */
    rc = plc_set_context(plc, context, NULL);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to initialize the PLC with the context, error %s!", plc_tag_decode_error(rc));
        mem_free(context);
        return rc;
    }

    /* start building up the layers. */
    rc = plc_set_number_of_layers(plc, 2); /* 2 layers */
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to initialize the PLC with the layer count and context, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* first layer, EIP */
    rc = eip_layer_setup(plc, 0, attribs);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to initialize the EIP layer, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    /* second layer, CIP */
    rc = cip_layer_setup(plc, 1, attribs);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to initialize the CIP layer, error %s!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}



