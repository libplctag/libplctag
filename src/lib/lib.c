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
 * 2012-02-23  KRH - Created file.                                        *
 *                                                                        *
 * 2012-06-24  KRH - Updated plc_err() calls for new API.                 *
 *                                                                        *
 * 2013-12-24  KRH - Various munging to make this compile under VS2012    *
 *                                                                        *
 **************************************************************************/


#define LIBPLCTAGDLL_EXPORTS 1

#include <limits.h>
#include <float.h>
#include <lib/libplctag.h>
#include <lib/tag.h>
#include <lib/init.h>
#include <platform.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/rc.h>
#include <ab/ab.h>



//static plc_tag_p map_id_to_tag(plc_tag plc_tag);
//static int add_tag_lookup(plc_tag_p tag);
//static int release_tag_to_id_mapping(plc_tag_p tag);
//static int api_lock(int index);
//static int api_unlock(int index);
//static int tag_ptr_to_tag_index(plc_tag tag_id_ptr);



#define TAG_ID_MASK (0xFFFFFFF)
#define TAG_INDEX_MASK (0x3FFF)
#define MAX_TAG_ENTRIES (TAG_INDEX_MASK + 1)
#define TAG_ID_ERROR INT_MIN

/* these are only internal to the file */

static volatile int next_tag_id = MAX_TAG_ENTRIES;
static volatile plc_tag_p tag_lookup_table[MAX_TAG_ENTRIES + 1] = {0,};
static volatile int library_terminating = 0;
static thread_p tag_tickler_thread = NULL;
static mutex_p tag_lookup_mutex = NULL;

mutex_p global_library_mutex = NULL;



/* helper functions. */
static plc_tag_p lookup_tag(plc_tag id);
static int add_tag_lookup(plc_tag_p tag);
static int tag_id_inc(int id);
static THREAD_FUNC(tag_tickler_func);
static int to_tag_index(int id);

/*
 * Initialize the library.  This is called in a threadsafe manner and
 * only called once.
 */

int lib_init(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO,"Setting up global library data.");

    pdebug(DEBUG_INFO,"Initializing library global mutex.");

    /* first see if the mutex is there. */
    if (!global_library_mutex) {
        rc = mutex_create((mutex_p*)&global_library_mutex);

        if (rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_ERROR, "Unable to create global tag mutex!");
        }
    }

//    /* initialize the mutex for API protection */
//    for(int i=0; i < (MAX_TAG_ENTRIES + 1); i++) {
//        rc = mutex_create((mutex_p*)&tag_api_mutex[i]);
//    }

    rc = mutex_create((mutex_p*)&tag_lookup_mutex);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to create tag lookup mutex!");
    }

    rc = thread_create(&tag_tickler_thread, tag_tickler_func, 32*1024, NULL);
    if (rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to create tag tickler thread!");
    }

    pdebug(DEBUG_INFO,"Done.");

    return rc;
}




void lib_teardown(void)
{
    pdebug(DEBUG_INFO,"Tearing down library.");

    pdebug(DEBUG_INFO,"Tearing down tag tickler thread.");
    library_terminating = 1;
    thread_join(tag_tickler_thread);
    thread_destroy(&tag_tickler_thread);

    pdebug(DEBUG_INFO,"Tearing down tag lookup mutex.");
    mutex_destroy(&tag_lookup_mutex);

    pdebug(DEBUG_INFO,"Destroying global library mutex.");
    if(global_library_mutex) {
        mutex_destroy((mutex_p*)&global_library_mutex);
    }


    pdebug(DEBUG_INFO,"Done.");
}




THREAD_FUNC(tag_tickler_func)
{
    (void)arg;

    pdebug(DEBUG_INFO,"Starting.");

    while(!library_terminating) {
        for(int i=0; i < MAX_TAG_ENTRIES; i++) {
            plc_tag_p tag = NULL;

            critical_block(tag_lookup_mutex) {
                tag = tag_lookup_table[i];

                if(tag) {
                    tag = rc_inc(tag);
                }
            }

            if(tag && tag->vtable->tickler) {
                if(mutex_try_lock(tag->api_mutex) == PLCTAG_STATUS_OK) {
                    tag->vtable->tickler(tag);

                    mutex_unlock(tag->api_mutex);
                }
            }

            if(tag) {
                rc_dec(tag);
            }
        }

        sleep_ms(1);
    }

    pdebug(DEBUG_INFO,"Terminating.");

    THREAD_RETURN(0);
}



/**************************************************************************
 ***************************  API Functions  ******************************
 **************************************************************************/


