/***************************************************************************
 *   Copyright (C) 2015 by OmanTek                                         *
 *   Author Kyle Hayes  kylehayes@omantek.com                              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

/**************************************************************************
 * CHANGE LOG                                                             *
 *                                                                        *
 * 2013-11-19  KRH - Created file.                                        *
 **************************************************************************/


#include <platform.h>
#include <lib/libplctag.h>
#include <lib/tag.h>
#include <ab/ab.h>
#include <ab/ab_common.h>
#include <ab/pccc.h>
#include <ab/cip.h>
#include <ab/defs.h>
#include <ab/eip.h>
#include <ab/eip_cip.h>
#include <ab/eip_lgx_pccc.h>
#include <ab/eip_pccc.h>
#include <ab/eip_dhp_pccc.h>
#include <ab/session.h>
//#include <ab/connection.h>
#include <ab/tag.h>
#include <ab/request.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/vector.h>


/*
 * Externally visible global variables
 */

volatile ab_session_p sessions = NULL;
volatile mutex_p global_session_mut = NULL;

volatile vector_p read_group_tags = NULL;


/* request/response handling thread */
volatile thread_p io_handler_thread = NULL;

volatile int library_terminating = 0;



/*
 * Generic Rockwell/Allen-Bradley protocol functions.
 *
 * These are the primary entry points into the AB protocol
 * stack.
 */


#define DEFAULT_NUM_RETRIES (5)
#define DEFAULT_RETRY_INTERVAL (300)


/* forward declarations*/
static void ab_tag_destroy(ab_tag_p tag);
//static tag_vtable_p set_tag_vtable(ab_tag_p tag);
static int default_abort(plc_tag_p tag);
static int default_read(plc_tag_p tag);
static int default_status(plc_tag_p tag);
static int default_tickler(plc_tag_p tag);
static int default_write(plc_tag_p tag);

/* vtables for different kinds of tags */
struct tag_vtable_t default_vtable = { default_abort, default_read, default_status, default_tickler, default_write };
struct tag_vtable_t cip_vtable = {0}/*= { ab_tag_abort, ab_tag_destroy, eip_cip_tag_read_start, eip_cip_tag_status, eip_cip_tag_write_start }*/;
struct tag_vtable_t plc_vtable = {0}/*= { ab_tag_abort, ab_tag_destroy, eip_pccc_tag_read_start, eip_pccc_tag_status, eip_pccc_tag_write_start }*/;
struct tag_vtable_t plc_dhp_vtable = {0}/*= { ab_tag_abort, ab_tag_destroy, eip_dhp_pccc_tag_read_start, eip_dhp_pccc_tag_status, eip_dhp_pccc_tag_write_start}*/;
struct tag_vtable_t lgx_pccc_vtable = {0}/*= { ab_tag_abort, ab_tag_destroy, eip_dhp_pccc_tag_read_start, eip_dhp_pccc_tag_status, eip_dhp_pccc_tag_write_start}*/;


/* declare this so that the library initializer can pass it to atexit() */

/*
 * Public functions.
 */


int ab_init(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO,"Initializing AB protocol library.");

    /* set up the vtables. */
    //default_vtable.destroy  = (tag_destroy_func)ab_tag_destroy;

//    plc_dhp_vtable.abort    = (tag_vtable_func)ab_tag_abort;
//    //plc_dhp_vtable.destroy  = (tag_destroy_func)ab_tag_destroy;
//    plc_dhp_vtable.read     = (tag_vtable_func)eip_dhp_pccc_tag_read_start;
//    plc_dhp_vtable.status   = (tag_vtable_func)eip_dhp_pccc_tag_status;
//    plc_dhp_vtable.tickler  = (tag_vtable_func)eip_dhp_pccc_tag_tickler;
//    plc_dhp_vtable.write    = (tag_vtable_func)eip_dhp_pccc_tag_write_start;

//    plc_vtable.abort        = (tag_vtable_func)ab_tag_abort;
//    //plc_vtable.destroy      = (tag_destroy_func)ab_tag_destroy;
//    plc_vtable.read         = (tag_vtable_func)eip_pccc_tag_read_start;
//    plc_vtable.status       = (tag_vtable_func)eip_pccc_tag_status;
//    plc_vtable.tickler      = (tag_vtable_func)eip_pccc_tag_tickler;
//    plc_vtable.write        = (tag_vtable_func)eip_pccc_tag_write_start;

