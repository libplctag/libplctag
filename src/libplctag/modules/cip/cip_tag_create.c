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

/*
 * The CIP PLC dispatch table.
 *
 * Eleven rows for eight PLC types, because three of them -- PLC/5, SLC and MicroLogix --
 * are reached either directly or across a DH+ bridge and the two cases set different
 * messaging.  Rows map many-to-one onto name parsers: PLC/5 addressing, SLC addressing,
 * Logix symbolic, generic and OMRON symbolic.
 *
 * Below the table is the one CIP tag constructor.  It finds the row and the connection, then
 * builds the tag from the row.
 */

#include <libplctag/api/libplctag.h>
#include <libplctag/modules/ab/ab_common.h>
#include <libplctag/modules/ab/eip_lgx_pccc.h>
#include <libplctag/modules/ab/eip_plc5_pccc.h>
#include <libplctag/modules/ab/eip_slc_pccc.h>
#include <libplctag/modules/ab/pccc.h>
#include <libplctag/modules/omron/omron_common.h>
#include <platform.h>
#include <libplctag/lib/byte_order.h>
#include <libplctag/lib/connection_tag.h>
#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/cip/connection_tag.h>
#include <libplctag/modules/cip/path.h>
#include <libplctag/modules/cip/plc_type.h>
#include <libplctag/modules/cip/cip_tag_create.h>
#include <stdint.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/rc.h>


static int setup_tag(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs);
static int deferred_abort(plc_tag_p tag);
static int deferred_status(plc_tag_p tag);
static int deferred_tickler(plc_tag_p tag);

/* maximum for PCCC embedded within CIP. */
#define MAX_CIP_PCCC_MSG_SIZE (244)

#define MAX_CIP_LGX_MSG_SIZE (0x01FF & 504)
#define MAX_CIP_LGX_MSG_SIZE_EX (0xFFFF & 4000)

#define MAX_CIP_OMRON_MSG_SIZE (0x01FF & 502)
#define MAX_CIP_OMRON_MSG_SIZE_EX (0xFFFF & 1990)

/*
 * Smallest PCCC connection payload we can actually use.  A ForwardOpen error can offer a
 * smaller size than we asked for and we accept it, but below the fixed per-request overhead
 * there is no room left for a request.  PCCC needs about 92 bytes for its command header and
 * addressing; MIN_PAYLOAD_SIZE_CIP is the CIP equivalent.
 */
#define MIN_PAYLOAD_SIZE_PCCC (92)

/* the three connection shapes, which the rows share. */
#define PCCC_CONN                                                                                                       \
    .max_payload_capacity = MAX_CIP_PCCC_MSG_SIZE, .fo_conn_size = MAX_CIP_PCCC_MSG_SIZE, .fo_ex_conn_size = 0,         \
    .only_use_old_forward_open = true, .min_payload_size = MIN_PAYLOAD_SIZE_PCCC

#define LGX_CONN                                                                                                        \
    .max_payload_capacity = MAX_CIP_LGX_MSG_SIZE_EX, .fo_conn_size = MAX_CIP_LGX_MSG_SIZE,                              \
    .fo_ex_conn_size = MAX_CIP_LGX_MSG_SIZE_EX, .only_use_old_forward_open = false, .min_payload_size = MIN_PAYLOAD_SIZE_CIP

#define OMRON_CONN                                                                                                      \
    .max_payload_capacity = MAX_CIP_OMRON_MSG_SIZE_EX, .fo_conn_size = MAX_CIP_OMRON_MSG_SIZE,                          \
    .fo_ex_conn_size = MAX_CIP_OMRON_MSG_SIZE_EX, .only_use_old_forward_open = false, .min_payload_size = MIN_PAYLOAD_SIZE_CIP

