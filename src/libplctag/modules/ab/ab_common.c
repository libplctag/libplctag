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
#include <libplctag/lib/connection_tag.h>
#include <libplctag/lib/conn_attribs.h>
#include <libplctag/lib/tag.h>
#include <libplctag/modules/ab/ab.h>
#include <libplctag/modules/ab/ab_common.h>
#include <libplctag/modules/cip/cip_tag_create.h>
#include <libplctag/modules/ab/eip_lgx_pccc.h>
#include <libplctag/modules/ab/eip_plc5_pccc.h>
#include <libplctag/modules/ab/eip_slc_pccc.h>
#include <libplctag/modules/ab/pccc.h>
#include <libplctag/modules/cip/connection_tag.h>
#include <libplctag/modules/cip/raw_tag.h>
#include <libplctag/modules/cip/services.h>
#include <libplctag/modules/cip/standard_tag.h>
#include <libplctag/modules/cip/standard_tag.h>
#include <libplctag/modules/cip/tag.h>
#include <libplctag/modules/cip/wire.h>
#include <libplctag/modules/omron/omron.h>
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
static int get_tag_data_type(cip_tag_p tag, attr attribs);
static plc_tag_p ab_connection_tag_create(attr attribs,
                                          void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                          void *userdata, plc_tag_p src_tag);
static int setup_raw_tag(cip_tag_p tag);



/* vtables for different kinds of tags */
struct tag_vtable_t default_vtable = {
    .abort = cip_tag_unimplemented_abort,
    .read = cip_tag_unimplemented_read,
    .status = cip_tag_unimplemented_status,
    .tickler = cip_tag_unimplemented_tickler,
    .write = cip_tag_unimplemented_write,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .attribs = ab_attribs,
};

static struct tag_vtable_t raw_tag_vtable = {
    .abort = (tag_vtable_func)cip_tag_abort_request,
    .read = NULL,
    .status = (tag_vtable_func)cip_tag_status,
    .tickler = (tag_vtable_func)cip_raw_tag_tickler,
    .write = (tag_vtable_func)cip_raw_tag_write_start,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .attribs = ab_attribs,
};


/*
 * Public functions.
 */


int ab_init(void) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Initializing AB protocol library.");

    if((rc = session_startup()) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_ERROR, 0, "Unable to initialize session library!");
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Finished initializing AB protocol library.");

    return rc;
}

/*
 * called when the whole program is going to terminate.
 */
void ab_teardown(void) {
    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Releasing global AB protocol resources.");

    /*
     * The connection handler threads belong to the connections, not to this module: each one
     * is created and joined by modules/cip/conn.c.  session_teardown() waits for them.
     */
    session_teardown();

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Done.");
}