/*
 * plc_tag_decode_error()
 *
 * This takes an integer error value and turns it into a printable string.
 *
 * FIXME - this should produce better errors than this!
 */



LIB_EXPORT const char* plc_tag_decode_error(int rc)
{
    switch(rc) {
    case PLCTAG_STATUS_PENDING:
        return "PLCTAG_STATUS_PENDING";
    case PLCTAG_STATUS_OK:
        return "PLCTAG_STATUS_OK";
    case PLCTAG_ERR_ABORT:
        return "PLCTAG_ERR_ABORT";
    case PLCTAG_ERR_BAD_CONFIG:
        return "PLCTAG_ERR_BAD_CONFIG";
    case PLCTAG_ERR_BAD_CONNECTION:
        return "PLCTAG_ERR_BAD_CONNECTION";
    case PLCTAG_ERR_BAD_DATA:
        return "PLCTAG_ERR_BAD_DATA";
    case PLCTAG_ERR_BAD_DEVICE:
        return "PLCTAG_ERR_BAD_DEVICE";
    case PLCTAG_ERR_BAD_GATEWAY:
        return "PLCTAG_ERR_BAD_GATEWAY";
    case PLCTAG_ERR_BAD_PARAM:
        return "PLCTAG_ERR_BAD_PARAM";
    case PLCTAG_ERR_BAD_REPLY:
        return "PLCTAG_ERR_BAD_REPLY";
    case PLCTAG_ERR_BAD_STATUS:
        return "PLCTAG_ERR_BAD_STATUS";
    case PLCTAG_ERR_CLOSE:
        return "PLCTAG_ERR_CLOSE";
    case PLCTAG_ERR_CREATE:
        return "PLCTAG_ERR_CREATE";
    case PLCTAG_ERR_DUPLICATE:
        return "PLCTAG_ERR_DUPLICATE";
    case PLCTAG_ERR_ENCODE:
        return "PLCTAG_ERR_ENCODE";
    case PLCTAG_ERR_MUTEX_DESTROY:
        return "PLCTAG_ERR_MUTEX_DESTROY";
    case PLCTAG_ERR_MUTEX_INIT:
        return "PLCTAG_ERR_MUTEX_INIT";
    case PLCTAG_ERR_MUTEX_LOCK:
        return "PLCTAG_ERR_MUTEX_LOCK";
    case PLCTAG_ERR_MUTEX_UNLOCK:
        return "PLCTAG_ERR_MUTEX_UNLOCK";
    case PLCTAG_ERR_NOT_ALLOWED:
        return "PLCTAG_ERR_NOT_ALLOWED";
    case PLCTAG_ERR_NOT_FOUND:
        return "PLCTAG_ERR_NOT_FOUND";
    case PLCTAG_ERR_NOT_IMPLEMENTED:
        return "PLCTAG_ERR_NOT_IMPLEMENTED";
    case PLCTAG_ERR_NO_DATA:
        return "PLCTAG_ERR_NO_DATA";
    case PLCTAG_ERR_NO_MATCH:
        return "PLCTAG_ERR_NO_MATCH";
    case PLCTAG_ERR_NO_MEM:
        return "PLCTAG_ERR_NO_MEM";
    case PLCTAG_ERR_NO_RESOURCES:
        return "PLCTAG_ERR_NO_RESOURCES";
    case PLCTAG_ERR_NULL_PTR:
        return "PLCTAG_ERR_NULL_PTR";
    case PLCTAG_ERR_OPEN:
        return "PLCTAG_ERR_OPEN";
    case PLCTAG_ERR_OUT_OF_BOUNDS:
        return "PLCTAG_ERR_OUT_OF_BOUNDS";
    case PLCTAG_ERR_READ:
        return "PLCTAG_ERR_READ";
    case PLCTAG_ERR_REMOTE_ERR:
        return "PLCTAG_ERR_REMOTE_ERR";
    case PLCTAG_ERR_THREAD_CREATE:
        return "PLCTAG_ERR_THREAD_CREATE";
    case PLCTAG_ERR_THREAD_JOIN:
        return "PLCTAG_ERR_THREAD_JOIN";
    case PLCTAG_ERR_TIMEOUT:
        return "PLCTAG_ERR_TIMEOUT";
    case PLCTAG_ERR_TOO_LARGE:
        return "PLCTAG_ERR_TOO_LARGE";
    case PLCTAG_ERR_TOO_SMALL:
        return "PLCTAG_ERR_TOO_SMALL";
    case PLCTAG_ERR_UNSUPPORTED:
        return "PLCTAG_ERR_UNSUPPORTED";
    case PLCTAG_ERR_WINSOCK:
        return "PLCTAG_ERR_WINSOCK";
    case PLCTAG_ERR_WRITE:
        return "PLCTAG_ERR_WRITE";

    default:
        return "Unknown error.";
        break;
    }

    return "Unknown error.";
}





