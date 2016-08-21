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

#include <limits.h>
#include <float.h>
#include <lib/libplctag.h>
#include <lib/libplctag_tag.h>
#include <platform.h>
#include <util/attr.h>
#include <util/debug.h>
#include <ab/ab.h>



static plc_tag_p map_id_to_tag(plc_tag tag_id);
static int allocate_new_tag_to_id_mapping(plc_tag_p tag);
static int release_tag_to_id_mapping(plc_tag_p tag);
static int setup_global_mutex();




mutex_p global_library_mutex = NULL;
static lock_t global_library_mutex_lock = LOCK_INIT;


/**************************************************************************
 ***************************  API Functions  ******************************
 **************************************************************************/



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

    /* setup a global mutex that all other code can use as a guard. */
    if(setup_global_mutex() != PLCTAG_STATUS_OK) {
        return PLC_TAG_NULL;
    }

    if(!attrib_str || !str_length(attrib_str)) {
        return PLC_TAG_NULL;
    }

    attribs = attr_create_from_str(attrib_str);

    if(!attribs) {
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
    tag = ab_tag_create(attribs);

    /*
     * FIXME - this really should be here???  Maybe not?  But, this is
     * the only place it can be without making every protocol type do this automatically.
     */
    if(tag && tag->status == PLCTAG_STATUS_OK) {
        rc = mutex_create(&tag->mut);

        tag->status = rc;

        tag->read_cache_expire = (uint64_t)0;
        tag->read_cache_ms = attr_get_int(attribs,"read_cache_ms",0);
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
    plc_tag_p tag = map_id_to_tag(tag_id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag || !tag->mut) {
        pdebug(DEBUG_WARN,"Tag is missing or mutex is already cleaned up!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* lock the mutex */
    tag->status = mutex_lock(tag->mut);

    pdebug(DEBUG_INFO, "Done.");

    return tag->status;
}




/*
 * plc_tag_unlock
 *
 * The opposite action of plc_tag_unlock.  This allows other threads to access the
 * tag.
 */

LIB_EXPORT int plc_tag_unlock(plc_tag tag_id)
{
    plc_tag_p tag = map_id_to_tag(tag_id);

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag || !tag->mut) {
        pdebug(DEBUG_WARN,"Tag is missing or mutex is already cleaned up!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* unlock the mutex */
    tag->status = mutex_unlock(tag->mut);

    pdebug(DEBUG_INFO,"Done.");

    return tag->status;
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

    /* clear the status */
    tag->status = PLCTAG_STATUS_OK;

    /* who knows what state the tag data is in.  */
    tag->read_cache_expire = (uint64_t)0;

    if(!tag->vtable || !tag->vtable->abort) {
        pdebug(DEBUG_WARN,"Tag does not have a abort function!");
        tag->status = PLCTAG_ERR_NOT_IMPLEMENTED;
        return PLCTAG_ERR_NOT_IMPLEMENTED;
    }

    /* this may be synchronous. */
    rc = tag->vtable->abort(tag);

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}

LIB_EXPORT int plc_tag_abort(plc_tag tag_id)
{
    plc_tag_p tag = map_id_to_tag(tag_id);
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag is not mapped or null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_tag_abort_mapped(tag);

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
    mutex_p temp_mut;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag->vtable) {
        pdebug(DEBUG_WARN,"Tag vtable is missing!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* abort anything in flight */
    rc = plc_tag_abort_mapped(tag);

    /* clear the mutex */
    if(tag->mut) {
        temp_mut = tag->mut;
        tag->mut = NULL;

        critical_block(temp_mut) {
            release_tag_to_id_mapping(tag);

            if(!tag->vtable || !tag->vtable->destroy) {
                pdebug(DEBUG_ERROR, "tag destructor not defined!");
                tag->status = PLCTAG_ERR_NOT_IMPLEMENTED;
                return PLCTAG_ERR_NOT_IMPLEMENTED;
            }

            /*
             * It is the responsibility of the destroy
             * function to free all memory associated with
             * the tag.
             */
            rc = tag->vtable->destroy(tag);
        }

        mutex_destroy(&temp_mut);
    }

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}

LIB_EXPORT int plc_tag_destroy(plc_tag tag_id)
{
    plc_tag_p tag = map_id_to_tag(tag_id);
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag is null or not mapped!");
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_tag_destroy_mapped(tag);

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
    pdebug(DEBUG_INFO, "Starting.");

    pdebug(DEBUG_DETAIL,"Reading tag id ptr %p.", tag_id);
    pdebug(DEBUG_DETAIL,"Reading tag id %d", (int)(intptr_t)tag_id);

    plc_tag_p tag = map_id_to_tag(tag_id);
    int rc = PLCTAG_STATUS_OK;

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* check for null parts */
    if(!tag->vtable || !tag->vtable->read) {
        pdebug(DEBUG_WARN, "Tag does not have a read function!");
        tag->status = PLCTAG_ERR_NOT_IMPLEMENTED;
        return PLCTAG_ERR_NOT_IMPLEMENTED;
    }

    /* check read cache, if not expired, return existing data. */
    if(tag->read_cache_expire > time_ms()) {
        pdebug(DEBUG_INFO, "Returning cached data.");
        tag->status = PLCTAG_STATUS_OK;
        return tag->status;
    }

    /* the protocol implementation does not do the timeout. */
    rc = tag->vtable->read(tag);

    /* if error, return now */
    if(rc != PLCTAG_STATUS_PENDING && rc != PLCTAG_STATUS_OK) {
        return rc;
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
        uint64_t timeout_time = timeout + time_ms();
        uint64_t start_time = time_ms();

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
            tag->status = PLCTAG_ERR_TIMEOUT;
            rc = PLCTAG_ERR_TIMEOUT;
        }

        pdebug(DEBUG_INFO,"elapsed time %ldms",(time_ms()-start_time));
    }

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

    if(!tag->vtable || !tag->vtable->status) {
        if(tag->status) {
            pdebug(DEBUG_WARN, "tag status not ok!");
            return tag->status;
        }

        pdebug(DEBUG_ERROR, "tag status accessor not defined!");
        tag->status = PLCTAG_ERR_NOT_IMPLEMENTED;
        return PLCTAG_ERR_NOT_IMPLEMENTED;
    }

    /* clear the status */
    /*tag->status = PLCTAG_STATUS_OK;*/

    return tag->vtable->status(tag);
}



LIB_EXPORT int plc_tag_status(plc_tag tag_id)
{
    plc_tag_p tag = map_id_to_tag(tag_id);
    int rc = PLCTAG_STATUS_OK;

    /* commented out due to too much output. */
    /*pdebug(DEBUG_INFO,"Starting.");*/

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_tag_status_mapped(tag);

    /* pdebug(DEBUG_DETAIL, "Done."); */

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
    plc_tag_p tag = map_id_to_tag(tag_id);
    int rc;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN,"Tag is NULL!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* we are writing so the tag existing data is stale. */
    tag->read_cache_expire = (uint64_t)0;

    /* check the vtable */
    if(!tag->vtable || !tag->vtable->write) {
        pdebug(DEBUG_WARN, "Tag does not have a write function!");
        tag->status = PLCTAG_ERR_NOT_IMPLEMENTED;
        return PLCTAG_ERR_NOT_IMPLEMENTED;
    }

    /* the protocol implementation does not do the timeout. */
    rc = tag->vtable->write(tag);

    /* if error, return now */
    if(rc != PLCTAG_STATUS_PENDING && rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN,"Response from write command is not OK!");
        return rc;
    }

    /*
     * if there is a timeout, then loop until we get
     * an error or we timeout.
     */
    if(timeout) {
        uint64_t timeout_time = timeout + time_ms();

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
            tag->status = PLCTAG_ERR_TIMEOUT;
            rc = PLCTAG_ERR_TIMEOUT;
        }
    }

    pdebug(DEBUG_INFO, "Done");

    return rc;
}





/*
 * Tag data accessors.
 */



LIB_EXPORT int plc_tag_get_size(plc_tag tag_id)
{
    plc_tag_p tag = map_id_to_tag(tag_id);

    if(!tag) {
        return PLCTAG_ERR_NULL_PTR;
    }

    return tag->size;
}




LIB_EXPORT uint32_t plc_tag_get_uint32(plc_tag tag_id, int offset)
{
    plc_tag_p tag = map_id_to_tag(tag_id);

    uint32_t res = UINT32_MAX;

    /* is there a tag? */
    if(!tag) {
        return res;
    }

    /* is the tag ready for this operation? */
    if(plc_tag_status_mapped(tag) != PLCTAG_STATUS_OK && plc_tag_status_mapped(tag) != PLCTAG_ERR_OUT_OF_BOUNDS) {
        return res;
    }

    /* is there data? */
    if(!tag->data) {
        tag->status = PLCTAG_ERR_NULL_PTR;
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset + 3 >= tag->size)) { /*MAGIC*/
        tag->status = PLCTAG_ERR_OUT_OF_BOUNDS;
        return res;
    }

    /* check whether data is little endian or big endian */
    if(tag->endian == PLCTAG_DATA_LITTLE_ENDIAN) {
        res = ((uint32_t)(tag->data[offset])) +
              ((uint32_t)(tag->data[offset+1]) << 8) +
              ((uint32_t)(tag->data[offset+2]) << 16) +
              ((uint32_t)(tag->data[offset+3]) << 24);
    } else {
        res = ((uint32_t)(tag->data[offset]) << 24) +
              ((uint32_t)(tag->data[offset+1]) << 16) +
              ((uint32_t)(tag->data[offset+2]) << 8) +
              ((uint32_t)(tag->data[offset+3]));
    }

    tag->status = PLCTAG_STATUS_OK;

    return res;
}



LIB_EXPORT int plc_tag_set_uint32(plc_tag tag_id, int offset, uint32_t val)
{
    plc_tag_p t = map_id_to_tag(tag_id);

    int rc;

    /* is there a tag? */
    if(!t) {
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_tag_status_mapped(t);

    /* is the tag ready for this operation? */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
        return rc;
    }

    /* is there data? */
    if(!t->data) {
        t->status = PLCTAG_ERR_NULL_PTR;
        return PLCTAG_ERR_NULL_PTR;
    }

    /* is there enough data space to write the value? */
    if((offset < 0) || (offset + 3 >= t->size)) { /*MAGIC*/
        t->status = PLCTAG_ERR_OUT_OF_BOUNDS;
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /* check whether data is little endian or big endian */
    if(t->endian == PLCTAG_DATA_LITTLE_ENDIAN) {
        t->data[offset]   = (uint8_t)(val & 0xFF);
        t->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
        t->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
        t->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);
    } else {
        t->data[offset+3] = (uint8_t)(val & 0xFF);
        t->data[offset+2] = (uint8_t)((val >> 8) & 0xFF);
        t->data[offset+1] = (uint8_t)((val >> 16) & 0xFF);
        t->data[offset]   = (uint8_t)((val >> 24) & 0xFF);
    }

    t->status = PLCTAG_STATUS_OK;

    return PLCTAG_STATUS_OK;
}









LIB_EXPORT int32_t  plc_tag_get_int32(plc_tag tag_id, int offset)
{
    plc_tag_p t = map_id_to_tag(tag_id);

    int32_t res = INT32_MIN;

    /* is there a tag? */
    if(!t) {
        return res;
    }

    /* is the tag ready for this operation? */
    if(plc_tag_status_mapped(t) != PLCTAG_STATUS_OK && plc_tag_status_mapped(t) != PLCTAG_ERR_OUT_OF_BOUNDS) {
        return res;
    }

    /* is there data? */
    if(!t->data) {
        t->status = PLCTAG_ERR_NULL_PTR;
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset + 3 >= t->size)) { /*MAGIC*/
        t->status = PLCTAG_ERR_OUT_OF_BOUNDS;
        return res;
    }

    /* check whether data is little endian or big endian */
    if(t->endian == PLCTAG_DATA_LITTLE_ENDIAN) {
        res = (int32_t)(((uint32_t)(t->data[offset])) +
                        ((uint32_t)(t->data[offset+1]) << 8) +
                        ((uint32_t)(t->data[offset+2]) << 16) +
                        ((uint32_t)(t->data[offset+3]) << 24));
    } else {
        res = (int32_t)(((uint32_t)(t->data[offset]) << 24) +
                        ((uint32_t)(t->data[offset+1]) << 16) +
                        ((uint32_t)(t->data[offset+2]) << 8) +
                        ((uint32_t)(t->data[offset+3])));
    }

    t->status = PLCTAG_STATUS_OK;

    return res;
}



