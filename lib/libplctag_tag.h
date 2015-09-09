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
  **************************************************************************/


#ifndef __LIBPLCTAG_TAG_H__
#define __LIBPLCTAG_TAG_H__




#include "libplctag.h"
#include <platform.h>
#include <util/attr.h>

#define PLCTAG_CANARY (0xACA7CAFE)
#define PLCTAG_DATA_LITTLE_ENDIAN	(0)
#define PLCTAG_DATA_BIG_ENDIAN		(1)


/*
 * plc_err
 *
 * Use this routine to dump errors into the logs yourself.  Note
 * that it is a macro and thus the line number and function
 * will only be those of the C code.  If you use a wrapper around
 * this, you may want to call the underlying plc_err_impl
 * routine directly.
 */

/* helper functions for logging/errors */
/*LIB_EXPORT void plc_err_impl(int level, const char *func, int line_num, int err_code, const char *fmt, ...);
#if defined(USE_STD_VARARG_MACROS) || defined(WIN32)
#define plc_err(lib,level,e,f,...) \
   plc_err_impl(lib,level,__PRETTY_FUNCTION__,__LINE__,e,f,__VA_ARGS__)
#else
#define plc_err(lib,level,e,f,a...) \
   plc_err_impl(lib,level,__PRETTY_FUNCTION__,__LINE__,e,f,##a )
#endif
*/



/* define tag operation functions */
typedef int (*tag_abort_func)(plc_tag tag);
typedef int (*tag_destroy_func)(plc_tag tag);
typedef int (*tag_read_func)(plc_tag);
typedef int (*tag_status_func)(plc_tag);
typedef int (*tag_write_func)(plc_tag tag);

/* we'll need to set these per protocol type. */
struct tag_vtable_t {
	tag_abort_func 			abort;
	tag_destroy_func 		destroy;
	tag_read_func			read;
	tag_status_func 		status;
	tag_write_func 			write;
};

typedef struct tag_vtable_t *tag_vtable_p;


/*
 * The base definition of the tag structure.  This is used
 * by the protocol-specific implementations.
 *
 * The base type only has a vtable for operations.
 */

#define TAG_BASE_STRUCT tag_vtable_p vtable; \
						mutex_p mut; \
						int status; \
						int endian; \
						int debug; \
						uint64_t read_cache_expire; \
						uint64_t read_cache_ms; \
						int size; \
						uint8_t *data

struct plc_tag_t {
	TAG_BASE_STRUCT;
};





#endif