/*
 * plc_tag_create()
 *
 * Just pass through to the plc_tag_create_sync() function.
 */

LIB_EXPORT plc_tag plc_tag_create(const char *attrib_str)
{

    return plc_tag_create_sync(attrib_str, 0);
}


/*
 * plc_tag_create()
 *
 * Just pass through to the plc_tag_create_sync() function.
 *
 * This is where the dispatch occurs to the protocol specific implementation.
 */

LIB_EXPORT plc_tag plc_tag_create_sync(const char *attrib_str, int timeout)
{
    plc_tag_p tag = PLC_TAG_P_NULL;
    int id = PLCTAG_ERR_OUT_OF_BOUNDS;
    attr attribs = NULL;
    int rc = PLCTAG_STATUS_OK;
    int read_cache_ms = 0;
    tag_create_function tag_constructor;

    pdebug(DEBUG_INFO,"Starting");

    if(initialize_modules() != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR,"Unable to initialize the internal library state!");
        return NULL;
    }

    if(!attrib_str || str_length(attrib_str) == 0) {
        pdebug(DEBUG_WARN,"Tag attribute string is null or zero length!");
        return NULL;
    }

    attribs = attr_create_from_str(attrib_str);

    if(!attribs) {
        pdebug(DEBUG_WARN,"Unable to parse attribute string!");
        return NULL;
    }

    /* set debug level */
    set_debug_level(attr_get_int(attribs, "debug", DEBUG_NONE));

    /*
     * create the tag, this is protocol specific.
     *
     * If this routine wants to keep the attributes around, it needs
     * to clone them.
     */
    tag_constructor = find_tag_create_func(attribs);

    if(!tag_constructor) {
        pdebug(DEBUG_WARN,"Tag creation failed, no tag constructor found for tag type!");
        attr_destroy(attribs);
        return NULL;
    }

    tag = tag_constructor(attribs);

    /*
     * FIXME - this really should be here???  Maybe not?  But, this is
     * the only place it can be without making every protocol type do this automatically.
     */
    if(!tag) {
        pdebug(DEBUG_WARN, "Tag creation failed, skipping mutex creation and other generic setup.");
        attr_destroy(attribs);
        return NULL;
    }

    rc = mutex_create(&(tag->ext_mutex));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to create tag external mutex!");
        rc_dec(tag);
        return NULL;
    }

    rc = mutex_create(&(tag->api_mutex));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Unable to create tag API mutex!");
        rc_dec(tag);
        return NULL;
    }

    /* set up the read cache config. */
    read_cache_ms = attr_get_int(attribs,"read_cache_ms",0);
    if(read_cache_ms < 0) {
        pdebug(DEBUG_WARN, "read_cache_ms value must be positive, using zero.");
        read_cache_ms = 0;
    }

    tag->read_cache_expire = (uint64_t)0;
    tag->read_cache_ms = (uint64_t)read_cache_ms;

    /*
     * Release memory for attributes
     *
     * some code is commented out that would have kept a pointer
     * to the attributes in the tag and released the memory upon
     * tag destruction. To prevent a memory leak without maintaining
     * that pointer, the memory needs to be released here.
     */
    attr_destroy(attribs);

    /* map the tag to a tag ID */
    id = add_tag_lookup(tag);

    /* if the mapping failed, then punt */
    if(id < 0) {
        pdebug(DEBUG_ERROR, "Unable to map tag %p to lookup table entry, rc=%s", tag, plc_tag_decode_error(id));
        return (plc_tag)rc_dec(tag);
    }

    /*
    * if there is a timeout, then loop until we get
    * an error or we timeout.
    */
    if(timeout) {
        int64_t timeout_time = timeout + time_ms();
        int64_t start_time = time_ms();

        /* get the tag status. */
        rc = tag->vtable->status(tag);

        while(rc == PLCTAG_STATUS_PENDING && timeout_time > time_ms()) {
            /* give some time to the tickler function. */
            if(tag->vtable->tickler) {
                tag->vtable->tickler(tag);
            }

            rc = tag->vtable->status(tag);

            /*
             * terminate early and do not wait again if the
             * IO is done.
             */
            if(rc != PLCTAG_STATUS_PENDING) {
                break;
            }

            sleep_ms(1); /* MAGIC */
        }

        /*
         * if we dropped out of the while loop but the status is
         * still pending, then we timed out.
         *
         * Abort the operation and set the status to show the timeout.
         */
        if(rc == PLCTAG_STATUS_PENDING) {
            pdebug(DEBUG_WARN,"Timeout waiting for tag to be ready!");
            tag->vtable->abort(tag);
            rc = PLCTAG_ERR_TIMEOUT;
            tag->status = rc;
        }

        pdebug(DEBUG_INFO,"elapsed time %ldms",(time_ms()-start_time));
    }

    pdebug(DEBUG_INFO, "Returning mapped tag ID %d", id);

    pdebug(DEBUG_INFO,"Done.");

    return (plc_tag)(intptr_t)(id);
}



