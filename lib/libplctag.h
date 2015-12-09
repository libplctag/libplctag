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
 * 2012-02-22  KRH - Created file.                                        *
 *                                                                        *
 * 2013-05-03  KRH - Removed left-over cruft from previous version of API *
 *                                                                        *
 * 2015-02-21	KRH - Tweaked LIB_EXPORT definition to work with C++.      *
 **************************************************************************/

#ifndef __LIBPLCTAG_H__
#define __LIBPLCTAG_H__


#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


#ifdef WIN32
	#ifdef __cplusplus
		#define C_FUNC extern "C"
	#else
		#define C_FUNC
	#endif
	#ifdef LIBPLCTAGDLL_EXPORTS
		#define LIB_EXPORT __declspec(dllexport)
	#else
		#define LIB_EXPORT __declspec(dllimport)
	#endif
#else
	#define LIB_EXPORT extern
#endif


	/* WARNING THIS IS MACHINE/COMPILER DEPENDENT!!!! */
	typedef float real32_t;


	/* opaque type definitions */
	typedef struct plc_tag_t *plc_tag;
#define PLC_TAG_NULL ((plc_tag)NULL)



	/* library internal status. */
#define PLCTAG_STATUS_PENDING		(1)
#define PLCTAG_STATUS_OK			(0)

