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
#include <libplctag/modules/cip/identity_tag.h>
#include <libplctag/modules/cip/raw_tag.h>
#include <libplctag/modules/cip/services.h>
#include <libplctag/modules/cip/standard_tag.h>
#include <libplctag/modules/cip/tag.h>
#include <libplctag/modules/cip/wire.h>
#include <libplctag/modules/omron/omron_common.h>
#include <limits.h>
#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/attr.h>
#include <utils/byteorder.h>
#include <utils/debug.h>
#include <utils/rc.h>
#include <utils/vector.h>


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


/* vtables for different kinds of tags */
static struct tag_vtable_t omron_raw_tag_vtable = {
    .abort = (tag_vtable_func)cip_data_tag_abort,
    .read = NULL,
    .status = (tag_vtable_func)cip_tag_status,
    .tickler = (tag_vtable_func)cip_raw_tag_tickler,
    .write = (tag_vtable_func)cip_raw_tag_write_start,
    .wake_plc = NULL,
    .activate = cip_tag_activate,
    .tag_data_written = NULL,
    .encode_read = NULL,
    .decode_read = NULL,
    .encode_write = (tag_encode_func)cip_raw_tag_encode_write,
    .decode_write = (tag_decode_func)cip_raw_tag_decode_write,

    /* attribute accessors */
    .attribs = omron_attribs,
};


/* @identity: the body is shared, see modules/cip/identity_tag.c. */
static struct tag_vtable_t identity_tag_vtable = {
    .abort = (tag_vtable_func)cip_data_tag_abort,
    .read = (tag_vtable_func)cip_identity_tag_read_start,
    .status = (tag_vtable_func)cip_tag_status,
    .tickler = NULL,
    .write = NULL,
    .wake_plc = cip_tag_wake_plc,
    .activate = cip_tag_activate,
    .tag_data_written = cip_tag_wake_plc,
    .encode_read = (tag_encode_func)cip_identity_tag_encode_read,
    .decode_read = (tag_decode_func)cip_identity_tag_decode_read,
    .encode_write = NULL,
    .decode_write = NULL,

    /* attribute accessors */
    .attribs = omron_attribs,
};


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

/* Attribute accessors for the table below.  An accessor returns a status, or for a byte
 * array the number of bytes copied, and never touches tag->status -- the core records it. */

static int32_t omron_get_elem_type(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    *result = (int32_t)(tag->elem_type);

    return PLCTAG_STATUS_OK;
}


static int32_t omron_get_raw_tag_type_bytes_size(plc_tag_p raw_tag) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    return (int32_t)(tag->encoded_type_info_size);
}


static int32_t omron_get_raw_tag_type_bytes_length(plc_tag_p raw_tag, int32_t *result) {
    *result = omron_get_raw_tag_type_bytes_size(raw_tag);

    return PLCTAG_STATUS_OK;
}


static int32_t omron_get_raw_tag_type_bytes(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length) {
    cip_tag_p tag = (cip_tag_p)raw_tag;
    int32_t size = (int32_t)(tag->encoded_type_info_size);

    if(size > buffer_length) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id,
               "Tag type info is larger, %d bytes, than the buffer can hold, %d bytes.", (int)size, (int)buffer_length);
        return PLCTAG_ERR_TOO_SMALL;
    }

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, tag->tag_id, "Copying %d bytes of tag type information.", (int)size);

    mem_copy((void *)buffer, (void *)&(tag->encoded_type_info[0]), (int)size);

    /* the caller gets the number of bytes copied. */
    return size;
}


/* The PLC type as the tag string spells it, so cip_plc_type_t stays private. */
static const char *omron_plc_type_name(cip_plc_type_t plc_type) {
    switch(plc_type) {
        case CIP_PLC_OMRON_NJNX: return "omron-njnx";
        default: return NULL;
    }
}


/* The PLC family name is this module's own, so these two stay here. */
static int32_t omron_get_plc(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    return attr_copy_string(omron_plc_type_name(tag->plc_type), buffer, buffer_length);
}


static int32_t omron_get_plc_size(plc_tag_p raw_tag) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    return attr_string_size(omron_plc_type_name(tag->plc_type));
}