/*
 * plc_tag_lock
 *
 * Lock the tag against use by other threads.  Because operations on a tag are
 * very much asynchronous, actions like getting and extracting the data from
 * a tag take more than one API call.  If more than one thread is using the same tag,
 * then the internal state of the tag will get broken and you will probably experience
 * a crash.
 *
 * This should be used to initially lock a tag when starting operations with it
 * followed by a call to plc_tag_unlock when you have everything you need from the tag.
 */

LIB_EXPORT int plc_tag_lock(plc_tag id)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        rc = mutex_lock(tag->ext_mutex);
    }

    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




/*
 * plc_tag_unlock
 *
 * The opposite action of plc_tag_unlock.  This allows other threads to access the
 * tag.
 */

LIB_EXPORT int plc_tag_unlock(plc_tag id)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        rc = mutex_unlock(tag->ext_mutex);
    }

    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




/*
 * plc_tag_abort()
 *
 * This function calls through the vtable in the passed tag to call
 * the protocol-specific implementation.
 *
 * The implementation must do whatever is necessary to abort any
 * ongoing IO.
 *
 * The status of the operation is returned.
 */

int plc_tag_abort(plc_tag id)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* who knows what state the tag data is in.  */
        tag->read_cache_expire = (uint64_t)0;

        if(!tag->vtable || !tag->vtable->abort) {
            pdebug(DEBUG_WARN,"Tag does not have a abort function!");
            rc = PLCTAG_ERR_NOT_IMPLEMENTED;
        }

        /* this may be synchronous. */
        rc = tag->vtable->abort(tag);
    }

    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




/*
 * plc_tag_destroy()
 *
 * Remove all implementation specific details about a tag and clear its
 * memory.
 */


LIB_EXPORT int plc_tag_destroy(plc_tag tag_id)
{
    plc_tag_p tag = NULL;
    int id = (int)(intptr_t)(tag_id);
    int index = to_tag_index(id);

    pdebug(DEBUG_INFO, "Starting.");

    if(id <= 0 || index < 0) {
        pdebug(DEBUG_WARN, "Called with zero or invalid tag!");
        return PLCTAG_ERR_NULL_PTR;
    }

    critical_block(tag_lookup_mutex) {
        tag = tag_lookup_table[index];

        if(tag && tag->tag_id == id) {
            /* found the tag. Remove the entry so no one else gets it. */
            tag_lookup_table[index] = NULL;
        } else {
            tag = NULL;
        }
    }

    /* release the reference outside the mutex. */
    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}





/*
 * plc_tag_read()
 *
 * This function calls through the vtable in the passed tag to call
 * the protocol-specific implementation.  That starts the read operation.
 * If there is a timeout passed, then this routine waits for either
 * a timeout or an error.
 *
 * The status of the operation is returned.
 */

LIB_EXPORT int plc_tag_read(plc_tag id, int timeout)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* check read cache, if not expired, return existing data. */
        if(tag->read_cache_expire > time_ms()) {
            pdebug(DEBUG_INFO, "Returning cached data.");
            rc = PLCTAG_STATUS_OK;
            break;
        }

        /* the protocol implementation does not do the timeout. */
        rc = tag->vtable->read(tag);

        /* if error, return now */
        if(rc != PLCTAG_STATUS_PENDING && rc != PLCTAG_STATUS_OK) {
            break;
        }

        /* set up the cache time.  This works when read_cache_ms is zero as it is already expired. */
        tag->read_cache_expire = time_ms() + tag->read_cache_ms;

        /*
         * if there is a timeout, then loop until we get
         * an error or we timeout.
         */
        if(timeout) {
            int64_t timeout_time = timeout + time_ms();
            int64_t start_time = time_ms();

            while(rc == PLCTAG_STATUS_PENDING && timeout_time > time_ms()) {
                /* give some time to the tickler function. */
                if(tag->vtable->tickler) {
                    tag->vtable->tickler(tag);
                }

                rc = tag->vtable->status(tag);

                /*
                 * terminate early and do not wait again if the
                 * IO is done.
                 */
                if(rc != PLCTAG_STATUS_PENDING) {
                    break;
                }

                sleep_ms(1); /* MAGIC */
            }

            /*
             * if we dropped out of the while loop but the status is
             * still pending, then we timed out.
             *
             * Abort the operation and set the status to show the timeout.
             */
            if(rc == PLCTAG_STATUS_PENDING) {
                tag->vtable->abort(tag);
                rc = PLCTAG_ERR_TIMEOUT;
            }

            pdebug(DEBUG_INFO,"elapsed time %ldms",(time_ms()-start_time));
        }
    } /* end of api mutex block */

    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done");

    return rc;
}






