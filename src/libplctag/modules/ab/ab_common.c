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
#include <libplctag/modules/ab/ab_common.h>
#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/cip/family.h>
#include <libplctag/modules/ab/eip_lgx_pccc.h>
#include <libplctag/modules/ab/eip_plc5_pccc.h>
#include <libplctag/modules/ab/eip_slc_pccc.h>
#include <libplctag/modules/ab/pccc.h>
#include <libplctag/modules/cip/path.h>
#include <libplctag/modules/cip/identity_tag.h>
#include <libplctag/modules/cip/raw_tag.h>
#include <libplctag/modules/cip/services.h>
#include <libplctag/modules/cip/standard_tag.h>
#include <libplctag/modules/cip/tag.h>
#include <libplctag/modules/cip/wire.h>
#include <limits.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/byteorder.h>
#include <utils/debug.h>
#include <utils/rc.h>
#include <utils/vector.h>

/*
 * Externally visible global variables
 */

// volatile cip_conn_p sessions = NULL;
// volatile mutex_p global_session_mut = NULL;
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


/* forward declarations*/
static int setup_special_tag(cip_tag_p tag, const char *name, bool *is_special);
static int setup_pccc_name(cip_tag_p tag, attr attribs, int (*encode)(uint8_t *, int *, int, pccc_addr_t *));
static void setup_ab_defaults(cip_tag_p tag, tag_vtable_p vtable, tag_byte_order_t *byte_order);
static int setup_pccc_size(cip_tag_p tag, attr attribs);
static int setup_plc5_tag(cip_tag_p tag, attr attribs);
static int setup_slc_tag(cip_tag_p tag, attr attribs);
static int setup_lgx_pccc_tag(cip_tag_p tag, attr attribs);
static int setup_symbolic_name(cip_tag_p tag, attr attribs);
static int setup_logix_tag(cip_tag_p tag, attr attribs);
static int setup_micro800_tag(cip_tag_p tag, attr attribs);
static int setup_generic_tag(cip_tag_p tag, attr attribs);


/* vtables for different kinds of tags */
static struct tag_vtable_t raw_tag_vtable = {
    .abort = (tag_vtable_func)cip_data_tag_abort,
    .read = NULL,
    .status = (tag_vtable_func)cip_tag_status,
    .tickler = (tag_vtable_func)cip_raw_tag_tickler,
    .write = cip_tag_write_start,
    .wake_plc = cip_tag_wake_plc, /* plc_tag_write() queues the tag with it */
    .activate = cip_tag_activate,
    .tag_data_written = NULL,
    .encode_read = NULL,
    .decode_read = NULL,
    .encode_write = (tag_encode_func)cip_raw_tag_encode_write,
    .decode_write = (tag_decode_func)cip_raw_tag_decode_write,

    /* attribute accessors */
    .attribs = ab_attribs,
};


/* @identity: the body is shared, see modules/cip/identity_tag.c. */
static struct tag_vtable_t identity_tag_vtable = {
    .abort = (tag_vtable_func)cip_data_tag_abort,
    .read = cip_tag_read_start,
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
    .attribs = ab_attribs,
};


/*
 * Public functions.
 */


/*
 * The special tags every Rockwell family shares.  A DH+ node is not CIP, so the bridge would
 * answer an @identity for it; that is refused rather than passed off as the PLC's.
 */
static int setup_special_tag(cip_tag_p tag, const char *name, bool *is_special) {
    bool is_logix = (tag->plc_type == CIP_PLC_LGX || tag->plc_type == CIP_PLC_MICRO800);

    *is_special = true;

    if(str_cmp_i(name, "@identity") == 0) {
        if(tag->session->family->via_dhp) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "@identity is not supported on %s!",
                   tag->session->family->name);
            return PLCTAG_ERR_UNSUPPORTED;
        }

        return cip_identity_tag_setup(tag, &identity_tag_vtable, DEBUG_MODULE_AB_EIP_CIP_SPECIAL);
    }

    /* the rest are Logix services.  A listing may be program scoped, "Program:X.@tags". */
    if(is_logix && str_cmp_i(name, "@raw") == 0) {
        return cip_raw_tag_setup(tag, &raw_tag_vtable, DEBUG_MODULE_AB_EIP_CIP_SPECIAL);
    }

    if(is_logix && str_str_cmp_i(name, "@tags")) { return setup_tag_listing_tag(tag, name); }

    if(is_logix && str_str_cmp_i(name, "@udt/")) { return setup_udt_tag(tag, name); }

    *is_special = false;

    return PLCTAG_STATUS_OK;
}


