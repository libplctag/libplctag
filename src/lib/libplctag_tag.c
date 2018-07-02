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
#include <lib/libplctag_tag.h>
#include <lib/init.h>
#include <platform.h>
#include <util/attr.h>
#include <util/debug.h>
#include <ab/ab.h>



static plc_tag_p map_id_to_tag(plc_tag tag_id);
static int allocate_new_tag_to_id_mapping(plc_tag_p tag);
static int release_tag_to_id_mapping(plc_tag_p tag);
static int api_lock(int index);
static int api_unlock(int index);
static int tag_ptr_to_tag_index(plc_tag tag_id_ptr);



mutex_p global_library_mutex = NULL;




#define TAG_ID_MASK (0xFFFFFFF)
#define TAG_INDEX_MASK (0x3FFF)
#define MAX_TAG_ENTRIES (TAG_INDEX_MASK + 1)
#define TAG_ID_ERROR INT_MIN

/* these are only internal to the file */

static volatile int next_tag_id = MAX_TAG_ENTRIES;
static volatile plc_tag_p tag_map[MAX_TAG_ENTRIES + 1] = {0,};
static volatile mutex_p tag_api_mutex[MAX_TAG_ENTRIES + 1] = {0,};



#define api_block(tag_id)                                              \
for(int __sync_flag_api_block_foo_##__LINE__ = 1; __sync_flag_api_block_foo_##__LINE__ ; __sync_flag_api_block_foo_##__LINE__ = 0, api_unlock(tag_ptr_to_tag_index(tag_id)))\
for(int __sync_rc_api_block_foo_##__LINE__ = api_lock(tag_ptr_to_tag_index(tag_id)); __sync_rc_api_block_foo_##__LINE__ == PLCTAG_STATUS_OK && __sync_flag_api_block_foo_##__LINE__ ; __sync_flag_api_block_foo_##__LINE__ = 0)



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

    /* initialize the mutex for API protection */
    for(int i=0; i < (MAX_TAG_ENTRIES + 1); i++) {
        rc = mutex_create((mutex_p*)&tag_api_mutex[i]);
    }

    pdebug(DEBUG_INFO,"Done.");

    return rc;
}

void lib_teardown(void)
{
    pdebug(DEBUG_INFO,"Tearing down library.");

    /* destroy the mutex for API protection */
    for(int i=0; i < (MAX_TAG_ENTRIES + 1); i++) {
        mutex_destroy((mutex_p*)&tag_api_mutex[i]);
        if(tag_map[i]) {
            pdebug(DEBUG_WARN,"Tag %p at index %d was not destroyed!",tag_map[i],i);
        }
    }

    pdebug(DEBUG_INFO,"Destroying global library mutex.");
    if(global_library_mutex) {
        mutex_destroy((mutex_p*)&global_library_mutex);
    }


    pdebug(DEBUG_INFO,"Done.");
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
        case PLCTAG_ERR_BAD_CONFIG: return "PLCTAG_ERR_BAD_CONFIG";
        case PLCTAG_ERR_BAD_CONNECTION: return "PLCTAG_ERR_BAD_CONNECTION";
        case PLCTAG_ERR_BAD_DATA: return "PLCTAG_ERR_BAD_DATA";
        case PLCTAG_ERR_BAD_DEVICE: return "PLCTAG_ERR_BAD_DEVICE";
        case PLCTAG_ERR_BAD_GATEWAY: return "PLCTAG_ERR_BAD_GATEWAY";
        case PLCTAG_ERR_BAD_PARAM: return "PLCTAG_ERR_BAD_PARAM";
        case PLCTAG_ERR_BAD_REPLY: return "PLCTAG_ERR_BAD_REPLY";
        case PLCTAG_ERR_BAD_STATUS: return "PLCTAG_ERR_BAD_STATUS";
        case PLCTAG_ERR_CLOSE: return "PLCTAG_ERR_CLOSE";
        case PLCTAG_ERR_CREATE: return "PLCTAG_ERR_CREATE";
        case PLCTAG_ERR_DUPLICATE: return "PLCTAG_ERR_DUPLICATE";
        case PLCTAG_ERR_ENCODE: return "PLCTAG_ERR_ENCODE";
        case PLCTAG_ERR_MUTEX_DESTROY: return "PLCTAG_ERR_MUTEX_DESTROY";
        case PLCTAG_ERR_MUTEX_INIT: return "PLCTAG_ERR_MUTEX_INIT";
        case PLCTAG_ERR_MUTEX_LOCK: return "PLCTAG_ERR_MUTEX_LOCK";
        case PLCTAG_ERR_MUTEX_UNLOCK: return "PLCTAG_ERR_MUTEX_UNLOCK";
        case PLCTAG_ERR_NOT_ALLOWED: return "PLCTAG_ERR_NOT_ALLOWED";
        case PLCTAG_ERR_NOT_FOUND: return "PLCTAG_ERR_NOT_FOUND";
        case PLCTAG_ERR_NOT_IMPLEMENTED: return "PLCTAG_ERR_NOT_IMPLEMENTED";
        case PLCTAG_ERR_NO_DATA: return "PLCTAG_ERR_NO_DATA";
        case PLCTAG_ERR_NO_MATCH: return "PLCTAG_ERR_NO_MATCH";
        case PLCTAG_ERR_NO_MEM: return "PLCTAG_ERR_NO_MEM";
        case PLCTAG_ERR_NO_RESOURCES: return "PLCTAG_ERR_NO_RESOURCES";
        case PLCTAG_ERR_NULL_PTR: return "PLCTAG_ERR_NULL_PTR";
        case PLCTAG_ERR_OPEN: return "PLCTAG_ERR_OPEN";
        case PLCTAG_ERR_OUT_OF_BOUNDS: return "PLCTAG_ERR_OUT_OF_BOUNDS";
        case PLCTAG_ERR_READ: return "PLCTAG_ERR_READ";
        case PLCTAG_ERR_REMOTE_ERR: return "PLCTAG_ERR_REMOTE_ERR";
        case PLCTAG_ERR_THREAD_CREATE: return "PLCTAG_ERR_THREAD_CREATE";
        case PLCTAG_ERR_THREAD_JOIN: return "PLCTAG_ERR_THREAD_JOIN";
        case PLCTAG_ERR_TIMEOUT: return "PLCTAG_ERR_TIMEOUT";
        case PLCTAG_ERR_TOO_LARGE: return "PLCTAG_ERR_TOO_LARGE";
        case PLCTAG_ERR_TOO_SMALL: return "PLCTAG_ERR_TOO_SMALL";
        case PLCTAG_ERR_UNSUPPORTED: return "PLCTAG_ERR_UNSUPPORTED";
        case PLCTAG_ERR_WINSOCK: return "PLCTAG_ERR_WINSOCK";
        case PLCTAG_ERR_WRITE: return "PLCTAG_ERR_WRITE";

        default: return "Unknown error."; break;
    }

    return "Unknown error.";
}