/* what every Rockwell tag starts with; packing and size_from_plc default to false. */
#define AB_TAG(vt, bo)                                                                                                  \
    .protocol = TAG_PROTOCOL_AB, .vtable = (vt), .byte_order = (bo), .std_ops = &cip_standard_tag_ops_ab,               \
    .debug_module = DEBUG_MODULE_AB_EIP_CIP

/*
 * Micro800 supports the Extended Forward Open and asks for the same size *Logix does: the
 * only published figure for anything larger traces back to a single forum post and no
 * Rockwell document states one, while 4002 is measured -- 4082 comes back as an invalid
 * connection size and 4002 succeeds.  Asking higher costs a failed round trip on every
 * connection and sizes the per-connection buffer to match.  Raise it if a Micro800 is
 * measured accepting more.  Generic CIP takes the *Logix sizing too.
 */
static const cip_plc_entry_t cip_plc_entries[] = {
    {.name = "PLC/5", .plc_type = CIP_PLC_PLC5, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = 0,
     AB_TAG(&pccc_vtable, &plc5_tag_byte_order), .setup_name = ab_setup_plc5_name, PCCC_CONN},
    {.name = "PLC/5 via DH+", .plc_type = CIP_PLC_PLC5, .dialect = CIP_DIALECT_DHP, .use_connected_msg = 1,
     AB_TAG(&pccc_vtable, &plc5_tag_byte_order), .setup_name = ab_setup_plc5_name, PCCC_CONN},
    {.name = "SLC 500", .plc_type = CIP_PLC_SLC, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = 0,
     AB_TAG(&pccc_vtable, &slc_tag_byte_order), .setup_name = ab_setup_slc_name, PCCC_CONN},
    {.name = "SLC 500 via DH+", .plc_type = CIP_PLC_SLC, .dialect = CIP_DIALECT_DHP, .use_connected_msg = 1,
     AB_TAG(&pccc_vtable, &slc_tag_byte_order), .setup_name = ab_setup_slc_name, PCCC_CONN},
    {.name = "MicroLogix", .plc_type = CIP_PLC_MLGX, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = 0,
     AB_TAG(&pccc_vtable, &slc_tag_byte_order), .setup_name = ab_setup_slc_name, PCCC_CONN},
    {.name = "MicroLogix via DH+", .plc_type = CIP_PLC_MLGX, .dialect = CIP_DIALECT_DHP, .use_connected_msg = 1,
     AB_TAG(&pccc_vtable, &slc_tag_byte_order), .setup_name = ab_setup_slc_name, PCCC_CONN},
    /* SLC byte order, PLC/5 addressing. */
    {.name = "PCCC-mapped Logix", .plc_type = CIP_PLC_LGX_PCCC, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = 0,
     AB_TAG(&lgx_pccc_vtable, &slc_tag_byte_order), .setup_name = ab_setup_plc5_name, PCCC_CONN},
    {.name = "Logix", .plc_type = CIP_PLC_LGX, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = -1,
     AB_TAG(&cip_standard_tag_vtable_ab, &logix_tag_byte_order), .allow_packing = true, .packing_settable = true,
     .size_from_plc = true, .setup_name = ab_setup_logix_name, LGX_CONN},
    {.name = "Micro800", .plc_type = CIP_PLC_MICRO800, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = 1,
     AB_TAG(&cip_standard_tag_vtable_ab, &cip_short_string_byte_order), .size_from_plc = true,
     .setup_name = ab_setup_logix_name, LGX_CONN},
    /* only special tags, so no data-tag vtable. */
    {.name = "generic CIP", .plc_type = CIP_PLC_GENERIC, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = 0,
     AB_TAG(NULL, &logix_tag_byte_order), .size_from_plc = true, .setup_name = ab_setup_generic_name, LGX_CONN},
    {.name = "OMRON NJ/NX", .plc_type = CIP_PLC_OMRON_NJNX, .dialect = CIP_DIALECT_DIRECT, .use_connected_msg = 1,
     .protocol = TAG_PROTOCOL_OMRON, .vtable = &cip_standard_tag_vtable_omron, .byte_order = &omron_njnx_tag_byte_order,
     .std_ops = &cip_standard_tag_ops_omron, .debug_module = DEBUG_MODULE_OMRON_STANDARD_TAG, .packing_settable = true,
     .size_from_plc = true, .setup_name = omron_setup_njnx_name, OMRON_CONN},
};


