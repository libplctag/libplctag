/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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
#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/omron/cip.h>
#include <libplctag/protocols/omron/conn.h>
#include <libplctag/protocols/omron/defs.h>
#include <libplctag/protocols/omron/omron.h>
#include <libplctag/protocols/omron/omron_common.h>
#include <libplctag/protocols/omron/omron_raw_tag.h>
#include <libplctag/protocols/omron/omron_standard_tag.h>
#include <libplctag/protocols/omron/tag.h>
#include <limits.h>
#include <platform.h>
#include <utils/atomic_utils.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/vector.h>


/*
 * Externally visible global variables
 */

// volatile omron_conn_p conns = NULL;
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
static plc_type_t get_plc_type(attr attribs);
static int get_tag_data_type(omron_tag_p tag, attr attribs);
static int check_cpu(omron_tag_p tag, attr attribs);
static int check_tag_name(omron_tag_p tag, const char *name);

static void omron_tag_destroy(omron_tag_p tag);
static int default_abort(plc_tag_p tag);
static int default_read(plc_tag_p tag);
static int default_status(plc_tag_p tag);
static int default_tickler(plc_tag_p tag);
static int default_write(plc_tag_p tag);


/* vtables for different kinds of tags */
static struct tag_vtable_t default_vtable = {default_abort, default_read, default_status, default_tickler, default_write,
                                             (tag_vtable_func)NULL, /* this is not portable! */

                                             /* attribute accessors */
                                             omron_get_int_attrib, omron_set_int_attrib,

                                             omron_get_byte_array_attrib};


/*
 * Public functions.
 */


int omron_init(void) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Initializing Omron CIP protocol library.");

    omron_protocol_terminating = 0;

    if((rc = conn_startup()) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to initialize conn library!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Finished initializing AB protocol library.");

    return rc;
}

/*
 * called when the whole program is going to terminate.
 */
void omron_teardown(void) {
    pdebug(DEBUG_INFO, "Releasing global Omron CIP protocol resources.");

    if(omron_conn_handler_thread) {
        pdebug(DEBUG_INFO, "Terminating IO thread.");
        /* signal the IO thread to quit first. */
        omron_protocol_terminating = 1;

        /* wait for the thread to die */
        thread_join(omron_conn_handler_thread);
        thread_destroy((thread_p *)&omron_conn_handler_thread);
    } else {
        pdebug(DEBUG_INFO, "IO thread already stopped.");
    }

    pdebug(DEBUG_INFO, "Freeing conn information.");

    conn_teardown();

    omron_protocol_terminating = 0;

    pdebug(DEBUG_INFO, "Done.");
}