/*
 * plc_tag_create()
 *
 * This is where the dispatch occurs to the protocol specific implementation.
 */

LIB_EXPORT plc_tag plc_tag_create(const char *attrib_str)
{
    plc_tag_p tag = PLC_TAG_P_NULL;
    int tag_id = PLCTAG_ERR_OUT_OF_BOUNDS;
    attr attribs = NULL;
    int rc = PLCTAG_STATUS_OK;
    int read_cache_ms = 0;
    tag_create_function tag_constructor;

    pdebug(DEBUG_INFO,"Starting");

    if(initialize_modules() != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR,"Unable to initialize the internal library state!");
        return PLC_TAG_NULL;
    }

    if(!attrib_str || str_length(attrib_str) == 0) {
        pdebug(DEBUG_WARN,"Tag attribute string is null or zero length!");
        return PLC_TAG_NULL;
    }

    attribs = attr_create_from_str(attrib_str);

    if(!attribs) {
        pdebug(DEBUG_WARN,"Unable to parse attribute string!");
        return PLC_TAG_NULL;
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
        return PLC_TAG_NULL;
    }

    tag = tag_constructor(attribs);

    /*
     * FIXME - this really should be here???  Maybe not?  But, this is
     * the only place it can be without making every protocol type do this automatically.
     */
    if(!tag) {
        pdebug(DEBUG_WARN, "Tag creation failed, skipping mutex creation and other generic setup.");
        attr_destroy(attribs);
        return PLC_TAG_NULL;
    }

    /* set up the read cache config. */
    read_cache_ms = attr_get_int(attribs,"read_cache_ms",0);

    if(read_cache_ms < 0) {
        pdebug(DEBUG_WARN, "read_cache_ms value must be positive, using zero.");
        read_cache_ms = 0;
    }

    tag->read_cache_expire = (uint64_t)0;
    tag->read_cache_ms = (uint64_t)read_cache_ms;

    /* create tag mutex */
    rc = mutex_create(&tag->mut);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_ERROR, "Unable to create tag mutex!");

        /* this is fatal! */
        attr_destroy(attribs);
        plc_tag_destroy_mapped(tag);
    }

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
    tag_id = allocate_new_tag_to_id_mapping(tag);

    /* if the mapping failed, then punt */
    if(tag_id == PLCTAG_ERR_OUT_OF_BOUNDS) {
        pdebug(DEBUG_ERROR, "Unable to map tag %p to lookup table entry, rc=%d", tag_id);

        /* need to destroy the tag because we allocated memory etc. */
        plc_tag_destroy_mapped(tag);

        return PLC_TAG_NULL;
    }

    pdebug(DEBUG_INFO, "Returning mapped tag %p", (plc_tag)(intptr_t)tag_id);

    return (plc_tag)(intptr_t)tag_id;
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

