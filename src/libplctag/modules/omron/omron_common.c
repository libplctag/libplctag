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
#include <float.h>
#include <inttypes.h>
#include <libplctag/api/libplctag.h>
#include <libplctag/lib/conn_attribs.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/error_codes.h>
#include <libplctag/modules/cip/family.h>
#include <libplctag/modules/cip/identity_tag.h>
#include <libplctag/modules/cip/raw_tag.h>
#include <libplctag/modules/cip/services.h>
#include <libplctag/modules/cip/standard_tag.h>
#include <libplctag/modules/cip/tag.h>
#include <libplctag/modules/cip/wire.h>
#include <libplctag/modules/omron/omron_common.h>
#include <libplctag/modules/omron/omron_listing.h>
#include <limits.h>
#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/attr.h>
#include <utils/byteorder.h>
#include <utils/debug.h>
#include <utils/rc.h>
#include <utils/vector.h>


/* the CIP status an NJ/NX refuses an over-large read with. */
#define OMRON_STATUS_REPLY_TOO_LARGE ((uint8_t)0x11)

static int omron_standard_tag_encode_read(cip_tag_p tag, uint8_t *dest, int32_t capacity, tag_op_plan_t *plan);
static int omron_standard_tag_decode_read(cip_tag_p tag, uint8_t *reply, int32_t reply_size);
static int setup_njnx_tag(cip_tag_p tag, attr attribs);
static int setup_njnx_name(cip_tag_p tag, attr attribs);

/*
 * Externally visible global variables
 */

// volatile cip_conn_p conns = NULL;
// volatile mutex_p global_conn_mut = NULL;
//
// volatile vector_p read_group_tags = NULL;


/* request/response handling thread */


/*
 * Generic Rockwell/Allen-Bradley protocol functions.
 *
 * These are the primary entry points into the AB protocol
 * stack.
 */


#define DEFAULT_NUM_RETRIES (5)
#define DEFAULT_RETRY_INTERVAL (300)


/*
 * cip_tag_abort
 *
 * This does the work of stopping any inflight requests.
 * This is not thread-safe.  It must be called from a function
 * that locks the tag's mutex or only from a single thread.
 */


/*
 * cip_tag_status
 *
 * Generic status checker.   May be overridden by individual PLC types.
 */

/*
 * An NJ/NX tag: the CIP special tags, its own tag and UDT listing (omron_listing.c), or a
 * symbolic name.  Packing is off unless the tag string turns it on.
 */
static int setup_njnx_tag(cip_tag_p tag, attr attribs) {
    const char *name = attr_get_str(attribs, "name", NULL);
    int rc = PLCTAG_STATUS_OK;

    tag->vtable = &cip_standard_tag_vtable_omron;
    tag->codec = &cip_standard_tag_codec_omron;
    tag->byte_order = &omron_njnx_tag_byte_order;
    tag->std_ops = &cip_standard_tag_ops_omron;
    tag->debug_module = DEBUG_MODULE_OMRON_STANDARD_TAG;
    tag->allow_packing = (attr_get_int(attribs, "allow_packing", 0) != 0);

    if(str_cmp_i(name, "@identity") == 0) {
        rc = cip_identity_tag_setup(tag, DEBUG_MODULE_OMRON_COMMON);
    } else if(str_cmp_i(name, "@raw") == 0) {
        rc = cip_raw_tag_setup(tag, DEBUG_MODULE_OMRON_RAW_TAG);
    } else if(str_cmp_i(name, "@tags") == 0) {
        rc = omron_setup_listing_tag(tag);
    } else if(str_cmp_i_n(name, "@udt/", str_length("@udt/")) == 0) {
        rc = omron_setup_udt_tag(tag, name);
    } else {
        rc = setup_njnx_name(tag, attribs);
    }

    if(rc != PLCTAG_STATUS_OK) { return rc; }

    return cip_tag_size_from_plc(tag, DEBUG_MODULE_CIP);
}