/* the type and the size are in the file letter. */
static int setup_pccc_name(cip_tag_p tag, attr attribs, int (*encode)(uint8_t *, int *, int, pccc_addr_t *)) {
    const char *name = attr_get_str(attribs, "name", NULL);
    pccc_addr_t addr = {0};
    bool is_special = false;
    int rc = setup_special_tag(tag, name, &is_special);

    if(rc != PLCTAG_STATUS_OK || is_special) { return rc; }

    rc = parse_pccc_logical_address(name, &addr);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Unable to parse data file %s!", name);
        return rc;
    }

    /* with no path and no DH+ the request goes to the PLC at the gateway itself. */
    tag->unrouted = (tag->plc_type != CIP_PLC_LGX_PCCC && !tag->session->is_dhp && tag->session->conn_path_size == 0);

    tag->elem_size = addr.element_size_bytes;
    tag->file_type = addr.file_type;

    if(addr.is_bit) {
        tag->is_bit = 1;
        tag->bit = (int)(unsigned int)addr.bit;
    }

    rc = encode(tag->encoded_name, &(tag->encoded_name_size), MAX_TAG_NAME, &addr);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Encoding of PCCC tag name %s failed!", name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    return PLCTAG_STATUS_OK;
}


/* what every Rockwell tag starts with. */
static void setup_ab_defaults(cip_tag_p tag, tag_vtable_p vtable, tag_byte_order_t *byte_order) {
    tag->protocol_type = TAG_PROTOCOL_AB;
    if(vtable) { tag->vtable = vtable; }
    tag->byte_order = byte_order;
    tag->std_ops = &cip_standard_tag_ops_ab;
    tag->debug_module = DEBUG_MODULE_AB_EIP_CIP;
    tag->allow_packing = false;
}


/* a PCCC data file states its size; a special tag learns its size from the reply. */
static int setup_pccc_size(cip_tag_p tag, attr attribs) {
    if(tag->special_tag) { return cip_tag_size_from_plc(tag, DEBUG_MODULE_CIP); }

    return cip_tag_size_from_attributes(tag, attribs, DEBUG_MODULE_CIP);
}


static int setup_plc5_tag(cip_tag_p tag, attr attribs) {
    int rc = PLCTAG_STATUS_OK;

    setup_ab_defaults(tag, &pccc_vtable, &plc5_tag_byte_order);

    rc = setup_pccc_name(tag, attribs, plc5_encode_address);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    return setup_pccc_size(tag, attribs);
}


/* SLC 500 and MicroLogix. */
static int setup_slc_tag(cip_tag_p tag, attr attribs) {
    int rc = PLCTAG_STATUS_OK;

    setup_ab_defaults(tag, &pccc_vtable, &slc_tag_byte_order);

    rc = setup_pccc_name(tag, attribs, slc_encode_address);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    return setup_pccc_size(tag, attribs);
}


/* a Logix through its PCCC mapping: SLC byte order, PLC/5 addressing. */
static int setup_lgx_pccc_tag(cip_tag_p tag, attr attribs) {
    int rc = PLCTAG_STATUS_OK;

    setup_ab_defaults(tag, &lgx_pccc_vtable, &slc_tag_byte_order);

    rc = setup_pccc_name(tag, attribs, plc5_encode_address);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    return setup_pccc_size(tag, attribs);
}