int ab_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                  void *userdata, plc_tag_p src_tag, plc_tag_p *tag_out) {
    cip_tag_p tag = NULL;
    cip_conn_p session = NULL;
    const cip_plc_entry_t *entry = NULL;
    cip_plc_type_t plc_type = CIP_PLC_NONE;
    const char *path = NULL;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Starting.");

    /* short circuit for device tag */
    if(str_cmp(attr_get_str(attribs, "name", ""), "@connection") == 0) {
        *tag_out = (plc_tag_p)ab_connection_tag_create(attribs, tag_callback_func, userdata, src_tag);

        /* the connection tag types still report through the tag. */
        return *tag_out ? (int)(*tag_out)->status : PLCTAG_ERR_CREATE;
    }

    /*
     * Which family, which dialect, and which connection -- all settled before a tag exists.
     *
     * The family determines the connection: it decides the payload sizing and the messaging,
     * so a connection is keyed on its row as much as on its address.  That is why this runs
     * first rather than on a half-built tag.
     *
     * A clone takes its family from what it was cloned from and its dialect from the
     * connection it inherits.  Its own attributes cannot be asked: a clone need not repeat
     * the path that made the original DH+.
     */
    if(src_tag) {
        switch(src_tag->protocol_type) {
            case TAG_PROTOCOL_AB: {
                cip_tag_p src = (cip_tag_p)src_tag;
                plc_type = src->plc_type;
                session = src->session;
                break;
            }

            case TAG_PROTOCOL_AB_CONNECTION: {
                session = (cip_conn_p)((connection_tag_p)src_tag)->conn;
                plc_type = session ? session->profile->plc_type : CIP_PLC_NONE;
                break;
            }

            default: break;
        }

        if(!session) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Unable to acquire source session reference.");
            return PLCTAG_ERR_NOT_FOUND;
        }

        if(session->plc_entry) { entry = cip_plc_entry_find(plc_type, session->plc_entry->dialect); }
    } else {
        /* AB accepts every PLC family its own table knows; the table is the filter. */
        plc_type = cip_plc_type_from_attributes(attribs);

        if(plc_type == CIP_PLC_NONE) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "CPU type not valid or missing.");
            return PLCTAG_ERR_BAD_DEVICE;
        }

        entry = cip_plc_entry_find(plc_type, cip_dialect_from_attributes(plc_type, attribs));
    }

    if(!entry || !entry->setup_tag) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Unknown PLC type!");
        return PLCTAG_ERR_BAD_CONFIG;
    }

    path = attr_get_str(attribs, "path", NULL);

    /*
     * A Logix tag needs a path.  The check stays here rather than in the leaf because only
     * this function knows whether the tag was cloned from another, which supplies the
     * connection instead of a path.
     *
     * str_length() rather than a NULL test, which is what Omron always used: "path=" with
     * nothing after it is a present but empty attribute, and it used to pass here and fail
     * further in with a worse message.  str_length(NULL) is 0, so this covers both.
     */
    if(!src_tag && str_length(path) == 0 && plc_type == CIP_PLC_LGX) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "A path is required for Logix-class PLCs!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * Whether the requests will be connected ones.  The row answers it, which is what lets
     * the connection be created ahead of the tag; the tag reads the same answer back below.
     */
    attr_set_int(attribs, "use_connected_msg", (cip_entry_use_connected_msg(entry, attribs) ? 1 : 0));

    /*
     * Find or create a session.
     *
     * All tags need sessions.  They are the TCP connection to the gateway PLC.
     */
    if(src_tag) {
        /* the reference this tag will own.  The source keeps its own. */
        session = rc_inc(session);

        if(!session) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Unable to acquire source session reference.");
            return PLCTAG_ERR_NOT_FOUND;
        }
    } else {
        if(session_find_or_create(&session, attribs, NULL) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Unable to create session!");
            return PLCTAG_ERR_BAD_GATEWAY;
        }
    }

    tag = cip_tag_alloc(attribs, tag_callback_func, userdata, TAG_PROTOCOL_AB, &default_vtable, DEBUG_MODULE_AB_COMMON);
    if(!tag) {
        /* nothing owns the connection yet, so this is the one failure that has to say so. */
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "rc_dec: Releasing session reference.");
        rc_dec(session);
        return PLCTAG_ERR_NO_MEM;
    }

    /*
     * The tag owns the connection from here, so every failure below releases it along with
     * the tag and needs no unwinding of its own.
     */
    tag->session = session;
    tag->plc_type = plc_type;
    tag->use_connected_msg = (cip_entry_use_connected_msg(entry, attribs) ? 1 : 0);

    /*
     * Handed back even when this function goes on to fail, so the caller can release it.
     * The error itself travels in the return value from here on.
     */
    *tag_out = (plc_tag_p)tag;

    /*
     * The element type, and the special tags.
     *
     * A special tag sets its own vtable and byte order here, and the leaf below fills in the
     * family's defaults only where it finds none, so this has to run first.
     */
    rc = get_tag_data_type(tag, attribs);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Error %s getting tag element data type or handling special tag!",
               plc_tag_decode_error(rc));
        return rc;
    }

    /*
     * Everything Rockwell shares one answer about fragmentation and services, so set it
     * once here rather than in each leaf.  Tags that use one of the special-purpose engines
     * instead never read it.
     */
    tag->std_ops = &cip_standard_tag_ops_ab;
    tag->debug_module = DEBUG_MODULE_AB_EIP_CIP;

    rc = entry->setup_tag(entry, tag, attribs);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Unable to set up %s tag!", entry->name);
        return rc;
    }

    rc = cip_tag_check_setup(tag, entry->name, &default_vtable, DEBUG_MODULE_AB_COMMON);
    if(rc != PLCTAG_STATUS_OK) {
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "using session=%p", tag->session);

    rc = cip_tag_resolve_elem_count(tag, attribs, DEBUG_MODULE_AB_COMMON);
    if(rc != PLCTAG_STATUS_OK) {
        return rc;
    }

    switch(tag->plc_type) {
        case CIP_PLC_LGX:
            /* fall through */
        case CIP_PLC_MICRO800:
            /* fall through */
        case CIP_PLC_GENERIC:
            /* the PLC reports the size on the first read. */
            rc = cip_tag_size_from_plc(tag, DEBUG_MODULE_AB_COMMON);
            break;

        default:
            /* every other family states its size through elem_size. */
            rc = cip_tag_size_from_attributes(tag, attribs, DEBUG_MODULE_AB_COMMON);
            break;
    }

    if(rc != PLCTAG_STATUS_OK) {
        return rc;
    }

    /*
     * check the tag name, this is protocol specific.
     */

    if(!tag->special_tag && check_tag_name(tag, attr_get_str(attribs, "name", NULL)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Bad tag name!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* kick off a read to get the tag type and size. */
    if(!tag->special_tag && tag->vtable->read && tag->first_read) {
        /* trigger the first read. */
        /* convoluted logic that needs to be refactored */

        /* if this is a PLC that does not need first reads to get the type, then we do not trigger an
        early read during creation.   We do, however, need to flag that the read is the first one so
        that the created event will be raised. Ugh.  This is a mess. */
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Kicking off initial read.");

        tag->read_in_flight = 1;
        tag->vtable->read((plc_tag_p)tag);
    } else {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Tag does not need a first read, raising created event.");

        /* force the created event because we do not do an initial read here. */
        tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, tag->status);
    }

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Using vtable %p.", tag->vtable);

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * determine the tag's data type and size.  Or at least guess it.
 */

/*
 * Fill in what nobody else already has.
 *
 * A special tag -- @tags, @udt, @identity, @raw -- has its vtable and byte order set while
 * its name is parsed and must keep them, so every family's defaults are conditional.  A
 * NULL vtable argument means this family has no default and whatever parsed the name was
 * required to supply one.
 */
static void apply_tag_defaults(cip_tag_p tag, tag_byte_order_t *byte_order, tag_vtable_p vtable) {
    if(!tag->byte_order) { tag->byte_order = byte_order; }

    if(vtable && (tag->vtable == &default_vtable || !tag->vtable)) { tag->vtable = vtable; }
}


/*
 * The three PCCC families, direct or bridged.
 *
 * Only one thing differs across their six rows: the byte order, which the two leaves below
 * supply.  Whether the request is connected follows from the dialect alone -- a DH+ bridge
 * always needs a connection, a direct PCCC device never uses one -- so it is data on the row
 * and settled before the connection is created.
 */
static int setup_pccc_tag(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs,
                          tag_byte_order_t *byte_order) {
    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "Setting up %s tag.", entry->name);

    if(entry->dialect == CIP_DIALECT_DIRECT && str_length(attr_get_str(attribs, "path", NULL))) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id,
               "A path is not supported for this PLC type if it is not for a DH+ bridge.");
    }

    tag->allow_packing = 0;
    tag->first_read = 0; /* the data file in the name states the type, so there is nothing to discover. */

    apply_tag_defaults(tag, byte_order, &pccc_vtable);

    return PLCTAG_STATUS_OK;
}


