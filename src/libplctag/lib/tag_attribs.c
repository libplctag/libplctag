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
#include <libplctag/lib/conn_stats.h>
#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/lib/version.h>
#include <platform.h>
#include <stdint.h>
#include <utils/attr.h>
#include <utils/debug.h>
#include <utils/time_utils.h>


static int32_t core_get_size(plc_tag_p tag, int32_t *result);
static int32_t core_set_size(plc_tag_p tag, int32_t value);
static int32_t core_get_read_cache_ms(plc_tag_p tag, int32_t *result);
static int32_t core_set_read_cache_ms(plc_tag_p tag, int32_t value);
static int32_t core_get_auto_sync_read_ms(plc_tag_p tag, int32_t *result);
static int32_t core_set_auto_sync_read_ms(plc_tag_p tag, int32_t value);
static int32_t core_get_auto_sync_write_ms(plc_tag_p tag, int32_t *result);
static int32_t core_set_auto_sync_write_ms(plc_tag_p tag, int32_t value);
static int32_t core_get_destroy_tag_after_idle_sec(plc_tag_p tag, int32_t *result);
static int32_t core_set_destroy_tag_after_idle_sec(plc_tag_p tag, int32_t value);
static int32_t core_get_bit_num(plc_tag_p tag, int32_t *result);
static int32_t core_get_connection_group_id(plc_tag_p tag, int32_t *result);
static int32_t core_get_allow_field_resize(plc_tag_p tag, int32_t *result);
static int32_t core_set_allow_field_resize(plc_tag_p tag, int32_t value);
static int32_t core_get_find_or_create_tag(plc_tag_p tag, int32_t *result);

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

/* connection statistics, on @connection tags. */
static int32_t stats_get_connect_count(plc_tag_p tag, int32_t *result);
static int32_t stats_get_connect_failure_count(plc_tag_p tag, int32_t *result);
static int32_t stats_get_idle_disconnect_count(plc_tag_p tag, int32_t *result);
static int32_t stats_get_retry_delay_ms(plc_tag_p tag, int32_t *result);
static int32_t stats_get_retry_wait_left_ms(plc_tag_p tag, int32_t *result);
static int32_t stats_get_tag_count(plc_tag_p tag, int32_t *result);
static int32_t stats_get_requests_in_flight(plc_tag_p tag, int32_t *result);
static int32_t stats_get_response_time_us(plc_tag_p tag, int32_t *result);
static int32_t stats_get_response_time_avg_us(plc_tag_p tag, int32_t *result);
static int32_t stats_get_response_time_min_us(plc_tag_p tag, int32_t *result);
static int32_t stats_get_response_time_max_us(plc_tag_p tag, int32_t *result);
static int32_t stats_get_connection_age_ms(plc_tag_p tag, int64_t *result);
static int32_t stats_get_connected_time_ms(plc_tag_p tag, int64_t *result);
static int32_t stats_get_requests_sent(plc_tag_p tag, int64_t *result);
static int32_t stats_get_requests_sent_since_connect(plc_tag_p tag, int64_t *result);
static int32_t stats_get_responses_received(plc_tag_p tag, int64_t *result);
static int32_t stats_get_responses_received_since_connect(plc_tag_p tag, int64_t *result);
static int32_t stats_get_bytes_sent(plc_tag_p tag, int64_t *result);
static int32_t stats_get_bytes_sent_since_connect(plc_tag_p tag, int64_t *result);
static int32_t stats_get_bytes_received(plc_tag_p tag, int64_t *result);
static int32_t stats_get_bytes_received_since_connect(plc_tag_p tag, int64_t *result);
static int32_t stats_get_tag_ops_completed(plc_tag_p tag, int64_t *result);
static int32_t stats_get_tag_ops_completed_since_connect(plc_tag_p tag, int64_t *result);
static int32_t stats_get_request_timeouts(plc_tag_p tag, int64_t *result);
static int32_t stats_get_error_responses(plc_tag_p tag, int64_t *result);
static int32_t retry_wait_left_ms(conn_stats_t *s);
static int64_t connected_time_ms(conn_stats_t *s);

