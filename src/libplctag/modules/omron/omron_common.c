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
#include <libplctag/lib/tag.h>
#include <libplctag/modules/cip/standard_tag.h>
#include <libplctag/modules/cip/tag.h>
#include <libplctag/modules/cip/wire.h>
#include <libplctag/modules/omron/conn.h>
#include <libplctag/modules/omron/omron.h>
#include <libplctag/modules/omron/omron_common.h>
#include <libplctag/modules/omron/omron_connection_tag.h>
#include <libplctag/modules/omron/omron_raw_tag.h>
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
volatile thread_p omron_conn_handler_thread = NULL;

volatile int omron_protocol_terminating = 0;


/*
 * Generic Rockwell/Allen-Bradley protocol functions.
 *
 * These are the primary entry points into the AB protocol
 * stack.
 */


#define DEFAULT_NUM_RETRIES (5)
#define DEFAULT_RETRY_INTERVAL (300)


/* forward declarations*/
static cip_plc_type_t get_plc_type(attr attribs);
static int get_tag_data_type(cip_tag_p tag, attr attribs);
static int check_cpu(cip_tag_p tag, attr attribs);
static int check_tag_name(cip_tag_p tag, const char *name);

static void omron_tag_destroy(cip_tag_p tag);
static int default_abort(plc_tag_p tag);
static int default_read(plc_tag_p tag);
static int default_status(plc_tag_p tag);
static int default_tickler(plc_tag_p tag);
static int default_write(plc_tag_p tag);

/* vtables for different kinds of tags */
static struct tag_vtable_t default_vtable = {
    .abort = default_abort,
    .read = default_read,
    .status = default_status,
    .tickler = default_tickler,
    .write = default_write,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .attribs = omron_attribs,
};


/*
 * Public functions.
 */


int omron_init(void) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, 0, "Initializing Omron CIP protocol library.");

    omron_protocol_terminating = 0;

    if((rc = conn_startup()) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_ERROR, 0, "Unable to initialize conn library!");
        return rc;
    }

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, 0, "Finished initializing AB protocol library.");

    return rc;
}

/*
 * called when the whole program is going to terminate.
 */
void omron_teardown(void) {
    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, 0, "Releasing global Omron CIP protocol resources.");

    if(omron_conn_handler_thread) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, 0, "Terminating IO thread.");
        /* signal the IO thread to quit first. */
        omron_protocol_terminating = 1;

        /* wait for the thread to die */
        thread_join(omron_conn_handler_thread);
        thread_destroy((thread_p *)&omron_conn_handler_thread);
    } else {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, 0, "IO thread already stopped.");
    }

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, 0, "Freeing conn information.");

    conn_teardown();

    omron_protocol_terminating = 0;

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, 0, "Done.");
}