/*
 * plc_tag_status
 *
 * Return the current status of the tag.  This will be PLCTAG_STATUS_PENDING if there is
 * an uncompleted IO operation.  It will be PLCTAG_STATUS_OK if everything is fine.  Other
 * errors will be returned as appropriate.
 *
 * This is a function provided by the underlying protocol implementation.
 */


int plc_tag_status(plc_tag id)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        if(tag && tag->vtable->tickler) {
            tag->vtable->tickler(tag);
        }

        rc = tag->vtable->status(tag);
    }

    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return rc;
}





/*
 * plc_tag_write()
 *
 * This function calls through the vtable in the passed tag to call
 * the protocol-specific implementation.  That starts the write operation.
 * If there is a timeout passed, then this routine waits for either
 * a timeout or an error.
 *
 * The status of the operation is returned.
 */

LIB_EXPORT int plc_tag_write(plc_tag id, int timeout)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* the protocol implementation does not do the timeout. */
        rc = tag->vtable->write(tag);

        /* if error, return now */
        if(rc != PLCTAG_STATUS_PENDING && rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN,"Response from write command is not OK!");
            break;
        }

        /*
         * if there is a timeout, then loop until we get
         * an error or we timeout.
         */
        if(timeout) {
            int64_t start_time = time_ms();
            int64_t timeout_time = timeout + start_time;

            while(rc == PLCTAG_STATUS_PENDING && timeout_time > time_ms()) {
                /* give some time to the tickler function. */
                if(tag->vtable->tickler) {
                    tag->vtable->tickler(tag);
                }

                rc = tag->vtable->status(tag);

                /*
                 * terminate early and do not wait again if the
                 * IO is done.
                 */
                if(rc != PLCTAG_STATUS_PENDING) {
                    break;
                }

                sleep_ms(1); /* MAGIC */
            }

            /*
             * if we dropped out of the while loop but the status is
             * still pending, then we timed out.
             *
             * Abort the operation and set the status to show the timeout.
             */
            if(rc == PLCTAG_STATUS_PENDING) {
                pdebug(DEBUG_WARN, "Write operation timed out.");
                tag->vtable->abort(tag);
                rc = PLCTAG_ERR_TIMEOUT;
            }

            pdebug(DEBUG_INFO,"elapsed time %lldms",(time_ms()-start_time));
        }
    } /* end of api mutex block */

    rc_dec(tag);

    pdebug(DEBUG_INFO, "Done");

    return rc;
}





/*
 * Tag data accessors.
 */



LIB_EXPORT int plc_tag_get_size(plc_tag id)
{
    int result = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        result = tag->size;
    }

    rc_dec(tag);

    pdebug(DEBUG_SPEW, "Done.");

    return result;
}






LIB_EXPORT uint64_t plc_tag_get_uint64(plc_tag id, int offset)
{
    uint64_t res = UINT64_MAX;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(uint64_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            break;
        }

        res = ((uint64_t)(tag->data[offset])) +
              ((uint64_t)(tag->data[offset+1]) << 8) +
              ((uint64_t)(tag->data[offset+2]) << 16) +
              ((uint64_t)(tag->data[offset+3]) << 24) +
              ((uint64_t)(tag->data[offset+4]) << 32) +
              ((uint64_t)(tag->data[offset+5]) << 40) +
              ((uint64_t)(tag->data[offset+6]) << 48) +
              ((uint64_t)(tag->data[offset+7]) << 56);

    }

    rc_dec(tag);

    return res;
}



LIB_EXPORT int plc_tag_set_uint64(plc_tag id, int offset, uint64_t val)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(uint64_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        /* write the data. */
        tag->data[offset]   = (uint8_t)(val & 0xFF);
        tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
        tag->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
        tag->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);
        tag->data[offset+4] = (uint8_t)((val >> 32) & 0xFF);
        tag->data[offset+5] = (uint8_t)((val >> 40) & 0xFF);
        tag->data[offset+6] = (uint8_t)((val >> 48) & 0xFF);
        tag->data[offset+7] = (uint8_t)((val >> 56) & 0xFF);
    }

    rc_dec(tag);

    return rc;
}