int ab_setup_plc5_tag(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs) {
    return setup_pccc_tag(entry, tag, attribs, &plc5_tag_byte_order);
}


int ab_setup_slc_tag(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs) {
    /* SLC 500 and MicroLogix share a layout, which is why they share a leaf. */
    return setup_pccc_tag(entry, tag, attribs, &slc_tag_byte_order);
}


int ab_setup_lgx_pccc_tag(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs) {
    (void)attribs;

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "Setting up %s tag.", entry->name);

    /*
     * A Logix reached through its PCCC mapping speaks SLC file addressing over a routed,
     * unconnected request.  It is not DH+ capable, so it has one row and no dialect to
     * consider.
     */
    tag->allow_packing = 0;
    tag->first_read = 0;

    apply_tag_defaults(tag, &slc_tag_byte_order, &lgx_pccc_vtable);

    return PLCTAG_STATUS_OK;
}


int ab_setup_logix_tag(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs) {
    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "Setting up %s tag.", entry->name);

    /* packing is wanted by default and can be turned off; the connection is settled by the row. */
    tag->allow_packing = attr_get_int(attribs, "allow_packing", 1);

    /* the first read is what discovers the element type and size. */
    tag->first_read = 1;

    apply_tag_defaults(tag, &logix_tag_byte_order, &cip_standard_tag_vtable_ab);

    return PLCTAG_STATUS_OK;
}


