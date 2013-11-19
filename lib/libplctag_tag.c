/***************************************************************************
 *   Copyright (C) 2012 by Process Control Engineers                       *
 *   Author Kyle Hayes  kylehayes@processcontrolengineers.com              *
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

 /**************************************************************************
  * CHANGE LOG                                                             *
  *                                                                        *
  * 2012-02-23  KRH - Created file.                                        *
  *                                                                        *
  * 2012-06-24  KRH - Updated plc_err() calls for new API.                 *
  *                                                                        *
  **************************************************************************/



#ifdef __cplusplus
extern "C"
{
#endif


#include <libplctag.h>
#include <libplctag_tag.h>
#include <platform.h>
#include <util/attr.h>
#include <ab/ab.h>





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
    plc_tag tag = PLC_TAG_NULL;
    attr attribs = NULL;

    if(!attrib_str || !str_length(attrib_str)) {
        return PLC_TAG_NULL;
    }

    attribs = attr_create_from_str(attrib_str);

    if(!attribs) {
    	return PLC_TAG_NULL;
    }

    /*
     * create the tag, this is protocol specific.
     *
     * This routine must free the attributes if it does not
     * need them any more.
     */
    tag = ab_tag_create(attribs);

    if(!tag) {
        return tag;
    }

    return tag;
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

LIB_EXPORT int plc_tag_abort(plc_tag tag)
{
	int debug = tag->debug;

    pdebug(debug, "Starting.");

    if(!tag || !tag->vtable)
        return PLCTAG_ERR_NULL_PTR;

    /* clear the status */
    tag->status = PLCTAG_STATUS_OK;

    if(!tag->vtable->abort) {
        pdebug(debug,"Tag does not have a abort function!");
        tag->status = PLCTAG_ERR_NOT_IMPLEMENTED;
        return PLCTAG_ERR_NOT_IMPLEMENTED;
    }

    /* this may be synchronous. */
    return tag->vtable->abort(tag);
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

LIB_EXPORT int plc_tag_destroy(plc_tag tag)
{
	int debug = tag->debug;

    pdebug(debug, "Starting.");

    if(!tag)
        return PLCTAG_STATUS_OK;

    /* clear the status */
    /*tag->status = PLCTAG_STATUS_OK;*/

    if(!tag->vtable || !tag->vtable->destroy) {
        pdebug(debug, "tag destructor not defined!");
        tag->status = PLCTAG_ERR_NOT_IMPLEMENTED;
        return PLCTAG_ERR_NOT_IMPLEMENTED;
    }

    /*
     * It is the responsibility of the destroy
     * function to free all memory associated with
     * the tag.
     */
    return tag->vtable->destroy(tag);
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

LIB_EXPORT int plc_tag_read(plc_tag tag, int timeout)
{
    int debug = tag->debug;
	int rc;

    pdebug(debug, "Starting.");

    if(!tag)
        return PLCTAG_ERR_NULL_PTR;

    /* check for null parts */
    if(!tag->vtable || !tag->vtable->read) {
        pdebug(debug, "Tag does not have a read function!");
        tag->status = PLCTAG_ERR_NOT_IMPLEMENTED;
        return PLCTAG_ERR_NOT_IMPLEMENTED;
    }

    /* clear the status */
    /*tag->status = PLCTAG_STATUS_OK;*/

    /* the protocol implementation does not do the timeout. */
    rc = tag->vtable->read(tag);

    /* if error, return now */
    if(rc != PLCTAG_STATUS_PENDING && rc != PLCTAG_STATUS_OK) {
        return rc;
    }

    /*
     * if there is a timeout, then loop until we get
     * an error or we timeout.
     */
    if(timeout) {
    	uint64_t timeout_time = timeout + time_ms();

    	while(rc == PLCTAG_STATUS_PENDING && timeout_time > time_ms()) {
			rc = plc_tag_status(tag);

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
    		plc_tag_abort(tag);
    		tag->status = PLCTAG_ERR_TIMEOUT;
    		rc = PLCTAG_ERR_TIMEOUT;
    	}
    }

    pdebug(debug, "Done");

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
LIB_EXPORT int plc_tag_status(plc_tag tag)
{
    /*pdebug("Starting.");*/

    if(!tag)
        return PLCTAG_ERR_NULL_PTR;

    if(!tag->vtable || !tag->vtable->status) {
        pdebug(tag->debug, "tag status accessor not defined!");
        tag->status = PLCTAG_ERR_NOT_IMPLEMENTED;
        return PLCTAG_ERR_NOT_IMPLEMENTED;
    }

    /* clear the status */
    /*tag->status = PLCTAG_STATUS_OK;*/

    return tag->vtable->status(tag);
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

LIB_EXPORT int plc_tag_write(plc_tag tag, int timeout)
{
    int debug = tag->debug;
	int rc;

    pdebug(debug, "Starting.");

    if(!tag)
        return PLCTAG_ERR_NULL_PTR;

    /* clear the status */
    /*tag->status = PLCTAG_STATUS_OK;*/

    if(!tag->vtable || !tag->vtable->write) {
        pdebug(debug, "Tag does not have a write function!");
        tag->status = PLCTAG_ERR_NOT_IMPLEMENTED;
        return PLCTAG_ERR_NOT_IMPLEMENTED;
    }

    /* the protocol implementation does not do the timeout. */
    rc = tag->vtable->write(tag);

    /* if error, return now */
    if(rc != PLCTAG_STATUS_PENDING && rc != PLCTAG_STATUS_OK)
        return rc;

    /*
     * if there is a timeout, then loop until we get
     * an error or we timeout.
     */
    if(timeout) {
    	uint64_t timeout_time = timeout + time_ms();

    	while(rc == PLCTAG_STATUS_PENDING && timeout_time > time_ms()) {
			rc = plc_tag_status(tag);

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
    		plc_tag_abort(tag);
    		tag->status = PLCTAG_ERR_TIMEOUT;
    		rc = PLCTAG_ERR_TIMEOUT;
    	}
    }

    pdebug(debug, "Done");

    return rc;
}





/*
 * Tag data accessors.
 */



LIB_EXPORT int plc_tag_get_size(plc_tag tag)
{
    if(!tag)
        return PLCTAG_ERR_NULL_PTR;

    return tag->size;
}




extern uint32_t plc_tag_get_uint32(plc_tag t, int offset)
{
	uint32_t res = UINT32_MAX;

	/* is there a tag? */
	if(!t)
		return res;

	/* is the tag ready for this operation? */
	if(plc_tag_status(t) != PLCTAG_STATUS_OK && plc_tag_status(t) != PLCTAG_ERR_OUT_OF_BOUNDS) {
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
		res = ((uint32_t)(t->data[offset])) +
			  ((uint32_t)(t->data[offset+1]) << 8) +
			  ((uint32_t)(t->data[offset+2]) << 16) +
			  ((uint32_t)(t->data[offset+3]) << 24);
	} else {
		res = ((uint32_t)(t->data[offset]) << 24) +
			  ((uint32_t)(t->data[offset+1]) << 16) +
			  ((uint32_t)(t->data[offset+2]) << 8) +
			  ((uint32_t)(t->data[offset+3]));
	}

	t->status = PLCTAG_STATUS_OK;

	return res;
}



extern int plc_tag_set_uint32(plc_tag t, int offset, uint32_t val)
{
	int rc;

	/* is there a tag? */
	if(!t)
		return PLCTAG_ERR_NULL_PTR;

	rc = plc_tag_status(t);

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









extern int32_t  plc_tag_get_int32(plc_tag t, int offset)
{
	int32_t res = INT32_MIN;

	/* is there a tag? */
	if(!t)
		return res;

	/* is the tag ready for this operation? */
	if(plc_tag_status(t) != PLCTAG_STATUS_OK && plc_tag_status(t) != PLCTAG_ERR_OUT_OF_BOUNDS) {
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



extern int plc_tag_set_int32(plc_tag t, int offset, int32_t ival)
{
	int rc;

	uint32_t val = (uint32_t)(ival);

	/* is there a tag? */
	if(!t)
		return -1;

	rc = plc_tag_status(t);

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









extern uint16_t plc_tag_get_uint16(plc_tag t, int offset)
{
	uint16_t res = UINT16_MAX;

	/* is there a tag? */
	if(!t)
		return res;

	/* is the tag ready for this operation? */
	if(plc_tag_status(t) != PLCTAG_STATUS_OK && plc_tag_status(t) != PLCTAG_ERR_OUT_OF_BOUNDS) {
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




extern int plc_tag_set_uint16(plc_tag t, int offset, uint16_t val)
{
	int rc;

	/* is there a tag? */
	if(!t)
		return PLCTAG_ERR_NULL_PTR;

	rc = plc_tag_status(t);

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










extern int16_t  plc_tag_get_int16(plc_tag t, int offset)
{
	int16_t res = INT16_MIN;

	/* is there a tag? */
	if(!t)
		return res;

	/* is the tag ready for this operation? */
	if(plc_tag_status(t) != PLCTAG_STATUS_OK && plc_tag_status(t) != PLCTAG_ERR_OUT_OF_BOUNDS) {
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




extern int plc_tag_set_int16(plc_tag t, int offset, int16_t ival)
{
	int rc;

	uint16_t val = (uint16_t)ival;

	/* is there a tag? */
	if(!t)
		return PLCTAG_ERR_NULL_PTR;

	rc = plc_tag_status(t);

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










extern uint8_t  plc_tag_get_uint8(plc_tag t, int offset)
{
	uint8_t res = UINT8_MAX;

	/* is there a tag? */
	if(!t)
		return res;

	/* is the tag ready for this operation? */
	if(plc_tag_status(t) != PLCTAG_STATUS_OK && plc_tag_status(t) != PLCTAG_ERR_OUT_OF_BOUNDS) {
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




extern int plc_tag_set_uint8(plc_tag t, int offset, uint8_t val)
{
	int rc;

	/* is there a tag? */
	if(!t)
		return PLCTAG_ERR_NULL_PTR;

	rc = plc_tag_status(t);

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











extern int8_t   plc_tag_get_int8(plc_tag t, int offset)
{
	int8_t res = INT8_MIN;

	/* is there a tag? */
	if(!t)
		return res;

	/* is the tag ready for this operation? */
	if(plc_tag_status(t) != PLCTAG_STATUS_OK && plc_tag_status(t) != PLCTAG_ERR_OUT_OF_BOUNDS) {
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




extern int plc_tag_set_int8(plc_tag t, int offset, int8_t val)
{
	int rc;

	/* is there a tag? */
	if(!t)
		return PLCTAG_ERR_NULL_PTR;

	rc = plc_tag_status(t);

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
extern float    plc_tag_get_float32(plc_tag t, int offset)
{
	uint32_t ures;
	float res = INFINITY;

	/* is there a tag? */
	if(!t)
		return res;

	/* is the tag ready for this operation? */
	if(plc_tag_status(t) != PLCTAG_STATUS_OK && plc_tag_status(t) != PLCTAG_ERR_OUT_OF_BOUNDS) {
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
extern int plc_tag_set_float32(plc_tag t, int offset, float fval)
{
	int rc;

	uint32_t val = *((uint32_t *)(&fval));

	/* is there a tag? */
	if(!t)
		return PLCTAG_ERR_NULL_PTR;

	rc = plc_tag_status(t);

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