LIB_EXPORT int64_t  plc_tag_get_int64(plc_tag id, int offset)
{
    int64_t res = INT64_MIN;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(int64_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            break;
        }

        res = (int64_t)(((uint64_t)(tag->data[offset])) +
                        ((uint64_t)(tag->data[offset+1]) << 8) +
                        ((uint64_t)(tag->data[offset+2]) << 16) +
                        ((uint64_t)(tag->data[offset+3]) << 24) +
                        ((uint64_t)(tag->data[offset+4]) << 32) +
                        ((uint64_t)(tag->data[offset+5]) << 40) +
                        ((uint64_t)(tag->data[offset+6]) << 48) +
                        ((uint64_t)(tag->data[offset+7]) << 56));
    }

    rc_dec(tag);

    return res;
}



LIB_EXPORT int plc_tag_set_int64(plc_tag id, int offset, int64_t ival)
{
    uint64_t val = (uint64_t)(ival);
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(int64_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        tag->data[offset]   = (uint8_t)(val & 0xFF);
        tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
        tag->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
        tag->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);
        tag->data[offset+4] = (uint8_t)((val >> 32) & 0xFF);
        tag->data[offset+5] = (uint8_t)((val >> 40) & 0xFF);
        tag->data[offset+6] = (uint8_t)((val >> 48) & 0xFF);
        tag->data[offset+7] = (uint8_t)((val >> 56) & 0xFF);
    }

    rc_dec(tag);

    return rc;
}









LIB_EXPORT uint32_t plc_tag_get_uint32(plc_tag id, int offset)
{
    uint32_t res = UINT32_MAX;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(uint32_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            break;
        }

        res = ((uint32_t)(tag->data[offset])) +
              ((uint32_t)(tag->data[offset+1]) << 8) +
              ((uint32_t)(tag->data[offset+2]) << 16) +
              ((uint32_t)(tag->data[offset+3]) << 24);
    }

    rc_dec(tag);

    return res;
}



LIB_EXPORT int plc_tag_set_uint32(plc_tag id, int offset, uint32_t val)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(uint32_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        /* write the data. */
        tag->data[offset]   = (uint8_t)(val & 0xFF);
        tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
        tag->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
        tag->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);
    }

    rc_dec(tag);

    return rc;
}




LIB_EXPORT int32_t  plc_tag_get_int32(plc_tag id, int offset)
{
    int32_t res = INT32_MIN;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(int32_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            break;
        }

        res = (int32_t)(((uint32_t)(tag->data[offset])) +
                        ((uint32_t)(tag->data[offset+1]) << 8) +
                        ((uint32_t)(tag->data[offset+2]) << 16) +
                        ((uint32_t)(tag->data[offset+3]) << 24));
    }

    rc_dec(tag);

    return res;
}



LIB_EXPORT int plc_tag_set_int32(plc_tag id, int offset, int32_t ival)
{
    uint32_t val = (uint32_t)(ival);
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(int32_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        tag->data[offset]   = (uint8_t)(val & 0xFF);
        tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
        tag->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
        tag->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);
    }

    rc_dec(tag);

    return rc;
}









LIB_EXPORT uint16_t plc_tag_get_uint16(plc_tag id, int offset)
{
    uint16_t res = UINT16_MAX;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(uint16_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            break;
        }

        res = (uint16_t)((tag->data[offset]) +
              ((tag->data[offset+1]) << 8));
    }

    rc_dec(tag);

    return res;
}




LIB_EXPORT int plc_tag_set_uint16(plc_tag id, int offset, uint16_t val)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(uint16_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        tag->data[offset]   = (uint8_t)(val & 0xFF);
        tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
    }

    rc_dec(tag);

    return rc;
}









LIB_EXPORT int16_t  plc_tag_get_int16(plc_tag id, int offset)
{
    int16_t res = INT16_MIN;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(int16_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            break;
        }

        res = (int16_t)(((tag->data[offset])) +
                        ((tag->data[offset+1]) << 8));
    }

    rc_dec(tag);

    return res;
}




LIB_EXPORT int plc_tag_set_int16(plc_tag id, int offset, int16_t ival)
{
    uint16_t val = (uint16_t)(ival);
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(int16_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        tag->data[offset]   = (uint8_t)(val & 0xFF);
        tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
    }

    rc_dec(tag);

    return rc;
}








LIB_EXPORT uint8_t plc_tag_get_uint8(plc_tag id, int offset)
{
    uint8_t res = UINT8_MAX;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(uint8_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            break;
        }

        res = tag->data[offset];
    }

    rc_dec(tag);

    return res;
}