plc_tag_p omron_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                           void *userdata, plc_tag_p src_tag) {
    cip_tag_p tag = NULL;
    const char *path = NULL;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, 0, "Starting.");

    if(str_cmp(attr_get_str(attribs, "name", ""), "@connection") == 0) {
        return omron_connection_tag_create(attribs, tag_callback_func, userdata, src_tag);
    }

    /*
     * allocate memory for the new tag.  Do this first so that
     * we have a vehicle for returning status.
     */

    tag = (cip_tag_p)rc_alloc(sizeof(struct cip_tag_t), (rc_cleanup_func)omron_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_ERROR, 0, "Unable to allocate memory for AB EIP tag!");
        return (plc_tag_p)NULL;
    }

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "tag=%p", tag);

    /*
     * we got far enough to allocate memory, set the default vtable up
     * in case we need to abort later.
     */

    tag->vtable = &default_vtable;
    tag->protocol_type = TAG_PROTOCOL_OMRON;

    /* set up the generic parts. */
    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "Unable to initialize generic tag parts!");
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".",
               tag->tag_id);
        rc_dec(tag);
        return (plc_tag_p)NULL;
    }

    /*
     * check the CPU type.
     *
     * This determines the protocol type.
     */

    if(src_tag) {
        switch(src_tag->protocol_type) {
            case TAG_PROTOCOL_OMRON: tag->plc_type = ((cip_tag_p)src_tag)->plc_type; break;

            case TAG_PROTOCOL_OMRON_CONNECTION: {
                cip_conn_p src_conn = (cip_conn_p)((connection_tag_p)src_tag)->conn;
                tag->plc_type = src_conn ? src_conn->plc_type : CIP_PLC_NONE;
                break;
            }

            default: tag->plc_type = CIP_PLC_NONE; break;
        }
    } else {
        if(check_cpu(tag, attribs) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "CPU type not valid or missing.");
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "rc_dec: Releasing reference to tag %" PRId32 ".",
                   tag->tag_id);
            rc_dec(tag);
            return (plc_tag_p)NULL;
        }
    }

    /* set up any required settings based on the PLC type. */
    tag->use_connected_msg = 1;

    /* make sure that the connection requirement is forced. */
    attr_set_int(attribs, "use_connected_msg", tag->use_connected_msg);

    /* get the connection path.  We need this to make a decision about the PLC. */
    path = attr_get_str(attribs, "path", NULL);

    /*
     * Find or create a conn.
     *
     * All tags need conns.  They are the TCP connection to the gateway PLC.
     */
    if(src_tag) {
        switch(src_tag->protocol_type) {
            case TAG_PROTOCOL_OMRON: tag->session = rc_inc(((cip_tag_p)src_tag)->session); break;

            case TAG_PROTOCOL_OMRON_CONNECTION: {
                tag->session = rc_inc((cip_conn_p)((connection_tag_p)src_tag)->conn);
                break;
            }

            default: tag->session = NULL; break;
        }

        if(!tag->session) {
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, tag->tag_id, "Unable to reuse source conn!");
            tag->status = PLCTAG_ERR_NOT_FOUND;
            return (plc_tag_p)tag;
        }
    } else {
        if(conn_find_or_create(&tag->session, attribs, NULL) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, tag->tag_id, "Unable to create conn!");
            tag->status = PLCTAG_ERR_BAD_GATEWAY;
            return (plc_tag_p)tag;
        }
    }

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "using conn=%p", tag->session);

    /* get the tag data type, or try. */
    rc = get_tag_data_type(tag, attribs);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id,
               "Error %s getting tag element data type or handling special tag!", plc_tag_decode_error(rc));
        tag->status = (int8_t)rc;
        return (plc_tag_p)tag;
    }

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Setting up OMRON NJ/NX Series tag.");

    if(!src_tag && str_length(path) == 0) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "A path is required for this PLC type.");
        tag->status = PLCTAG_ERR_BAD_PARAM;
        return (plc_tag_p)tag;
    }

    /* if we did not fill in the byte order elsewhere, fill it in now. */
    if(!tag->byte_order) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Using default Omron byte order.");
        tag->byte_order = &omron_njnx_tag_byte_order;
    }

    /* if this was not filled in elsewhere default to generic *Logix */
    if(tag->vtable == &default_vtable || !tag->vtable) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Setting default Logix vtable.");
        tag->vtable = &cip_standard_tag_vtable_omron;
    }

    tag->use_connected_msg = 1;
    tag->allow_packing = attr_get_int(attribs, "allow_packing", 0);

    /* pass the connection requirement since it may be overridden above. */
    attr_set_int(attribs, "use_connected_msg", tag->use_connected_msg);

    /* get the element count, default to 1 if missing. */
    tag->elem_count = attr_get_int(attribs, "elem_count", 1);

    /*
     * An array can legitimately have up to INT32_MAX elements, but it cannot have zero or a
     * negative number of them.  Several places divide the tag size by this to recover the
     * element size, so a zero here is a division by zero later.
     */
    if(tag->elem_count < 1) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "Element count must be at least one, was %d!",
               tag->elem_count);
        tag->status = PLCTAG_ERR_BAD_PARAM;
        return (plc_tag_p)tag;
    }

    /*
     * The CIP read and write services carry the element count in a two-byte field, and every
     * place that builds one casts to uint16_t explicitly, so nothing downstream objects to a
     * larger value -- the count simply wraps and the PLC is asked for a different number of
     * elements than the caller asked for.  There is no size check here of the kind ab_common.c
     * has because an Omron tag learns its size from the PLC rather than from the attributes.
     */
    if(tag->elem_count > UINT16_MAX) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "Element count must be no more than %d, was %d!", UINT16_MAX,
               tag->elem_count);
        tag->status = PLCTAG_ERR_TOO_LARGE;
        return (plc_tag_p)tag;
    }

    tag->size = 0;
    tag->data = NULL;

    /*
     * check the tag name, this is protocol specific.
     */

    if(!tag->special_tag && check_tag_name(tag, attr_get_str(attribs, "name", NULL)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, tag->tag_id, "Bad tag name!");
        tag->status = PLCTAG_ERR_BAD_PARAM;
        return (plc_tag_p)tag;
    }

    /* kick off a read to get the tag type and size. */
    if(!tag->special_tag && tag->vtable->read) {
        /* trigger the first read. */
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Kicking off initial read.");

        tag->first_read = 1;
        tag->read_in_flight = 1;
        tag->vtable->read((plc_tag_p)tag);
        // tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_READ_STARTED, tag->status);
    } else {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id,
               "Not kicking off initial read: tag is special or does not have read function.");

        /* force the created event because we do not do an initial read here. */
        tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, tag->status);
    }

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Using vtable %p.", tag->vtable);

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, tag->tag_id, "Done.");

    return (plc_tag_p)tag;
}