LIB_EXPORT int plc_tag_set_int32(plc_tag tag_id, int offset, int32_t ival)
{
    plc_tag_p t = map_id_to_tag(tag_id);

    int rc;

    uint32_t val = (uint32_t)(ival);

    /* is there a tag? */
    if(!t) {
        return -1;
    }

    rc = plc_tag_status_mapped(t);

    /* is the tag ready for this operation? */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
        return rc;
    }

    /* is there data? */
    if(!t->data) {
        t->status = PLCTAG_ERR_NULL_PTR;
        return PLCTAG_ERR_NULL_PTR;
    }

    /* is there enough data space to write the value? */
    if((offset < 0) || (offset + 3 >= t->size)) { /*MAGIC*/
        t->status = PLCTAG_ERR_OUT_OF_BOUNDS;
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /* check whether data is little endian or big endian */
    if(t->endian == PLCTAG_DATA_LITTLE_ENDIAN) {
        t->data[offset]   = (uint8_t)(val & 0xFF);
        t->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
        t->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
        t->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);
    } else {
        t->data[offset+3] = (uint8_t)(val & 0xFF);
        t->data[offset+2] = (uint8_t)((val >> 8) & 0xFF);
        t->data[offset+1] = (uint8_t)((val >> 16) & 0xFF);
        t->data[offset]   = (uint8_t)((val >> 24) & 0xFF);
    }

    t->status = PLCTAG_STATUS_OK;

    return PLCTAG_STATUS_OK;
}