static const attr_def_t *attr_table_find(const attr_def_t *table, const char *name);


/* Attributes the core handles for every tag.  A protocol overrides one by listing the same
 * name and type, and suppresses one by listing it with both accessors NULL. */
static const attr_def_t core_attribs[] = {
    {.name = "size",
     .type = ATTR_TYPE_INT,
     .description = "The size of the tag data buffer in bytes.  Setting it resizes the buffer, as plc_tag_set_size() does.",
     .get_int = core_get_size,
     .set_int = core_set_size},

    {.name = "readcachems",
     .type = ATTR_TYPE_INT,
     .description = "Reuse the result of a read for this many milliseconds before going to the PLC again.",
     .get_int = core_get_read_cache_ms,
     .set_int = core_set_read_cache_ms},

    {.name = "autosyncreadms",
     .type = ATTR_TYPE_INT,
     .description = "Automatically read the tag every this many milliseconds.  Zero disables automatic reads.",
     .get_int = core_get_auto_sync_read_ms,
     .set_int = core_set_auto_sync_read_ms},

    {.name = "autosyncwritems",
     .type = ATTR_TYPE_INT,
     .description = "Wait this many milliseconds after a write to the tag data, then write the tag to the PLC.",
     .get_int = core_get_auto_sync_write_ms,
     .set_int = core_set_auto_sync_write_ms},

    {.name = "destroytagafteridlesec",
     .type = ATTR_TYPE_INT,
     .description = "Destroy the tag after this many seconds with no read or write.  Zero disables it, the minimum is 10.",
     .get_int = core_get_destroy_tag_after_idle_sec,
     .set_int = core_set_destroy_tag_after_idle_sec},

    {.name = "bitnum",
     .type = ATTR_TYPE_INT,
     .description = "The bit within the PLC tag that this tag maps, for a tag created with a bit number.",
     .get_int = core_get_bit_num},

    {.name = "connectiongroupid",
     .type = ATTR_TYPE_INT,
     .description = "The group this tag's session or connection belongs to.  Set when the tag is created.",
     .get_int = core_get_connection_group_id},

    {.name = "allowfieldresize",
     .type = ATTR_TYPE_INT,
     .description = "Allow the tag data buffer to be resized when the PLC reports a different size.",
     .get_int = core_get_allow_field_resize,
     .set_int = core_set_allow_field_resize},

    {.name = "findorcreatetag",
     .type = ATTR_TYPE_INT,
     .description = "A create with exactly the same tag string returns this tag.  Set when the tag is created.",
     .get_int = core_get_find_or_create_tag},

    /* String and byte order layout.  Read-only after creation; they live on tag->byte_order. */
    {.name = "striscounted",
     .type = ATTR_TYPE_INT,
     .description = "The string data starts with a count word.",
     .get_int = core_get_str_is_counted},

    {.name = "strisfixedlength",
     .type = ATTR_TYPE_INT,
     .description = "The string data occupies a fixed number of bytes.",
     .get_int = core_get_str_is_fixed_length},

    {.name = "striszeroterminated",
     .type = ATTR_TYPE_INT,
     .description = "The string data ends with a zero byte.",
     .get_int = core_get_str_is_zero_terminated},

    {.name = "strisbyteswapped",
     .type = ATTR_TYPE_INT,
     .description = "The string characters are swapped in pairs.",
     .get_int = core_get_str_is_byte_swapped},

    {.name = "strcountwordbytes",
     .type = ATTR_TYPE_INT,
     .description = "The size in bytes of the string's count word.",
     .get_int = core_get_str_count_word_bytes},

    {.name = "strmaxcapacity",
     .type = ATTR_TYPE_INT,
     .description = "The most characters the string can hold.",
     .get_int = core_get_str_max_capacity},

    {.name = "strtotallength",
     .type = ATTR_TYPE_INT,
     .description = "The total size in bytes a string occupies, including its count word and padding.",
     .get_int = core_get_str_total_length},

    {.name = "strpadbytes",
     .type = ATTR_TYPE_INT,
     .description = "The number of padding bytes after the string data.",
     .get_int = core_get_str_pad_bytes},

    {.name = "strpadtomultiplebytes",
     .type = ATTR_TYPE_INT,
     .description = "Pad the string data out to a multiple of this many bytes.",
     .get_int = core_get_str_pad_to_multiple_bytes},

    {.name = "writepadalignmentbytes",
     .type = ATTR_TYPE_INT,
     .description = "Pad written data out to a multiple of this many bytes: 1, 2 or 4.",
     .get_int = core_get_str_pad_to_multiple_bytes},

    {.name = "int16byteorder",
     .type = ATTR_TYPE_STRING,
     .description = "The byte order of a 16-bit integer, as the comma-separated list the tag string uses.",
     .get_bytes = core_get_int16_byte_order,
     .get_bytes_size = core_get_int16_byte_order_size},

    {.name = "int32byteorder",
     .type = ATTR_TYPE_STRING,
     .description = "The byte order of a 32-bit integer, as the comma-separated list the tag string uses.",
     .get_bytes = core_get_int32_byte_order,
     .get_bytes_size = core_get_int32_byte_order_size},

    {.name = "int64byteorder",
     .type = ATTR_TYPE_STRING,
     .description = "The byte order of a 64-bit integer, as the comma-separated list the tag string uses.",
     .get_bytes = core_get_int64_byte_order,
     .get_bytes_size = core_get_int64_byte_order_size},

    {.name = "float32byteorder",
     .type = ATTR_TYPE_STRING,
     .description = "The byte order of a 32-bit float, as the comma-separated list the tag string uses.",
     .get_bytes = core_get_float32_byte_order,
     .get_bytes_size = core_get_float32_byte_order_size},

    {.name = "float64byteorder",
     .type = ATTR_TYPE_STRING,
     .description = "The byte order of a 64-bit float, as the comma-separated list the tag string uses.",
     .get_bytes = core_get_float64_byte_order,
     .get_bytes_size = core_get_float64_byte_order_size},

    /* connection statistics: PLCTAG_ERR_UNSUPPORTED on a tag that is not an @connection tag. */
    {.name = "connectcount",
     .type = ATTR_TYPE_INT,
     .description = "@connection only.  Successful connects since the connection was created, the first included.",
     .get_int = stats_get_connect_count},

    {.name = "connectfailurecount",
     .type = ATTR_TYPE_INT,
     .description = "@connection only.  Connect attempts that failed before the connection came up.",
     .get_int = stats_get_connect_failure_count},

    {.name = "idledisconnectcount",
     .type = ATTR_TYPE_INT,
     .description = "@connection only.  Disconnects after connection_inactivity_timeout_ms with nothing to do.",
     .get_int = stats_get_idle_disconnect_count},

    {.name = "retrydelayms",
     .type = ATTR_TYPE_INT,
     .description = "@connection only.  The latest wait before reconnecting after an error, in milliseconds.",
     .get_int = stats_get_retry_delay_ms},

    {.name = "retrywaitleftms",
     .type = ATTR_TYPE_INT,
     .description = "@connection only.  Milliseconds left before the next reconnect while waiting after an error, otherwise 0.",
     .get_int = stats_get_retry_wait_left_ms},

    {.name = "tagcount",
     .type = ATTR_TYPE_INT,
     .description = "@connection only.  Tags using the connection, @connection tags included.",
     .get_int = stats_get_tag_count},

    {.name = "requestsinflight",
     .type = ATTR_TYPE_INT,
     .description = "@connection only.  Requests sent and not yet answered.",
     .get_int = stats_get_requests_in_flight},

    {.name = "responsetimeus",
     .type = ATTR_TYPE_INT,
     .description = "@connection only.  The latest request's round trip, in microseconds.",
     .get_int = stats_get_response_time_us},

    {.name = "responsetimeavgus",
     .type = ATTR_TYPE_INT,
     .description = "@connection only.  Average round trip since the latest connect in microseconds, weighting each new request 1/8.",
     .get_int = stats_get_response_time_avg_us},

    {.name = "responsetimeminus",
     .type = ATTR_TYPE_INT,
     .description = "@connection only.  Shortest round trip since the latest connect, in microseconds.",
     .get_int = stats_get_response_time_min_us},

    {.name = "responsetimemaxus",
     .type = ATTR_TYPE_INT,
     .description = "@connection only.  Longest round trip since the latest connect, in microseconds.",
     .get_int = stats_get_response_time_max_us},

    {.name = "connectionagems",
     .type = ATTR_TYPE_INT64,
     .description = "@connection only.  Milliseconds since the connection was created.",
     .get_int64 = stats_get_connection_age_ms},

    {.name = "connectedtimems",
     .type = ATTR_TYPE_INT64,
     .description = "@connection only.  Milliseconds since the latest connect, 0 while not connected.",
     .get_int64 = stats_get_connected_time_ms},

    {.name = "requestssent",
     .type = ATTR_TYPE_INT64,
     .description = "@connection only.  Packets sent since the connection was created.  A packed request counts once.",
     .get_int64 = stats_get_requests_sent},

    {.name = "requestssentsinceconnect",
     .type = ATTR_TYPE_INT64,
     .description = "@connection only.  Packets sent since the latest connect.",
     .get_int64 = stats_get_requests_sent_since_connect},

    {.name = "responsesreceived",
     .type = ATTR_TYPE_INT64,
     .description = "@connection only.  Replies received since the connection was created.",
     .get_int64 = stats_get_responses_received},

    {.name = "responsesreceivedsinceconnect",
     .type = ATTR_TYPE_INT64,
     .description = "@connection only.  Replies received since the latest connect.",
     .get_int64 = stats_get_responses_received_since_connect},

    {.name = "bytessent",
     .type = ATTR_TYPE_INT64,
     .description = "@connection only.  Bytes written to the socket since the connection was created.",
     .get_int64 = stats_get_bytes_sent},

    {.name = "bytessentsinceconnect",
     .type = ATTR_TYPE_INT64,
     .description = "@connection only.  Bytes written to the socket since the latest connect.",
     .get_int64 = stats_get_bytes_sent_since_connect},

    {.name = "bytesreceived",
     .type = ATTR_TYPE_INT64,
     .description = "@connection only.  Bytes read from the socket since the connection was created.",
     .get_int64 = stats_get_bytes_received},

    {.name = "bytesreceivedsinceconnect",
     .type = ATTR_TYPE_INT64,
     .description = "@connection only.  Bytes read from the socket since the latest connect.",
     .get_int64 = stats_get_bytes_received_since_connect},

    {.name = "tagopscompleted",
     .type = ATTR_TYPE_INT64,
     .description = "@connection only.  Tag reads and writes completed; each tag in a packed request counts.",
     .get_int64 = stats_get_tag_ops_completed},

    {.name = "tagopscompletedsinceconnect",
     .type = ATTR_TYPE_INT64,
     .description = "@connection only.  Tag reads and writes completed since the latest connect.",
     .get_int64 = stats_get_tag_ops_completed_since_connect},

    {.name = "requesttimeouts",
     .type = ATTR_TYPE_INT64,
     .description = "@connection only.  Requests that got no reply in time.",
     .get_int64 = stats_get_request_timeouts},

    {.name = "errorresponses",
     .type = ATTR_TYPE_INT64,
     .description = "@connection only.  Replies with a CIP or Modbus error status.",
     .get_int64 = stats_get_error_responses},

    {.name = NULL},
};