//    cip_vtable.abort        = (tag_vtable_func)ab_tag_abort;
//    //cip_vtable.destroy      = (tag_destroy_func)ab_tag_destroy;
//    cip_vtable.read         = (tag_vtable_func)eip_cip_tag_read_start;
//    cip_vtable.status       = (tag_vtable_func)eip_cip_tag_status;
//    cip_vtable.tickler      = (tag_vtable_func)eip_cip_tag_tickler;
//    cip_vtable.write        = (tag_vtable_func)eip_cip_tag_write_start;

    /* this is a mutex used to synchronize most activities in this protocol */
    rc = mutex_create((mutex_p*)&global_session_mut);

    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to create global tag mutex!");
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

    pdebug(DEBUG_INFO,"Terminating IO thread.");
    /* kill the IO thread first. */
    library_terminating = 1;

    /* wait for the thread to die */
    thread_join(io_handler_thread);
    thread_destroy((thread_p*)&io_handler_thread);

    pdebug(DEBUG_INFO,"Freeing global session mutex.");
    /* clean up the mutex */
    mutex_destroy((mutex_p*)&global_session_mut);

    pdebug(DEBUG_INFO, "Removing the read group vector.");
    vector_destroy(read_group_tags);

    pdebug(DEBUG_INFO,"Done.");
}



plc_tag_p ab_tag_create(attr attribs)
{
    ab_tag_p tag = AB_TAG_NULL;
    const char *path = NULL;

    pdebug(DEBUG_INFO,"Starting.");

    /*
     * allocate memory for the new tag.  Do this first so that
     * we have a vehicle for returning status.
     */

    tag = (ab_tag_p)rc_alloc(sizeof(struct ab_tag_t), (rc_cleanup_func)ab_tag_destroy);

    if(!tag) {
        pdebug(DEBUG_ERROR,"Unable to allocate memory for AB EIP tag!");
        return PLC_TAG_P_NULL;
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
        tag->status = PLCTAG_ERR_BAD_DEVICE;
        return (plc_tag_p)tag;
    }

    /* AB PLCs are little endian. */
    tag->endian = PLCTAG_DATA_LITTLE_ENDIAN;

    /* allocate memory for the data */
    tag->elem_count = attr_get_int(attribs,"elem_count",1);
    tag->elem_size = attr_get_int(attribs,"elem_size",0);
    tag->size = (tag->elem_count) * (tag->elem_size);

    if(tag->size == 0) {
        /* failure! Need data_size! */
        pdebug(DEBUG_WARN,"Tag size is zero!");
        tag->status = PLCTAG_ERR_BAD_PARAM;
        return (plc_tag_p)tag;
    }

    tag->data = (uint8_t*)mem_alloc(tag->size);

    if(tag->data == NULL) {
        pdebug(DEBUG_WARN,"Unable to allocate tag data!");
        tag->status = PLCTAG_ERR_NO_MEM;
        return (plc_tag_p)tag;
    }

    /* special features for Logix tags. */
    if(tag->protocol_type == AB_PROTOCOL_LGX) {
        /* default to allow packing */
        tag->allow_packing = attr_get_int(attribs, "allow_packing", 1);
    }

    /* get the connection path.  We need this to make a decision about the PLC. */
    path = attr_get_str(attribs,"path",NULL);

    /* set up PLC-specific information. */
    switch(tag->protocol_type) {
    case AB_PROTOCOL_PLC:
        if(!path) {
            pdebug(DEBUG_DETAIL, "Setting up PLC/5, SLC tag.");
            tag->use_connected_msg = 0;
            tag->vtable = &eip_pccc_vtable;
        } else {
            pdebug(DEBUG_DETAIL, "Setting up PLC/5 via DH+ bridge tag.");
            tag->use_connected_msg = 1;
//          tag->use_dhp_direct = 1;
            tag->vtable = &eip_dhp_pccc_vtable;
        }

        tag->allow_packing = 0;
        break;

    case AB_PROTOCOL_MLGX:
        pdebug(DEBUG_DETAIL, "Setting up MicroLogix tag.");
        tag->use_connected_msg = 0;
        tag->allow_packing = 0;
        tag->vtable = &eip_pccc_vtable;
        break;

    case AB_PROTOCOL_LGX:
        pdebug(DEBUG_DETAIL, "Setting up Logix tag.");

        /* Logix tags need a path. */
        if(path == NULL && tag->protocol_type == AB_PROTOCOL_LGX) {
            pdebug(DEBUG_WARN,"A path is required for Logix-class PLCs!");
            tag->status = PLCTAG_ERR_BAD_PARAM;
            return (plc_tag_p)tag;
        }

        /* default to requiring a connection. */
        tag->use_connected_msg = attr_get_int(attribs,"use_connected_msg", 1);
        tag->allow_packing = attr_get_int(attribs, "allow_packing", 1);
        tag->vtable = &eip_cip_vtable;

        break;

    case AB_PROTOCOL_MLGX800:
        pdebug(DEBUG_DETAIL, "Setting up Micro8X0 tag.");
        tag->use_connected_msg = 1;
        tag->allow_packing = 0;
        tag->vtable = &eip_cip_vtable;
        break;

    default:
        pdebug(DEBUG_WARN, "Unknown PLC type!");
        tag->status = PLCTAG_ERR_BAD_CONFIG;
        return (plc_tag_p)tag;
        break;
    }

    /* pass the connection requirement since it may be overridden above. */
    attr_set_int(attribs, "use_connected_msg", tag->use_connected_msg);

//    /*
//     * set up tag vtable.  This is protocol specific
//     */
//    tag->vtable = set_tag_vtable(tag);
//
//    if(!tag->vtable) {
//        pdebug(DEBUG_INFO,"Unable to set tag vtable!");
//        tag->status = PLCTAG_ERR_BAD_PARAM;
//        return (plc_tag_p)tag;
//    }

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

    /*
     * check the tag name, this is protocol specific.
     */

    if(check_tag_name(tag, attr_get_str(attribs,"name",NULL)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO,"Bad tag name!");
        tag->status = PLCTAG_ERR_BAD_PARAM;
        return (plc_tag_p)tag;
    }

    /* trigger the first read. */
    tag->first_read = 1;

    pdebug(DEBUG_INFO,"Done.");

    return (plc_tag_p)tag;
}