LIB_EXPORT int plc_tag_lock(plc_tag tag_id)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            rc = PLCTAG_ERR_NOT_FOUND;
        } else {
            rc = mutex_lock(tag->mut);
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}




/*
 * plc_tag_unlock
 *
 * The opposite action of plc_tag_unlock.  This allows other threads to access the
 * tag.
 */

LIB_EXPORT int plc_tag_unlock(plc_tag tag_id)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            rc = PLCTAG_ERR_NOT_FOUND;
        } else {
            rc = mutex_unlock(tag->mut);
        }
    }

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

int plc_tag_abort_mapped(plc_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;
    pdebug(DEBUG_INFO, "Starting.");

    /* who knows what state the tag data is in.  */
    tag->read_cache_expire = (uint64_t)0;

    if(!tag->vtable || !tag->vtable->abort) {
        pdebug(DEBUG_WARN,"Tag does not have a abort function!");
        return PLCTAG_ERR_NOT_IMPLEMENTED;
    }

    /* this may be synchronous. */
    rc = tag->vtable->abort(tag);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


LIB_EXPORT int plc_tag_abort(plc_tag tag_id)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            rc = PLCTAG_ERR_NOT_FOUND;
        } else {
            rc = plc_tag_abort_mapped(tag);
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}







/*
 * plc_tag_destroy()
 *
 * Remove all implementation specific details about a tag and clear its
 * memory.
 *
 * FIXME - this leaves a dangling pointer.  Should we take the address
 * of the tag pointer as an arg and zero out the pointer?  That may not be
 * as portable.
 */