LIB_EXPORT int plc_tag_set_uint8(plc_tag id, int offset, uint8_t val)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(uint8_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        tag->data[offset] = val;
    }

    rc_dec(tag);

    return rc;
}





LIB_EXPORT int8_t plc_tag_get_int8(plc_tag id, int offset)
{
    int8_t res = INT8_MIN;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(int8_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            break;
        }

        res = (int8_t)(tag->data[offset]);
    }

    rc_dec(tag);

    return res;
}




LIB_EXPORT int plc_tag_set_int8(plc_tag id, int offset, int8_t ival)
{
    uint8_t val = (uint8_t)(ival);
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(int8_t)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        tag->data[offset] = (uint8_t)val;
    }

    rc_dec(tag);

    return rc;
}






LIB_EXPORT double plc_tag_get_float64(plc_tag id, int offset)
{
    uint64_t ures = 0;
    double res = DBL_MAX;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(ures)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            break;
        }

        ures = ((uint64_t)(tag->data[offset])) +
               ((uint64_t)(tag->data[offset+1]) << 8) +
               ((uint64_t)(tag->data[offset+2]) << 16) +
               ((uint64_t)(tag->data[offset+3]) << 24) +
               ((uint64_t)(tag->data[offset+4]) << 32) +
               ((uint64_t)(tag->data[offset+5]) << 40) +
               ((uint64_t)(tag->data[offset+6]) << 48) +
               ((uint64_t)(tag->data[offset+7]) << 56);
    }

    rc_dec(tag);

    /* copy the data */
    mem_copy(&res,&ures,sizeof(res));

    return res;
}




LIB_EXPORT int plc_tag_set_float64(plc_tag id, int offset, double fval)
{
    int rc = PLCTAG_STATUS_OK;
    uint64_t val = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    mem_copy(&val, &fval, sizeof(val));

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(val)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        tag->data[offset]   = (uint8_t)(val & 0xFF);
        tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
        tag->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
        tag->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);
        tag->data[offset+4] = (uint8_t)((val >> 32) & 0xFF);
        tag->data[offset+5] = (uint8_t)((val >> 40) & 0xFF);
        tag->data[offset+6] = (uint8_t)((val >> 48) & 0xFF);
        tag->data[offset+7] = (uint8_t)((val >> 56) & 0xFF);
    }

    rc_dec(tag);

    return rc;
}



LIB_EXPORT float plc_tag_get_float32(plc_tag id, int offset)
{
    uint32_t ures;
    float res = FLT_MAX;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return res;
    }

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(ures)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            break;
        }

        ures = ((uint32_t)(tag->data[offset])) +
               ((uint32_t)(tag->data[offset+1]) << 8) +
               ((uint32_t)(tag->data[offset+2]) << 16) +
               ((uint32_t)(tag->data[offset+3]) << 24);
    }

    rc_dec(tag);

    /* copy the data */
    mem_copy(&res,&ures,sizeof(res));

    return res;
}




