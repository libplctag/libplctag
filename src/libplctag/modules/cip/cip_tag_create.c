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
 * The one CIP tag constructor.  It picks the family -- from the plc attribute, or from the
 * Identity object a probed connection reads -- finds or makes the connection, and sets the tag
 * up with the family that connection holds.  The families themselves live with their modules.
 */

#include <libplctag/api/libplctag.h>
#include <libplctag/modules/ab/ab_common.h>
#include <libplctag/modules/omron/omron_common.h>
#include <platform.h>
#include <libplctag/lib/byte_order.h>
#include <libplctag/lib/connection_tag.h>
#include <libplctag/modules/cip/conn.h>
#include <libplctag/modules/cip/connection_tag.h>
#include <libplctag/modules/cip/family.h>
#include <libplctag/modules/cip/cip_tag_create.h>
#include <stdint.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/rc.h>


static int setup_tag(const cip_family_t *family, cip_tag_p tag, attr attribs);
static int deferred_abort(plc_tag_p tag);
static int deferred_status(plc_tag_p tag);
static int deferred_tickler(plc_tag_p tag);


bool cip_family_use_connected_msg(const cip_family_t *family, attr attribs) {
    /* the one family that lets the tag string decide, and it defaults to connected. */
    if(family->use_connected_msg < 0) { return (attr_get_int(attribs, "use_connected_msg", 1) != 0); }

    return (family->use_connected_msg != 0);
}


#define CIP_VENDOR_ROCKWELL ((uint16_t)0x0001)

/*
 * Identity product-name prefixes, first match wins.  A NULL family refuses the device: every
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
    const cip_family_t *family;
} identity_map[] = {
    {CIP_VENDOR_ROCKWELL, "1756-L", &ab_family_logix},
    {CIP_VENDOR_ROCKWELL, "1756-", NULL},
    {CIP_VENDOR_ROCKWELL, "1769-L", &ab_family_logix},
    {CIP_VENDOR_ROCKWELL, "5069-L", &ab_family_logix},
    {CIP_VENDOR_ROCKWELL, "2080-", &ab_family_micro800},
    {CIP_VENDOR_ROCKWELL, "PLC-5", &ab_family_plc5},
    {CIP_VENDOR_ROCKWELL, "1763-", &ab_family_mlgx},
};


int cip_family_from_identity(uint16_t vendor_id, const char *product_name, bool has_path, const cip_family_t **family_out) {
    const cip_family_t *family = &ab_family_generic;

    for(size_t index = 0; index < (sizeof(identity_map) / sizeof(identity_map[0])); index++) {
        const char *prefix = identity_map[index].prefix;

        if(identity_map[index].vendor_id == vendor_id && str_cmp_i_n(product_name, prefix, str_length(prefix)) == 0) {
            family = identity_map[index].family;
            break;
        }
    }

    if(!family) {
        if(has_path) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "The path reaches a %s, not a CPU!", product_name);
        } else {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "The gateway is a %s.  A path to the CPU's slot is required!",
                   product_name);
        }

        return PLCTAG_ERR_BAD_PARAM;
    }

    *family_out = family;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Vendor %u \"%s\" is a %s.", (unsigned int)vendor_id, product_name, family->name);

    return PLCTAG_STATUS_OK;
}


const cip_family_t *cip_family_from_attributes(attr attribs) {
    const char *cpu_type = attr_get_str(attribs, "plc", attr_get_str(attribs, "cpu", NULL));

    /* not an error: the connection identifies the device instead. */
    if(!cpu_type) { return NULL; }

    if(!str_cmp_i(cpu_type, "plc") || !str_cmp_i(cpu_type, "plc5")) {
        return &ab_family_plc5;
    } else if(!str_cmp_i(cpu_type, "slc") || !str_cmp_i(cpu_type, "slc500")) {
        return &ab_family_slc;
    } else if(!str_cmp_i(cpu_type, "lgxpccc") || !str_cmp_i(cpu_type, "logixpccc") || !str_cmp_i(cpu_type, "lgxplc5")
              || !str_cmp_i(cpu_type, "logixplc5") || !str_cmp_i(cpu_type, "lgx-pccc") || !str_cmp_i(cpu_type, "logix-pccc")
              || !str_cmp_i(cpu_type, "lgx-plc5") || !str_cmp_i(cpu_type, "logix-plc5")) {
        return &ab_family_lgx_pccc;
    } else if(!str_cmp_i(cpu_type, "micrologix800") || !str_cmp_i(cpu_type, "mlgx800") || !str_cmp_i(cpu_type, "micro800")) {
        return &ab_family_micro800;
    } else if(!str_cmp_i(cpu_type, "micrologix") || !str_cmp_i(cpu_type, "mlgx")) {
        return &ab_family_mlgx;
    } else if(!str_cmp_i(cpu_type, "compactlogix") || !str_cmp_i(cpu_type, "clgx") || !str_cmp_i(cpu_type, "lgx")
              || !str_cmp_i(cpu_type, "controllogix") || !str_cmp_i(cpu_type, "contrologix") || !str_cmp_i(cpu_type, "logix")) {
        return &ab_family_logix;
    } else if(!str_cmp_i(cpu_type, "omron-njnx") || !str_cmp_i(cpu_type, "omron-nj") || !str_cmp_i(cpu_type, "omron-nx")
              || !str_cmp_i(cpu_type, "njnx") || !str_cmp_i(cpu_type, "nx1p2")) {
        return &omron_family_njnx;
    } else if(!str_cmp_i(cpu_type, "generic") || !str_cmp_i(cpu_type, "cip")) {
        return &ab_family_generic;
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "Unsupported device type: %s", cpu_type);

    return NULL;
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
 * A spelling and not a family: ControlLogix and CompactLogix are one family,
 * but a CompactLogix answers on its own Ethernet port.  The bare "lgx" and "logix" spellings
 * have always required a path, so they still do.
 */