LIB_EXPORT uint16_t plc_tag_get_uint16(plc_tag tag_id, int offset)
{
    plc_tag_p t = map_id_to_tag(tag_id);

    uint16_t res = UINT16_MAX;

    /* is there a tag? */
    if(!t) {
        return res;
    }

    /* is the tag ready for this operation? */
    if(plc_tag_status_mapped(t) != PLCTAG_STATUS_OK && plc_tag_status_mapped(t) != PLCTAG_ERR_OUT_OF_BOUNDS) {
        return res;
    }

    /* is there data? */
    if(!t->data) {
        t->status = PLCTAG_ERR_NULL_PTR;
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset + 1 >= t->size)) { /*MAGIC*/
        t->status = PLCTAG_ERR_OUT_OF_BOUNDS;
        return res;
    }

    /* check whether data is little endian or big endian */
    if(t->endian == PLCTAG_DATA_LITTLE_ENDIAN) {
        res = ((uint16_t)(t->data[offset])) +
              ((uint16_t)(t->data[offset+1]) << 8);
    } else {
        res = ((uint16_t)(t->data[offset+2]) << 8) +
              ((uint16_t)(t->data[offset+3]));
    }

    t->status = PLCTAG_STATUS_OK;

    return res;
}




LIB_EXPORT int plc_tag_set_uint16(plc_tag tag_id, int offset, uint16_t val)
{
    plc_tag_p t = map_id_to_tag(tag_id);
    int rc;

    /* is there a tag? */
    if(!t) {
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_tag_status_mapped(t);

    /* is the tag ready for this operation? */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
        return rc;
    }

    /* is there data? */
    if(!t->data) {
        t->status = PLCTAG_ERR_NULL_PTR;
        return PLCTAG_ERR_NULL_PTR;
    }

    /* is there enough data space to write the value? */
    if((offset < 0) || (offset + 1 >= t->size)) { /*MAGIC*/
        t->status = PLCTAG_ERR_OUT_OF_BOUNDS;
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /* check whether data is little endian or big endian */
    if(t->endian == PLCTAG_DATA_LITTLE_ENDIAN) {
        t->data[offset]   = (uint8_t)(val & 0xFF);
        t->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
    } else {
        t->data[offset+1] = (uint8_t)(val & 0xFF);
        t->data[offset]   = (uint8_t)((val >> 8) & 0xFF);
    }

    t->status = PLCTAG_STATUS_OK;

    return PLCTAG_STATUS_OK;
}










LIB_EXPORT int16_t  plc_tag_get_int16(plc_tag tag_id, int offset)
{
    plc_tag_p t = map_id_to_tag(tag_id);
    int16_t res = INT16_MIN;

    /* is there a tag? */
    if(!t) {
        return res;
    }

    /* is the tag ready for this operation? */
    if(plc_tag_status_mapped(t) != PLCTAG_STATUS_OK && plc_tag_status_mapped(t) != PLCTAG_ERR_OUT_OF_BOUNDS) {
        return res;
    }

    /* is there data? */
    if(!t->data) {
        t->status = PLCTAG_ERR_NULL_PTR;
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset + 1 >= t->size)) { /*MAGIC*/
        t->status = PLCTAG_ERR_OUT_OF_BOUNDS;
        return res;
    }

    /* check whether data is little endian or big endian */
    if(t->endian == PLCTAG_DATA_LITTLE_ENDIAN) {
        res = (int16_t)(((uint16_t)(t->data[offset])) +
                        ((uint16_t)(t->data[offset+1]) << 8));
    } else {
        res = (int16_t)(((uint16_t)(t->data[offset+2]) << 8) +
                        ((uint16_t)(t->data[offset+3])));
    }

    t->status = PLCTAG_STATUS_OK;

    return res;
}