/*
 * determine the tag's data type and size.  Or at least guess it.
 */

/*
 * An Omron string has no element size.
 *
 * It is a two-byte count, that many characters and a zero terminator, packed at its actual
 * length with no padding out to a capacity -- omron_njnx_tag_byte_order says as much with
 * str_is_fixed_length of zero, str_max_capacity of zero and str_total_length of zero.  An
 * array of them therefore has no stride, and the library's string layer reaches element N by
 * walking the count words of the N before it rather than by multiplying.
 *
 * So the string types below take an element size of one byte, the same answer the tag
 * listing and @udt tags give for the same reason.  It used to be 88 here, which is the Logix
 * STRING layout of a four-byte count, 82 characters and two pad bytes -- a different PLC
 * family's structure, and one that contradicts the byte order a few lines away in
 * omron_standard_tag.c.
 */
int get_tag_data_type(cip_tag_p tag, attr attribs) {
    const char *elem_type = NULL;

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Starting.");

    /* look for the elem_type attribute. */
    elem_type = attr_get_str(attribs, "elem_type", NULL);
    if(elem_type) {
        if(str_cmp_i(elem_type, "lint") == 0 || str_cmp_i(elem_type, "ulint") == 0) {
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Found tag element type of 64-bit integer.");
            tag->elem_size = 8;
            tag->elem_type = CIP_TYPE_INT64;
        } else if(str_cmp_i(elem_type, "dint") == 0 || str_cmp_i(elem_type, "udint") == 0) {
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Found tag element type of 32-bit integer.");
            tag->elem_size = 4;
            tag->elem_type = CIP_TYPE_INT32;
        } else if(str_cmp_i(elem_type, "int") == 0 || str_cmp_i(elem_type, "uint") == 0) {
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Found tag element type of 16-bit integer.");
            tag->elem_size = 2;
            tag->elem_type = CIP_TYPE_INT16;
        } else if(str_cmp_i(elem_type, "sint") == 0 || str_cmp_i(elem_type, "usint") == 0) {
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Found tag element type of 8-bit integer.");
            tag->elem_size = 1;
            tag->elem_type = CIP_TYPE_INT8;
        } else if(str_cmp_i(elem_type, "bool") == 0) {
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Found tag element type of bit.");
            tag->elem_size = 1;
            tag->elem_type = CIP_TYPE_BOOL;
        } else if(str_cmp_i(elem_type, "bool array") == 0) {
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Found tag element type of bool array.");
            tag->elem_size = 4;
            tag->elem_type = CIP_TYPE_BOOL_ARRAY;
        } else if(str_cmp_i(elem_type, "real") == 0) {
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Found tag element type of 32-bit float.");
            tag->elem_size = 4;
            tag->elem_type = CIP_TYPE_FLOAT32;
        } else if(str_cmp_i(elem_type, "lreal") == 0) {
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Found tag element type of 64-bit float.");
            tag->elem_size = 8;
            tag->elem_type = CIP_TYPE_FLOAT64;
        } else if(str_cmp_i(elem_type, "string") == 0) {
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Found tag element type of string.");
            tag->elem_size = 1;
            tag->elem_type = CIP_TYPE_STRING;
        } else if(str_cmp_i(elem_type, "short string") == 0) {
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Found tag element type of short string.");
            tag->elem_size = 1;
            tag->elem_type = CIP_TYPE_SHORT_STRING;
        } else {
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Unknown tag type %s", elem_type);
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
        const char *tmp_tag_name = attr_get_str(attribs, "name", NULL);
        int special_tag_rc = PLCTAG_STATUS_OK;

        /* check for special tags. */
        if(str_cmp_i(tmp_tag_name, "@raw") == 0) { special_tag_rc = omron_setup_raw_tag(tag); }

        // else if(str_str_cmp_i(tmp_tag_name, "@tags")) {
        //         special_tag_rc = omron_setup_tag_listing_tag(tag, tmp_tag_name);
        // } else if(str_str_cmp_i(tmp_tag_name, "@udt/")) {
        //         special_tag_rc = omron_setup_udt_tag(tag, tmp_tag_name);
        // } /* else not a special tag. */

        if(special_tag_rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "Error parsing tag listing name!");
            return special_tag_rc;
        }

        /* if we did not set an element size yet, set one. */
        if(tag->elem_size == 0) {
            if(elem_size > 0) {
                pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, tag->tag_id, "Setting element size to %d.", elem_size);
                tag->elem_size = elem_size;
            }
        } else {
            if(elem_size > 0) {
                pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id,
                       "Tag has elem_size and either is a tag listing or has elem_type, only use one!");
            }
        }
    }

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