/* The encoded CIP path, not the "18,127.0.0.1" text it was built from. */
const attr_def_t omron_attribs[] = {
    {.name = "elem_size",
     .type = ATTR_TYPE_INT,
     .description = "The size in bytes of a single element of this tag.",
     .get_int = cip_tag_get_elem_size},

    {.name = "elem_count",
     .type = ATTR_TYPE_INT,
     .description = "The number of elements this tag holds.",
     .get_int = cip_tag_get_elem_count},

    {.name = "elem_type",
     .type = ATTR_TYPE_INT,
     .description = "The PLC's type code for a single element of this tag.",
     .get_int = omron_get_elem_type},

    {.name = "connection_status",
     .type = ATTR_TYPE_INT,
     .description = "The state of the connection this tag uses, as a plc_tag_conn_status_t.",
     .get_int = cip_tag_get_connection_status},

    {.name = "connection_inactivity_timeout_ms",
     .type = ATTR_TYPE_INT,
     .description = "Disconnect the connection after this many milliseconds without traffic.",
     .get_int = cip_tag_get_connection_inactivity_timeout_ms,
     .set_int = cip_tag_set_connection_inactivity_timeout_ms},

    {.name = "connection_retry_max_delay_ms",
     .type = ATTR_TYPE_INT,
     .description = "Longest delay between reconnect attempts after an error, in milliseconds.  Shared by every tag on "
                    "this connection.  A change applies from the next retry; a wait already running is not cut short.",
     .get_int = cip_tag_get_connection_retry_max_delay_ms,
     .set_int = cip_tag_set_connection_retry_max_delay_ms},

    {.name = "raw_tag_type_bytes",
     .type = ATTR_TYPE_BYTES,
     .description = "The encoded CIP type information for this tag.",
     .get_bytes = omron_get_raw_tag_type_bytes,
     .get_bytes_size = omron_get_raw_tag_type_bytes_size},

    /* Deprecated: it predates plc_tag_get_attribute_size().  No new byte array gets a length
     * attribute of its own. */
    {.name = "raw_tag_type_bytes.length",
     .type = ATTR_TYPE_INT,
     .description = "Deprecated, use plc_tag_get_attribute_size(). The size of raw_tag_type_bytes in bytes.",
     .get_int = omron_get_raw_tag_type_bytes_length},

    /* Read-only after creation.  Those on the shared connection report PLCTAG_ERR_NOT_FOUND
     * until it exists. */
    {.name = "plc",
     .type = ATTR_TYPE_STRING,
     .description = "The PLC family this tag talks to, as a canonical name.",
     .get_bytes = omron_get_plc,
     .get_bytes_size = omron_get_plc_size},

    {.name = "use_connected_msg",
     .type = ATTR_TYPE_INT,
     .description = "This tag uses CIP connected messaging.",
     .get_int = cip_tag_get_use_connected_msg},

    {.name = "allow_packing",
     .type = ATTR_TYPE_INT,
     .description = "This tag's requests may be packed with others into one CIP request.",
     .get_int = cip_tag_get_allow_packing},

    {.name = "gateway",
     .type = ATTR_TYPE_STRING,
     .description = "The host name or address of the gateway this tag's connection uses.",
     .get_bytes = cip_tag_get_gateway,
     .get_bytes_size = cip_tag_get_gateway_size},

    {.name = "gateway_port",
     .type = ATTR_TYPE_INT,
     .description = "The TCP port this tag's connection uses.",
     .get_int = cip_tag_get_gateway_port},

    {.name = "path",
     .type = ATTR_TYPE_BYTES,
     .description = "The encoded CIP path from the gateway to the PLC.",
     .get_bytes = cip_tag_get_path,
     .get_bytes_size = cip_tag_get_path_size},

    {.name = "conn_only_use_old_forward_open",
     .type = ATTR_TYPE_INT,
     .description = "This tag's connection uses the original Forward Open only, never the large one.",
     .get_int = cip_tag_get_conn_only_use_old_forward_open},

    {.name = NULL},
};


/* an NJ/NX has the CIP special tags, but not the Logix listing services. */
int omron_setup_njnx_name(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs) {
    const char *name = attr_get_str(attribs, "name", NULL);
    int rc = PLCTAG_STATUS_OK;

    (void)entry;

    if(str_cmp_i(name, "@identity") == 0) {
        return cip_identity_tag_setup(tag, &identity_tag_vtable, DEBUG_MODULE_OMRON_COMMON);
    }

    if(str_cmp_i(name, "@raw") == 0) { return cip_raw_tag_setup(tag, &omron_raw_tag_vtable, DEBUG_MODULE_OMRON_RAW_TAG); }

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

                                              .str_pad_to_multiple_bytes = 1,
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

                                                           .encode_read_offset = cip_encode_no_offset};


/* the standard (symbolic, named) tag type.  The engine is shared; see modules/cip/standard_tag.c. */
struct tag_vtable_t cip_standard_tag_vtable_omron = {
    .abort = (tag_vtable_func)cip_data_tag_abort,
    .read = (tag_vtable_func)cip_standard_tag_read_start,
    .status = (tag_vtable_func)cip_tag_status,
    .tickler = NULL,
    .write = (tag_vtable_func)cip_standard_tag_write_start,
    .wake_plc = cip_tag_wake_plc,
    .activate = cip_tag_activate,
    .tag_data_written = cip_tag_wake_plc,
    .encode_read = (tag_encode_func)cip_standard_tag_encode_read,
    .decode_read = (tag_decode_func)cip_standard_tag_decode_read,
    .encode_write = (tag_encode_func)cip_standard_tag_encode_write,
    .decode_write = (tag_decode_func)cip_standard_tag_decode_write,

    /* attribute accessors */
    .attribs = omron_attribs,
};