static bool plc_needs_path(attr attribs) {
    const char *plc = attr_get_str(attribs, "plc", attr_get_str(attribs, "cpu", NULL));

    return (str_cmp_i(plc, "controllogix") == 0 || str_cmp_i(plc, "contrologix") == 0 || str_cmp_i(plc, "lgx") == 0
            || str_cmp_i(plc, "logix") == 0);
}


/*
 * What a tag dispatches through until its family sets it up, so an abort during creation is
 * safe.  No attributes: a tag that fails to create never reaches the caller.
 */
static struct tag_vtable_t unimplemented_vtable = {
    .abort = cip_tag_unimplemented_abort,
    .read = cip_tag_unimplemented_read,
    .status = cip_tag_unimplemented_status,
    .tickler = NULL,
    .write = cip_tag_unimplemented_write,
    .wake_plc = NULL,
    .activate = cip_tag_activate,
    .tag_data_written = NULL,
    .attribs = NULL,
};


/*
 * What a tag dispatches through while its connection identifies the device.  The tickler
 * finishes the create once the family is known.
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
 * Allocate a tag on a connection the caller has settled, then set it up with the connection's
 * family.  Takes ownership of the connection reference, on failure as well as success.
 *
 * Two steps because they need different things: the tag needs only the connection, the setup
 * needs the connection's family.  With no family yet -- a probed connection still identifying
 * the device -- the setup waits for the tickler, on a copy of the attributes.  The family sets
 * the tag's protocol, so the allocation's is only a placeholder.
 */
