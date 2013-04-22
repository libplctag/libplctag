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
  **************************************************************************/


#ifndef __LIBPLCTAG_TAG_H__
#define __LIBPLCTAG_TAG_H__




#include "libplctag.h"
#include <platform.h>
#include <util/attr.h>

#define PCOMM_CANARY (0xACA7CAFE)
#define PCOMM_DATA_LITTLE_ENDIAN	(0)
#define PCOMM_DATA_BIG_ENDIAN		(1)


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

struct plc_tag_t {
	tag_vtable_p vtable;
	int status;
	int endian;
	int size;
	uint8_t *data;
};





#endif