bool cip_entry_use_connected_msg(const cip_plc_entry_t *entry, attr attribs) {
    /* the one family that lets the tag string decide, and it defaults to connected. */
    if(entry->use_connected_msg < 0) { return (attr_get_int(attribs, "use_connected_msg", 1) != 0); }

    return (entry->use_connected_msg != 0);
}


const cip_plc_entry_t *cip_plc_entry_find(cip_plc_type_t plc_type, cip_dialect_t dialect) {
    size_t index = 0;

    for(index = 0; index < (sizeof(cip_plc_entries) / sizeof(cip_plc_entries[0])); index++) {
        if(cip_plc_entries[index].plc_type == plc_type && cip_plc_entries[index].dialect == dialect) {
            return &cip_plc_entries[index];
        }
    }

    /*
     * No warning here.  Connection creation asks this for a DH+ row on every new connection
     * precisely to find out whether the family has one, so a miss is the normal answer rather
     * than a problem.  The callers that do treat a miss as an error say so
     * themselves, with the PLC type they were given.
     */
    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "No PLC table entry for PLC type %d, dialect %d.", (int)plc_type, (int)dialect);

    return NULL;
}


int cip_plc_entries_max_payload_capacity(void) {
    int max_capacity = 0;

    for(size_t index = 0; index < (sizeof(cip_plc_entries) / sizeof(cip_plc_entries[0])); index++) {
        if(cip_plc_entries[index].max_payload_capacity > max_capacity) {
            max_capacity = cip_plc_entries[index].max_payload_capacity;
        }
    }

    return max_capacity;
}


#define CIP_VENDOR_ROCKWELL ((uint16_t)0x0001)

/*
 * Identity product-name prefixes, first match wins.  CIP_PLC_NONE refuses the device: every
 * 1756 part that is not a CPU is a chassis module, and reaching one means the path stops
 * short of the CPU.  Device type is no help: a MicroLogix 1100 reports itself as a
 * communications adapter, as an ENBT does.
 *
 * ponytail: only names seen in real Identity replies, on the bench or published by pylogix and
 * pycomm3.  Anything else is generic CIP and needs plc=; add a prefix when a reply has been seen.
 */
static const struct {
    uint16_t vendor_id;
    const char *prefix;
    cip_plc_type_t plc_type;
} identity_map[] = {
    {CIP_VENDOR_ROCKWELL, "1756-L", CIP_PLC_LGX},
    {CIP_VENDOR_ROCKWELL, "1756-", CIP_PLC_NONE},
    {CIP_VENDOR_ROCKWELL, "1769-L", CIP_PLC_LGX},
    {CIP_VENDOR_ROCKWELL, "5069-L", CIP_PLC_LGX},
    {CIP_VENDOR_ROCKWELL, "2080-", CIP_PLC_MICRO800},
    {CIP_VENDOR_ROCKWELL, "PLC-5", CIP_PLC_PLC5},
    {CIP_VENDOR_ROCKWELL, "1763-", CIP_PLC_MLGX},
};