LIB_EXPORT int plc_tag_set_float32(plc_tag id, int offset, float fval)
{
    int rc = PLCTAG_STATUS_OK;
    uint32_t val = 0;
    plc_tag_p tag = lookup_tag(id);

    pdebug(DEBUG_SPEW, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag not found.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    mem_copy(&val, &fval, sizeof(val));

    critical_block(tag->api_mutex) {
        /* is there data? */
        if(!tag->data) {
            pdebug(DEBUG_WARN,"Tag has no data!");
            rc = PLCTAG_ERR_NO_DATA;
            break;
        }

        /* is there enough data */
        if((offset < 0) || (offset + ((int)sizeof(val)) > tag->size)) {
            pdebug(DEBUG_WARN,"Data offset out of bounds.");
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        tag->data[offset]   = (uint8_t)(val & 0xFF);
        tag->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
        tag->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
        tag->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);
    }

    rc_dec(tag);

    return rc;
}



/*****************************************************************************************************
 *****************************  Support routines for extra indirection *******************************
 ****************************************************************************************************/


plc_tag_p lookup_tag(plc_tag tag_id)
{
    plc_tag_p tag = NULL;
    int id = (int)(intptr_t)(tag_id);
    int index = to_tag_index(id);

    if(index <= 0 || index == TAG_ID_ERROR) {
        pdebug(DEBUG_ERROR, "Incoming ID is not valid! Got %d",id);
        return NULL;
    }

    critical_block(tag_lookup_mutex) {
        tag = tag_lookup_table[index];

        if(tag && tag->tag_id == id) {
            tag = rc_inc(tag);
        } else {
            tag = NULL;
        }
    }

    return tag;
}



int tag_id_inc(int id)
{
    if(id <= 0 || id == TAG_ID_ERROR) {
        pdebug(DEBUG_ERROR, "Incoming ID is not valid! Got %d",id);
        return TAG_ID_ERROR;
    }

    id = (id + 1) & TAG_ID_MASK;

    if(id == 0) {
        id = 1; /* skip zero intentionally! Can't return an ID of zero because it looks like a NULL pointer */
    }

    return id;
}



int to_tag_index(int id)
{
    if(id <= 0 || id == TAG_ID_ERROR) {
        pdebug(DEBUG_ERROR, "Incoming ID is not valid! Got %d",id);
        return TAG_ID_ERROR;
    }
    return (id & TAG_INDEX_MASK);
}



int add_tag_lookup(plc_tag_p tag)
{
    int new_id = next_tag_id;
    int index = 0;
    int found = 0;

    critical_block(tag_lookup_mutex) {
        for(int count=0; !found && count < MAX_TAG_ENTRIES && new_id != TAG_ID_ERROR; count++) {
            new_id = tag_id_inc(new_id);

            /* everything OK? */
            if(new_id == TAG_ID_ERROR) break;

            index = to_tag_index(new_id);

            /* is the slot empty? */
            if(index != TAG_ID_ERROR && !tag_lookup_table[index]) {
                next_tag_id = new_id;
                tag->tag_id = new_id;

                tag_lookup_table[index] = tag;

                found = 1;
            }

            if(index == TAG_ID_ERROR) break;
        }
    }

    if(found) {
        return new_id;
    }

    if(index != TAG_ID_ERROR) {
        pdebug(DEBUG_ERROR, "Unable to find empty mapping slot!");
        return PLCTAG_ERR_NO_RESOURCES;
    }

    /* this did not work */
    return PLCTAG_ERR_NOT_ALLOWED;
}


//
///*
// * This MUST be called while the API mutex for this tag is held!
// */
//
//static plc_tag_p map_id_to_tag(plc_tag tag_id_ptr)
//{
//    plc_tag_p result = NULL;
//    int plc_tag = (int)(intptr_t)tag_id_ptr;
//    int index = to_tag_index(plc_tag);
//
//    pdebug(DEBUG_SPEW, "Starting");
//
//    if(index == TAG_ID_ERROR) {
//        pdebug(DEBUG_ERROR,"Bad tag ID passed! %d", plc_tag);
//        return (plc_tag_p)0;
//    }
//
//    result = tag_lookup_table[index];
//    if(result && result->plc_tag == plc_tag) {
//        pdebug(DEBUG_SPEW, "Correct mapping at index %d for id %d found with tag %p", index, plc_tag, result);
//    } else {
//        pdebug(DEBUG_WARN, "Not found, tag id %d maps to a different tag", plc_tag);
//        result = NULL;
//    }
//
//    pdebug(DEBUG_SPEW,"Done with tag %p", result);
//
//    /* either nothing was there or it is the wrong tag. */
//    return result;
//}
//
//
//
//
///*
// * It is REQUIRED that the tag API mutex be held when this is called!
// */
//
//static int release_tag_to_id_mapping(plc_tag_p tag)
//{
//    int map_index = 0;
//    int rc = PLCTAG_STATUS_OK;
//
//    pdebug(DEBUG_DETAIL, "Starting");
//
//    if(!tag || tag->plc_tag == 0) {
//        pdebug(DEBUG_ERROR, "Tag null or tag ID is zero.");
//        return PLCTAG_ERR_NOT_FOUND;
//    }
//
//    map_index = to_tag_index(tag->plc_tag);
//
//    if(map_index == TAG_ID_ERROR) {
//        pdebug(DEBUG_ERROR,"Bad tag ID %d!", tag->plc_tag);
//        return PLCTAG_ERR_BAD_DATA;
//    }
//
//    /* find the actual slot and check if it is the right tag */
//    if(!tag_lookup_table[map_index] || tag_lookup_table[map_index] != tag) {
//        pdebug(DEBUG_WARN, "Tag not found or entry is already clear.");
//        rc = PLCTAG_ERR_NOT_FOUND;
//    } else {
//        pdebug(DEBUG_DETAIL,"Releasing tag %p(%d) at location %d",tag, tag->plc_tag, map_index);
//        tag_lookup_table[map_index] = (plc_tag_p)(intptr_t)0;
//    }
//
//    pdebug(DEBUG_DETAIL, "Done.");
//
//    return rc;
//}