static int create_tag(const cip_family_t *family, cip_conn_p conn, attr attribs,
                      void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata), void *userdata,
                      plc_tag_p *tag_out) {
    cip_tag_p tag = NULL;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    tag = cip_tag_alloc(attribs, tag_callback_func, userdata, TAG_PROTOCOL_AB, &unimplemented_vtable, DEBUG_MODULE_CIP);
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

    if(family) { return setup_tag(family, tag, attribs); }

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
 * Finish a create once the connection knows its family.  The library applied its byte-order
 * overrides before the tag had a byte order, so they are applied here.  Runs under the tag's
 * api_mutex, like every tickler, on the connection's handler thread.
 */
static int deferred_tickler(plc_tag_p raw_tag) {
    cip_tag_p tag = (cip_tag_p)raw_tag;
    attr attribs = tag->deferred_attribs;
    const cip_family_t *family = NULL;
    int identify_rc = PLCTAG_STATUS_OK;
    int rc = PLCTAG_STATUS_OK;

    if(tag->create_read_status != PLCTAG_STATUS_PENDING || !attribs) { return PLCTAG_STATUS_OK; }

    family = cip_conn_family(tag->session, &identify_rc);
    if(!family) {
        if(identify_rc != PLCTAG_STATUS_OK) { cip_tag_fail_create(tag, (plctag_error_code_t)identify_rc); }
        return PLCTAG_STATUS_OK;
    }

    tag->deferred_attribs = NULL;

    rc = setup_tag(family, tag, attribs);
    if(rc == PLCTAG_STATUS_OK) { rc = set_tag_byte_order((plc_tag_p)tag, attribs); }

    attr_destroy(attribs);

    if(rc != PLCTAG_STATUS_OK) { cip_tag_fail_create(tag, (plctag_error_code_t)rc); }

    return PLCTAG_STATUS_OK;
}


/* The setup every family shares, around the family's own, ending with the create read. */
static int setup_tag(const cip_family_t *family, cip_tag_p tag, attr attribs) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, tag->tag_id, "Setting up %s tag.", family->name);

    tag->plc_type = family->plc_type;
    tag->use_connected_msg = tag->session->use_connected_msg;
    /* the create read checks the tag against the PLC; ending it queues CREATED. */
    tag->create_read_status = PLCTAG_STATUS_PENDING;

    rc = cip_tag_resolve_elem_count(tag, attribs, DEBUG_MODULE_CIP);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    rc = family->setup_tag(tag, attribs);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, tag->tag_id, "Unable to set up %s tag, error %s!", family->name,
               plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->vtable->read) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, tag->tag_id, "Kicking off initial read.");
        tag_op_start((plc_tag_p)tag, true);
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
    const cip_family_t *family = NULL;
    cip_conn_p conn = NULL;
    const char *path = attr_get_str(attribs, "path", NULL);
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Starting.");

    if(str_cmp(attr_get_str(attribs, "name", ""), "@connection") == 0) {
        bool is_omron = (src_tag ? (src_tag->protocol_type == TAG_PROTOCOL_OMRON
                                    || src_tag->protocol_type == TAG_PROTOCOL_OMRON_CONNECTION)
                                 : (cip_family_from_attributes(attribs) == &omron_family_njnx));

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

        family = cip_conn_family(conn, &rc);

        /* the connection decides use_connected_msg, so a clone cannot ask for the other setting. */
        if(family && attr_get_str(attribs, "use_connected_msg", NULL)
           && (cip_family_use_connected_msg(family, attribs) ? 1 : 0) != conn->use_connected_msg) {
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
        /* the connection settles the family; these two need only what the tag string named. */

        /*
         * str_length() rather than a NULL test: "path=" is a present but empty attribute and
         * would otherwise fail further in with a worse message.
         */
        if(str_length(path) == 0 && plc_needs_path(attribs)) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "A path to the CPU's slot is required for a ControlLogix PLC!");
            return PLCTAG_ERR_BAD_PARAM;
        }

        if(str_length(path) && cip_family_from_attributes(attribs) == &ab_family_micro800) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, 0, "A path is not supported for Micro800 PLCs.");
        }

        rc = cip_conn_find_or_create(&conn, attribs, NULL);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_INFO, 0, "Unable to create connection, error %s!", plc_tag_decode_error(rc));
            return rc;
        }

        family = cip_conn_family(conn, &rc);
    }

    /* a probed connection's refusal is the answer, so a tag created after it fails at once. */
    if(!family && rc != PLCTAG_STATUS_OK) {
        rc_dec(conn);
        return rc;
    }

    return create_tag(family, conn, attribs, tag_callback_func, userdata, tag_out);
}