int ab_setup_micro800_tag(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs) {
    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "Setting up %s tag.", entry->name);

    if(str_length(attr_get_str(attribs, "path", NULL))) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "A path is not supported for this PLC type.");
    }

    /* a Micro800 cannot pack requests; its row says it only answers over a connection. */
    tag->allow_packing = 0;
    tag->first_read = 1;

    apply_tag_defaults(tag, &cip_short_string_byte_order, &cip_standard_tag_vtable_ab);

    return PLCTAG_STATUS_OK;
}


int ab_setup_generic_tag(const cip_plc_entry_t *entry, cip_tag_p tag, attr attribs) {
    const char *path = attr_get_str(attribs, "path", NULL);

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "Setting up %s tag.", entry->name);

    /* a generic device may be reached through a chassis path, unlike the families above. */
    if(str_length(path)) { pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "Using path: %s", path); }

    /* every generic operation is stateless, so there is nothing to pack. */
    tag->allow_packing = 0;
    tag->first_read = 0;

    /*
     * No default vtable.  A generic tag is only ever one of the special tags -- @identity,
     * @tags, @udt, @raw -- and whichever one it is set the vtable while the name was parsed.
     * Supplying a fallback here would hide a name this family cannot serve.
     */
    apply_tag_defaults(tag, &logix_tag_byte_order, NULL);

    return PLCTAG_STATUS_OK;
}


