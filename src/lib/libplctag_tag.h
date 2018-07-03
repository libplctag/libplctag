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




#include <lib/libplctag.h>
#include <platform.h>
#include <util/attr.h>

#define PLCTAG_CANARY (0xACA7CAFE)
#define PLCTAG_DATA_LITTLE_ENDIAN   (0)
#define PLCTAG_DATA_BIG_ENDIAN      (1)

extern mutex_p global_library_mutex;

typedef struct plc_tag_t *plc_tag_p;


/* define tag operation functions */
typedef int (*tag_abort_func)(plc_tag_p tag);
typedef int (*tag_destroy_func)(plc_tag_p tag);
typedef int (*tag_read_func)(plc_tag_p tag);
typedef int (*tag_status_func)(plc_tag_p tag);
typedef int (*tag_write_func)(plc_tag_p tag);

typedef int (*tag_vtable_func)(plc_tag_p tag);

/* we'll need to set these per protocol type. */
struct tag_vtable_t {
    tag_vtable_func abort;
    /* tag_vtable_func destroy; */
    tag_vtable_func read;
    tag_vtable_func status;
    tag_vtable_func write;
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
                        int tag_id; \
                        int64_t read_cache_expire; \
                        int64_t read_cache_ms; \
                        int size; \
                        uint8_t *data

struct plc_tag_dummy {
    int tag_id;
};

struct plc_tag_t {
    TAG_BASE_STRUCT;
};

#define PLC_TAG_P_NULL ((plc_tag_p)0)


/* the following may need to be used where the tag is already mapped or is not yet mapped */
extern int lib_init(void);
extern void lib_teardown(void);
extern int plc_tag_abort_mapped(plc_tag_p tag);
extern int plc_tag_destroy_mapped(plc_tag_p tag);
extern int plc_tag_status_mapped(plc_tag_p tag);



#endif
