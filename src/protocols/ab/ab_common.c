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
#include <limits.h>
#include <float.h>
#include <platform.h>
#include <lib/libplctag.h>
#include <lib/tag.h>
#include <ab/ab.h>
#include <ab/ab_common.h>
#include <ab/pccc.h>
#include <ab/cip.h>
#include <ab/defs.h>
#include <ab/eip_cip.h>
#include <ab/eip_cip_special.h>
#include <ab/eip_lgx_pccc.h>
#include <ab/eip_plc5_pccc.h>
#include <ab/eip_plc5_dhp.h>
#include <ab/eip_slc_pccc.h>
#include <ab/eip_slc_dhp.h>
#include <ab/session.h>
#include <ab/tag.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/vector.h>


/*
 * Externally visible global variables
 */

//volatile ab_session_p sessions = NULL;
//volatile mutex_p global_session_mut = NULL;
//
//volatile vector_p read_group_tags = NULL;


/* request/response handling thread */
volatile thread_p io_handler_thread = NULL;

volatile int ab_protocol_terminating = 0;



/*
 * Generic Rockwell/Allen-Bradley protocol functions.
 *
 * These are the primary entry points into the AB protocol
 * stack.
 */


#define DEFAULT_NUM_RETRIES (5)
#define DEFAULT_RETRY_INTERVAL (300)


/* forward declarations*/
static int get_tag_data_type(ab_tag_p tag, attr attribs);

static void ab_tag_destroy(ab_tag_p tag);
static int default_abort(plc_tag_p tag);
static int default_read(plc_tag_p tag);
static int default_status(plc_tag_p tag);
static int default_tickler(plc_tag_p tag);
static int default_write(plc_tag_p tag);


/* vtables for different kinds of tags */
struct tag_vtable_t default_vtable = {
    default_abort,
    default_read,
    default_status,
    default_tickler,
    default_write,

    /* attribute accessors */
    ab_get_int_attrib,
    ab_set_int_attrib
};


/*
 * Public functions.
 */


int ab_init(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO,"Initializing AB protocol library.");

    ab_protocol_terminating = 0;

    if((rc = session_startup()) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to initialize session library!");
        return rc;
    }

    pdebug(DEBUG_INFO,"Finished initializing AB protocol library.");

    return rc;
}

/*
 * called when the whole program is going to terminate.
 */
void ab_teardown(void)
{
    pdebug(DEBUG_INFO,"Releasing global AB protocol resources.");

    if(io_handler_thread) {
        pdebug(DEBUG_INFO,"Terminating IO thread.");
        /* signal the IO thread to quit first. */
        ab_protocol_terminating = 1;

        /* wait for the thread to die */
        thread_join(io_handler_thread);
        thread_destroy((thread_p*)&io_handler_thread);
    } else {
        pdebug(DEBUG_INFO, "IO thread already stopped.");
    }

    pdebug(DEBUG_INFO,"Freeing session information.");

    session_teardown();

    ab_protocol_terminating = 0;

    pdebug(DEBUG_INFO,"Done.");
}