static int setup_njnx_name(cip_tag_p tag, attr attribs) {
    const char *name = attr_get_str(attribs, "name", NULL);
    int rc = PLCTAG_STATUS_OK;

    rc = cip_tag_parse_elem_type(tag, attribs, DEBUG_MODULE_OMRON_COMMON);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    /* any failure to encode the name is a bad name. */
    rc = cip_encode_tag_name(tag, name);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "Parse of CIP-style tag name %s failed!", name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    return PLCTAG_STATUS_OK;
}


/**
 * @brief Check the status of the request
 *
 * This function checks the request itself and updates the
 * tag if there are any failures or changes that need to be
 * made due to the request status.
 *
 * The tag and the request must not be deleted out from underneath
 * this function.   Both must be held with write mutexes.
 *
 * @return status of the request.
 */


tag_byte_order_t omron_njnx_tag_byte_order = {.is_allocated = 0,

                                              .int16_order = {0, 1},
                                              .int32_order = {0, 1, 2, 3},
                                              .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                              .float32_order = {0, 1, 2, 3},
                                              .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                              .str_is_defined = 1,
                                              .str_is_counted = 1,
                                              .str_is_fixed_length = 0,
                                              .str_is_zero_terminated = 1,
                                              .str_is_byte_swapped = 0,

                                              .write_pad_alignment_bytes = 1,
                                              .str_count_word_bytes = 2,
                                              .str_max_capacity = 0,
                                              .str_total_length = 0,
                                              .str_pad_bytes = 0};


/*
 * What an OMRON NJ/NX can do, for the shared standard-tag engine.
 *
 * There are no fragmented services here: the PLC answers a whole tag or refuses, and it
 * never returns a partial-transfer status.  That is stated as a limit of one packet per
 * transfer, which is what stops the engine asking for a continuation it could not send.
 */
const cip_standard_tag_ops_t cip_standard_tag_ops_omron = {.read_service = CIP_SVC_READ,
                                                           .write_service = CIP_SVC_WRITE,
                                                           .write_service_split = CIP_SVC_WRITE,

                                                           .max_transfer_packets = CIP_TRANSFER_SINGLE_PACKET,

                                                           .sized_segments = true,

                                                           .encode_read_offset = cip_encode_no_offset};


/* the standard (symbolic, named) tag type.  The engine is shared; see modules/cip/standard_tag.c. */
struct tag_vtable_t cip_standard_tag_vtable_omron = {
    .abort = cip_data_tag_abort,
    .read = cip_tag_read_start,
    .status = cip_tag_status,
    .write = cip_tag_write_start,
    .wake_plc = cip_tag_wake_plc,
    .activate = cip_tag_activate,
    .tag_data_written = cip_tag_wake_plc,
    .attribs = cip_tag_attribs,
};

const cip_tag_codec_t cip_standard_tag_codec_omron = {.encode_read = omron_standard_tag_encode_read,
                                                      .decode_read = omron_standard_tag_decode_read,
                                                      .encode_write = cip_standard_tag_encode_write,
                                                      .decode_write = cip_standard_tag_decode_write};


/*
 * Reads of a whole variable too large for one reply.  An NJ/NX refuses the plain read with
 * status 0x11 rather than sending part of it, and the reply does not say how big the variable
 * is.  Get Attributes All on the variable does: its size (4), type (1) and array element type
 * (1).  The tag's elem_count elements from the start are then read in sized segments by the
 * shared engine.
 *
 * ponytail: 0x11 is the CIP code for this refusal; that an NJ/NX sends it, rather than another
 * error, is not confirmed on hardware.
 */
static int omron_standard_tag_encode_read(cip_tag_p tag, uint8_t *dest, int32_t capacity, tag_op_plan_t *plan) {
    if(!tag->size_probe) { return cip_standard_tag_encode_read(tag, dest, capacity, plan); }

    if(1 + tag->encoded_name_size > capacity) { return PLCTAG_ERR_TOO_SMALL; }

    dest[0] = CIP_GET_ATTRIBUTES_ALL;
    mem_copy(dest + 1, tag->encoded_name, tag->encoded_name_size);

    plan->req_size = 1 + tag->encoded_name_size;
    plan->reply_size = 0;
    plan->packable = false;

    return PLCTAG_STATUS_OK;
}