LIB_EXPORT int plc_tag_set_int16(plc_tag tag_id, int offset, int16_t ival)
{
    plc_tag_p t = map_id_to_tag(tag_id);
    int rc;

    uint16_t val = (uint16_t)ival;

    /* is there a tag? */
    if(!t) {
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_tag_status_mapped(t);

    /* is the tag ready for this operation? */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
        return rc;
    }

    /* is there data? */
    if(!t->data) {
        t->status = PLCTAG_ERR_NULL_PTR;
        return PLCTAG_ERR_NULL_PTR;
    }

    /* is there enough data space to write the value? */
    if((offset < 0) || (offset + 1 >= t->size)) { /*MAGIC*/
        t->status = PLCTAG_ERR_OUT_OF_BOUNDS;
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /* check whether data is little endian or big endian */
    if(t->endian == PLCTAG_DATA_LITTLE_ENDIAN) {
        t->data[offset]   = (uint8_t)(val & 0xFF);
        t->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
    } else {
        t->data[offset+1] = (uint8_t)(val & 0xFF);
        t->data[offset]   = (uint8_t)((val >> 8) & 0xFF);
    }

    t->status = PLCTAG_STATUS_OK;

    return PLCTAG_STATUS_OK;
}










LIB_EXPORT uint8_t plc_tag_get_uint8(plc_tag tag_id, int offset)
{
    plc_tag_p t = map_id_to_tag(tag_id);
    uint8_t res = UINT8_MAX;

    /* is there a tag? */
    if(!t)
        return res;

    /* is the tag ready for this operation? */
    if(plc_tag_status_mapped(t) != PLCTAG_STATUS_OK && plc_tag_status_mapped(t) != PLCTAG_ERR_OUT_OF_BOUNDS) {
        return res;
    }

    /* is there data? */
    if(!t->data) {
        t->status = PLCTAG_ERR_NULL_PTR;
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset >= t->size)) {
        t->status = PLCTAG_ERR_OUT_OF_BOUNDS;
        return res;
    }

    res = t->data[offset];

    t->status = PLCTAG_STATUS_OK;

    return res;
}