plc_tag_p ab_tag_create(attr attribs)
{
    ab_tag_p tag = AB_TAG_NULL;
    const char *path = NULL;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO,"Starting.");

    /*
     * allocate memory for the new tag.  Do this first so that
     * we have a vehicle for returning status.
     */

    tag = (ab_tag_p)rc_alloc(sizeof(struct ab_tag_t), (rc_cleanup_func)ab_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_ERROR,"Unable to allocate memory for AB EIP tag!");
        return (plc_tag_p)NULL;
    }

    pdebug(DEBUG_DETAIL, "tag=%p", tag);

    /*
     * we got far enough to allocate memory, set the default vtable up
     * in case we need to abort later.
     */

    tag->vtable = &default_vtable;

    /*
     * check the CPU type.
     *
     * This determines the protocol type.
     */

    if(check_cpu(tag, attribs) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"CPU type not valid or missing.");
        /* tag->status = PLCTAG_ERR_BAD_DEVICE; */
        rc_dec(tag);
        return (plc_tag_p)NULL;
    }

    /* set up any required settings based on the cpu type. */
    switch(tag->plc_type) {
    case AB_PLC_PLC5:
        tag->use_connected_msg = 0;
        tag->allow_packing = 0;
        break;

    case AB_PLC_SLC:
        tag->use_connected_msg = 0;
        tag->allow_packing = 0;
        break;

    case AB_PLC_MLGX:
        tag->use_connected_msg = 0;
        tag->allow_packing = 0;
        break;

    case AB_PLC_LGX_PCCC:
        tag->use_connected_msg = 0;
        tag->allow_packing = 0;
        break;

    case AB_PLC_LGX:
        /* default to requiring a connection and allowing packing. */
        tag->use_connected_msg = attr_get_int(attribs,"use_connected_msg", 1);
        tag->allow_packing = attr_get_int(attribs, "allow_packing", 1);
        break;

    case AB_PLC_MICRO800:
        /* we must use connected messaging here. */
        pdebug(DEBUG_DETAIL, "Micro800 needs connected messaging.");
        tag->use_connected_msg = 1;

        /* Micro800 cannot pack requests. */
        tag->allow_packing = 0;
        break;

    case AB_PLC_OMRON_NJNX:
        tag->use_connected_msg = 1;
        tag->allow_packing = 0;
        break;

    default:
        pdebug(DEBUG_WARN, "Unknown PLC type!");
        tag->status = PLCTAG_ERR_BAD_CONFIG;
        return (plc_tag_p)tag;
        break;
    }

    /* make sure that the connection requirement is forced. */
    attr_set_int(attribs, "use_connected_msg", tag->use_connected_msg);

    /* get the connection path.  We need this to make a decision about the PLC. */
    path = attr_get_str(attribs,"path",NULL);

    /*
     * Find or create a session.
     *
     * All tags need sessions.  They are the TCP connection to the gateway PLC.
     */
    if(session_find_or_create(&tag->session, attribs) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO,"Unable to create session!");
        tag->status = PLCTAG_ERR_BAD_GATEWAY;
        return (plc_tag_p)tag;
    }

    pdebug(DEBUG_DETAIL, "using session=%p", tag->session);

    /* get the tag data type, or try. */
    rc = get_tag_data_type(tag, attribs);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error getting tag element data type %s!", plc_tag_decode_error(rc));
        tag->status = (int8_t)rc;
        return (plc_tag_p)tag;
    }

    /* set up PLC-specific information. */
    switch(tag->plc_type) {
    case AB_PLC_PLC5:
        if(!tag->session->dhp_dest) {
            pdebug(DEBUG_DETAIL, "Setting up PLC/5 tag.");

            if(str_length(path)) {
                pdebug(DEBUG_WARN, "A path is not supported for this PLC type if it is not for a DH+ bridge.");
            }

            tag->use_connected_msg = 0;
            tag->vtable = &plc5_vtable;
        } else {
            pdebug(DEBUG_DETAIL, "Setting up PLC/5 via DH+ bridge tag.");
            tag->use_connected_msg = 1;
            tag->vtable = &eip_plc5_dhp_vtable;
        }

        tag->byte_order = &plc5_tag_byte_order;

        tag->allow_packing = 0;
        break;

    case AB_PLC_SLC:
    case AB_PLC_MLGX:
        if(!tag->session->dhp_dest) {

            if(str_length(path)) {
                pdebug(DEBUG_WARN, "A path is not supported for this PLC type if it is not for a DH+ bridge.");
            }

            pdebug(DEBUG_DETAIL, "Setting up SLC/MicroLogix tag.");
            tag->use_connected_msg = 0;
            tag->vtable = &slc_vtable;
        } else {
            pdebug(DEBUG_DETAIL, "Setting up SLC/MicroLogix via DH+ bridge tag.");
            tag->use_connected_msg = 1;
            tag->vtable = &eip_slc_dhp_vtable;
        }

        tag->byte_order = &slc_tag_byte_order;

        tag->allow_packing = 0;
        break;

    case AB_PLC_LGX_PCCC:
        pdebug(DEBUG_DETAIL, "Setting up PCCC-mapped Logix tag.");
        tag->use_connected_msg = 0;
        tag->allow_packing = 0;
        tag->vtable = &lgx_pccc_vtable;

        tag->byte_order = &slc_tag_byte_order;

        break;

    case AB_PLC_LGX:
        pdebug(DEBUG_DETAIL, "Setting up Logix tag.");

        /* Logix tags need a path. */
        if(path == NULL && tag->plc_type == AB_PLC_LGX) {
            pdebug(DEBUG_WARN,"A path is required for Logix-class PLCs!");
            tag->status = PLCTAG_ERR_BAD_PARAM;
            return (plc_tag_p)tag;
        }

        /* if we did not fill in the byte order elsewhere, fill it in now. */
        if(!tag->byte_order) {
            pdebug(DEBUG_DETAIL, "Using default Logix vtable.");
            // if(!tag->tag_list) {
                tag->byte_order = &logix_tag_byte_order;
            // } else {
            //     tag->byte_order = &logix_tag_listing_byte_order;
            // }
        }

        if(!tag->tag_list) {
            tag->byte_order = &logix_tag_byte_order;
        } else {
            tag->byte_order = &logix_tag_listing_byte_order;
        }

        /* default to requiring a connection. */
        tag->use_connected_msg = attr_get_int(attribs,"use_connected_msg", 1);
        tag->allow_packing = attr_get_int(attribs, "allow_packing", 1);

        /* if this was not filled in elsewhere default to Logix */
        if(!tag->vtable) {
            pdebug(DEBUG_DETAIL, "Setting default Logix vtable.");
            tag->vtable = &eip_cip_vtable;
        }

        break;

    case AB_PLC_MICRO800:
        pdebug(DEBUG_DETAIL, "Setting up Micro8X0 tag.");

        if(path || str_length(path)) {
            pdebug(DEBUG_WARN, "A path is not supported for this PLC type.");
        }

        tag->byte_order = &logix_tag_byte_order;

        tag->use_connected_msg = 1;
        tag->allow_packing = 0;
        tag->vtable = &eip_cip_vtable;
        break;

    case AB_PLC_OMRON_NJNX:
        pdebug(DEBUG_DETAIL, "Setting up OMRON NJ/NX Series tag.");

        if(str_length(path) == 0) {
            pdebug(DEBUG_WARN,"A path is required for this PLC type.");
            tag->status = PLCTAG_ERR_BAD_PARAM;
            return (plc_tag_p)tag;
        }

        tag->byte_order = &logix_tag_byte_order;

        tag->use_connected_msg = 1;
        tag->allow_packing = 0;
        tag->vtable = &eip_cip_vtable;
        break;

    default:
        pdebug(DEBUG_WARN, "Unknown PLC type!");
        tag->status = PLCTAG_ERR_BAD_CONFIG;
        return (plc_tag_p)tag;
    }

    /* pass the connection requirement since it may be overridden above. */
    attr_set_int(attribs, "use_connected_msg", tag->use_connected_msg);

    /* get the element count, default to 1 if missing. */
    tag->elem_count = attr_get_int(attribs,"elem_count", 1);

    switch(tag->plc_type) {
    case AB_PLC_OMRON_NJNX:
        if (tag->elem_count != 1) {
            tag->elem_count = 1;
            pdebug(DEBUG_WARN,"Attribute elem_count should be 1!");
        }

        /* from here is the same as a AB_PLC_MICRO800. */

        /* fall through */
    case AB_PLC_LGX:
    case AB_PLC_MICRO800:
        /* fill this in when we read the tag. */
        //tag->elem_size = 0;
        tag->size = 0;
        tag->data = NULL;
        break;

    default:
        /* we still need size on non Logix-class PLCs */
        /* get the element size if it is not already set. */
        if(!tag->elem_size) {
            tag->elem_size = attr_get_int(attribs, "elem_size", 0);
        }

        /* Determine the tag size */
        tag->size = (tag->elem_count) * (tag->elem_size);
        if(tag->size == 0) {
            /* failure! Need data_size! */
            pdebug(DEBUG_WARN,"Tag size is zero!");
            tag->status = PLCTAG_ERR_BAD_PARAM;
            return (plc_tag_p)tag;
        }

        /* this may be changed in the future if this is a tag list request. */
        tag->data = (uint8_t*)mem_alloc(tag->size);

        if(tag->data == NULL) {
            pdebug(DEBUG_WARN,"Unable to allocate tag data!");
            tag->status = PLCTAG_ERR_NO_MEM;
            return (plc_tag_p)tag;
        }
        break;
    }

    /*
     * check the tag name, this is protocol specific.
     */

    if(!tag->special_tag && check_tag_name(tag, attr_get_str(attribs,"name",NULL)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO,"Bad tag name!");
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
    }

    pdebug(DEBUG_INFO,"Done.");

    return (plc_tag_p)tag;
}