/* the symbolic name of a Logix or Micro800 tag, or one of their special tags. */
static int setup_symbolic_name(cip_tag_p tag, attr attribs) {
    const char *name = attr_get_str(attribs, "name", NULL);
    bool is_special = false;
    int rc = setup_special_tag(tag, name, &is_special);

    if(rc != PLCTAG_STATUS_OK || is_special) { return rc; }

    rc = cip_tag_parse_elem_type(tag, attribs, DEBUG_MODULE_AB_COMMON);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    /* any failure to encode the name is a bad name. */
    rc = cip_encode_tag_name(tag, name);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Parse of CIP-style tag name %s failed!", name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    return PLCTAG_STATUS_OK;
}


/* ControlLogix and CompactLogix: packing on unless the tag string turns it off. */
static int setup_logix_tag(cip_tag_p tag, attr attribs) {
    int rc = PLCTAG_STATUS_OK;

    setup_ab_defaults(tag, &cip_standard_tag_vtable_ab, &logix_tag_byte_order);
    tag->allow_packing = (attr_get_int(attribs, "allow_packing", 1) != 0);

    rc = setup_symbolic_name(tag, attribs);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    return cip_tag_size_from_plc(tag, DEBUG_MODULE_CIP);
}


static int setup_micro800_tag(cip_tag_p tag, attr attribs) {
    int rc = PLCTAG_STATUS_OK;

    setup_ab_defaults(tag, &cip_standard_tag_vtable_ab, &cip_short_string_byte_order);

    rc = setup_symbolic_name(tag, attribs);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    return cip_tag_size_from_plc(tag, DEBUG_MODULE_CIP);
}


/* a generic device has only the special tags, so no data-tag vtable. */
static int setup_generic_tag(cip_tag_p tag, attr attribs) {
    const char *name = attr_get_str(attribs, "name", NULL);
    bool is_special = false;
    int rc = PLCTAG_STATUS_OK;

    setup_ab_defaults(tag, NULL, &logix_tag_byte_order);

    rc = setup_special_tag(tag, name, &is_special);
    if(rc == PLCTAG_STATUS_OK && !is_special) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "A %s device has only special tags, got: %s",
               tag->session->family->name, name ? name : "");
        rc = PLCTAG_ERR_UNSUPPORTED;
    }

    if(rc != PLCTAG_STATUS_OK) { return rc; }

    return cip_tag_size_from_plc(tag, DEBUG_MODULE_CIP);
}


/*
 * The Rockwell families.  Three of them -- PLC/5, SLC and MicroLogix -- can also be reached
 * across a DH+ bridge, which needs a connection where the direct route does not.
 *
 * Micro800 supports the Extended Forward Open and asks for the same size *Logix does: the
 * only published figure for anything larger traces back to a single forum post and no
 * Rockwell document states one, while 4002 is measured -- 4082 comes back as an invalid
 * connection size and 4002 succeeds.  Asking higher costs a failed round trip on every
 * connection and sizes the per-connection buffer to match.  Raise it if a Micro800 is
 * measured accepting more.  Generic CIP takes the *Logix sizing too.
 */

/* maximum for PCCC embedded within CIP. */
#define MAX_CIP_PCCC_MSG_SIZE (244)

#define MAX_CIP_LGX_MSG_SIZE (0x01FF & 504)
#define MAX_CIP_LGX_MSG_SIZE_EX (0xFFFF & 4000)

/*
 * Smallest PCCC connection payload we can actually use.  A ForwardOpen error can offer a
 * smaller size than we asked for and we accept it, but below the fixed per-request overhead
 * there is no room left for a request.  PCCC needs about 92 bytes for its command header and
 * addressing; MIN_PAYLOAD_SIZE_CIP is the CIP equivalent.
 */
#define MIN_PAYLOAD_SIZE_PCCC (92)

#define PCCC_CONN                                                                                                       \
    .max_payload_capacity = MAX_CIP_PCCC_MSG_SIZE, .fo_conn_size = MAX_CIP_PCCC_MSG_SIZE, .fo_ex_conn_size = 0,         \
    .only_use_old_forward_open = true, .min_payload_size = MIN_PAYLOAD_SIZE_PCCC

