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

/* Core runtime attribute table and the lookup behind the public attribute API. */

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

static int32_t core_get_str_is_counted(plc_tag_p tag, int32_t *result);
static int32_t core_get_str_is_fixed_length(plc_tag_p tag, int32_t *result);
static int32_t core_get_str_is_zero_terminated(plc_tag_p tag, int32_t *result);
static int32_t core_get_str_is_byte_swapped(plc_tag_p tag, int32_t *result);
static int32_t core_get_str_count_word_bytes(plc_tag_p tag, int32_t *result);
static int32_t core_get_str_max_capacity(plc_tag_p tag, int32_t *result);
static int32_t core_get_str_total_length(plc_tag_p tag, int32_t *result);
static int32_t core_get_str_pad_bytes(plc_tag_p tag, int32_t *result);
static int32_t core_get_str_pad_to_multiple_bytes(plc_tag_p tag, int32_t *result);

static int32_t core_get_int16_byte_order(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length);
static int32_t core_get_int16_byte_order_size(plc_tag_p tag);
static int32_t core_get_int32_byte_order(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length);
static int32_t core_get_int32_byte_order_size(plc_tag_p tag);
static int32_t core_get_int64_byte_order(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length);
static int32_t core_get_int64_byte_order_size(plc_tag_p tag);
static int32_t core_get_float32_byte_order(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length);
static int32_t core_get_float32_byte_order_size(plc_tag_p tag);
static int32_t core_get_float64_byte_order(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length);
static int32_t core_get_float64_byte_order_size(plc_tag_p tag);

static const attr_def_t *attr_table_find(const attr_def_t *table, const char *name);


/* Attributes the core handles for every tag.  A protocol overrides one by listing the same
 * name and type, and suppresses one by listing it with both accessors NULL. */
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

    /* String and byte order layout.  Read-only after creation; they live on tag->byte_order. */
    {.name = "str_is_counted",
     .type = ATTR_TYPE_INT,
     .description = "The string data starts with a count word.",
     .get_int = core_get_str_is_counted},

    {.name = "str_is_fixed_length",
     .type = ATTR_TYPE_INT,
     .description = "The string data occupies a fixed number of bytes.",
     .get_int = core_get_str_is_fixed_length},

    {.name = "str_is_zero_terminated",
     .type = ATTR_TYPE_INT,
     .description = "The string data ends with a zero byte.",
     .get_int = core_get_str_is_zero_terminated},

    {.name = "str_is_byte_swapped",
     .type = ATTR_TYPE_INT,
     .description = "The string characters are swapped in pairs.",
     .get_int = core_get_str_is_byte_swapped},

    {.name = "str_count_word_bytes",
     .type = ATTR_TYPE_INT,
     .description = "The size in bytes of the string's count word.",
     .get_int = core_get_str_count_word_bytes},

    {.name = "str_max_capacity",
     .type = ATTR_TYPE_INT,
     .description = "The most characters the string can hold.",
     .get_int = core_get_str_max_capacity},

    {.name = "str_total_length",
     .type = ATTR_TYPE_INT,
     .description = "The total size in bytes a string occupies, including its count word and padding.",
     .get_int = core_get_str_total_length},

    {.name = "str_pad_bytes",
     .type = ATTR_TYPE_INT,
     .description = "The number of padding bytes after the string data.",
     .get_int = core_get_str_pad_bytes},

    {.name = "str_pad_to_multiple_bytes",
     .type = ATTR_TYPE_INT,
     .description = "Pad the string data out to a multiple of this many bytes.",
     .get_int = core_get_str_pad_to_multiple_bytes},

    {.name = "int16_byte_order",
     .type = ATTR_TYPE_STRING,
     .description = "The byte order of a 16-bit integer, as the comma-separated list the tag string uses.",
     .get_bytes = core_get_int16_byte_order,
     .get_bytes_size = core_get_int16_byte_order_size},

    {.name = "int32_byte_order",
     .type = ATTR_TYPE_STRING,
     .description = "The byte order of a 32-bit integer, as the comma-separated list the tag string uses.",
     .get_bytes = core_get_int32_byte_order,
     .get_bytes_size = core_get_int32_byte_order_size},

    {.name = "int64_byte_order",
     .type = ATTR_TYPE_STRING,
     .description = "The byte order of a 64-bit integer, as the comma-separated list the tag string uses.",
     .get_bytes = core_get_int64_byte_order,
     .get_bytes_size = core_get_int64_byte_order_size},

    {.name = "float32_byte_order",
     .type = ATTR_TYPE_STRING,
     .description = "The byte order of a 32-bit float, as the comma-separated list the tag string uses.",
     .get_bytes = core_get_float32_byte_order,
     .get_bytes_size = core_get_float32_byte_order_size},

    {.name = "float64_byte_order",
     .type = ATTR_TYPE_STRING,
     .description = "The byte order of a 64-bit float, as the comma-separated list the tag string uses.",
     .get_bytes = core_get_float64_byte_order,
     .get_bytes_size = core_get_float64_byte_order_size},

    {.name = NULL},
};