int default_abort(plc_tag_p tag) {
    (void)tag;

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}


int default_read(plc_tag_p tag) {
    (void)tag;

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}

int default_status(plc_tag_p tag) {
    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    if(tag) {
        return tag->status;
    } else {
        return PLCTAG_ERR_NOT_FOUND;
    }
}


int default_tickler(plc_tag_p tag) {
    (void)tag;

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    return PLCTAG_STATUS_OK;
}


int default_write(plc_tag_p tag) {
    (void)tag;

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
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
 * omron_tag_abort
 *
 * This does the work of stopping any inflight requests.
 * This is not thread-safe.  It must be called from a function
 * that locks the tag's mutex or only from a single thread.
 */

int omron_tag_abort(cip_tag_p tag) {
    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Starting.");

    if(tag) {
        cip_request_p req = NULL;

        critical_block(tag->api_mutex) { req = rc_inc(tag->req); }

        if(req) {
            spin_block(&req->lock) { atomic_set_int32(&req->abort_request, 1); }

            /* do a real abort */
            cip_tag_abort_request(tag);

            req = rc_dec(req);
        } else {
            /* do a real abort even if there's no current request */
            cip_tag_abort_request(tag);
        }

        tag->status = PLCTAG_ERR_ABORT;
        return tag->status;
    } else {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, 0, "Called with a null tag pointer.");
    }

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * omron_tag_status
 *
 * Generic status checker.   May be overridden by individual PLC types.
 */
int omron_tag_status(cip_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    if(tag->read_in_progress) { return PLCTAG_STATUS_PENDING; }

    if(tag->write_in_progress) { return PLCTAG_STATUS_PENDING; }

    if(tag->session) {
        rc = tag->status;
    } else {
        /* this is not OK.  This is fatal! */
        rc = PLCTAG_ERR_CREATE;
    }

    return rc;
}


/*
 * omron_tag_destroy
 *
 * This blocks on the global library mutex.  This should
 * be fixed to allow for more parallelism.  For now, safety is
 * the primary concern.
 */

void omron_tag_destroy(cip_tag_p tag) {
    cip_conn_p conn = NULL;

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, tag->tag_id, "Starting.");

    /* already destroyed? */
    if(!tag) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "Tag pointer is null!");

        return;
    }

    /* abort anything in flight */
    omron_tag_abort(tag);

    conn = tag->session;

    /* tags should always have a conn.  Release it. */
    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "Getting ready to release tag conn %p", tag->session);
    if(conn) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, tag->tag_id, "rc_dec: Releasing reference to conn of tag %" PRId32 ".",
               tag->tag_id);
        tag->session = rc_dec(tag->session);
    } else {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "No conn pointer!");
    }

    if(tag->ext_mutex) {
        mutex_destroy(&(tag->ext_mutex));
        tag->ext_mutex = NULL;
    }

    if(tag->api_mutex) {
        mutex_destroy(&(tag->api_mutex));
        tag->api_mutex = NULL;
    }

    if(tag->tag_cond_wait) {
        cond_destroy(&(tag->tag_cond_wait));
        tag->tag_cond_wait = NULL;
    }

    if(tag->byte_order && tag->byte_order->is_allocated) {
        mem_free(tag->byte_order);
        tag->byte_order = NULL;
    }

    if(tag->data) {
        mem_free(tag->data);
        tag->data = NULL;
    }

    if(tag->instance) {
        rc_dec(tag->instance);
        tag->instance = NULL;
    }

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, tag->tag_id, "Finished releasing all tag resources.");

    pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_INFO, tag->tag_id, "done");
}