int cip_plc_entry_from_identity(uint16_t vendor_id, const char *product_name, bool has_path,
                                const cip_plc_entry_t **entry_out) {
    cip_plc_type_t plc_type = CIP_PLC_GENERIC;

    for(size_t index = 0; index < (sizeof(identity_map) / sizeof(identity_map[0])); index++) {
        const char *prefix = identity_map[index].prefix;

        if(identity_map[index].vendor_id == vendor_id && str_cmp_i_n(product_name, prefix, str_length(prefix)) == 0) {
            plc_type = identity_map[index].plc_type;
            break;
        }
    }

    if(plc_type == CIP_PLC_NONE) {
        if(has_path) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "The path reaches a %s, not a CPU!", product_name);
        } else {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "The gateway is a %s.  A path to the CPU's slot is required!",
                   product_name);
        }

        return PLCTAG_ERR_BAD_PARAM;
    }

    *entry_out = cip_plc_entry_find(plc_type, CIP_DIALECT_DIRECT);

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Vendor %u \"%s\" is a %s.", (unsigned int)vendor_id, product_name,
           (*entry_out)->name);

    return PLCTAG_STATUS_OK;
}


cip_plc_type_t cip_plc_type_from_attributes(attr attribs) {
    const char *cpu_type = attr_get_str(attribs, "plc", attr_get_str(attribs, "cpu", NULL));

    /* not an error: the connection identifies the device instead. */
    if(!cpu_type) { return CIP_PLC_NONE; }

    if(!str_cmp_i(cpu_type, "plc") || !str_cmp_i(cpu_type, "plc5")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found PLC/5 PLC.");
        return CIP_PLC_PLC5;
    } else if(!str_cmp_i(cpu_type, "slc") || !str_cmp_i(cpu_type, "slc500")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found SLC 500 PLC.");
        return CIP_PLC_SLC;
    } else if(!str_cmp_i(cpu_type, "lgxpccc") || !str_cmp_i(cpu_type, "logixpccc") || !str_cmp_i(cpu_type, "lgxplc5")
              || !str_cmp_i(cpu_type, "logixplc5") || !str_cmp_i(cpu_type, "lgx-pccc") || !str_cmp_i(cpu_type, "logix-pccc")
              || !str_cmp_i(cpu_type, "lgx-plc5") || !str_cmp_i(cpu_type, "logix-plc5")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found Logix-class PLC using PCCC protocol.");
        return CIP_PLC_LGX_PCCC;
    } else if(!str_cmp_i(cpu_type, "micrologix800") || !str_cmp_i(cpu_type, "mlgx800") || !str_cmp_i(cpu_type, "micro800")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found Micro8xx PLC.");
        return CIP_PLC_MICRO800;
    } else if(!str_cmp_i(cpu_type, "micrologix") || !str_cmp_i(cpu_type, "mlgx")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found MicroLogix PLC.");
        return CIP_PLC_MLGX;
    } else if(!str_cmp_i(cpu_type, "compactlogix") || !str_cmp_i(cpu_type, "clgx") || !str_cmp_i(cpu_type, "lgx")
              || !str_cmp_i(cpu_type, "controllogix") || !str_cmp_i(cpu_type, "contrologix") || !str_cmp_i(cpu_type, "logix")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found ControlLogix/CompactLogix PLC.");
        return CIP_PLC_LGX;
    } else if(!str_cmp_i(cpu_type, "omron-njnx") || !str_cmp_i(cpu_type, "omron-nj") || !str_cmp_i(cpu_type, "omron-nx")
              || !str_cmp_i(cpu_type, "njnx") || !str_cmp_i(cpu_type, "nx1p2")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found OMRON NJ/NX Series PLC.");
        return CIP_PLC_OMRON_NJNX;
    } else if(!str_cmp_i(cpu_type, "generic") || !str_cmp_i(cpu_type, "cip")) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "Found generic CIP device.");
        return CIP_PLC_GENERIC;
    } else {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unsupported device type: %s", cpu_type);

        return CIP_PLC_NONE;
    }
}


/*
 * @connection, for either family.  The ops are the one place the families differ: each keeps
 * its own debug module ID -- public API that callers filter on -- and its own protocol.
 *
 * AB adopts a connection from an OMRON tag as well as its own, since both hang the same
 * cip_conn_p off the same field.  Both pass a find-or-create failure up unchanged.
 */