/* Attributes of the library itself, reached with a tag id of zero.  These accessors are
 * passed a NULL tag and must not touch it. */
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

    /* DEBUG_SPEW is deliberately excluded here; only the tag creation string can reach it. */
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


int32_t attr_string_size(const char *str) {
    if(!str) { return PLCTAG_ERR_NO_DATA; }

    /* the terminator is part of the value. */
    return (int32_t)(str_length(str) + 1);
}


int32_t attr_copy_string(const char *str, uint8_t *buffer, int32_t buffer_length) {
    int32_t size = attr_string_size(str);

    if(size < 0) { return size; }

    if(size > buffer_length) { return PLCTAG_ERR_TOO_SMALL; }

    mem_copy((void *)buffer, (void *)str, (int)size);

    return size;
}


/* Render a byte order array as the "0,1,2,3" text a tag string takes.  Each position is one
 * digit; an order array is never longer than eight entries. */
static int32_t attr_format_byte_order(const int *order, size_t order_len, char *out, size_t out_len) {
    size_t used = 0;

    if(!order || order_len == 0 || out_len < ((order_len * 2))) { return PLCTAG_ERR_TOO_SMALL; }

    for(size_t index = 0; index < order_len; index++) {
        if(index > 0) { out[used++] = ','; }

        out[used++] = (char)('0' + (order[index] % 10));
    }

    out[used] = (char)0;

    return (int32_t)(used + 1);
}


int32_t attr_byte_order_size(const int *order, size_t order_len) {
    char scratch[32] = {0};

    return attr_format_byte_order(order, order_len, &scratch[0], sizeof(scratch));
}


int32_t attr_copy_byte_order(const int *order, size_t order_len, uint8_t *buffer, int32_t buffer_length) {
    char scratch[32] = {0};
    int32_t size = attr_format_byte_order(order, order_len, &scratch[0], sizeof(scratch));

    if(size < 0) { return size; }

    if(size > buffer_length) { return PLCTAG_ERR_TOO_SMALL; }

    mem_copy((void *)buffer, (void *)&scratch[0], (int)size);

    return size;
}


static int32_t core_get_str_is_counted(plc_tag_p tag, int32_t *result) {
    *result = (int32_t)tag->byte_order->str_is_counted;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_is_fixed_length(plc_tag_p tag, int32_t *result) {
    *result = (int32_t)tag->byte_order->str_is_fixed_length;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_is_zero_terminated(plc_tag_p tag, int32_t *result) {
    *result = (int32_t)tag->byte_order->str_is_zero_terminated;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_is_byte_swapped(plc_tag_p tag, int32_t *result) {
    *result = (int32_t)tag->byte_order->str_is_byte_swapped;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_count_word_bytes(plc_tag_p tag, int32_t *result) {
    *result = (int32_t)tag->byte_order->str_count_word_bytes;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_max_capacity(plc_tag_p tag, int32_t *result) {
    *result = (int32_t)tag->byte_order->str_max_capacity;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_total_length(plc_tag_p tag, int32_t *result) {
    *result = (int32_t)tag->byte_order->str_total_length;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_pad_bytes(plc_tag_p tag, int32_t *result) {
    *result = (int32_t)tag->byte_order->str_pad_bytes;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_pad_to_multiple_bytes(plc_tag_p tag, int32_t *result) {
    *result = (int32_t)tag->byte_order->str_pad_to_multiple_bytes;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_int16_byte_order(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length) {
    return attr_copy_byte_order(&tag->byte_order->int16_order[0], 2, buffer, buffer_length);
}


static int32_t core_get_int16_byte_order_size(plc_tag_p tag) {
    return attr_byte_order_size(&tag->byte_order->int16_order[0], 2);
}


static int32_t core_get_int32_byte_order(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length) {
    return attr_copy_byte_order(&tag->byte_order->int32_order[0], 4, buffer, buffer_length);
}


static int32_t core_get_int32_byte_order_size(plc_tag_p tag) {
    return attr_byte_order_size(&tag->byte_order->int32_order[0], 4);
}


static int32_t core_get_int64_byte_order(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length) {
    return attr_copy_byte_order(&tag->byte_order->int64_order[0], 8, buffer, buffer_length);
}


static int32_t core_get_int64_byte_order_size(plc_tag_p tag) {
    return attr_byte_order_size(&tag->byte_order->int64_order[0], 8);
}


static int32_t core_get_float32_byte_order(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length) {
    return attr_copy_byte_order(&tag->byte_order->float32_order[0], 4, buffer, buffer_length);
}


static int32_t core_get_float32_byte_order_size(plc_tag_p tag) {
    return attr_byte_order_size(&tag->byte_order->float32_order[0], 4);
}


static int32_t core_get_float64_byte_order(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length) {
    return attr_copy_byte_order(&tag->byte_order->float64_order[0], 8, buffer, buffer_length);
}


static int32_t core_get_float64_byte_order_size(plc_tag_p tag) {
    return attr_byte_order_size(&tag->byte_order->float64_order[0], 8);
}