plc_tag_p omron_tag_create(attr attribs, void (*tag_callback_func)(int32_t tag_id, int event, int status, void *userdata),
                           void *userdata) {
    omron_tag_p tag = OMRON_TAG_NULL;
    const char *path = NULL;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    /*
     * allocate memory for the new tag.  Do this first so that
     * we have a vehicle for returning status.
     */

    tag = (omron_tag_p)rc_alloc(sizeof(struct omron_tag_t), (rc_cleanup_func)omron_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_ERROR, "Unable to allocate memory for AB EIP tag!");
        return (plc_tag_p)NULL;
    }

    pdebug(DEBUG_DETAIL, "tag=%p", tag);

    /*
     * we got far enough to allocate memory, set the default vtable up
     * in case we need to abort later.
     */

    tag->vtable = &default_vtable;

    /* set up the generic parts. */
    rc = plc_tag_generic_init_tag((plc_tag_p)tag, attribs, tag_callback_func, userdata);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to initialize generic tag parts!");
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return (plc_tag_p)NULL;
    }

    /*
     * check the CPU type.
     *
     * This determines the protocol type.
     */

    if(check_cpu(tag, attribs) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "CPU type not valid or missing.");
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to tag %" PRId32 ".", tag->tag_id);
        rc_dec(tag);
        return (plc_tag_p)NULL;
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
    if(conn_find_or_create(&tag->conn, attribs) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Unable to create conn!");
        tag->status = PLCTAG_ERR_BAD_GATEWAY;
        return (plc_tag_p)tag;
    }

    pdebug(DEBUG_DETAIL, "using conn=%p", tag->conn);

    /* get the tag data type, or try. */
    rc = get_tag_data_type(tag, attribs);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error %s getting tag element data type or handling special tag!", plc_tag_decode_error(rc));
        tag->status = (int8_t)rc;
        return (plc_tag_p)tag;
    }

    pdebug(DEBUG_DETAIL, "Setting up OMRON NJ/NX Series tag.");

    if(str_length(path) == 0) {
        pdebug(DEBUG_WARN, "A path is required for this PLC type.");
        tag->status = PLCTAG_ERR_BAD_PARAM;
        return (plc_tag_p)tag;
    }

    /* if we did not fill in the byte order elsewhere, fill it in now. */
    if(!tag->byte_order) {
        pdebug(DEBUG_DETAIL, "Using default Omron byte order.");
        tag->byte_order = &omron_njnx_tag_byte_order;
    }

    /* if this was not filled in elsewhere default to generic *Logix */
    if(tag->vtable == &default_vtable || !tag->vtable) {
        pdebug(DEBUG_DETAIL, "Setting default Logix vtable.");
        tag->vtable = &omron_standard_tag_vtable;
    }

    tag->use_connected_msg = 1;
    tag->allow_packing = attr_get_int(attribs, "allow_packing", 0);
    tag->supports_fragmented_read = 0; /* fragmented read is not currently supported */

    /* pass the connection requirement since it may be overridden above. */
    attr_set_int(attribs, "use_connected_msg", tag->use_connected_msg);

    /* get the element count, default to 1 if missing. */
    tag->elem_count = attr_get_int(attribs, "elem_count", 1);

    tag->size = 0;
    tag->data = NULL;

    /*
     * check the tag name, this is protocol specific.
     */

    if(!tag->special_tag && check_tag_name(tag, attr_get_str(attribs, "name", NULL)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Bad tag name!");
        tag->status = PLCTAG_ERR_BAD_PARAM;
        return (plc_tag_p)tag;
    }

    /* kick off a read to get the tag type and size. */
    if(!tag->special_tag && tag->vtable->read) {
        /* trigger the first read. */
        pdebug(DEBUG_DETAIL, "Kicking off initial read.");

        tag->first_read = 1;
        tag->read_in_flight = 1;
        tag->vtable->read((plc_tag_p)tag);
        // tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_READ_STARTED, tag->status);
    } else {
        pdebug(DEBUG_DETAIL, "Not kicking off initial read: tag is special or does not have read function.");

        /* force the created event because we do not do an initial read here. */
        tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, tag->status);
    }

    pdebug(DEBUG_DETAIL, "Using vtable %p.", tag->vtable);

    pdebug(DEBUG_INFO, "Done.");

    return (plc_tag_p)tag;
}


/*
 * determine the tag's data type and size.  Or at least guess it.
 */