/* Attributes of the library itself, reached with a tag id of zero.  These accessors are
 * passed a NULL tag and must not touch it. */
static const attr_def_t lib_attribs[] = {
    {.name = "versionmajor",
     .type = ATTR_TYPE_INT,
     .description = "The major part of the library version.",
     .get_int = lib_get_version_major},

    {.name = "versionminor",
     .type = ATTR_TYPE_INT,
     .description = "The minor part of the library version.",
     .get_int = lib_get_version_minor},

    {.name = "versionpatch",
     .type = ATTR_TYPE_INT,
     .description = "The patch part of the library version.",
     .get_int = lib_get_version_patch},

    {.name = "debug",
     .type = ATTR_TYPE_INT,
     .description = "The library-wide logging level, from DEBUG_ERROR to DEBUG_SPEW.",
     .get_int = lib_get_debug,
     .set_int = lib_set_debug},

    /* Deprecated spelling of "debug".  A separate entry so that it can carry its own warning. */
    {.name = "debuglevel",
     .type = ATTR_TYPE_INT,
     .description = "Deprecated, use \"debug\". The library-wide logging level.",
     .get_int = lib_get_debug_level,
     .set_int = lib_set_debug_level},

    {.name = NULL},
};


const attr_def_t *attr_find_lib(const char *name) {
    char canonical[ATTR_NAME_MAX_SIZE];

    if(attr_canonical_name(canonical, (int32_t)sizeof(canonical), name) < 0) { return NULL; }

    return attr_table_find(lib_attribs, canonical);
}