static plc_tag_p create_connection_tag(attr attribs, bool is_omron,
                                      void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                      void *userdata, plc_tag_p src_tag) {
    static const cip_connection_tag_ops_t ab_ops = {.connection_protocol = TAG_PROTOCOL_AB_CONNECTION,
                                                    .debug_module = DEBUG_MODULE_AB_CONNECTION,
                                                    .source_protocols = {TAG_PROTOCOL_AB, TAG_PROTOCOL_OMRON}};
    static const cip_connection_tag_ops_t omron_ops = {.connection_protocol = TAG_PROTOCOL_OMRON_CONNECTION,
                                                       .debug_module = DEBUG_MODULE_OMRON_CONNECTION,
                                                       .source_protocols = {TAG_PROTOCOL_OMRON, TAG_PROTOCOL_UNKNOWN}};

    return cip_connection_tag_create(attribs, is_omron ? &omron_ops : &ab_ops, tag_callback_func, userdata, src_tag);
}


/*
 * Whether the tag string names a PLC that sits in a chassis behind a communication module,
 * so a tag without a path would reach the module rather than the CPU.
 *
 * A spelling and not a row: ControlLogix and CompactLogix are one family and share a row,
 * but a CompactLogix answers on its own Ethernet port.  The bare "lgx" and "logix" spellings
 * have always required a path, so they still do.
 */
static bool plc_needs_path(attr attribs) {
    const char *plc = attr_get_str(attribs, "plc", attr_get_str(attribs, "cpu", NULL));

    return (str_cmp_i(plc, "controllogix") == 0 || str_cmp_i(plc, "contrologix") == 0 || str_cmp_i(plc, "lgx") == 0
            || str_cmp_i(plc, "logix") == 0);
}


/*
 * What a tag dispatches through until its row sets it up, so an abort during creation is
 * safe.  No attributes: a tag that fails to create never reaches the caller.
 */
static struct tag_vtable_t unimplemented_vtable = {
    .abort = cip_tag_unimplemented_abort,
    .read = cip_tag_unimplemented_read,
    .status = cip_tag_unimplemented_status,
    .tickler = cip_tag_unimplemented_tickler,
    .write = cip_tag_unimplemented_write,
    .wake_plc = NULL,
    .activate = cip_tag_activate,
    .tag_data_written = NULL,
    .attribs = NULL,
};


/*
 * What a tag dispatches through while its connection identifies the device.  The tickler
 * finishes the create once the row is known.
 */
static struct tag_vtable_t deferred_vtable = {
    .abort = deferred_abort,
    .read = cip_tag_unimplemented_read,
    .status = deferred_status,
    .tickler = deferred_tickler,
    .write = cip_tag_unimplemented_write,
    .wake_plc = NULL,
    .activate = cip_tag_activate,
    .tag_data_written = NULL,
    .attribs = NULL,
};


/*
 * Allocate a tag on a connection the caller has settled, then set it up from the row.  Takes
 * ownership of the connection reference, on failure as well as success.
 *
 * Two steps because they need different things: the tag needs only the connection, the setup
 * needs the connection's row.  With no row yet -- a probed connection still identifying the
 * device -- the setup waits for the tickler, on a copy of the attributes.
 */
static int create_tag(const cip_plc_entry_t *entry, cip_conn_p conn, attr attribs,
                      void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata), void *userdata,
                      plc_tag_p *tag_out) {
    cip_tag_p tag = NULL;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    tag = cip_tag_alloc(attribs, tag_callback_func, userdata, (entry ? entry->protocol : TAG_PROTOCOL_AB),
                        &unimplemented_vtable, DEBUG_MODULE_CIP);
    if(!tag) {
        /* nothing owns the connection yet, so this is the one failure that has to say so. */
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, 0, "rc_dec: Releasing connection reference.");
        rc_dec(conn);
        return PLCTAG_ERR_NO_MEM;
    }

    /*
     * The tag owns the connection from here, so every failure below releases it along with
     * the tag.  The tag is handed back even on failure, so the caller can release it.
     */
    tag->session = conn;
    *tag_out = (plc_tag_p)tag;

    if(entry) { return setup_tag(entry, tag, attribs); }

    tag->deferred_attribs = attr_copy(attribs);
    if(!tag->deferred_attribs) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Unable to copy the attributes!");
        return PLCTAG_ERR_NO_MEM;
    }

    tag->vtable = &deferred_vtable;
    tag->create_read_status = PLCTAG_STATUS_PENDING;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Waiting for the connection to identify the device.");

    return PLCTAG_STATUS_PENDING;
}