LIB_EXPORT int plc_tag_set_uint8(plc_tag tag_id, int offset, uint8_t val)
{
    plc_tag_p t = map_id_to_tag(tag_id);
    int rc;

    /* is there a tag? */
    if(!t)
        return PLCTAG_ERR_NULL_PTR;

    rc = plc_tag_status_mapped(t);

    /* is the tag ready for this operation? */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
        return rc;
    }

    /* is there data? */
    if(!t->data) {
        t->status = PLCTAG_ERR_NULL_PTR;
        return PLCTAG_ERR_NULL_PTR;
    }

    /* is there enough data space to write the value? */
    if((offset < 0) || (offset >= t->size)) {
        t->status = PLCTAG_ERR_OUT_OF_BOUNDS;
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    t->data[offset] = val;

    t->status = PLCTAG_STATUS_OK;

    return PLCTAG_STATUS_OK;
}





LIB_EXPORT int8_t plc_tag_get_int8(plc_tag tag_id, int offset)
{
    plc_tag_p t = map_id_to_tag(tag_id);
    int8_t res = INT8_MIN;

    /* is there a tag? */
    if(!t) {
        return res;
    }

    /* is the tag ready for this operation? */
    if(plc_tag_status_mapped(t) != PLCTAG_STATUS_OK && plc_tag_status_mapped(t) != PLCTAG_ERR_OUT_OF_BOUNDS) {
        return res;
    }

    /* is there data? */
    if(!t->data) {
        t->status = PLCTAG_ERR_NULL_PTR;
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset >= t->size)) {
        t->status = PLCTAG_ERR_OUT_OF_BOUNDS;
        return res;
    }

    res = (int8_t)(t->data[offset]);

    t->status = PLCTAG_STATUS_OK;

    return res;
}