const attr_def_t *attr_find(plc_tag_p tag, const char *name) {
    const attr_def_t *def = NULL;
    char canonical[ATTR_NAME_MAX_SIZE];

    if(!tag) { return NULL; }

    /* a name too long to canonicalize here is longer than any name in the tables. */
    if(attr_canonical_name(canonical, (int32_t)sizeof(canonical), name) < 0) { return NULL; }

    if(tag->vtable) { def = attr_table_find(tag->vtable->attribs, canonical); }

    if(!def) { def = attr_table_find(core_attribs, canonical); }

    return def;
}


/* name is canonical, and so is every name in the tables. */
static const attr_def_t *attr_table_find(const attr_def_t *table, const char *name) {
    if(!table) { return NULL; }

    for(size_t index = 0; table[index].name; index++) {
        if(str_cmp(table[index].name, name) == 0) { return &table[index]; }
    }

    return NULL;
}


static int32_t core_get_size(plc_tag_p tag, int32_t *result) {
    *result = tag->size;

    return PLCTAG_STATUS_OK;
}


static int32_t core_set_size(plc_tag_p tag, int32_t value) { return tag_set_size_unsafe(tag, value); }


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

    if(value > 0 && atomic_get_int32(&tag->destroy_idle_sec) > 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "auto_sync_read_ms cannot be used with destroy_tag_after_idle_sec!");
        return PLCTAG_ERR_NOT_ALLOWED;
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

    if(value > 0 && atomic_get_int32(&tag->destroy_idle_sec) > 0) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "auto_sync_write_ms cannot be used with destroy_tag_after_idle_sec!");
        return PLCTAG_ERR_NOT_ALLOWED;
    }

    tag->auto_sync_write_ms = value;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_destroy_tag_after_idle_sec(plc_tag_p tag, int32_t *result) {
    *result = atomic_get_int32(&tag->destroy_idle_sec);

    return PLCTAG_STATUS_OK;
}