int plc_tag_destroy_mapped(plc_tag_p tag)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag->vtable) {
        pdebug(DEBUG_WARN,"Tag vtable is missing!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /*
     * first, unmap the tag.
     *
     * This might be called from something other than plc_tag_destroy, so
     * do not make assumptions that this was already done.  However, it is
     * required that if this is called directly, then it must always be
     * the case that the tag has not been handed to the library client!
     *
     * If that happens, then it is possible that two threads could try to
     * delete the same tag at the same time.
     */

    pdebug(DEBUG_DETAIL, "Releasing tag mapping.");

    release_tag_to_id_mapping(tag);

    /* destroy the tag's mutex */
    mutex_destroy(&tag->mut);

    /* abort anything in flight */
    rc = plc_tag_abort_mapped(tag);

    /* call the destructor */
    if(!tag->vtable || !tag->vtable->destroy) {
        pdebug(DEBUG_ERROR, "tag destructor not defined!");
        rc = PLCTAG_ERR_NOT_IMPLEMENTED;
    } else {
        /*
         * It is the responsibility of the destroy
         * function to free all memory associated with
         * the tag.
         */
        rc = tag->vtable->destroy(tag);
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}

LIB_EXPORT int plc_tag_destroy(plc_tag tag_id)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            rc = PLCTAG_ERR_NOT_FOUND;
        } else {
            /* the tag was still mapped, so destroy it. */
            rc = plc_tag_destroy_mapped(tag);
        }
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
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

LIB_EXPORT int plc_tag_read(plc_tag tag_id, int timeout)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            rc = PLCTAG_ERR_NOT_FOUND;
            break;
        }

        /* check for null parts */
        if(!tag->vtable || !tag->vtable->read) {
            pdebug(DEBUG_WARN, "Tag does not have a read function!");
            rc = PLCTAG_ERR_NOT_IMPLEMENTED;
            break;
        }

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

        /* set up the cache time */
        if(tag->read_cache_ms) {
            tag->read_cache_expire = time_ms() + tag->read_cache_ms;
        }

        /*
         * if there is a timeout, then loop until we get
         * an error or we timeout.
         */
        if(timeout) {
            int64_t timeout_time = timeout + time_ms();
            int64_t start_time = time_ms();

            while(rc == PLCTAG_STATUS_PENDING && timeout_time > time_ms()) {
                rc = plc_tag_status_mapped(tag);

                /*
                 * terminate early and do not wait again if the
                 * IO is done.
                 */
                if(rc != PLCTAG_STATUS_PENDING) {
                    break;
                }

                sleep_ms(5); /* MAGIC */
            }

            /*
             * if we dropped out of the while loop but the status is
             * still pending, then we timed out.
             *
             * Abort the operation and set the status to show the timeout.
             */
            if(rc == PLCTAG_STATUS_PENDING) {
                plc_tag_abort_mapped(tag);
                rc = PLCTAG_ERR_TIMEOUT;
            }

            pdebug(DEBUG_INFO,"elapsed time %ldms",(time_ms()-start_time));
        }
    } /* end of api block */

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


int plc_tag_status_mapped(plc_tag_p tag)
{
    /* pdebug(DEBUG_DETAIL, "Starting."); */
    if(!tag) {
        pdebug(DEBUG_ERROR,"Null tag passed!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(!tag->vtable || !tag->vtable->status) {
        pdebug(DEBUG_ERROR, "tag status accessor not defined!");
        return PLCTAG_ERR_NOT_IMPLEMENTED;
    }

    return tag->vtable->status(tag);
}



LIB_EXPORT int plc_tag_status(plc_tag tag_id)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            rc = PLCTAG_ERR_NOT_FOUND;
            break;
        }

        rc = plc_tag_status_mapped(tag);
    }

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