#define LGX_CONN                                                                                                        \
    .max_payload_capacity = MAX_CIP_LGX_MSG_SIZE_EX, .fo_conn_size = MAX_CIP_LGX_MSG_SIZE,                              \
    .fo_ex_conn_size = MAX_CIP_LGX_MSG_SIZE_EX, .only_use_old_forward_open = false, .min_payload_size = MIN_PAYLOAD_SIZE_CIP

static const cip_family_t ab_family_plc5_dhp = {.name = "PLC/5 via DH+", .plc_type = CIP_PLC_PLC5, .via_dhp = true,
                                         .use_connected_msg = 1, .setup_tag = setup_plc5_tag, PCCC_CONN};
const cip_family_t ab_family_plc5 = {.name = "PLC/5", .plc_type = CIP_PLC_PLC5, .dhp = &ab_family_plc5_dhp,
                                     .use_connected_msg = 0, .setup_tag = setup_plc5_tag, PCCC_CONN};
static const cip_family_t ab_family_slc_dhp = {.name = "SLC 500 via DH+", .plc_type = CIP_PLC_SLC, .via_dhp = true,
                                        .use_connected_msg = 1, .setup_tag = setup_slc_tag, PCCC_CONN};
const cip_family_t ab_family_slc = {.name = "SLC 500", .plc_type = CIP_PLC_SLC, .dhp = &ab_family_slc_dhp,
                                    .use_connected_msg = 0, .setup_tag = setup_slc_tag, PCCC_CONN};
static const cip_family_t ab_family_mlgx_dhp = {.name = "MicroLogix via DH+", .plc_type = CIP_PLC_MLGX, .via_dhp = true,
                                         .use_connected_msg = 1, .setup_tag = setup_slc_tag, PCCC_CONN};
const cip_family_t ab_family_mlgx = {.name = "MicroLogix", .plc_type = CIP_PLC_MLGX, .dhp = &ab_family_mlgx_dhp,
                                     .use_connected_msg = 0, .setup_tag = setup_slc_tag, PCCC_CONN};
const cip_family_t ab_family_lgx_pccc = {.name = "PCCC-mapped Logix", .plc_type = CIP_PLC_LGX_PCCC, .use_connected_msg = 0,
                                         .setup_tag = setup_lgx_pccc_tag, PCCC_CONN};
const cip_family_t ab_family_logix = {.name = "Logix", .plc_type = CIP_PLC_LGX, .use_connected_msg = -1,
                                      .setup_tag = setup_logix_tag, LGX_CONN};
const cip_family_t ab_family_micro800 = {.name = "Micro800", .plc_type = CIP_PLC_MICRO800, .use_connected_msg = 1,
                                         .setup_tag = setup_micro800_tag, LGX_CONN};
const cip_family_t ab_family_generic = {.name = "generic CIP", .plc_type = CIP_PLC_GENERIC, .use_connected_msg = 0,
                                        .setup_tag = setup_generic_tag, LGX_CONN};


/*
 * cip_tag_status
 *
 * Generic status checker.   May be overridden by individual PLC types.
 */

/* Attribute accessors for the table below.  An accessor returns a status, or for a byte
 * array the number of bytes copied, and never touches tag->status -- the core records it. */

static int32_t ab_get_elem_type(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    switch(tag->plc_type) {
        case CIP_PLC_PLC5: /* fall through */
        case CIP_PLC_MLGX: /* fall through */
        case CIP_PLC_SLC:  /* fall through */
        case CIP_PLC_LGX_PCCC: *result = (int32_t)(tag->file_type); break;

        case CIP_PLC_LGX:      /* fall through */
        case CIP_PLC_MICRO800: *result = (int32_t)(tag->elem_type); break;

        default:
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Unsupported PLC type %d!", tag->plc_type);
            return PLCTAG_ERR_UNSUPPORTED;
    }

    return PLCTAG_STATUS_OK;
}