static int32_t core_set_destroy_tag_after_idle_sec(plc_tag_p tag, int32_t value) {
    if(value < 0 || (value > 0 && value < TAG_AUTO_DESTROY_MIN_SEC)) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "destroy_tag_after_idle_sec must be zero or at least %d!",
               TAG_AUTO_DESTROY_MIN_SEC);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(value > 0 && !tag_can_auto_destroy(tag)) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "destroy_tag_after_idle_sec cannot be used with this kind of tag!");
        return PLCTAG_ERR_NOT_ALLOWED;
    }

    if(value > 0 && (tag->auto_sync_read_ms > 0 || tag->auto_sync_write_ms > 0)) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "destroy_tag_after_idle_sec cannot be used with auto-sync!");
        return PLCTAG_ERR_NOT_ALLOWED;
    }

    /* a new idle time counts from now. */
    if(value > 0) { tag->last_activity_ms = time_ms(); }

    atomic_set_int32(&tag->destroy_idle_sec, value);

    /* a protocol that schedules its own idle check re-arms it. */
    if(tag->vtable && tag->vtable->idle_changed) { tag->vtable->idle_changed(tag); }

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_bit_num(plc_tag_p tag, int32_t *result) {
    /* FIXME: does not check tag->is_bit, so a tag that is not a bit tag reports bit 0. */
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


static int32_t core_get_find_or_create_tag(plc_tag_p tag, int32_t *result) {
    *result = (tag->find_or_create_tag ? 1 : 0);

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
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = (int32_t)tag->byte_order->str_is_counted;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_is_fixed_length(plc_tag_p tag, int32_t *result) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = (int32_t)tag->byte_order->str_is_fixed_length;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_is_zero_terminated(plc_tag_p tag, int32_t *result) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = (int32_t)tag->byte_order->str_is_zero_terminated;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_is_byte_swapped(plc_tag_p tag, int32_t *result) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = (int32_t)tag->byte_order->str_is_byte_swapped;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_count_word_bytes(plc_tag_p tag, int32_t *result) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = (int32_t)tag->byte_order->str_count_word_bytes;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_max_capacity(plc_tag_p tag, int32_t *result) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = (int32_t)tag->byte_order->str_max_capacity;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_total_length(plc_tag_p tag, int32_t *result) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = (int32_t)tag->byte_order->str_total_length;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_pad_bytes(plc_tag_p tag, int32_t *result) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = (int32_t)tag->byte_order->str_pad_bytes;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_str_pad_to_multiple_bytes(plc_tag_p tag, int32_t *result) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = (int32_t)tag->byte_order->str_pad_to_multiple_bytes;

    return PLCTAG_STATUS_OK;
}


static int32_t core_get_int16_byte_order(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    return attr_copy_byte_order(&tag->byte_order->int16_order[0], 2, buffer, buffer_length);
}


static int32_t core_get_int16_byte_order_size(plc_tag_p tag) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    return attr_byte_order_size(&tag->byte_order->int16_order[0], 2);
}