#define PLCTAG_ERR_NULL_PTR			(-1)
#define PLCTAG_ERR_OUT_OF_BOUNDS	(-2)
#define PLCTAG_ERR_NO_MEM			(-3)
#define PLCTAG_ERR_LL_ADD			(-4)
#define PLCTAG_ERR_BAD_PARAM		(-5)
#define PLCTAG_ERR_CREATE			(-6)
#define PLCTAG_ERR_NOT_EMPTY		(-7)
#define PLCTAG_ERR_OPEN				(-8)
#define PLCTAG_ERR_SET				(-9)
#define PLCTAG_ERR_WRITE			(-10)
#define PLCTAG_ERR_TIMEOUT			(-11)
#define PLCTAG_ERR_TIMEOUT_ACK		(-12)
#define PLCTAG_ERR_RETRIES			(-13)
#define PLCTAG_ERR_READ				(-14)
#define PLCTAG_ERR_BAD_DATA			(-15)
#define PLCTAG_ERR_ENCODE			(-16)
#define PLCTAG_ERR_DECODE			(-17)
#define PLCTAG_ERR_UNSUPPORTED		(-18)
#define PLCTAG_ERR_TOO_LONG			(-19)
#define PLCTAG_ERR_CLOSE			(-20)
#define PLCTAG_ERR_NOT_ALLOWED		(-21)
#define PLCTAG_ERR_THREAD			(-22)
#define PLCTAG_ERR_NO_DATA			(-23)
#define PLCTAG_ERR_THREAD_JOIN		(-24)
#define PLCTAG_ERR_THREAD_CREATE	(-25)
#define PLCTAG_ERR_MUTEX_DESTROY	(-26)
#define PLCTAG_ERR_MUTEX_UNLOCK		(-27)
#define PLCTAG_ERR_MUTEX_INIT		(-28)
#define PLCTAG_ERR_MUTEX_LOCK		(-29)
#define PLCTAG_ERR_NOT_IMPLEMENTED	(-30)
#define PLCTAG_ERR_BAD_DEVICE		(-31)
#define PLCTAG_ERR_BAD_GATEWAY		(-32)
#define PLCTAG_ERR_REMOTE_ERR		(-33)
#define PLCTAG_ERR_NOT_FOUND		(-34)
#define PLCTAG_ERR_ABORT			(-35)
#define PLCTAG_ERR_WINSOCK			(-36)




	/*
	 * tag functions
	 *
	 * The following is the public API for tag operations.
	 *
	 * These are implemented in a protocol-specific manner.
	 */

	/*
	 * plc_tag_create
	 *
	 * Create a new tag based on the passed attributed string.  The attributes
	 * are protocol-specific.  The only required part of the string is the key-
	 * value pair "protocol=XXX" where XXX is one of the supported protocol
	 * types.
	 *
	 * An opaque pointer is returned on success.  NULL is returned on allocation
	 * failure.  Other failures will set the tag status.
	 */

	LIB_EXPORT plc_tag plc_tag_create(const char *attrib_str);


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

	LIB_EXPORT int plc_tag_lock(plc_tag tag);



	/*
	 * plc_tag_unlock
	 *
	 * The opposite action of plc_tag_unlock.  This allows other threads to access the
	 * tag.
	 */

	LIB_EXPORT int plc_tag_unlock(plc_tag tag);





	/*
	 * plc_tag_abort
	 *
	 * Abort any outstanding IO to the PLC.  If there is something in flight, then
	 * it is marked invalid.  Note that this does not abort anything that might
	 * be still processing in the report PLC.
	 *
	 * The status will be PLCTAG_STATUS_OK unless there is an error such as
	 * a null pointer.
	 *
	 * This is a function provided by the underlying protocol implementation.
	 */
	LIB_EXPORT int plc_tag_abort(plc_tag tag);




	/*
	 * plc_tag_destroy
	 *
	 * This frees all resources associated with the tag.  Internally, it may result in closed
	 * connections etc.   This calls through to a protocol-specific function.
	 *
	 * This is a function provided by the underlying protocol implementation.
	 */
	LIB_EXPORT int plc_tag_destroy(plc_tag tag);






	/*
	 * plc_tag_read
	 *
	 * Start a read.  If the timeout value is zero, then wait until the read
	 * returns or the timeout occurs, whichever is first.  Return the status.
	 * If the timeout value is zero, then plc_tag_read will normally return
	 * PLCTAG_STATUS_PENDING.
	 *
	 * This is a function provided by the underlying protocol implementation.
	 */
	LIB_EXPORT int plc_tag_read(plc_tag tag, int timeout);




	/*
	 * plc_tag_status
	 *
	 * Return the current status of the tag.  This will be PLCTAG_STATUS_PENDING if there is
	 * an uncompleted IO operation.  It will be PLCTAG_STATUS_OK if everything is fine.  Other
	 * errors will be returned as appropriate.
	 *
	 * This is a function provided by the underlying protocol implementation.
	 */
	LIB_EXPORT int plc_tag_status(plc_tag tag);






	/*
	 * plc_tag_write
	 *
	 * Start a write.  If the timeout value is zero, then wait until the write
	 * returns or the timeout occurs, whichever is first.  Return the status.
	 * If the timeout value is zero, then plc_tag_write will usually return
	 * PLCTAG_STATUS_PENDING.  The write is considered done
	 * when it has been written to the socket.
	 *
	 * This is a function provided by the underlying protocol implementation.
	 */
	LIB_EXPORT int plc_tag_write(plc_tag tag, int timeout);




	/*
	 * Tag data accessors.
	 */

	LIB_EXPORT int plc_tag_get_size(plc_tag tag);

	LIB_EXPORT uint32_t plc_tag_get_uint32(plc_tag tag, int offset);
	LIB_EXPORT int plc_tag_set_uint32(plc_tag tag, int offset, uint32_t val);

	LIB_EXPORT int32_t plc_tag_get_int32(plc_tag tag, int offset);
	LIB_EXPORT int plc_tag_set_int32(plc_tag, int offset, int32_t val);


	LIB_EXPORT uint16_t plc_tag_get_uint16(plc_tag tag, int offset);
	LIB_EXPORT int plc_tag_set_uint16(plc_tag tag, int offset, uint16_t val);

	LIB_EXPORT int16_t plc_tag_get_int16(plc_tag tag, int offset);
	LIB_EXPORT int plc_tag_set_int16(plc_tag, int offset, int16_t val);


	LIB_EXPORT uint8_t plc_tag_get_uint8(plc_tag tag, int offset);
	LIB_EXPORT int plc_tag_set_uint8(plc_tag tag, int offset, uint8_t val);

	LIB_EXPORT int8_t plc_tag_get_int8(plc_tag tag, int offset);
	LIB_EXPORT int plc_tag_set_int8(plc_tag, int offset, int8_t val);


	LIB_EXPORT float plc_tag_get_float32(plc_tag tag, int offset);
	LIB_EXPORT int plc_tag_set_float32(plc_tag tag, int offset, float val);


#ifdef __cplusplus
}
#endif



/*end of header */
#endif