static int deferred_abort(plc_tag_p tag) {
    if(((cip_tag_p)tag)->create_read_status == PLCTAG_STATUS_PENDING) { cip_tag_fail_create((cip_tag_p)tag, PLCTAG_ERR_ABORT); }

    return PLCTAG_STATUS_OK;
}


static int deferred_status(plc_tag_p tag) { return ((cip_tag_p)tag)->create_read_status; }


/*
 * Finish a create once the connection knows its row.  The library applied its byte-order
 * overrides before the tag had a byte order, so they are applied here.  Runs under the tag's
 * api_mutex, like every tickler, on the connection's handler thread.
 */
static int deferred_tickler(plc_tag_p raw_tag) {
    cip_tag_p tag = (cip_tag_p)raw_tag;
    attr attribs = tag->deferred_attribs;
    const cip_plc_entry_t *entry = NULL;
    int identify_rc = PLCTAG_STATUS_OK;
    int rc = PLCTAG_STATUS_OK;

    if(tag->create_read_status != PLCTAG_STATUS_PENDING || !attribs) { return PLCTAG_STATUS_OK; }

    entry = cip_conn_plc_entry(tag->session, &identify_rc);
    if(!entry) {
        if(identify_rc != PLCTAG_STATUS_OK) { cip_tag_fail_create(tag, (plctag_error_code_t)identify_rc); }
        return PLCTAG_STATUS_OK;
    }

    tag->deferred_attribs = NULL;
    tag->protocol_type = entry->protocol;

    rc = setup_tag(entry, tag, attribs);
    if(rc == PLCTAG_STATUS_OK) { rc = set_tag_byte_order((plc_tag_p)tag, attribs); }

    attr_destroy(attribs);

    if(rc != PLCTAG_STATUS_OK) { cip_tag_fail_create(tag, (plctag_error_code_t)rc); }

    return PLCTAG_STATUS_OK;
}