LIB_EXPORT int plc_tag_set_int8(plc_tag tag_id, int offset, int8_t val)
{
    plc_tag_p t = map_id_to_tag(tag_id);
    int rc;

    /* is there a tag? */
    if(!t) {
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_tag_status_mapped(t);

    /* is the tag ready for this operation? */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
        return rc;
    }

    /* is there data? */
    if(!t->data) {
        t->status = PLCTAG_ERR_NULL_PTR;
        return PLCTAG_ERR_NULL_PTR;
    }

    /* is there enough data space to write the value? */
    if((offset < 0) || (offset >= t->size)) {
        t->status = PLCTAG_ERR_OUT_OF_BOUNDS;
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    t->data[offset] = (uint8_t)val;

    t->status = PLCTAG_STATUS_OK;

    return PLCTAG_STATUS_OK;
}










/*
 * FIXME FIXME FIXME
 *
 * This is not portable!
 */
LIB_EXPORT float plc_tag_get_float32(plc_tag tag_id, int offset)
{
    plc_tag_p t = map_id_to_tag(tag_id);
    uint32_t ures;
    float res = FLT_MAX;

    /* is there a tag? */
    if(!t) {
        return res;
    }

    /* is the tag ready for this operation? */
    if(plc_tag_status_mapped(t) != PLCTAG_STATUS_OK && plc_tag_status_mapped(t) != PLCTAG_ERR_OUT_OF_BOUNDS) {
        return res;
    }

    /* is there data? */
    if(!t->data) {
        t->status = PLCTAG_ERR_NULL_PTR;
        return res;
    }

    /* is there enough data */
    if((offset < 0) || (offset + 3 >= t->size)) { /*MAGIC*/
        t->status = PLCTAG_ERR_OUT_OF_BOUNDS;
        return res;
    }

    /* check whether data is little endian or big endian */
    if(t->endian == PLCTAG_DATA_LITTLE_ENDIAN) {
        ures = ((uint32_t)(t->data[offset])) +
               ((uint32_t)(t->data[offset+1]) << 8) +
               ((uint32_t)(t->data[offset+2]) << 16) +
               ((uint32_t)(t->data[offset+3]) << 24);
    } else {
        ures = ((uint32_t)(t->data[offset]) << 24) +
               ((uint32_t)(t->data[offset+1]) << 16) +
               ((uint32_t)(t->data[offset+2]) << 8) +
               ((uint32_t)(t->data[offset+3]));
    }

    t->status = PLCTAG_STATUS_OK;

    /* FIXME - this is not portable! */
    return *((float *)(&ures));
}




/*
 * FIXME FIXME FIXME
 *
 * This is not portable!
 */
LIB_EXPORT int plc_tag_set_float32(plc_tag tag_id, int offset, float fval)
{
    plc_tag_p t = map_id_to_tag(tag_id);
    int rc;

    uint32_t val = *((uint32_t *)(&fval));

    /* is there a tag? */
    if(!t) {
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_tag_status_mapped(t);

    /* is the tag ready for this operation? */
    if(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_OUT_OF_BOUNDS) {
        return rc;
    }

    /* is there data? */
    if(!t->data) {
        t->status = PLCTAG_ERR_NULL_PTR;
        return PLCTAG_ERR_NULL_PTR;
    }

    /* is there enough data space to write the value? */
    if((offset < 0) || (offset + 3 >= t->size)) { /*MAGIC*/
        t->status = PLCTAG_ERR_OUT_OF_BOUNDS;
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    /* check whether data is little endian or big endian */
    if(t->endian == PLCTAG_DATA_LITTLE_ENDIAN) {
        t->data[offset]   = (uint8_t)(val & 0xFF);
        t->data[offset+1] = (uint8_t)((val >> 8) & 0xFF);
        t->data[offset+2] = (uint8_t)((val >> 16) & 0xFF);
        t->data[offset+3] = (uint8_t)((val >> 24) & 0xFF);
    } else {
        t->data[offset+3] = (uint8_t)(val & 0xFF);
        t->data[offset+2] = (uint8_t)((val >> 8) & 0xFF);
        t->data[offset+1] = (uint8_t)((val >> 16) & 0xFF);
        t->data[offset]   = (uint8_t)((val >> 24) & 0xFF);
    }

    t->status = PLCTAG_STATUS_OK;

    return PLCTAG_STATUS_OK;
}



/*****************************************************************************************************
 *****************************  Support routines for extra indirection *******************************
 ****************************************************************************************************/


#define MAX_TAGS (16384)
#define MAX_TAG_IDS (982451683)  /* a big prime */
static int next_tag_id = MAX_TAGS;
static plc_tag_p tag_map[MAX_TAGS] = {0,};






static int allocate_new_tag_to_id_mapping(plc_tag_p tag)
{
    int entry_offset = 1;
    int actual_index = 0;

    pdebug(DEBUG_DETAIL,"Starting.");

    critical_block(global_library_mutex) {
        for(entry_offset = 1; entry_offset < MAX_TAGS; entry_offset++)  {
            actual_index = ((next_tag_id + entry_offset) % MAX_TAG_IDS) % MAX_TAGS;

            pdebug(DEBUG_DETAIL,"Trying index %d", actual_index);

            /* remember to skip 0.  On most machines it maps to null */
            if(actual_index != 0 && !tag_map[actual_index]) {
                /* found an empty slot */
                next_tag_id = (next_tag_id + entry_offset) % MAX_TAG_IDS;
                tag_map[actual_index] = tag;
                tag->tag_id = next_tag_id;

                pdebug(DEBUG_DETAIL, "Using index %d for tag %p", next_tag_id, tag);

                break;
            }
        }
    }

    if(entry_offset >= MAX_TAGS) {
        /* no entries left in map table! */
        pdebug(DEBUG_ERROR, "Unable to map tag, no slots left!");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return next_tag_id;
}


static plc_tag_p map_id_to_tag(plc_tag tag_id_ptr)
{
    plc_tag_p result = NULL;
    int tag_id = (int)(intptr_t)tag_id_ptr;

    pdebug(DEBUG_DETAIL, "Starting");

    critical_block(global_library_mutex) {
        result = tag_map[tag_id % MAX_TAGS];

        pdebug(DEBUG_DETAIL, "Tag id %d maps to tag %p", tag_id, result);
    }

    if(result && result->tag_id == tag_id) {
        return result;
    }

    pdebug(DEBUG_WARN, "No mapping or incorrect mapping.");

    /* either nothing was there or it is the wrong tag. */
    return (plc_tag_p)0;
}


static int release_tag_to_id_mapping(plc_tag_p tag)
{
    int map_index = 0;
    int rc = PLCTAG_STATUS_OK;

    if(!tag || tag->tag_id == 0) {
        return PLCTAG_ERR_NOT_FOUND;
    }

    map_index = tag->tag_id % MAX_TAGS;

    critical_block(global_library_mutex) {
        /* find the actual slot and check if it is the right tag */
        if(!tag_map[map_index] || tag_map[map_index] != tag) {
            rc = PLCTAG_ERR_NOT_FOUND;
        } else {
            tag_map[map_index] = NULL;
        }
    }

    return rc;
}



static int setup_global_mutex(void)
{
    int rc = PLCTAG_STATUS_OK;

    /* loop until we get the lock flag */
    while (!lock_acquire((lock_t*)&global_library_mutex_lock)) {
        sleep_ms(1);
    }

    /* first see if the mutex is there. */
    if (!global_library_mutex) {
        rc = mutex_create((mutex_p*)&global_library_mutex);

        if (rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_ERROR, "Unable to create global tag mutex!");
        }
    }

    /* we hold the lock, so clear it.*/
    lock_release((lock_t*)&global_library_mutex_lock);

    return rc;
}