/* Only the PLC types that encode CIP type information carry this. */
static int32_t ab_get_raw_tag_type_bytes_size(plc_tag_p raw_tag) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    switch(tag->plc_type) {
        case CIP_PLC_LGX:      /* fall through */
        case CIP_PLC_MICRO800: return (int32_t)(tag->encoded_type_info_size);

        default:
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Unsupported PLC type %d!", tag->plc_type);
            return PLCTAG_ERR_UNSUPPORTED;
    }
}


static int32_t ab_get_raw_tag_type_bytes_length(plc_tag_p raw_tag, int32_t *result) {
    int32_t size = ab_get_raw_tag_type_bytes_size(raw_tag);

    if(size < 0) { return size; }

    *result = size;

    return PLCTAG_STATUS_OK;
}


static int32_t ab_get_raw_tag_type_bytes(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length) {
    cip_tag_p tag = (cip_tag_p)raw_tag;
    int32_t size = ab_get_raw_tag_type_bytes_size(raw_tag);

    if(size < 0) { return size; }

    if(size > buffer_length) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id,
               "Tag type info is larger, %d bytes, than the buffer can hold, %d bytes.", (int)size, (int)buffer_length);
        return PLCTAG_ERR_TOO_SMALL;
    }

    mem_copy((void *)buffer, (void *)&(tag->encoded_type_info[0]), (int)size);

    /* the caller gets the number of bytes copied. */
    return size;
}


/* The PLC type as the tag string spells it, so cip_plc_type_t stays private. */
static const char *ab_plc_type_name(cip_plc_type_t plc_type) {
    switch(plc_type) {
        case CIP_PLC_PLC5: return "plc5";
        case CIP_PLC_SLC: return "slc500";
        case CIP_PLC_MLGX: return "micrologix";
        case CIP_PLC_LGX: return "ControlLogix";
        case CIP_PLC_LGX_PCCC: return "logix-pccc";
        case CIP_PLC_MICRO800: return "micro800";
        case CIP_PLC_OMRON_NJNX: return "omron-njnx";
        case CIP_PLC_GENERIC: return "generic";
        default: return NULL;
    }
}


/* The PLC family name is this module's own, so these two stay here. */
static int32_t ab_get_plc(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    return attr_copy_string(ab_plc_type_name(tag->plc_type), buffer, buffer_length);
}


static int32_t ab_get_plc_size(plc_tag_p raw_tag) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    return attr_string_size(ab_plc_type_name(tag->plc_type));
}


/* The encoded CIP path, not the "1,0" text it was built from. */
const attr_def_t ab_attribs[] = {
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
     .get_int = ab_get_elem_type},

    {.name = "connection_status",
     .type = ATTR_TYPE_INT,
     .description = "The state of the session or connection this tag uses, as a plc_tag_conn_status_t.",
     .get_int = cip_tag_get_connection_status},

    {.name = "connection_inactivity_timeout_ms",
     .type = ATTR_TYPE_INT,
     .description = "Disconnect the session after this many milliseconds without traffic.",
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
     .get_bytes = ab_get_raw_tag_type_bytes,
     .get_bytes_size = ab_get_raw_tag_type_bytes_size},

    /* Deprecated: it predates plc_tag_get_attribute_size().  No new byte array gets a length
     * attribute of its own. */
    {.name = "raw_tag_type_bytes.length",
     .type = ATTR_TYPE_INT,
     .description = "Deprecated, use plc_tag_get_attribute_size(). The size of raw_tag_type_bytes in bytes.",
     .get_int = ab_get_raw_tag_type_bytes_length},

    /* Read-only after creation.  Those on the shared session report PLCTAG_ERR_NOT_FOUND
     * until it exists. */
    {.name = "plc",
     .type = ATTR_TYPE_STRING,
     .description = "The PLC family this tag talks to, as a canonical name.",
     .get_bytes = ab_get_plc,
     .get_bytes_size = ab_get_plc_size},

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
     .description = "The host name or address of the gateway this tag's session connects to.",
     .get_bytes = cip_tag_get_gateway,
     .get_bytes_size = cip_tag_get_gateway_size},

    {.name = "gateway_port",
     .type = ATTR_TYPE_INT,
     .description = "The TCP port this tag's session connects to.",
     .get_int = cip_tag_get_gateway_port},

    {.name = "path",
     .type = ATTR_TYPE_BYTES,
     .description = "The encoded CIP path from the gateway to the PLC.",
     .get_bytes = cip_tag_get_path,
     .get_bytes_size = cip_tag_get_path_size},

    {.name = "conn_only_use_old_forward_open",
     .type = ATTR_TYPE_INT,
     .description = "This tag's session uses the original Forward Open only, never the large one.",
     .get_int = cip_tag_get_conn_only_use_old_forward_open},

    {.name = NULL},
};


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