LIB_EXPORT int plc_tag_write(plc_tag tag_id, int timeout)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            rc = PLCTAG_ERR_NOT_FOUND;
            break;
        }

        /* check for null parts */
        if(!tag->vtable || !tag->vtable->write) {
            pdebug(DEBUG_WARN, "Tag does not have a write function!");
            rc = PLCTAG_ERR_NOT_IMPLEMENTED;
            break;
        }

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
            int64_t timeout_time = timeout + time_ms();

            while(rc == PLCTAG_STATUS_PENDING && timeout_time > time_ms()) {
                rc = plc_tag_status_mapped(tag);

                /*
                 * terminate early and do not wait again if the
                 * IO is done.
                 */
                if(rc != PLCTAG_STATUS_PENDING) {
                    break;
                }

                sleep_ms(5); /* MAGIC */
            }

            /*
             * if we dropped out of the while loop but the status is
             * still pending, then we timed out.
             *
             * Abort the operation and set the status to show the timeout.
             */
            if(rc == PLCTAG_STATUS_PENDING) {
                pdebug(DEBUG_WARN, "Write operation timed out.");
                plc_tag_abort_mapped(tag);
                rc = PLCTAG_ERR_TIMEOUT;
            }
        }
    } /* end of api block */

    pdebug(DEBUG_INFO, "Done");

    return rc;
}





/*
 * Tag data accessors.
 */



LIB_EXPORT int plc_tag_get_size(plc_tag tag_id)
{
    int result = 0;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            result = PLCTAG_ERR_NOT_FOUND;
            break;
        }

        result = tag->size;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return result;
}




LIB_EXPORT uint32_t plc_tag_get_uint32(plc_tag tag_id, int offset)
{
    uint32_t res = UINT32_MAX;
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            break;
        }

        /* is the tag ready for this operation? */
        rc = plc_tag_status_mapped(tag);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
            pdebug(DEBUG_WARN,"Tag not in good state!");
            break;
        }

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

    return res;
}



LIB_EXPORT int plc_tag_set_uint32(plc_tag tag_id, int offset, uint32_t val)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            break;
        }

        /* is the tag ready for this operation? */
        rc = plc_tag_status_mapped(tag);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
            pdebug(DEBUG_WARN,"Tag not in good state!");
            break;
        }

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

    return rc;
}




LIB_EXPORT int32_t  plc_tag_get_int32(plc_tag tag_id, int offset)
{
    int32_t res = INT32_MIN;
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            break;
        }

        /* is the tag ready for this operation? */
        rc = plc_tag_status_mapped(tag);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
            pdebug(DEBUG_WARN,"Tag not in good state!");
            break;
        }

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

    return res;
}



LIB_EXPORT int plc_tag_set_int32(plc_tag tag_id, int offset, int32_t ival)
{
    uint32_t val = (uint32_t)(ival);
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            break;
        }

        /* is the tag ready for this operation? */
        rc = plc_tag_status_mapped(tag);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
            pdebug(DEBUG_WARN,"Tag not in good state!");
            break;
        }

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

    return rc;
}









LIB_EXPORT uint16_t plc_tag_get_uint16(plc_tag tag_id, int offset)
{
    uint16_t res = UINT16_MAX;
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            break;
        }

        /* is the tag ready for this operation? */
        rc = plc_tag_status_mapped(tag);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
            pdebug(DEBUG_WARN,"Tag not in good state!");
            break;
        }

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

        res = ((uint16_t)(tag->data[offset])) +
              ((uint16_t)(tag->data[offset+1]) << 8);
    }

    return res;
}




LIB_EXPORT int plc_tag_set_uint16(plc_tag tag_id, int offset, uint16_t val)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            break;
        }

        /* is the tag ready for this operation? */
        rc = plc_tag_status_mapped(tag);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
            pdebug(DEBUG_WARN,"Tag not in good state!");
            break;
        }

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

    return rc;
}









LIB_EXPORT int16_t  plc_tag_get_int16(plc_tag tag_id, int offset)
{
    int16_t res = INT16_MIN;
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            break;
        }

        /* is the tag ready for this operation? */
        rc = plc_tag_status_mapped(tag);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
            pdebug(DEBUG_WARN,"Tag not in good state!");
            break;
        }

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

        res = (int16_t)(((uint16_t)(tag->data[offset])) +
                        ((uint16_t)(tag->data[offset+1]) << 8));
    }

    return res;
}