static int omron_standard_tag_decode_read(cip_tag_p tag, uint8_t *reply, int32_t reply_size) {
    cip_header *resp = (cip_header *)reply;
    uint8_t *data = reply + sizeof(cip_header);
    int32_t data_size = reply_size - (int32_t)sizeof(cip_header);
    int64_t total = 0;
    int rc = PLCTAG_STATUS_OK;

    if(!tag->size_probe) {
        /* only a refused plain read asks for the size; a refused segment would ask forever. */
        bool was_plain = !cip_standard_tag_reads_in_segments(tag);

        rc = cip_standard_tag_decode_read(tag, reply, reply_size);

        if(rc == PLCTAG_ERR_TOO_LARGE && was_plain && reply_size >= (int32_t)sizeof(cip_header)
           && resp->status == OMRON_STATUS_REPLY_TOO_LARGE && cip_standard_tag_uses_segments(tag)) {
            pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Too large for one reply, asking for its size.");
            tag->size_probe = true;
            rc = PLCTAG_STATUS_PENDING;
        }

        return rc;
    }

    tag->size_probe = false;

    rc = cip_check_reply(reply, reply_size, CIP_GET_ATTRIBUTES_ALL, false, DEBUG_MODULE_OMRON_STANDARD_TAG, tag->tag_id);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(data_size < 8) { return PLCTAG_ERR_TOO_SMALL; }

    /* the size is one element's for an array and the whole variable's otherwise. */
    total = (int64_t)((uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24))
            * tag->elem_count;

    if(data[4] == CIP_DATA_STRING || data[5] == CIP_DATA_STRING || total <= 0 || total > CIP_MAX_TAG_DATA_SIZE) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id,
               "Variable of %" PRId64 " bytes, type 0x%02x, cannot be read in segments!", total, data[4]);
        return PLCTAG_ERR_TOO_LARGE;
    }

    rc = byte_buffer_set_len(&tag->buf, (int32_t)total);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    tag->elem_size = byte_buffer_len(tag->buf) / tag->elem_count;
    tag->offset = 0;

    /* small enough for one read means the refusal was not about size; asking again would loop. */
    if(!cip_standard_tag_reads_in_segments(tag)) {
        pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_WARN, tag->tag_id, "Read of %d bytes was refused as too large!", byte_buffer_len(tag->buf));
        return PLCTAG_ERR_TOO_LARGE;
    }

    pdebug(DEBUG_MODULE_OMRON_STANDARD_TAG, DEBUG_DETAIL, tag->tag_id, "Variable is %d bytes.", byte_buffer_len(tag->buf));

    return PLCTAG_STATUS_PENDING;
}


#define MAX_CIP_OMRON_MSG_SIZE (0x01FF & 502)
#define MAX_CIP_OMRON_MSG_SIZE_EX (0xFFFF & 1990)

/* the OMRON NJ/NX family; see cip/family.h. */
const cip_family_t omron_family_njnx = {.name = "OMRON NJ/NX",
                                        .canonical_name = "omron-njnx",
                                        .use_connected_msg = 1,
                                        .setup_tag = setup_njnx_tag,
                                        .max_payload_capacity = MAX_CIP_OMRON_MSG_SIZE_EX,
                                        .fo_conn_size = MAX_CIP_OMRON_MSG_SIZE,
                                        .fo_ex_conn_size = MAX_CIP_OMRON_MSG_SIZE_EX,
                                        .only_use_old_forward_open = false,
                                        .min_payload_size = MIN_PAYLOAD_SIZE_CIP,
                                        .connection_debug_module = DEBUG_MODULE_OMRON_CONNECTION};