static int32_t core_get_int32_byte_order(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    return attr_copy_byte_order(&tag->byte_order->int32_order[0], 4, buffer, buffer_length);
}


static int32_t core_get_int32_byte_order_size(plc_tag_p tag) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    return attr_byte_order_size(&tag->byte_order->int32_order[0], 4);
}


static int32_t core_get_int64_byte_order(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    return attr_copy_byte_order(&tag->byte_order->int64_order[0], 8, buffer, buffer_length);
}


static int32_t core_get_int64_byte_order_size(plc_tag_p tag) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    return attr_byte_order_size(&tag->byte_order->int64_order[0], 8);
}


static int32_t core_get_float32_byte_order(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    return attr_copy_byte_order(&tag->byte_order->float32_order[0], 4, buffer, buffer_length);
}


static int32_t core_get_float32_byte_order_size(plc_tag_p tag) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    return attr_byte_order_size(&tag->byte_order->float32_order[0], 4);
}


static int32_t core_get_float64_byte_order(plc_tag_p tag, uint8_t *buffer, int32_t buffer_length) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    return attr_copy_byte_order(&tag->byte_order->float64_order[0], 8, buffer, buffer_length);
}


static int32_t core_get_float64_byte_order_size(plc_tag_p tag) {
    /* tags with no data, such as connection tags, have no byte order. */
    if(!tag->byte_order) { return PLCTAG_ERR_UNSUPPORTED; }

    return attr_byte_order_size(&tag->byte_order->float64_order[0], 8);
}


