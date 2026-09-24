/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
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

/*
 * Core runtime attribute table and the lookup used by the public attribute API.
 *
 * Step one of docs/attribute_redesign.md: the attributes the library core owns move out
 * of the if-else chains in lib.c and into core_attribs[] below.  No protocol module has a
 * table yet, so every protocol attribute still reaches its module through the migration
 * fallback in lib.c.
 */

#include <inttypes.h>
#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/lib/version.h>
#include <platform.h>
#include <stdint.h>
#include <utils/debug.h>


static int32_t core_get_size(plc_tag_p tag, int32_t *result);
static int32_t core_get_read_cache_ms(plc_tag_p tag, int32_t *result);
static int32_t core_set_read_cache_ms(plc_tag_p tag, int32_t value);
static int32_t core_get_auto_sync_read_ms(plc_tag_p tag, int32_t *result);
static int32_t core_set_auto_sync_read_ms(plc_tag_p tag, int32_t value);
static int32_t core_get_auto_sync_write_ms(plc_tag_p tag, int32_t *result);
static int32_t core_set_auto_sync_write_ms(plc_tag_p tag, int32_t value);
static int32_t core_get_bit_num(plc_tag_p tag, int32_t *result);
static int32_t core_get_connection_group_id(plc_tag_p tag, int32_t *result);
static int32_t core_get_allow_field_resize(plc_tag_p tag, int32_t *result);
static int32_t core_set_allow_field_resize(plc_tag_p tag, int32_t value);

static int32_t lib_get_version_major(plc_tag_p tag, int32_t *result);
static int32_t lib_get_version_minor(plc_tag_p tag, int32_t *result);
static int32_t lib_get_version_patch(plc_tag_p tag, int32_t *result);
static int32_t lib_get_debug(plc_tag_p tag, int32_t *result);
static int32_t lib_set_debug(plc_tag_p tag, int32_t value);
static int32_t lib_get_debug_level(plc_tag_p tag, int32_t *result);
static int32_t lib_set_debug_level(plc_tag_p tag, int32_t value);

static const attr_def_t *attr_table_find(const attr_def_t *table, const char *name);


/*
 * Attributes handled by the library core for every tag, whatever its protocol.  A protocol
 * overrides one by listing the same name and type in its own table, and suppresses one by
 * listing it with both accessors NULL.
 */
static const attr_def_t core_attribs[] = {
    {.name = "size",
     .type = ATTR_TYPE_INT,
     .description = "The size of the tag data buffer in bytes.",
     .get_int = core_get_size},

    {.name = "read_cache_ms",
     .type = ATTR_TYPE_INT,
     .description = "Reuse the result of a read for this many milliseconds before going to the PLC again.",
     .get_int = core_get_read_cache_ms,
     .set_int = core_set_read_cache_ms},

    {.name = "auto_sync_read_ms",
     .type = ATTR_TYPE_INT,
     .description = "Automatically read the tag every this many milliseconds.  Zero disables automatic reads.",
     .get_int = core_get_auto_sync_read_ms,
     .set_int = core_set_auto_sync_read_ms},

    {.name = "auto_sync_write_ms",
     .type = ATTR_TYPE_INT,
     .description = "Wait this many milliseconds after a write to the tag data, then write the tag to the PLC.",
     .get_int = core_get_auto_sync_write_ms,
     .set_int = core_set_auto_sync_write_ms},

    {.name = "bit_num",
     .type = ATTR_TYPE_INT,
     .description = "The bit within the PLC tag that this tag maps, for a tag created with a bit number.",
     .get_int = core_get_bit_num},

    {.name = "connection_group_id",
     .type = ATTR_TYPE_INT,
     .description = "The group this tag's session or connection belongs to.  Set when the tag is created.",
     .get_int = core_get_connection_group_id},

    {.name = "allow_field_resize",
     .type = ATTR_TYPE_INT,
     .description = "Allow the tag data buffer to be resized when the PLC reports a different size.",
     .get_int = core_get_allow_field_resize,
     .set_int = core_set_allow_field_resize},

    {.name = NULL},
};


/*
 * Attributes of the library itself, reached with a tag id of zero.  There is no tag at
 * library scope, so these accessors are passed a NULL tag and must not touch it.  The
 * descriptor is shared with the tag tables so that one lookup and one set of public entry
 * points serve both scopes.
 */
static const attr_def_t lib_attribs[] = {
    {.name = "version_major",
     .type = ATTR_TYPE_INT,
     .description = "The major part of the library version.",
     .get_int = lib_get_version_major},

    {.name = "version_minor",
     .type = ATTR_TYPE_INT,
     .description = "The minor part of the library version.",
     .get_int = lib_get_version_minor},

    {.name = "version_patch",
     .type = ATTR_TYPE_INT,
     .description = "The patch part of the library version.",
     .get_int = lib_get_version_patch},

    {.name = "debug",
     .type = ATTR_TYPE_INT,
     .description = "The library-wide logging level, from DEBUG_ERROR to DEBUG_SPEW.",
     .get_int = lib_get_debug,
     .set_int = lib_set_debug},

    /* Deprecated spelling of "debug".  A separate entry so that it can carry its own warning. */
    {.name = "debug_level",
     .type = ATTR_TYPE_INT,
     .description = "Deprecated, use \"debug\". The library-wide logging level.",
     .get_int = lib_get_debug_level,
     .set_int = lib_set_debug_level},

    {.name = NULL},
};