/* Everything about a tag that comes from its family's row, ending with the create read. */
static int setup_tag(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Setting up %s tag.", entry->name);

    tag->plc_type = entry->plc_type;
    tag->use_connected_msg = tag->session->use_connected_msg;
    if(entry->vtable) { tag->vtable = entry->vtable; }
    tag->byte_order = entry->byte_order;
    tag->std_ops = entry->std_ops;
    tag->debug_module = entry->debug_module;
    tag->allow_packing = (entry->packing_settable ? (attr_get_int(attribs, "allow_packing", entry->allow_packing) != 0)
                                                  : entry->allow_packing);
    /* the create read checks the tag against the PLC; the tickler raises CREATED when it ends. */
    tag->create_read_status = PLCTAG_STATUS_PENDING;

    rc = cip_tag_resolve_elem_count(tag, attribs, DEBUG_MODULE_CIP);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    /* a special tag replaces the vtable, byte order and debug module set above. */
    rc = entry->setup_name(entry, tag, attribs);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Unable to set up %s tag, error %s!", entry->name,
               plc_tag_decode_error(rc));
        return rc;
    }

    /* a special tag learns its size from the reply. */
    if(entry->size_from_plc || tag->special_tag) {
        rc = cip_tag_size_from_plc(tag, DEBUG_MODULE_CIP);
    } else {
        rc = cip_tag_size_from_attributes(tag, attribs, DEBUG_MODULE_CIP);
    }

    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->vtable->read) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Kicking off initial read.");
        tag->read_in_flight = 1;
        tag->vtable->read((plc_tag_p)tag);
    } else {
        /* @raw has nothing to read; its tickler completes the create when the connection is up. */
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Waiting for the connection to complete the create.");
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * Which family and which connection, settled before a tag exists, then the family's own
 * constructor on that connection.
 *
 * A clone takes its family from the connection it inherits.  Its own attributes cannot be
 * asked: a clone need not repeat the path that made the original DH+.
 */
int cip_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                   void *userdata, plc_tag_p src_tag, plc_tag_p *tag_out) {
    const cip_plc_entry_t *entry = NULL;
    cip_conn_p conn = NULL;
    cip_plc_type_t plc_type = CIP_PLC_NONE;
    const char *path = attr_get_str(attribs, "path", NULL);
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    if(str_cmp(attr_get_str(attribs, "name", ""), "@connection") == 0) {
        bool is_omron = (src_tag ? (src_tag->protocol_type == TAG_PROTOCOL_OMRON
                                    || src_tag->protocol_type == TAG_PROTOCOL_OMRON_CONNECTION)
                                 : (cip_plc_type_from_attributes(attribs) == CIP_PLC_OMRON_NJNX));

        *tag_out = create_connection_tag(attribs, is_omron, tag_callback_func, userdata, src_tag);

        /* the connection tag types still report through the tag. */
        return *tag_out ? (int)(*tag_out)->status : PLCTAG_ERR_CREATE;
    }

    if(src_tag) {
        switch(src_tag->protocol_type) {
            case TAG_PROTOCOL_AB:
            case TAG_PROTOCOL_OMRON: conn = ((cip_tag_p)src_tag)->session; break;

            case TAG_PROTOCOL_AB_CONNECTION:
            case TAG_PROTOCOL_OMRON_CONNECTION: conn = (cip_conn_p)((connection_tag_p)src_tag)->conn; break;

            default: break;
        }

        if(!conn) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to acquire source connection reference.");
            return PLCTAG_ERR_NOT_FOUND;
        }

        entry = cip_conn_plc_entry(conn, &rc);

        /* the connection decides use_connected_msg, so a clone cannot ask for the other setting. */
        if(entry && attr_get_str(attribs, "use_connected_msg", NULL)
           && (cip_entry_use_connected_msg(entry, attribs) ? 1 : 0) != conn->use_connected_msg) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "use_connected_msg conflicts with the source tag's connection!");
            return PLCTAG_ERR_BAD_PARAM;
        }

        /* the reference the new tag will own.  The source keeps its own. */
        conn = rc_inc(conn);
        if(!conn) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unable to acquire source connection reference.");
            return PLCTAG_ERR_NOT_FOUND;
        }
    } else {
        /* the connection settles the family and its row; these two need only the PLC type. */
        plc_type = cip_plc_type_from_attributes(attribs);

        /*
         * str_length() rather than a NULL test: "path=" is a present but empty attribute and
         * would otherwise fail further in with a worse message.
         */
        if(str_length(path) == 0 && plc_needs_path(attribs)) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "A path to the CPU's slot is required for a ControlLogix PLC!");
            return PLCTAG_ERR_BAD_PARAM;
        }

        if(str_length(path) && plc_type == CIP_PLC_MICRO800) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "A path is not supported for Micro800 PLCs.");
        }

        rc = cip_conn_find_or_create(&conn, attribs, NULL);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Unable to create connection, error %s!", plc_tag_decode_error(rc));
            return rc;
        }

        entry = cip_conn_plc_entry(conn, &rc);
    }

    /* a probed connection's refusal is the answer, so a tag created after it fails at once. */
    if(!entry && rc != PLCTAG_STATUS_OK) {
        rc_dec(conn);
        return rc;
    }

    return create_tag(entry, conn, attribs, tag_callback_func, userdata, tag_out);
}