int get_tag_data_type(cip_tag_p tag, attr attribs) {
    int rc = PLCTAG_STATUS_OK;
    const char *elem_type = NULL;
    const char *tag_name = NULL;
    pccc_addr_t file_addr = {0};

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Starting.");

    switch(tag->plc_type) {
        case CIP_PLC_PLC5:
        case CIP_PLC_SLC:
        case CIP_PLC_LGX_PCCC:
        case CIP_PLC_MLGX:
            tag_name = attr_get_str(attribs, "name", NULL);

            rc = parse_pccc_logical_address(tag_name, &file_addr);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Unable to parse data file %s!", tag_name);
                return rc;
            }

            tag->elem_size = file_addr.element_size_bytes;
            tag->file_type = file_addr.file_type;

            break;

        case CIP_PLC_LGX:
        case CIP_PLC_MICRO800:
            // case CIP_PLC_OMRON_NJNX:
            /* look for the elem_type attribute. */
            elem_type = attr_get_str(attribs, "elem_type", NULL);
            if(elem_type) {
                if(str_cmp_i(elem_type, "lint") == 0 || str_cmp_i(elem_type, "ulint") == 0) {
                    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of 64-bit integer.");
                    tag->elem_size = 8;
                    tag->elem_type = CIP_TYPE_INT64;
                } else if(str_cmp_i(elem_type, "dint") == 0 || str_cmp_i(elem_type, "udint") == 0) {
                    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of 32-bit integer.");
                    tag->elem_size = 4;
                    tag->elem_type = CIP_TYPE_INT32;
                } else if(str_cmp_i(elem_type, "int") == 0 || str_cmp_i(elem_type, "uint") == 0) {
                    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of 16-bit integer.");
                    tag->elem_size = 2;
                    tag->elem_type = CIP_TYPE_INT16;
                } else if(str_cmp_i(elem_type, "sint") == 0 || str_cmp_i(elem_type, "usint") == 0) {
                    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of 8-bit integer.");
                    tag->elem_size = 1;
                    tag->elem_type = CIP_TYPE_INT8;
                } else if(str_cmp_i(elem_type, "bool") == 0) {
                    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of bit.");
                    tag->elem_size = 1;
                    tag->elem_type = CIP_TYPE_BOOL;
                } else if(str_cmp_i(elem_type, "bool array") == 0) {
                    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of bool array.");
                    tag->elem_size = 4;
                    tag->elem_type = CIP_TYPE_BOOL_ARRAY;
                } else if(str_cmp_i(elem_type, "real") == 0) {
                    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of 32-bit float.");
                    tag->elem_size = 4;
                    tag->elem_type = CIP_TYPE_FLOAT32;
                } else if(str_cmp_i(elem_type, "lreal") == 0) {
                    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of 64-bit float.");
                    tag->elem_size = 8;
                    tag->elem_type = CIP_TYPE_FLOAT64;
                } else if(str_cmp_i(elem_type, "string") == 0) {
                    /*
                     * A CIP STRING and a CIP SHORT_STRING are both a count word followed by
                     * exactly that many characters, so neither has an element size -- see
                     * tag_elements_are_variable() in eip_cip.c.  One byte, as the tag listing
                     * and @udt tags use for the same reason.  This used to be 88 and 256, the
                     * Logix STRING UDT size and the SHORT_STRING worst case respectively.
                     */
                    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of string.");
                    tag->elem_size = 1;
                    tag->elem_type = CIP_TYPE_STRING;
                } else if(str_cmp_i(elem_type, "short string") == 0) {
                    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Found tag element type of short string.");
                    tag->elem_size = 1;
                    tag->elem_type = CIP_TYPE_SHORT_STRING;
                } else {
                    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Unknown tag type %s", elem_type);
                    return PLCTAG_ERR_UNSUPPORTED;
                }
            } else {
                /*
                 * We have two cases
                 *      * tag listing, but only for CIP PLCs (but not for UDTs!).
                 *      * no type, just elem_size.
                 * Otherwise this is an error.
                 */
                int elem_size = attr_get_int(attribs, "elem_size", 0);
                int cip_plc =
                    !!(tag->plc_type == CIP_PLC_LGX || tag->plc_type == CIP_PLC_MICRO800 /*|| tag->plc_type == CIP_PLC_OMRON_NJNX*/);

                if(cip_plc) {
                    const char *tmp_tag_name = attr_get_str(attribs, "name", NULL);
                    int special_tag_rc = PLCTAG_STATUS_OK;

                    /* check for special tags. */
                    if(str_cmp_i(tmp_tag_name, "@raw") == 0) {
                        special_tag_rc = setup_raw_tag(tag);
                    } else if(str_str_cmp_i(tmp_tag_name, "@tags")) {
                        special_tag_rc = setup_tag_listing_tag(tag, tmp_tag_name);
                    } else if(str_str_cmp_i(tmp_tag_name, "@udt/")) {
                        special_tag_rc = setup_udt_tag(tag, tmp_tag_name);
                    } /* else not a special tag. */

                    if(special_tag_rc != PLCTAG_STATUS_OK) {
                        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Error parsing tag listing name!");
                        return special_tag_rc;
                    }
                }

                /* if we did not set an element size yet, set one. */
                if(tag->elem_size == 0) {
                    if(elem_size > 0) {
                        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_INFO, 0, "Setting element size to %d.", elem_size);
                        tag->elem_size = elem_size;
                    }
                } else {
                    if(elem_size > 0) {
                        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0,
                               "Tag has elem_size and either is a tag listing or has elem_type, only use one!");
                    }
                }
            }

            break;

        case CIP_PLC_GENERIC:
            /* Generic PLC type only supports special tags like @identity */
            tag_name = attr_get_str(attribs, "name", NULL);

            if(str_cmp_i(tag_name, "@identity") == 0) {
                rc = setup_identity_tag(tag);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Error setting up identity tag!");
                    return rc;
                }
            } else {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Generic PLC type only supports @identity tag, got: %s", tag_name);
                return PLCTAG_ERR_UNSUPPORTED;
            }

            break;

        default:
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, 0, "Unknown PLC type!");
            return PLCTAG_ERR_BAD_DEVICE;
            break;
    }

    pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}





/*
 * cip_tag_abort_request_only
 *
 * clean up the tag state for the request but not the offset.
 */