int default_abort(plc_tag_p tag)
{
    (void)tag;

    pdebug(DEBUG_WARN, "This should be overridden by a PLC-specific function!");

    return PLCTAG_STATUS_OK;
}


int default_read(plc_tag_p tag)
{
    (void)tag;

    pdebug(DEBUG_WARN, "This should be overridden by a PLC-specific function!");

    return PLCTAG_STATUS_OK;
}

int default_status(plc_tag_p tag)
{
    (void)tag;

    pdebug(DEBUG_WARN, "This should be overridden by a PLC-specific function!");

    return PLCTAG_STATUS_OK;
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

    return PLCTAG_STATUS_OK;
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
        request_abort(tag->req);
        tag->req = rc_dec(tag->req);
    }

    tag->read_in_progress = 0;
    tag->write_in_progress = 0;
    tag->byte_offset = 0;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
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
//    ab_connection_p connection = NULL;
    ab_session_p session = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    /* already destroyed? */
    if (!tag) {
        pdebug(DEBUG_WARN,"Tag pointer is null!");
        //~ break;
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

    if (tag->data) {
        mem_free(tag->data);
        tag->data = NULL;
    }

    pdebug(DEBUG_INFO,"Finished releasing all tag resources.");

    pdebug(DEBUG_INFO, "done");
}



int check_cpu(ab_tag_p tag, attr attribs)
{
    const char* cpu_type = attr_get_str(attribs, "cpu", "NONE");

    if (!str_cmp_i(cpu_type, "plc") || !str_cmp_i(cpu_type, "plc5") || !str_cmp_i(cpu_type, "slc") ||
        !str_cmp_i(cpu_type, "slc500")) {
        tag->protocol_type = AB_PROTOCOL_PLC;
    } else if (!str_cmp_i(cpu_type, "lgxpccc") || !str_cmp_i(cpu_type, "logixpccc") || !str_cmp_i(cpu_type, "lgxplc5") ||
               !str_cmp_i(cpu_type, "lgx-pccc") || !str_cmp_i(cpu_type, "logix-pccc") || !str_cmp_i(cpu_type, "lgx-plc5")) {
        tag->protocol_type = AB_PROTOCOL_LGX_PCCC;
    } else if (!str_cmp_i(cpu_type, "micrologix800") || !str_cmp_i(cpu_type, "mlgx800") || !str_cmp_i(cpu_type, "micro800")) {
        tag->protocol_type = AB_PROTOCOL_MLGX800;
    } else if (!str_cmp_i(cpu_type, "micrologix") || !str_cmp_i(cpu_type, "mlgx")) {
        tag->protocol_type = AB_PROTOCOL_MLGX;
    } else if (!str_cmp_i(cpu_type, "compactlogix") || !str_cmp_i(cpu_type, "clgx") || !str_cmp_i(cpu_type, "lgx") ||
               !str_cmp_i(cpu_type, "controllogix") || !str_cmp_i(cpu_type, "contrologix") ||
               !str_cmp_i(cpu_type, "flexlogix") || !str_cmp_i(cpu_type, "flgx")) {
        tag->protocol_type = AB_PROTOCOL_LGX;
    } else {
        pdebug(DEBUG_WARN, "Unsupported device type: %s", cpu_type);

        return PLCTAG_ERR_BAD_DEVICE;
    }

    return PLCTAG_STATUS_OK;
}