int get_tag_data_type(omron_tag_p tag, attr attribs) {
    const char *elem_type = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* look for the elem_type attribute. */
    elem_type = attr_get_str(attribs, "elem_type", NULL);
    if(elem_type) {
        if(str_cmp_i(elem_type, "lint") == 0 || str_cmp_i(elem_type, "ulint") == 0) {
            pdebug(DEBUG_DETAIL, "Found tag element type of 64-bit integer.");
            tag->elem_size = 8;
            tag->elem_type = OMRON_TYPE_INT64;
        } else if(str_cmp_i(elem_type, "dint") == 0 || str_cmp_i(elem_type, "udint") == 0) {
            pdebug(DEBUG_DETAIL, "Found tag element type of 32-bit integer.");
            tag->elem_size = 4;
            tag->elem_type = OMRON_TYPE_INT32;
        } else if(str_cmp_i(elem_type, "int") == 0 || str_cmp_i(elem_type, "uint") == 0) {
            pdebug(DEBUG_DETAIL, "Found tag element type of 16-bit integer.");
            tag->elem_size = 2;
            tag->elem_type = OMRON_TYPE_INT16;
        } else if(str_cmp_i(elem_type, "sint") == 0 || str_cmp_i(elem_type, "usint") == 0) {
            pdebug(DEBUG_DETAIL, "Found tag element type of 8-bit integer.");
            tag->elem_size = 1;
            tag->elem_type = OMRON_TYPE_INT8;
        } else if(str_cmp_i(elem_type, "bool") == 0) {
            pdebug(DEBUG_DETAIL, "Found tag element type of bit.");
            tag->elem_size = 1;
            tag->elem_type = OMRON_TYPE_BOOL;
        } else if(str_cmp_i(elem_type, "bool array") == 0) {
            pdebug(DEBUG_DETAIL, "Found tag element type of bool array.");
            tag->elem_size = 4;
            tag->elem_type = OMRON_TYPE_BOOL_ARRAY;
        } else if(str_cmp_i(elem_type, "real") == 0) {
            pdebug(DEBUG_DETAIL, "Found tag element type of 32-bit float.");
            tag->elem_size = 4;
            tag->elem_type = OMRON_TYPE_FLOAT32;
        } else if(str_cmp_i(elem_type, "lreal") == 0) {
            pdebug(DEBUG_DETAIL, "Found tag element type of 64-bit float.");
            tag->elem_size = 8;
            tag->elem_type = OMRON_TYPE_FLOAT64;
        } else if(str_cmp_i(elem_type, "string") == 0) {
            /* FIXME - is this correct? */
            pdebug(DEBUG_DETAIL, "Fount tag element type of string.");
            tag->elem_size = 88;
            tag->elem_type = OMRON_TYPE_STRING;
        } else if(str_cmp_i(elem_type, "short string") == 0) {
            pdebug(DEBUG_DETAIL, "Found tag element type of short string.");
            tag->elem_size = 256; /* TODO - find the real length */
            tag->elem_type = OMRON_TYPE_SHORT_STRING;
        } else {
            pdebug(DEBUG_DETAIL, "Unknown tag type %s", elem_type);
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
            pdebug(DEBUG_WARN, "Error parsing tag listing name!");
            return special_tag_rc;
        }

        /* if we did not set an element size yet, set one. */
        if(tag->elem_size == 0) {
            if(elem_size > 0) {
                pdebug(DEBUG_INFO, "Setting element size to %d.", elem_size);
                tag->elem_size = elem_size;
            }
        } else {
            if(elem_size > 0) {
                pdebug(DEBUG_WARN, "Tag has elem_size and either is a tag listing or has elem_type, only use one!");
            }
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int default_abort(plc_tag_p tag) {
    (void)tag;

    pdebug(DEBUG_WARN, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}


int default_read(plc_tag_p tag) {
    (void)tag;

    pdebug(DEBUG_WARN, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}

int default_status(plc_tag_p tag) {
    pdebug(DEBUG_WARN, "This should be overridden by a PLC-specific function!");

    if(tag) {
        return tag->status;
    } else {
        return PLCTAG_ERR_NOT_FOUND;
    }
}


int default_tickler(plc_tag_p tag) {
    (void)tag;

    pdebug(DEBUG_WARN, "This should be overridden by a PLC-specific function!");

    return PLCTAG_STATUS_OK;
}


int default_write(plc_tag_p tag) {
    (void)tag;

    pdebug(DEBUG_WARN, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}


/*
 * omron_tag_abort_request_only
 *
 * clean up the tag state for the request but not the offset.
 */

int omron_tag_abort_request_only(omron_tag_p tag) {
    pdebug(DEBUG_DETAIL, "Starting.");

    if(tag) {
        omron_request_p req = NULL;

        critical_block(tag->api_mutex) { req = rc_inc(tag->req); }

        if(req) {
            spin_block(&req->lock) { req->abort_request = 1; }

            pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to request of tag %" PRId32 ".", tag->tag_id);
            critical_block(tag->api_mutex) {
                if(tag->req != req) { pdebug(DEBUG_WARN, "Request got changed out from underneath us!"); }
                if(tag->req == req) { tag->req = rc_dec(tag->req); }
            }

            req = rc_dec(req);
        } else {
            pdebug(DEBUG_DETAIL, "Called without a request in flight.");
        }

        tag->read_in_progress = 0;
        tag->write_in_progress = 0;
    } else {
        pdebug(DEBUG_DETAIL, "Called with a null tag pointer.");
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}

/*
 * omron_tag_abort_request
 *
 * This does the work of stopping any inflight requests.
 * This is not thread-safe.  It must be called from a function
 * that locks the tag's mutex or only from a single thread.
 */

int omron_tag_abort_request(omron_tag_p tag) {
    pdebug(DEBUG_DETAIL, "Starting.");

    if(tag) {
        tag->offset = 0;

        omron_tag_abort_request_only(tag);
    } else {
        pdebug(DEBUG_DETAIL, "Called with a null tag pointer.");
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * omron_tag_abort
 *
 * This does the work of stopping any inflight requests.
 * This is not thread-safe.  It must be called from a function
 * that locks the tag's mutex or only from a single thread.
 */

int omron_tag_abort(omron_tag_p tag) {
    pdebug(DEBUG_DETAIL, "Starting.");

    if(tag) {
        omron_request_p req = NULL;

        critical_block(tag->api_mutex) { req = rc_inc(tag->req); }

        if(req) {
            spin_block(&req->lock) { req->abort_request = 1; }
            req = rc_dec(req);
        }

        /* do a real abort */
        omron_tag_abort_request(tag);

        tag->status = PLCTAG_ERR_ABORT;
        return tag->status;
    } else {
        pdebug(DEBUG_DETAIL, "Called with a null tag pointer.");
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * omron_tag_status
 *
 * Generic status checker.   May be overridden by individual PLC types.
 */
int omron_tag_status(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    if(tag->read_in_progress) { return PLCTAG_STATUS_PENDING; }

    if(tag->write_in_progress) { return PLCTAG_STATUS_PENDING; }

    if(tag->conn) {
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

void omron_tag_destroy(omron_tag_p tag) {
    omron_conn_p conn = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    /* already destroyed? */
    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is null!");

        return;
    }

    /* abort anything in flight */
    omron_tag_abort(tag);

    conn = tag->conn;

    /* tags should always have a conn.  Release it. */
    pdebug(DEBUG_DETAIL, "Getting ready to release tag conn %p", tag->conn);
    if(conn) {
        pdebug(DEBUG_DETAIL, "rc_dec: Releasing reference to conn of tag %" PRId32 ".", tag->tag_id);
        tag->conn = rc_dec(tag->conn);
    } else {
        pdebug(DEBUG_WARN, "No conn pointer!");
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

    pdebug(DEBUG_INFO, "Finished releasing all tag resources.");

    pdebug(DEBUG_INFO, "done");
}


int omron_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value) {
    int res = default_value;
    omron_tag_p tag = (omron_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* assume we have a match. */
    tag->status = PLCTAG_STATUS_OK;

    /* match the attribute. */
    if(str_cmp_i(attrib_name, "elem_size") == 0) {
        res = tag->elem_size;
    } else if(str_cmp_i(attrib_name, "elem_count") == 0) {
        res = tag->elem_count;
    } else if(str_cmp_i(attrib_name, "elem_type") == 0) {
        res = (int)(tag->elem_type);
    } else if(str_cmp_i(attrib_name, "raw_tag_type_bytes.length") == 0) {
        res = (int)(tag->encoded_type_info_size);
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        tag->status = PLCTAG_ERR_UNSUPPORTED;
    }

    return res;
}


int omron_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value) {
    (void)attrib_name;
    (void)new_value;

    pdebug(DEBUG_WARN, "Unsupported attribute \"%s\"!", attrib_name);

    raw_tag->status = PLCTAG_ERR_UNSUPPORTED;

    return PLCTAG_ERR_UNSUPPORTED;
}

int omron_get_byte_array_attrib(plc_tag_p raw_tag, const char *attrib_name, uint8_t *buffer, int buffer_length) {
    int rc = PLCTAG_STATUS_OK;
    omron_tag_p tag = (omron_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* assume we have a match. */
    tag->status = PLCTAG_STATUS_OK;

    /* match the attribute. */
    if(str_cmp_i(attrib_name, "raw_tag_type_bytes") == 0) {
        if(tag->encoded_type_info_size > buffer_length) {
            pdebug(DEBUG_WARN, "Tag type info is larger, %d bytes, than the buffer can hold, %d bytes.",
                   tag->encoded_type_info_size, buffer_length);
            rc = PLCTAG_ERR_TOO_SMALL;
        } else if(tag->encoded_type_info_size <= buffer_length) {
            pdebug(DEBUG_INFO, "Copying %d bytes of tag type information.", tag->encoded_type_info_size, buffer_length);

            /* copy the data */
            mem_copy((void *)buffer, (void *)&(tag->encoded_type_info[0]), tag->encoded_type_info_size);

            /* return the number of bytes copied */
            rc = tag->encoded_type_info_size;
        }
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        tag->status = PLCTAG_ERR_UNSUPPORTED;
    }

    return rc;
}


static plc_type_t get_plc_type(attr attribs) {
    const char *cpu_type = attr_get_str(attribs, "plc", attr_get_str(attribs, "cpu", "NONE"));

    if(!str_cmp_i(cpu_type, "omron-njnx") || !str_cmp_i(cpu_type, "omron-nj") || !str_cmp_i(cpu_type, "omron-nx")
       || !str_cmp_i(cpu_type, "njnx") || !str_cmp_i(cpu_type, "nx1p2")) {
        pdebug(DEBUG_DETAIL, "Found OMRON NJ/NX Series PLC.");
        return OMRON_PLC_OMRON_NJNX;
    } else {
        pdebug(DEBUG_WARN, "Unsupported device type: %s", cpu_type);

        return OMRON_PLC_NONE;
    }
}


int check_cpu(omron_tag_p tag, attr attribs) {
    plc_type_t result = get_plc_type(attribs);

    if(result == OMRON_PLC_OMRON_NJNX) {
        tag->plc_type = result;
        return PLCTAG_STATUS_OK;
    } else {
        tag->plc_type = result;
        return PLCTAG_ERR_BAD_DEVICE;
    }
}

int check_tag_name(omron_tag_p tag, const char *name) {
    int rc = PLCTAG_STATUS_OK;

    if(!name) {
        pdebug(DEBUG_WARN, "No tag name parameter found!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* attempt to parse the tag name */
    if((rc = CIP.encode_tag_name(tag, name)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "parse of CIP-style tag name %s failed!", name);

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

int omron_check_request_status(omron_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    omron_request_p req = NULL;
    eip_encap *eip_header = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    do {
        if(!tag) {
            pdebug(DEBUG_WARN, "Called with null tag pointer!");
            rc = PLCTAG_ERR_NULL_PTR;
            break;
        }

        /* do we have an abort outstanding? */
        if(atomic_get_bool(&tag->abort_requested)) {
            omron_tag_abort_request(tag);
            atomic_set_bool(&tag->abort_requested, 0);
            rc = PLCTAG_ERR_ABORT;
            break;
        }

        critical_block(tag->api_mutex) { req = rc_inc(tag->req); }

        if(!req) {
            if(tag->read_in_progress || tag->write_in_progress) {
                tag->read_in_progress = 0;
                tag->write_in_progress = 0;
                tag->offset = 0;

                pdebug(DEBUG_WARN, "A request was in progress, but no request in flight!");
            }

            rc = PLCTAG_STATUS_OK;
            break;
        }

        /* request can be used by more than one thread at once. */
        spin_block(&req->lock) {
            if(!req->resp_received) {
                rc = PLCTAG_STATUS_PENDING;
                break;
            }

            /* check to see if it was an abort on the session side. */
            if(req->status != PLCTAG_STATUS_OK) {
                rc = req->status;
                break;
            }
        }

        /* check the length */
        if((req->request_size < 0) || (size_t)req->request_size < sizeof(*eip_header)) {
            pdebug(DEBUG_WARN, "Insufficient data returned for even an EIP header!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        eip_header = (eip_encap *)(req->data);

        if(le2h32(eip_header->encap_status) != OMRON_EIP_OK) {
            pdebug(DEBUG_WARN, "EIP command failed, response code: %d", le2h32(eip_header->encap_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        switch(le2h16(eip_header->encap_command)) {
            case OMRON_EIP_CONNECTED_SEND: pdebug(DEBUG_WARN, "Received a connected send EIP packet."); break;
            case OMRON_EIP_UNCONNECTED_SEND: pdebug(DEBUG_WARN, "Received an unconnected send EIP packet."); break;
            default:
                pdebug(DEBUG_WARN, "Received an unknown EIP packet type %04" PRIx16 ".", le2h16(eip_header->encap_command));
                rc = PLCTAG_ERR_BAD_DATA;
                break;
        }
    } while(0);

    if(req) { req = rc_dec(req); }

    if(rc_is_error(rc)) {
        /* the request is dead, from session side. */
        omron_tag_abort(tag);

        pdebug(DEBUG_INFO, "Response not OK with status %s.", plc_tag_decode_error(rc));
    }

    tag->status = (int8_t)rc;

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}