/* Attribute accessors for the table below.  An accessor returns a status, or for a byte
 * array the number of bytes copied, and never touches tag->status -- the core records it. */

static int32_t omron_get_elem_size(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    *result = (int32_t)tag->elem_size;

    return PLCTAG_STATUS_OK;
}


static int32_t omron_get_elem_count(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    *result = (int32_t)tag->elem_count;

    return PLCTAG_STATUS_OK;
}


static int32_t omron_get_elem_type(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    *result = (int32_t)(tag->elem_type);

    return PLCTAG_STATUS_OK;
}


static int32_t omron_get_connection_status(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    /* no connection means the tag is not connected, which is a state and not an error. */
    *result = (tag->session ? atomic_get_int32(&tag->session->watch.status) : (int32_t)PLCTAG_CONN_STATUS_DOWN);

    return PLCTAG_STATUS_OK;
}


static int32_t omron_get_connection_inactivity_timeout_ms(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    /* no connection means the connection that will be created uses the default. */
    *result =
        (tag->session ? atomic_get_int32(&tag->session->connection_inactivity_timeout_ms) : (int32_t)SESSION_DISCONNECT_TIMEOUT);

    return PLCTAG_STATUS_OK;
}


static int32_t omron_set_connection_inactivity_timeout_ms(plc_tag_p raw_tag, int32_t value) {
    cip_tag_p tag = (cip_tag_p)raw_tag;
    int32_t clamped_value = value;
    int32_t rc = PLCTAG_STATUS_OK;

    /* Clamp to valid range: 100ms minimum, SESSION_DISCONNECT_TIMEOUT (31000ms) maximum */
    if(clamped_value < 100) {
        clamped_value = 100;
        rc = PLCTAG_ERR_OUT_OF_BOUNDS;
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id,
               "connection_inactivity_timeout_ms value %d clamped to minimum 100ms.", (int)value);
    } else if(clamped_value > SESSION_DISCONNECT_TIMEOUT) {
        clamped_value = SESSION_DISCONNECT_TIMEOUT;
        rc = PLCTAG_ERR_OUT_OF_BOUNDS;
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id,
               "connection_inactivity_timeout_ms value %d clamped to maximum %d ms.", (int)value, SESSION_DISCONNECT_TIMEOUT);
    }

    if(!tag->session) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id,
               "Cannot set connection_inactivity_timeout_ms: no connection exists.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    atomic_set_int32(&tag->session->connection_inactivity_timeout_ms, clamped_value);

    return rc;
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


static int32_t omron_get_plc(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    return attr_copy_string(omron_plc_type_name(tag->plc_type), buffer, buffer_length);
}


static int32_t omron_get_plc_size(plc_tag_p raw_tag) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    return attr_string_size(omron_plc_type_name(tag->plc_type));
}


static int32_t omron_get_use_connected_msg(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    *result = (int32_t)tag->use_connected_msg;

    return PLCTAG_STATUS_OK;
}


static int32_t omron_get_allow_packing(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    *result = (int32_t)tag->allow_packing;

    return PLCTAG_STATUS_OK;
}


static int32_t omron_get_gateway(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    return attr_copy_string(tag->session->host, buffer, buffer_length);
}


static int32_t omron_get_gateway_size(plc_tag_p raw_tag) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    return attr_string_size(tag->session->host);
}


static int32_t omron_get_gateway_port(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    *result = (int32_t)tag->session->port;

    return PLCTAG_STATUS_OK;
}


/* The encoded CIP path, not the "18,127.0.0.1" text it was built from. */
static int32_t omron_get_path(plc_tag_p raw_tag, uint8_t *buffer, int32_t buffer_length) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    if((int32_t)tag->session->conn_path_size > buffer_length) { return PLCTAG_ERR_TOO_SMALL; }

    mem_copy((void *)buffer, (void *)tag->session->conn_path, (int)tag->session->conn_path_size);

    return (int32_t)tag->session->conn_path_size;
}


static int32_t omron_get_path_size(plc_tag_p raw_tag) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    return (int32_t)tag->session->conn_path_size;
}


static int32_t omron_get_conn_only_use_old_forward_open(plc_tag_p raw_tag, int32_t *result) {
    cip_tag_p tag = (cip_tag_p)raw_tag;

    if(!tag->session) { return PLCTAG_ERR_NOT_FOUND; }

    *result = (int32_t)tag->session->only_use_old_forward_open;

    return PLCTAG_STATUS_OK;
}