LIB_EXPORT int plc_tag_set_int16(plc_tag tag_id, int offset, int16_t ival)
{
    uint16_t val = (uint16_t)(ival);
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            break;
        }

        /* is the tag ready for this operation? */
        rc = plc_tag_status_mapped(tag);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
            pdebug(DEBUG_WARN,"Tag not in good state!");
            break;
        }

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

    return rc;
}








LIB_EXPORT uint8_t plc_tag_get_uint8(plc_tag tag_id, int offset)
{
    uint8_t res = UINT8_MAX;
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            break;
        }

        /* is the tag ready for this operation? */
        rc = plc_tag_status_mapped(tag);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
            pdebug(DEBUG_WARN,"Tag not in good state!");
            break;
        }

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

    return res;
}




LIB_EXPORT int plc_tag_set_uint8(plc_tag tag_id, int offset, uint8_t val)
{
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            break;
        }

        /* is the tag ready for this operation? */
        rc = plc_tag_status_mapped(tag);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
            pdebug(DEBUG_WARN,"Tag not in good state!");
            break;
        }

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

    return rc;
}





LIB_EXPORT int8_t plc_tag_get_int8(plc_tag tag_id, int offset)
{
    int8_t res = INT8_MIN;
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
       if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            break;
        }

        /* is the tag ready for this operation? */
        rc = plc_tag_status_mapped(tag);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
            pdebug(DEBUG_WARN,"Tag not in good state!");
            break;
        }

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

    return res;
}




LIB_EXPORT int plc_tag_set_int8(plc_tag tag_id, int offset, int8_t ival)
{
    uint8_t val = (uint8_t)(ival);
    int rc = PLCTAG_STATUS_OK;
    plc_tag_p tag = NULL;

    pdebug(DEBUG_SPEW, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            break;
        }

        /* is the tag ready for this operation? */
        rc = plc_tag_status_mapped(tag);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
            pdebug(DEBUG_WARN,"Tag not in good state!");
            break;
        }

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

    return rc;
}










LIB_EXPORT float plc_tag_get_float32(plc_tag tag_id, int offset)
{
    uint32_t ures;
    float res = FLT_MAX;
    plc_tag_p tag = NULL;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            break;
        }

        /* is the tag ready for this operation? */
        rc = plc_tag_status_mapped(tag);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
            pdebug(DEBUG_WARN,"Tag not in good state!");
            break;
        }

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

    /* copy the data */
    mem_copy(&res,&ures,sizeof(res));

    return res;
}




LIB_EXPORT int plc_tag_set_float32(plc_tag tag_id, int offset, float fval)
{
    int rc = PLCTAG_STATUS_OK;
    uint32_t val = 0;
    plc_tag_p tag = NULL;

    /* copy the data */
    mem_copy(&val, &fval, sizeof(val));

    api_block(tag_id) {
        tag = map_id_to_tag(tag_id);
        if(!tag) {
            pdebug(DEBUG_WARN,"Tag not found.");
            break;
        }

        /* is the tag ready for this operation? */
        rc = plc_tag_status_mapped(tag);
        if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
            pdebug(DEBUG_WARN,"Tag not in good state!");
            break;
        }

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

    return rc;
}



/*****************************************************************************************************
 *****************************  Support routines for extra indirection *******************************
 ****************************************************************************************************/


static inline int tag_id_inc(int id)
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

static inline int to_tag_index(int id)
{
    if(id <= 0 || id == TAG_ID_ERROR) {
        pdebug(DEBUG_ERROR, "Incoming ID is not valid! Got %d",id);
        return TAG_ID_ERROR;
    }
    return (id & TAG_INDEX_MASK);
}

static inline int tag_ptr_to_tag_index(plc_tag tag_id_ptr)
{
    int tag_id = (int)(intptr_t)tag_id_ptr;
    return to_tag_index(tag_id);
}



static int api_lock(int index)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW,"Starting");

    if(index < 0 || index > MAX_TAG_ENTRIES) {
        pdebug(DEBUG_WARN,"Illegal tag index %d",index);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    rc = mutex_lock(tag_api_mutex[index]);

    pdebug(DEBUG_SPEW,"Done with status %d", rc);

    return rc;
}