/*
 * cip_tag_abort_request
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


int check_tag_name(cip_tag_p tag, const char *name) {
    int rc = PLCTAG_STATUS_OK;
    pccc_addr_t pccc_address;

    if(!name) {
        pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "No tag name parameter found!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    mem_set(&pccc_address, 0, sizeof(pccc_address));

    /* attempt to parse the tag name */
    switch(tag->plc_type) {
        case CIP_PLC_PLC5:
        case CIP_PLC_LGX_PCCC:
            if((rc = parse_pccc_logical_address(name, &pccc_address))) {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Parse of PCCC-style tag name %s failed!", name);
                return rc;
            }

            if(pccc_address.is_bit) {
                tag->is_bit = 1;
                tag->bit = (int)(unsigned int)pccc_address.bit;
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "PLC/5 address references bit %d.", tag->bit);
            }

            if((rc = plc5_encode_address(tag->encoded_name, &(tag->encoded_name_size), MAX_TAG_NAME, &pccc_address))
               != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Encoding of PLC/5-style tag name %s failed!", name);
                return rc;
            }

            break;

        case CIP_PLC_SLC:
        case CIP_PLC_MLGX:
            if((rc = parse_pccc_logical_address(name, &pccc_address))) {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Parse of PCCC-style tag name %s failed!", name);
                return rc;
            }

            if(pccc_address.is_bit) {
                tag->is_bit = 1;
                tag->bit = (int)(unsigned int)pccc_address.bit;
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_DETAIL, tag->tag_id, "SLC/Micrologix address references bit %d.", tag->bit);
            }

            if((rc = slc_encode_address(tag->encoded_name, &(tag->encoded_name_size), MAX_TAG_NAME, &pccc_address))
               != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "Encoding of SLC-style tag name %s failed!", name);
                return rc;
            }

            break;

        case CIP_PLC_MICRO800:
        case CIP_PLC_LGX:
            // case CIP_PLC_OMRON_NJNX:
            if((rc = cip_encode_tag_name(tag, name)) != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "parse of CIP-style tag name %s failed!", name);

                return rc;
            }

            break;

        case CIP_PLC_GENERIC:
            /* Generic PLC type only supports special tags like @identity */
            /* Special tag handling is done elsewhere, just validate the name format */
            if(name[0] != '@') {
                pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id,
                       "Generic PLC type only supports special tags starting with @, got: %s", name);
                return PLCTAG_ERR_UNSUPPORTED;
            }

            /* Placeholder - actual tag handling is done in special tag setup */
            break;

        default:
            /* how would we get here? */
            pdebug(DEBUG_MODULE_AB_COMMON, DEBUG_WARN, tag->tag_id, "unsupported PLC type %d", tag->plc_type);

            return PLCTAG_ERR_BAD_PARAM;

            break;
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

                                                        .encode_read_offset = cip_encode_offset_u32};


/* the standard (symbolic, named) tag type.  The engine is shared; see modules/cip/standard_tag.c. */
struct tag_vtable_t cip_standard_tag_vtable_ab = {
    .abort = (tag_vtable_func)cip_tag_abort_request,
    .read = (tag_vtable_func)cip_standard_tag_read_start,
    .status = (tag_vtable_func)cip_tag_status,
    .tickler = (tag_vtable_func)cip_standard_tag_tickler,
    .write = (tag_vtable_func)cip_standard_tag_write_start,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .attribs = ab_attribs,
};


static plc_tag_p ab_connection_tag_create(attr attribs,
                                          void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                                          void *userdata, plc_tag_p src_tag) {
    /*
     * AB adopts a connection from an OMRON tag as well as its own: both families hang the
     * same cip_conn_p off the same field, and a user who already has one should not be
     * made to open a second.  The BAD_GATEWAY override is long-standing behaviour -- AB
     * reports it for any find-or-create failure, where OMRON passes the real error up.
     */
    static const cip_connection_tag_ops_t ops = {.connection_protocol = TAG_PROTOCOL_AB_CONNECTION,
                                                 .debug_module = DEBUG_MODULE_AB_CONNECTION,
                                                 .source_protocols = {TAG_PROTOCOL_AB, TAG_PROTOCOL_OMRON},
                                                 .find_or_create = session_find_or_create,
                                                 .create_failure_rc = PLCTAG_ERR_BAD_GATEWAY};

    return cip_connection_tag_create(attribs, &ops, tag_callback_func, userdata, src_tag);
}


static int setup_raw_tag(cip_tag_p tag) { return cip_raw_tag_setup(tag, &raw_tag_vtable, DEBUG_MODULE_AB_EIP_CIP_SPECIAL); }