/* time left of the wait before reconnecting; 0 when not waiting. */
static int32_t retry_wait_left_ms(conn_stats_t *s) {
    int64_t retry_at = atomic_get_int64(&s->next_retry_at_ms);
    int64_t left = (retry_at == 0) ? 0 : retry_at - time_ms();

    return (int32_t)((left < 0) ? 0 : left);
}


static int64_t connected_time_ms(conn_stats_t *s) {
    int64_t connected_at = atomic_get_int64(&s->connected_at_ms);

    return (connected_at == 0) ? 0 : time_ms() - connected_at;
}


static int32_t stats_get_connect_count(plc_tag_p tag, int32_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int32(&s->connect_count);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_connect_failure_count(plc_tag_p tag, int32_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int32(&s->connect_failure_count);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_idle_disconnect_count(plc_tag_p tag, int32_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int32(&s->idle_disconnect_count);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_retry_delay_ms(plc_tag_p tag, int32_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int32(&s->retry_delay_ms);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_retry_wait_left_ms(plc_tag_p tag, int32_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = retry_wait_left_ms(s);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_tag_count(plc_tag_p tag, int32_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int32(&s->tag_count);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_requests_in_flight(plc_tag_p tag, int32_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int32(&s->requests_in_flight);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_response_time_us(plc_tag_p tag, int32_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int32(&s->response_us_last);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_response_time_avg_us(plc_tag_p tag, int32_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int32(&s->response_us_avg);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_response_time_min_us(plc_tag_p tag, int32_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int32(&s->response_us_min);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_response_time_max_us(plc_tag_p tag, int32_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int32(&s->response_us_max);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_connection_age_ms(plc_tag_p tag, int64_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = time_ms() - atomic_get_int64(&s->created_ms);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_connected_time_ms(plc_tag_p tag, int64_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = connected_time_ms(s);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_requests_sent(plc_tag_p tag, int64_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int64(&s->requests_sent);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_requests_sent_since_connect(plc_tag_p tag, int64_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = conn_stats_since_connect(&s->requests_sent, &s->requests_sent_at_connect);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_responses_received(plc_tag_p tag, int64_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int64(&s->responses_received);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_responses_received_since_connect(plc_tag_p tag, int64_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = conn_stats_since_connect(&s->responses_received, &s->responses_received_at_connect);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_bytes_sent(plc_tag_p tag, int64_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int64(&s->bytes_sent);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_bytes_sent_since_connect(plc_tag_p tag, int64_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = conn_stats_since_connect(&s->bytes_sent, &s->bytes_sent_at_connect);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_bytes_received(plc_tag_p tag, int64_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int64(&s->bytes_received);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_bytes_received_since_connect(plc_tag_p tag, int64_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = conn_stats_since_connect(&s->bytes_received, &s->bytes_received_at_connect);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_tag_ops_completed(plc_tag_p tag, int64_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int64(&s->tag_ops_completed);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_tag_ops_completed_since_connect(plc_tag_p tag, int64_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = conn_stats_since_connect(&s->tag_ops_completed, &s->tag_ops_completed_at_connect);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_request_timeouts(plc_tag_p tag, int64_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int64(&s->request_timeouts);

    return PLCTAG_STATUS_OK;
}


static int32_t stats_get_error_responses(plc_tag_p tag, int64_t *result) {
    conn_stats_t *s = tag->conn_stats;

    if(!s) { return PLCTAG_ERR_UNSUPPORTED; }

    *result = atomic_get_int64(&s->error_responses);

    return PLCTAG_STATUS_OK;
}