static int api_unlock(int index)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW,"Starting");

    if(index < 0 || index > MAX_TAG_ENTRIES) {
        pdebug(DEBUG_WARN,"Illegal tag index %d",index);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    rc = mutex_unlock(tag_api_mutex[index]);

    pdebug(DEBUG_SPEW,"Done with status %d", rc);

    return rc;
}




static int allocate_new_tag_to_id_mapping(plc_tag_p tag)
{
    int new_id = next_tag_id;
    int index = 0;
    int found = 0;

    for(int count=0; !found && count < MAX_TAG_ENTRIES && new_id != TAG_ID_ERROR; count++) {
        new_id = tag_id_inc(new_id);

        /* everything OK? */
        if(new_id == TAG_ID_ERROR) break;

        index = to_tag_index(new_id);

        /* is the slot empty? */
        if(index != TAG_ID_ERROR && !tag_map[index]) {
            /* Must lock the api mutex so that we can change the mapping. */
            api_lock(index);

            /* recheck if the slot is empty. It could have changed while we locked the mutex. */
            if(!tag_map[index]) {
                next_tag_id = new_id;
                tag->tag_id = new_id;

                tag_map[index] = tag;

                found = 1;
            }

            api_unlock(index);
        }

        if(index == TAG_ID_ERROR) break;
    }

    if(found) {
        return new_id;
    }

    if(index != TAG_ID_ERROR) {
        pdebug(DEBUG_ERROR, "Unable to find empty mapping slot!");
        return PLCTAG_ERR_NO_MEM; /* not really the right error, but close */
    }

    /* this did not work */
    return PLCTAG_ERR_NOT_ALLOWED;
}



/*
 * This MUST be called while the API mutex for this tag is held!
 */

static plc_tag_p map_id_to_tag(plc_tag tag_id_ptr)
{
    plc_tag_p result = NULL;
    int tag_id = (int)(intptr_t)tag_id_ptr;
    int index = to_tag_index(tag_id);

    pdebug(DEBUG_SPEW, "Starting");

    if(index == TAG_ID_ERROR) {
        pdebug(DEBUG_ERROR,"Bad tag ID passed! %d", tag_id);
        return (plc_tag_p)0;
    }

    result = tag_map[index];
    if(result && result->tag_id == tag_id) {
        pdebug(DEBUG_SPEW, "Correct mapping at index %d for id %d found with tag %p", index, tag_id, result);
    } else {
        pdebug(DEBUG_WARN, "Not found, tag id %d maps to a different tag", tag_id);
        result = NULL;
    }

    pdebug(DEBUG_SPEW,"Done with tag %p", result);

    /* either nothing was there or it is the wrong tag. */
    return result;
}




/*
 * It is REQUIRED that the tag API mutex be held when this is called!
 */

static int release_tag_to_id_mapping(plc_tag_p tag)
{
    int map_index = 0;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting");

    if(!tag || tag->tag_id == 0) {
        pdebug(DEBUG_ERROR, "Tag null or tag ID is zero.");
        return PLCTAG_ERR_NOT_FOUND;
    }

    map_index = to_tag_index(tag->tag_id);

    if(map_index == TAG_ID_ERROR) {
        pdebug(DEBUG_ERROR,"Bad tag ID %d!", tag->tag_id);
        return PLCTAG_ERR_BAD_DATA;
    }

    /* find the actual slot and check if it is the right tag */
    if(!tag_map[map_index] || tag_map[map_index] != tag) {
        pdebug(DEBUG_WARN, "Tag not found or entry is already clear.");
        rc = PLCTAG_ERR_NOT_FOUND;
    } else {
        pdebug(DEBUG_DETAIL,"Releasing tag %p(%d) at location %d",tag, tag->tag_id, map_index);
        tag_map[map_index] = (plc_tag_p)(intptr_t)0;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}