const attr_def_t omron_attribs[] = {
    {.name = "elem_size",
     .type = ATTR_TYPE_INT,
     .description = "The size in bytes of a single element of this tag.",
     .get_int = omron_get_elem_size},

    {.name = "elem_count",
     .type = ATTR_TYPE_INT,
     .description = "The number of elements this tag holds.",
     .get_int = omron_get_elem_count},

    {.name = "elem_type",
     .type = ATTR_TYPE_INT,
     .description = "The PLC's type code for a single element of this tag.",
     .get_int = omron_get_elem_type},

    {.name = "connection_status",
     .type = ATTR_TYPE_INT,
     .description = "The state of the connection this tag uses, as a plc_tag_conn_status_t.",
     .get_int = omron_get_connection_status},

    {.name = "connection_inactivity_timeout_ms",
     .type = ATTR_TYPE_INT,
     .description = "Disconnect the connection after this many milliseconds without traffic.",
     .get_int = omron_get_connection_inactivity_timeout_ms,
     .set_int = omron_set_connection_inactivity_timeout_ms},

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
     .get_int = omron_get_use_connected_msg},

    {.name = "allow_packing",
     .type = ATTR_TYPE_INT,
     .description = "This tag's requests may be packed with others into one CIP request.",
     .get_int = omron_get_allow_packing},

    {.name = "gateway",
     .type = ATTR_TYPE_STRING,
     .description = "The host name or address of the gateway this tag's connection uses.",
     .get_bytes = omron_get_gateway,
     .get_bytes_size = omron_get_gateway_size},

    {.name = "gateway_port",
     .type = ATTR_TYPE_INT,
     .description = "The TCP port this tag's connection uses.",
     .get_int = omron_get_gateway_port},

    {.name = "path",
     .type = ATTR_TYPE_BYTES,
     .description = "The encoded CIP path from the gateway to the PLC.",
     .get_bytes = omron_get_path,
     .get_bytes_size = omron_get_path_size},

    {.name = "conn_only_use_old_forward_open",
     .type = ATTR_TYPE_INT,
     .description = "This tag's connection uses the original Forward Open only, never the large one.",
     .get_int = omron_get_conn_only_use_old_forward_open},

    {.name = NULL},
};


static cip_plc_type_t get_plc_type(attr attribs) {
    const char *cpu_type = attr_get_str(attribs, "plc", attr_get_str(attribs, "cpu", "NONE"));

    if(!str_cmp_i(cpu_type, "omron-njnx") || !str_cmp_i(cpu_type, "omron-nj") || !str_cmp_i(cpu_type, "omron-nx")
       || !str_cmp_i(cpu_type, "njnx") || !str_cmp_i(cpu_type, "nx1p2")) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_DETAIL, 0, "Found OMRON NJ/NX Series PLC.");
        return CIP_PLC_OMRON_NJNX;
    } else {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, 0, "Unsupported device type: %s", cpu_type);

        return CIP_PLC_NONE;
    }
}


int check_cpu(cip_tag_p tag, attr attribs) {
    cip_plc_type_t result = get_plc_type(attribs);

    if(result == CIP_PLC_OMRON_NJNX) {
        tag->plc_type = result;
        return PLCTAG_STATUS_OK;
    } else {
        tag->plc_type = result;
        return PLCTAG_ERR_BAD_DEVICE;
    }
}

int check_tag_name(cip_tag_p tag, const char *name) {
    int rc = PLCTAG_STATUS_OK;

    if(!name) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "No tag name parameter found!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* attempt to parse the tag name */
    if((rc = cip_encode_tag_name(tag, name)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_OMRON_COMMON, DEBUG_WARN, tag->tag_id, "parse of CIP-style tag name %s failed!", name);

        return rc;
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


/* the standard (symbolic, named) tag type.  The engine is shared; see modules/cip/standard_tag.c. */
struct tag_vtable_t cip_standard_tag_vtable_omron = {
    .abort = (tag_vtable_func)omron_tag_abort,
    .read = (tag_vtable_func)cip_standard_tag_read_start,
    .status = (tag_vtable_func)omron_tag_status,
    .tickler = (tag_vtable_func)cip_standard_tag_tickler,
    .write = (tag_vtable_func)cip_standard_tag_write_start,
    .wake_plc = NULL,
    .tag_data_written = NULL,

    /* attribute accessors */
    .attribs = omron_attribs,
};