/*
 * determine the tag's data type and size.  Or at least guess it.
 */

int get_tag_data_type(ab_tag_p tag, attr attribs)
{
    const char *elem_type = NULL;
    const char *tag_name = NULL;

    pdebug(DEBUG_DETAIL, "Starting.");

    switch(tag->plc_type) {
    case AB_PLC_PLC5:
    case AB_PLC_SLC:
    case AB_PLC_LGX_PCCC:
    case AB_PLC_MLGX:
        tag_name = attr_get_str(attribs,"name", NULL);

        /* FIXME - rewrite this into a function depending on the PLC type. */

        /* the first two characters are the important ones. */
        if(tag_name && str_length(tag_name) >= 2) {
            switch(tag_name[0]) {
            case 'b':
            case 'B':
                /*bit*/
                pdebug(DEBUG_DETAIL,"Found tag element type of bit.");
                tag->elem_size=2;
                tag->elem_type = AB_TYPE_BOOL;
                break;

            case 'c':
            case 'C':
                /* counter */
                pdebug(DEBUG_DETAIL,"Found tag element type of counter.");
                tag->elem_size=6;
                tag->elem_type = AB_TYPE_COUNTER;
                break;

            case 'f':
            case 'F':
                /* 32-bit float */
                pdebug(DEBUG_DETAIL,"Found tag element type of float.");
                tag->elem_size=4;
                tag->elem_type = AB_TYPE_FLOAT32;
                break;

            case 'l':
            case 'L':
                /* 32-bit integer */
                pdebug(DEBUG_DETAIL,"Found tag element type of long int.");
                tag->elem_size=4;
                tag->elem_type = AB_TYPE_INT32;
                break;

            case 'n':
            case 'N':
                /* 16-bit integer */
                pdebug(DEBUG_DETAIL,"Found tag element type of 16-bit integer.");
                tag->elem_size=2;
                tag->elem_type = AB_TYPE_INT16;
                break;

            case 'r':
            case 'R':
                /* control */
                pdebug(DEBUG_DETAIL,"Found tag element type of control.");
                tag->elem_size=6;
                tag->elem_type = AB_TYPE_CONTROL;
                break;

            case 's':
            case 'S':
                /* Status or String */
                if(tag_name[1] == 't' || tag_name[1] == 'T') {
                    /* string */
                    pdebug(DEBUG_DETAIL,"Found tag element type of string.");
                    tag->elem_size = 84;
                    tag->elem_type = AB_TYPE_STRING;
                } else {
                    /* status */
                    pdebug(DEBUG_DETAIL,"Found tag element type of status word.");
                    tag->elem_size = 2;
                    tag->elem_type = AB_TYPE_INT16;
                }
                break;

            case 't':
            case 'T':
                /* timer */
                pdebug(DEBUG_DETAIL,"Found tag element type of timer.");
                tag->elem_size=6;
                tag->elem_type = AB_TYPE_TIMER;
                break;

            default:
                pdebug(DEBUG_DETAIL,"Unknown tag type for tag %s", tag_name);
                return PLCTAG_ERR_UNSUPPORTED;
                break;
            }
        }

        break;

    case AB_PLC_LGX:
    case AB_PLC_MICRO800:
    case AB_PLC_OMRON_NJNX:
        /* look for the elem_type attribute. */
        elem_type = attr_get_str(attribs, "elem_type", NULL);
        if(elem_type) {
            if(str_cmp_i(elem_type,"lint") == 0 || str_cmp_i(elem_type, "ulint") == 0) {
                pdebug(DEBUG_DETAIL,"Found tag element type of 64-bit integer.");
                tag->elem_size = 8;
                tag->elem_type = AB_TYPE_INT64;
            } else if(str_cmp_i(elem_type,"dint") == 0 || str_cmp_i(elem_type,"udint") == 0) {
                pdebug(DEBUG_DETAIL,"Found tag element type of 32-bit integer.");
                tag->elem_size = 4;
                tag->elem_type = AB_TYPE_INT32;
            } else if(str_cmp_i(elem_type,"int") == 0 || str_cmp_i(elem_type,"uint") == 0) {
                pdebug(DEBUG_DETAIL,"Found tag element type of 16-bit integer.");
                tag->elem_size = 2;
                tag->elem_type = AB_TYPE_INT16;
            } else if(str_cmp_i(elem_type,"sint") == 0 || str_cmp_i(elem_type,"usint") == 0) {
                pdebug(DEBUG_DETAIL,"Found tag element type of 8-bit integer.");
                tag->elem_size = 1;
                tag->elem_type = AB_TYPE_INT8;
            } else if(str_cmp_i(elem_type,"bool") == 0) {
                pdebug(DEBUG_DETAIL,"Found tag element type of bit.");
                tag->elem_size = 1;
                tag->elem_type = AB_TYPE_BOOL;
            } else if(str_cmp_i(elem_type,"bool array") == 0) {
                pdebug(DEBUG_DETAIL,"Found tag element type of bit array.");
                tag->elem_size = 4;
                tag->elem_type = AB_TYPE_BOOL_ARRAY;
            } else if(str_cmp_i(elem_type,"real") == 0) {
                pdebug(DEBUG_DETAIL,"Found tag element type of 32-bit float.");
                tag->elem_size = 4;
                tag->elem_type = AB_TYPE_FLOAT32;
            } else if(str_cmp_i(elem_type,"lreal") == 0) {
                pdebug(DEBUG_DETAIL,"Found tag element type of 64-bit float.");
                tag->elem_size = 8;
                tag->elem_type = AB_TYPE_FLOAT64;
            } else if(str_cmp_i(elem_type,"string") == 0) {
                pdebug(DEBUG_DETAIL,"Fount tag element type of string.");
                tag->elem_size = 88;
                tag->elem_type = AB_TYPE_STRING;
            } else if(str_cmp_i(elem_type,"short string") == 0) {
                pdebug(DEBUG_DETAIL,"Found tag element type of short string.");
                tag->elem_size = 256; /* FIXME */
                tag->elem_type = AB_TYPE_SHORT_STRING;
            } else {
                pdebug(DEBUG_DETAIL, "Unknown tag type %s", elem_type);
                return PLCTAG_ERR_UNSUPPORTED;
            }
        } else {
            /*
             * We have two cases
             *      * tag listing, but only for LGX.
             *      * no type, just elem_size.
             * Otherwise this is an error.
             */
            {
                int elem_size = attr_get_int(attribs, "elem_size", 0);

                if(tag->plc_type == AB_PLC_LGX) {
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

                    if(special_tag_rc == PLCTAG_ERR_BAD_PARAM) {
                        pdebug(DEBUG_WARN, "Error parsing tag listing name!");
                        return PLCTAG_ERR_BAD_PARAM;
                    }
                }

                /* if we did not set an element size yet, set one. */
                if(tag->elem_size == 0) {
                    if(elem_size > 0) {
                        pdebug(DEBUG_INFO, "Setting element size to %d.", elem_size);
                        tag->elem_size = elem_size;
                    }

                    // else {
                    //     pdebug(DEBUG_WARN, "You must set a element type or an element size!");
                    //     return PLCTAG_ERR_BAD_PARAM;
                    // }
                } else {
                    if(elem_size > 0) {
                        pdebug(DEBUG_WARN, "Tag has elem_size and either is a tag listing or has elem_type, only use one!");
                    }
                }

                /* if we did not set an element size yet, set one. */
                if(tag->elem_size == 0) {
                    if(elem_size > 0) {
                        pdebug(DEBUG_INFO, "Setting element size to %d.", elem_size);
                        tag->elem_size = elem_size;
                    }

                    // else {
                    //     pdebug(DEBUG_WARN, "You must set a element type or an element size!");
                    //     return PLCTAG_ERR_BAD_PARAM;
                    // }
                } else {
                    if(elem_size > 0) {
                        pdebug(DEBUG_WARN, "Tag has elem_size and either is a tag listing or has elem_type, only use one!");
                    }
                }
            }
        }

        break;

    default:
        pdebug(DEBUG_WARN, "Unknown PLC type!");
        return PLCTAG_ERR_BAD_DEVICE;
        break;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int default_abort(plc_tag_p tag)
{
    (void)tag;

    pdebug(DEBUG_WARN, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}


int default_read(plc_tag_p tag)
{
    (void)tag;

    pdebug(DEBUG_WARN, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}

int default_status(plc_tag_p tag)
{
    pdebug(DEBUG_WARN, "This should be overridden by a PLC-specific function!");

    if(tag) {
        return tag->status;
    } else {
        return PLCTAG_ERR_NOT_FOUND;
    }
}


int default_tickler(plc_tag_p tag)
{
    (void)tag;

    pdebug(DEBUG_WARN, "This should be overridden by a PLC-specific function!");

    return PLCTAG_STATUS_OK;
}



int default_write(plc_tag_p tag)
{
    (void)tag;

    pdebug(DEBUG_WARN, "This should be overridden by a PLC-specific function!");

    return PLCTAG_ERR_NOT_IMPLEMENTED;
}



/*
* ab_tag_abort
*
* This does the work of stopping any inflight requests.
* This is not thread-safe.  It must be called from a function
* that locks the tag's mutex or only from a single thread.
*/

int ab_tag_abort(ab_tag_p tag)
{
    pdebug(DEBUG_DETAIL, "Starting.");

    if(tag->req) {
        spin_block(&tag->req->lock) {
            tag->req->abort_request = 1;
        }

        tag->req = rc_dec(tag->req);
    } else {
        pdebug(DEBUG_DETAIL, "Called without a request in flight.");
    }

    tag->read_in_progress = 0;
    tag->write_in_progress = 0;
    tag->offset = 0;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}




/*
 * ab_tag_status
 *
 * Generic status checker.   May be overridden by individual PLC types.
 */
int ab_tag_status(ab_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    if (tag->read_in_progress) {
        return PLCTAG_STATUS_PENDING;
    }

    if (tag->write_in_progress) {
        return PLCTAG_STATUS_PENDING;
    }

    if(tag->session) {
        rc = tag->status;
    } else {
        /* this is not OK.  This is fatal! */
        rc = PLCTAG_ERR_CREATE;
    }

    return rc;
}






/*
 * ab_tag_destroy
 *
 * This blocks on the global library mutex.  This should
 * be fixed to allow for more parallelism.  For now, safety is
 * the primary concern.
 */

void ab_tag_destroy(ab_tag_p tag)
{
    ab_session_p session = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    /* already destroyed? */
    if (!tag) {
        pdebug(DEBUG_WARN,"Tag pointer is null!");

        return;
    }

    session = tag->session;

    /* tags should always have a session.  Release it. */
    pdebug(DEBUG_DETAIL,"Getting ready to release tag session %p",tag->session);
    if(session) {
        pdebug(DEBUG_DETAIL, "Removing tag from session.");
        rc_dec(session);
        tag->session = NULL;
    } else {
        pdebug(DEBUG_WARN,"No session pointer!");
    }

    if(tag->ext_mutex) {
        mutex_destroy(&(tag->ext_mutex));
        tag->ext_mutex = NULL;
    }

    if(tag->api_mutex) {
        mutex_destroy(&(tag->api_mutex));
        tag->api_mutex = NULL;
    }

    if(tag->byte_order && tag->byte_order->is_allocated) {
        mem_free(tag->byte_order);
        tag->byte_order = NULL;
    }

    if (tag->data) {
        mem_free(tag->data);
        tag->data = NULL;
    }

    pdebug(DEBUG_INFO,"Finished releasing all tag resources.");

    pdebug(DEBUG_INFO, "done");
}


int ab_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value)
{
    int res = default_value;
    ab_tag_p tag = (ab_tag_p)raw_tag;

    pdebug(DEBUG_SPEW, "Starting.");

    /* assume we have a match. */
    tag->status = PLCTAG_STATUS_OK;

    /* match the attribute. */
    if(str_cmp_i(attrib_name, "elem_size") == 0) {
        res = tag->elem_size;
    } else if(str_cmp_i(attrib_name, "elem_count") == 0) {
        res = tag->elem_count;
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        tag->status = PLCTAG_ERR_UNSUPPORTED;
    }

    return res;
}


int ab_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value)
{
    (void)attrib_name;
    (void)new_value;

    pdebug(DEBUG_WARN, "Unsupported attribute \"%s\"!", attrib_name);

    raw_tag->status  = PLCTAG_ERR_UNSUPPORTED;

    return PLCTAG_ERR_UNSUPPORTED;
}



plc_type_t get_plc_type(attr attribs)
{
    const char *cpu_type = attr_get_str(attribs, "plc", attr_get_str(attribs, "cpu", "NONE"));

    if (!str_cmp_i(cpu_type, "plc") || !str_cmp_i(cpu_type, "plc5")) {
        pdebug(DEBUG_DETAIL,"Found PLC/5 PLC.");
        return AB_PLC_PLC5;
    } else if ( !str_cmp_i(cpu_type, "slc") || !str_cmp_i(cpu_type, "slc500")) {
        pdebug(DEBUG_DETAIL,"Found SLC 500 PLC.");
        return AB_PLC_SLC;
    } else if (!str_cmp_i(cpu_type, "lgxpccc") || !str_cmp_i(cpu_type, "logixpccc") || !str_cmp_i(cpu_type, "lgxplc5") || !str_cmp_i(cpu_type, "logixplc5") ||
               !str_cmp_i(cpu_type, "lgx-pccc") || !str_cmp_i(cpu_type, "logix-pccc") || !str_cmp_i(cpu_type, "lgx-plc5") || !str_cmp_i(cpu_type, "logix-plc5")) {
        pdebug(DEBUG_DETAIL,"Found Logix-class PLC using PCCC protocol.");
        return AB_PLC_LGX_PCCC;
    } else if (!str_cmp_i(cpu_type, "micrologix800") || !str_cmp_i(cpu_type, "mlgx800") || !str_cmp_i(cpu_type, "micro800")) {
        pdebug(DEBUG_DETAIL,"Found Micro8xx PLC.");
        return AB_PLC_MICRO800;
    } else if (!str_cmp_i(cpu_type, "micrologix") || !str_cmp_i(cpu_type, "mlgx")) {
        pdebug(DEBUG_DETAIL,"Found MicroLogix PLC.");
        return AB_PLC_MLGX;
    } else if (!str_cmp_i(cpu_type, "compactlogix") || !str_cmp_i(cpu_type, "clgx") || !str_cmp_i(cpu_type, "lgx") ||
               !str_cmp_i(cpu_type, "controllogix") || !str_cmp_i(cpu_type, "contrologix") ||
               !str_cmp_i(cpu_type, "logix")) {
        pdebug(DEBUG_DETAIL,"Found ControlLogix/CompactLogix PLC.");
        return AB_PLC_LGX;
    } else if (!str_cmp_i(cpu_type, "omron-njnx") || !str_cmp_i(cpu_type, "omron-nj") || !str_cmp_i(cpu_type, "omron-nx") || !str_cmp_i(cpu_type, "njnx")
               || !str_cmp_i(cpu_type, "nx1p2")) {
        pdebug(DEBUG_DETAIL,"Found OMRON NJ/NX Series PLC.");
        return AB_PLC_OMRON_NJNX;
    } else {
        pdebug(DEBUG_WARN, "Unsupported device type: %s", cpu_type);

        return AB_PLC_NONE;
    }
}



int check_cpu(ab_tag_p tag, attr attribs)
{
    plc_type_t result = get_plc_type(attribs);

    if(result != AB_PLC_NONE) {
        tag->plc_type = result;
        return PLCTAG_STATUS_OK;
    } else {
        tag->plc_type = result;
        return PLCTAG_ERR_BAD_DEVICE;
    }
}

int check_tag_name(ab_tag_p tag, const char* name)
{
    int rc = PLCTAG_STATUS_OK;

    if (!name) {
        pdebug(DEBUG_WARN,"No tag name parameter found!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* attempt to parse the tag name */
    switch (tag->plc_type) {
    case AB_PLC_PLC5:
    case AB_PLC_LGX_PCCC:
        if ((rc = plc5_encode_tag_name(tag->encoded_name, &(tag->encoded_name_size), &(tag->file_type), name, MAX_TAG_NAME)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "parse of PLC/5-style tag name %s failed!", name);

            return rc;
        }

        break;

    case AB_PLC_SLC:
    case AB_PLC_MLGX:
        if ((rc = slc_encode_tag_name(tag->encoded_name, &(tag->encoded_name_size), &(tag->file_type), name, MAX_TAG_NAME)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "parse of SLC-style tag name %s failed!", name);

            return rc;
        }

        break;

    case AB_PLC_MICRO800:
    case AB_PLC_LGX:
    case AB_PLC_OMRON_NJNX:
        if ((rc = cip_encode_tag_name(tag, name)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "parse of CIP-style tag name %s failed!", name);

            return rc;
        }

        break;

    default:
        /* how would we get here? */
        pdebug(DEBUG_WARN, "unsupported PLC type %d", tag->plc_type);

        return PLCTAG_ERR_BAD_PARAM;

        break;
    }

    return PLCTAG_STATUS_OK;
}




///*
// * setup_session_mutex
// *
// * check to see if the global mutex is set up.  If not, do an atomic
// * lock and set it up.
// */
//int setup_session_mutex(void)
//{
//    int rc = PLCTAG_STATUS_OK;
//
//    pdebug(DEBUG_INFO, "Starting.");
//
//    critical_block(global_library_mutex) {
//        /* first see if the mutex is there. */
//        if (!global_session_mut) {
//            rc = mutex_create((mutex_p*)&global_session_mut);
//
//            if (rc != PLCTAG_STATUS_OK) {
//                pdebug(DEBUG_ERROR, "Unable to create global tag mutex!");
//            }
//        }
//    }
//
//    pdebug(DEBUG_INFO, "Done.");
//
//    return rc;
//}