/* default string types used for ControlLogix-class PLCs. */
tag_byte_order_t logix_tag_byte_order = {.is_allocated = 0,

                                         .int16_order = {0, 1},
                                         .int32_order = {0, 1, 2, 3},
                                         .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                         .float32_order = {0, 1, 2, 3},
                                         .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                         .str_is_defined = 1,
                                         .str_is_counted = 1,
                                         .str_is_fixed_length = 1,
                                         .str_is_zero_terminated = 0,
                                         .str_is_byte_swapped = 0,

                                         .str_pad_to_multiple_bytes = 1,
                                         .str_count_word_bytes = 4,
                                         .str_max_capacity = 82,
                                         .str_total_length = 88,
                                         .str_pad_bytes = 2};


tag_byte_order_t logix_tag_listing_byte_order = {.is_allocated = 0,

                                                 .int16_order = {0, 1},
                                                 .int32_order = {0, 1, 2, 3},
                                                 .int64_order = {0, 1, 2, 3, 4, 5, 6, 7},
                                                 .float32_order = {0, 1, 2, 3},
                                                 .float64_order = {0, 1, 2, 3, 4, 5, 6, 7},

                                                 .str_is_defined = 1,
                                                 .str_is_counted = 1,
                                                 .str_is_fixed_length = 0,
                                                 .str_is_zero_terminated = 0,
                                                 .str_is_byte_swapped = 0,

                                                 .str_pad_to_multiple_bytes = 1,
                                                 .str_count_word_bytes = 2,
                                                 .str_max_capacity = 0,
                                                 .str_total_length = 0,
                                                 .str_pad_bytes = 0};


/*
 * What a Rockwell CIP PLC can do, for the shared standard-tag engine.
 *
 * The fragmented read is used whatever the offset: the plain Read Tag service carries no
 * offset, so it cannot continue a transfer the PLC split across replies, and the PLC
 * decides to split by answering CIP_STATUS_FRAG rather than by being asked.
 */
const cip_standard_tag_ops_t cip_standard_tag_ops_ab = {.read_service = CIP_SVC_READ_FRAG,
                                                        .write_service = CIP_SVC_WRITE,
                                                        .write_service_split = CIP_SVC_WRITE_FRAG,

                                                        .max_transfer_packets = CIP_TRANSFER_PACKETS_UNLIMITED,

                                                        .sized_segments = false,

                                                        .encode_read_offset = cip_encode_offset_u32};


/* the standard (symbolic, named) tag type.  The engine is shared; see modules/cip/standard_tag.c. */
struct tag_vtable_t cip_standard_tag_vtable_ab = {
    .abort = (tag_vtable_func)cip_data_tag_abort,
    .read = cip_tag_read_start,
    .status = (tag_vtable_func)cip_tag_status,
    .tickler = NULL,
    .write = cip_tag_write_start,
    .wake_plc = cip_tag_wake_plc,
    .activate = cip_tag_activate,
    .tag_data_written = cip_tag_wake_plc,
    .encode_read = (tag_encode_func)cip_standard_tag_encode_read,
    .decode_read = (tag_decode_func)cip_standard_tag_decode_read,
    .encode_write = (tag_encode_func)cip_standard_tag_encode_write,
    .decode_write = (tag_decode_func)cip_standard_tag_decode_write,

    /* attribute accessors */
    .attribs = ab_attribs,
};