int check_tag_name(ab_tag_p tag, const char* name)
{
    if (!name) {
        pdebug(DEBUG_WARN,"No tag name parameter found!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* attempt to parse the tag name */
    switch (tag->protocol_type) {
    case AB_PROTOCOL_PLC:
    case AB_PROTOCOL_MLGX:
        if (!pccc_encode_tag_name(tag->encoded_name, &(tag->encoded_name_size), name, MAX_TAG_NAME)) {
            pdebug(DEBUG_WARN, "parse of PCCC-style tag name %s failed!", name);

            return PLCTAG_ERR_BAD_PARAM;
        }

        break;

    case AB_PROTOCOL_MLGX800:
    case AB_PROTOCOL_LGX:
        if (!cip_encode_tag_name(tag, name)) {
            pdebug(DEBUG_WARN, "parse of CIP-style tag name %s failed!", name);

            return PLCTAG_ERR_BAD_PARAM;
        }

        break;

    default:
        /* how would we get here? */
        pdebug(DEBUG_WARN, "unsupported protocol %d", tag->protocol_type);

        return PLCTAG_ERR_BAD_PARAM;

        break;
    }

    return PLCTAG_STATUS_OK;
}




/*
 * setup_session_mutex
 *
 * check to see if the global mutex is set up.  If not, do an atomic
 * lock and set it up.
 */
int setup_session_mutex(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    critical_block(global_library_mutex) {
        /* first see if the mutex is there. */
        if (!global_session_mut) {
            rc = mutex_create((mutex_p*)&global_session_mut);

            if (rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_ERROR, "Unable to create global tag mutex!");
            }
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


/***********************************************************************
 *                           READ GROUP HANDLING                       *
 ***********************************************************************/

//int insert_read_group_tag(ab_tag_p tag)
//{
//    int rc = PLCTAG_STATUS_OK;
//
//    critical_block(global_library_mutex) {
//        vector_put(read_group_tags, vector_length(read_group_tags), tag);
//    }
//
//    return rc;
//}
//
//
//int remove_read_group_tag(ab_tag_p tag)
//{
//    int rc = PLCTAG_ERR_NOT_FOUND;
//    int i = 0;
//    int found = -1;
//
//    critical_block(global_library_mutex) {
//        for(i=0; i< vector_length(read_group_tags); i++) {
//            ab_tag_p tmp = vector_get(read_group_tags, i);
//            if(tmp == tag) {
//                found = i;
//                break;
//            }
//        }
//
//        if(found != -1) {
//            vector_remove(read_group_tags, found);
//            rc = PLCTAG_STATUS_OK;
//        }
//    }
//
//    return rc;
//}
//
//
//vector_p find_read_group_tags(ab_tag_p tag)
//{
//    int i = 0;
//    vector_p result = NULL;
//
//    result = vector_create(10,5); /* MAGIC */
//    if(!result) {
//        pdebug(DEBUG_WARN,"Unable to allocate new result vector!");
//        return NULL;
//    }
//
//    critical_block(global_library_mutex) {
//        for(i=0; i < vector_length(read_group_tags); i++) {
//            ab_tag_p tmp = vector_get(read_group_tags, i);
//
//            if(str_cmp_i(tag->read_group, tmp->read_group) == 0) {
//                /* found one, might even be this tag. */
//                if(rc_inc(tmp)) {
//                    vector_put(result, vector_length(result), tmp);
//                }
//            }
//        }
//    }
//
//    return result;
//}
//