const attr_def_t *attr_find_lib(const char *name) {
    if(!name) { return NULL; }

    return attr_table_find(lib_attribs, name);
}


const attr_def_t *attr_find(plc_tag_p tag, const char *name) {
    const attr_def_t *def = NULL;

    if(!tag || !name) { return NULL; }

    if(tag->vtable) { def = attr_table_find(tag->vtable->attribs, name); }

    if(!def) { def = attr_table_find(core_attribs, name); }

    return def;
}


static const attr_def_t *attr_table_find(const attr_def_t *table, const char *name) {
    if(!table) { return NULL; }

    for(size_t index = 0; table[index].name; index++) {
        if(str_cmp_i(table[index].name, name) == 0) { return &table[index]; }
    }

    return NULL;
}


static int32_t core_get_size(plc_tag_p tag, int32_t *result) {
    *result = tag->size;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_read_cache_ms(plc_tag_p tag, int32_t *result) {
    /* FIXME - what happens if this overflows? */
    *result = (int32_t)tag->read_cache_ms;

    return PLCTAG_STATUS_OK;
}


static int32_t core_set_read_cache_ms(plc_tag_p tag, int32_t value) {
    if(value < 0) { return PLCTAG_ERR_OUT_OF_BOUNDS; }

    /* expire the cache. */
    tag->read_cache_expire = (int64_t)0;
    tag->read_cache_ms = (int64_t)value;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_auto_sync_read_ms(plc_tag_p tag, int32_t *result) {
    *result = tag->auto_sync_read_ms;

    return PLCTAG_STATUS_OK;
}


static int32_t core_set_auto_sync_read_ms(plc_tag_p tag, int32_t value) {
    if(value < 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "auto_sync_read_ms must be greater than or equal to zero!");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    tag->auto_sync_read_ms = value;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_auto_sync_write_ms(plc_tag_p tag, int32_t *result) {
    *result = tag->auto_sync_write_ms;

    return PLCTAG_STATUS_OK;
}


static int32_t core_set_auto_sync_write_ms(plc_tag_p tag, int32_t value) {
    if(value < 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "auto_sync_write_ms must be greater than or equal to zero!");
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    tag->auto_sync_write_ms = value;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_bit_num(plc_tag_p tag, int32_t *result) {
    *result = (int32_t)(uint32_t)(tag->bit);

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_connection_group_id(plc_tag_p tag, int32_t *result) {
    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Getting the connection_group_id for tag %" PRId32 ".", tag->tag_id);

    *result = (int32_t)tag->connection_group_id;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_allow_field_resize(plc_tag_p tag, int32_t *result) {
    *result = (int32_t)tag->allow_field_resize;

    return PLCTAG_STATUS_OK;
}


static int32_t core_set_allow_field_resize(plc_tag_p tag, int32_t value) {
    tag->allow_field_resize = (value > 0 ? 1 : 0);

    return PLCTAG_STATUS_OK;
}


static int32_t lib_get_version_major(plc_tag_p tag, int32_t *result) {
    (void)tag;

    *result = (int32_t)version_major;

    return PLCTAG_STATUS_OK;
}


static int32_t lib_get_version_minor(plc_tag_p tag, int32_t *result) {
    (void)tag;

    *result = (int32_t)version_minor;

    return PLCTAG_STATUS_OK;
}


static int32_t lib_get_version_patch(plc_tag_p tag, int32_t *result) {
    (void)tag;

    *result = (int32_t)version_patch;

    return PLCTAG_STATUS_OK;
}


static int32_t lib_get_debug(plc_tag_p tag, int32_t *result) {
    (void)tag;

    *result = (int32_t)get_debug_level();

    return PLCTAG_STATUS_OK;
}


static int32_t lib_set_debug(plc_tag_p tag, int32_t value) {
    (void)tag;

    /*
     * DEBUG_SPEW is deliberately excluded: it is reachable from the tag creation string but
     * not from this attribute, which is how the library has always behaved here.
     */
    if(value < DEBUG_ERROR || value >= DEBUG_SPEW) { return PLCTAG_ERR_OUT_OF_BOUNDS; }

    set_debug_level((int)value);

    return PLCTAG_STATUS_OK;
}


static int32_t lib_get_debug_level(plc_tag_p tag, int32_t *result) {
    pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Deprecated attribute \"debug_level\" used, use \"debug\" instead.");

    return lib_get_debug(tag, result);
}


static int32_t lib_set_debug_level(plc_tag_p tag, int32_t value) {
    pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, 0, "Deprecated attribute \"debug_level\" used, use \"debug\" instead.");

    return lib_set_debug(tag, value);
}
