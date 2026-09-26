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

#include <ctype.h>
#include <libplctag/lib/byte_order.h>
#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <platform.h>
#include <utils/attr.h>
#include <utils/debug.h>

static int check_byte_order_str(const char *byte_order, int length, int32_t tag_id);

int set_tag_byte_order(plc_tag_p tag, attr attribs)

{
    int use_default = 1;

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Starting.");

    /* the default values are already set in the tag. */

    /* check for overrides. */
    if(attr_get_str(attribs, "int16_byte_order", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "int32_byte_order", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "int64_byte_order", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "float32_byte_order", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "float64_byte_order", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_is_counted", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_is_fixed_length", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_is_zero_terminated", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_is_byte_swapped", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_count_word_bytes", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_max_capacity", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_total_length", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_pad_bytes", NULL) != NULL) { use_default = 0; }

    if(attr_get_str(attribs, "str_pad_to_multiple_bytes_EXPERIMENTAL", NULL) != NULL) { use_default = 0; }

    /* if we need to override something, build a new byte order structure. */
    if(!use_default) {
        const char *byte_order_str = NULL;
        int str_param = 0;
        int rc = PLCTAG_STATUS_OK;
        tag_byte_order_t *new_byte_order = mem_alloc((int)(unsigned int)sizeof(*(tag->byte_order)));

        if(!new_byte_order) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Unable to allocate byte order struct for tag!");
            return PLCTAG_ERR_NO_MEM;
        }

        /* copy the defaults. */
        *new_byte_order = *(tag->byte_order);

        /* replace the old byte order. */
        tag->byte_order = new_byte_order;

        /* mark it as allocated so that we free it later. */
        tag->byte_order->is_allocated = 1;

        /* 16-bit ints. */
        byte_order_str = attr_get_str(attribs, "int16_byte_order", NULL);
        if(byte_order_str) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Override byte order int16_byte_order=%s", byte_order_str);

            rc = check_byte_order_str(byte_order_str, 2, tag->tag_id);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                       "Byte order string int16_byte_order, \"%s\", is illegal or malformed.", byte_order_str);
                return rc;
            }

            /* strange gyrations to make the compiler happy.   MSVC will probably complain. */
            tag->byte_order->int16_order[0] = (int)(unsigned int)(((unsigned int)byte_order_str[0] - (unsigned int)('0')) & 0x01);
            tag->byte_order->int16_order[1] = (int)(unsigned int)(((unsigned int)byte_order_str[1] - (unsigned int)('0')) & 0x01);
        }

        /* 32-bit ints. */
        byte_order_str = attr_get_str(attribs, "int32_byte_order", NULL);
        if(byte_order_str) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Override byte order int32_byte_order=%s", byte_order_str);

            rc = check_byte_order_str(byte_order_str, 4, tag->tag_id);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                       "Byte order string int32_byte_order, \"%s\", is illegal or malformed.", byte_order_str);
                return rc;
            }

            tag->byte_order->int32_order[0] = (int)(unsigned int)(((unsigned int)byte_order_str[0] - (unsigned int)('0')) & 0x03);
            tag->byte_order->int32_order[1] = (int)(unsigned int)(((unsigned int)byte_order_str[1] - (unsigned int)('0')) & 0x03);
            tag->byte_order->int32_order[2] = (int)(unsigned int)(((unsigned int)byte_order_str[2] - (unsigned int)('0')) & 0x03);
            tag->byte_order->int32_order[3] = (int)(unsigned int)(((unsigned int)byte_order_str[3] - (unsigned int)('0')) & 0x03);
        }

        /* 64-bit ints. */
        byte_order_str = attr_get_str(attribs, "int64_byte_order", NULL);
        if(byte_order_str) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Override byte order int64_byte_order=%s", byte_order_str);

            rc = check_byte_order_str(byte_order_str, 8, tag->tag_id);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                       "Byte order string int64_byte_order, \"%s\", is illegal or malformed.", byte_order_str);
                return rc;
            }

            tag->byte_order->int64_order[0] = (int)(unsigned int)(((unsigned int)byte_order_str[0] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[1] = (int)(unsigned int)(((unsigned int)byte_order_str[1] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[2] = (int)(unsigned int)(((unsigned int)byte_order_str[2] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[3] = (int)(unsigned int)(((unsigned int)byte_order_str[3] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[4] = (int)(unsigned int)(((unsigned int)byte_order_str[4] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[5] = (int)(unsigned int)(((unsigned int)byte_order_str[5] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[6] = (int)(unsigned int)(((unsigned int)byte_order_str[6] - (unsigned int)('0')) & 0x07);
            tag->byte_order->int64_order[7] = (int)(unsigned int)(((unsigned int)byte_order_str[7] - (unsigned int)('0')) & 0x07);
        }

        /* 32-bit floats. */
        byte_order_str = attr_get_str(attribs, "float32_byte_order", NULL);
        if(byte_order_str) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Override byte order float32_byte_order=%s", byte_order_str);

            rc = check_byte_order_str(byte_order_str, 4, tag->tag_id);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                       "Byte order string float32_byte_order, \"%s\", is illegal or malformed.", byte_order_str);
                return rc;
            }

            tag->byte_order->float32_order[0] =
                (int)(unsigned int)(((unsigned int)byte_order_str[0] - (unsigned int)('0')) & 0x03);
            tag->byte_order->float32_order[1] =
                (int)(unsigned int)(((unsigned int)byte_order_str[1] - (unsigned int)('0')) & 0x03);
            tag->byte_order->float32_order[2] =
                (int)(unsigned int)(((unsigned int)byte_order_str[2] - (unsigned int)('0')) & 0x03);
            tag->byte_order->float32_order[3] =
                (int)(unsigned int)(((unsigned int)byte_order_str[3] - (unsigned int)('0')) & 0x03);
        }

        /* 64-bit floats */
        byte_order_str = attr_get_str(attribs, "float64_byte_order", NULL);
        if(byte_order_str) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Override byte order float64_byte_order=%s", byte_order_str);

            rc = check_byte_order_str(byte_order_str, 8, tag->tag_id);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                       "Byte order string float64_byte_order, \"%s\", is illegal or malformed.", byte_order_str);
                return rc;
            }

            tag->byte_order->float64_order[0] =
                (int)(unsigned int)(((unsigned int)byte_order_str[0] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[1] =
                (int)(unsigned int)(((unsigned int)byte_order_str[1] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[2] =
                (int)(unsigned int)(((unsigned int)byte_order_str[2] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[3] =
                (int)(unsigned int)(((unsigned int)byte_order_str[3] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[4] =
                (int)(unsigned int)(((unsigned int)byte_order_str[4] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[5] =
                (int)(unsigned int)(((unsigned int)byte_order_str[5] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[6] =
                (int)(unsigned int)(((unsigned int)byte_order_str[6] - (unsigned int)('0')) & 0x07);
            tag->byte_order->float64_order[7] =
                (int)(unsigned int)(((unsigned int)byte_order_str[7] - (unsigned int)('0')) & 0x07);
        }

        /* string information. */

        /* is the string counted? */
        if(attr_get_str(attribs, "str_is_counted", NULL)) {
            str_param = attr_get_int(attribs, "str_is_counted", 0);
            if(str_param == 1 || str_param == 0) {
                tag->byte_order->str_is_counted = (str_param ? 1 : 0);
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                       "Tag string attribute str_is_counted must be missing, zero (0) or one (1)!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* is the string a fixed length? */
        if(attr_get_str(attribs, "str_is_fixed_length", NULL)) {
            str_param = attr_get_int(attribs, "str_is_fixed_length", 0);
            if(str_param == 1 || str_param == 0) {
                tag->byte_order->str_is_fixed_length = (str_param ? 1 : 0);
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                       "Tag string attribute str_is_fixed_length must be missing, zero (0) or one (1)!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* is the string zero terminated? */
        if(attr_get_str(attribs, "str_is_zero_terminated", NULL)) {
            str_param = attr_get_int(attribs, "str_is_zero_terminated", 0);
            if(str_param == 1 || str_param == 0) {
                tag->byte_order->str_is_zero_terminated = (str_param ? 1 : 0);
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                       "Tag string attribute str_is_zero_terminated must be missing, zero (0) or one (1)!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* is the string byteswapped like PLC/5? */
        if(attr_get_str(attribs, "str_is_byte_swapped", NULL)) {
            str_param = attr_get_int(attribs, "str_is_byte_swapped", 0);
            if(str_param == 1 || str_param == 0) {
                tag->byte_order->str_is_byte_swapped = (str_param ? 1 : 0);
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                       "Tag string attribute str_is_byte_swapped must be missing, zero (0) or one (1)!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* main string parameters. */

        /* how many bytes is the string count word? */
        if(attr_get_str(attribs, "str_count_word_bytes", NULL)) {
            str_param = attr_get_int(attribs, "str_count_word_bytes", 0);

            /*
             * Only 1, 2 and 4 are accepted.  Eight used to be allowed here, but no string
             * format in the wild uses a count word that wide and nothing downstream can read
             * or write one: both switches on this value handle 1/2/4 and fall through to an
             * error for anything else.  Accepting it here only produced tags whose sizes were
             * computed with a width that every accessor then refused to use.
             */
            if(str_param == 0 || str_param == 1 || str_param == 2 || str_param == 4) {
                tag->byte_order->str_count_word_bytes = (unsigned int)str_param;
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                       "Tag string attribute str_count_word_bytes must be missing, 0, 1, 2, or 4!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* What is the string maximum capacity */
        if(attr_get_str(attribs, "str_max_capacity", NULL)) {
            str_param = attr_get_int(attribs, "str_max_capacity", 0);
            if(str_param >= 0 && str_param <= MAX_STR_SIZE_PARAM) {
                tag->byte_order->str_max_capacity = (unsigned int)str_param;
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                       "Tag string attribute str_max_capacity must be missing, or between 0 and %d!", MAX_STR_SIZE_PARAM);
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* What is the string total length */
        if(attr_get_str(attribs, "str_total_length", NULL)) {
            str_param = attr_get_int(attribs, "str_total_length", 0);
            if(str_param >= 0 && str_param <= MAX_STR_SIZE_PARAM) {
                tag->byte_order->str_total_length = (unsigned int)str_param;
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                       "Tag string attribute str_total_length must be missing, or between 0 and %d!", MAX_STR_SIZE_PARAM);
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* What is the string padding length */
        if(attr_get_str(attribs, "str_pad_bytes", NULL)) {
            str_param = attr_get_int(attribs, "str_pad_bytes", 0);
            if(str_param >= 0 && str_param <= MAX_STR_SIZE_PARAM) {
                tag->byte_order->str_pad_bytes = (unsigned int)str_param;
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                       "Tag string attribute str_pad_bytes must be missing, or between 0 and %d!", MAX_STR_SIZE_PARAM);
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* Should we pad the string to a multiple of 1 (no padding), 2, or 4 bytes. Adding padding causes issues when writing
           OmronNJ strings, 2 byte padding is required for certain AB PLCs*/
        if(attr_get_str(attribs, "str_pad_to_multiple_bytes_EXPERIMENTAL", NULL)) {
            str_param = attr_get_int(attribs, "str_pad_to_multiple_bytes_EXPERIMENTAL", 0);
            if(str_param == 0 || str_param == 1 || str_param == 2 || str_param == 4) {
                if(str_param == 0) {
                    str_param = 1;
                } /* Padding to 0 bytes doesnt make much sense, so we overwride to 1 byte which means no padding */
                tag->byte_order->str_pad_to_multiple_bytes = (unsigned int)str_param;
            } else {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                       "Tag string attribute str_pad_to_multiple_bytes must be missing, 1, 2 or 4!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* now make sure that the combination of settings works. */

        /* if we have a counted string, we need the count! */
        if(tag->byte_order->str_is_counted) {
            if(tag->byte_order->str_count_word_bytes == 0) {
                pdebug(
                    DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                    "If a string definition is counted, you must use both \"str_is_counted\" and \"str_count_word_bytes\" parameters!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* if we have a fixed length string, we need to know what the length is! */
        if(tag->byte_order->str_is_fixed_length) {
            if(tag->byte_order->str_total_length == 0) {
                pdebug(
                    DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                    "If a string definition is fixed length, you must use both \"str_is_fixed_length\" and \"str_total_length\" parameters!");
                return PLCTAG_ERR_BAD_PARAM;
            }
        }

        /* check the total length. */
        if(tag->byte_order->str_total_length > 0
           && (tag->byte_order->str_is_zero_terminated + tag->byte_order->str_max_capacity + tag->byte_order->str_count_word_bytes
               + tag->byte_order->str_pad_bytes)
                  > tag->byte_order->str_total_length) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                   "Tag string total length, %d bytes, must be at least the sum, %d, of the other string components!",
                   tag->byte_order->str_total_length,
                   tag->byte_order->str_is_zero_terminated + tag->byte_order->str_max_capacity
                       + tag->byte_order->str_count_word_bytes + tag->byte_order->str_pad_bytes);
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id,
                   "str_is_zero_terminated=%d, str_max_capacity=%d, str_count_word_bytes=%d, str_pad_bytes=%d",
                   tag->byte_order->str_is_zero_terminated, tag->byte_order->str_max_capacity,
                   tag->byte_order->str_count_word_bytes, tag->byte_order->str_pad_bytes);
            return PLCTAG_ERR_BAD_PARAM;
        }

        /* Do we have enough of a definition for a string? */
        /* FIXME - This is probably not enough checking! */
        if(tag->byte_order->str_is_counted || tag->byte_order->str_is_zero_terminated) {
            tag->byte_order->str_is_defined = 1;
        } else {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Insufficient definitions found to support strings!");
        }
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}

int check_byte_order_str(const char *byte_order, int length, int32_t tag_id) {
    int taken[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int byte_order_len = str_length(byte_order);

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "Starting.");

    /* check the size. */
    if(byte_order_len != length) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Byte order string, \"%s\", must be %d characters long!", byte_order,
               length);
        return (byte_order_len < length ? PLCTAG_ERR_TOO_SMALL : PLCTAG_ERR_TOO_LARGE);
    }

    /* check each character. */
    for(int i = 0; i < byte_order_len; i++) {
        int val = 0;

        if(!isdigit(byte_order[i]) || byte_order[i] < '0' || byte_order[i] > '7') {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Byte order string, \"%s\", must be only characters from '0' to '7'!",
                   byte_order);
            return PLCTAG_ERR_BAD_DATA;
        }

        /* get the numeric value. */
        val = byte_order[i] - '0';

        if(val < 0 || val > (length - 1)) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Byte order string, \"%s\", must only values from 0 to %d!", byte_order,
                   (length - 1));
            return PLCTAG_ERR_BAD_DATA;
        }

        if(taken[val]) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag_id, "Byte order string, \"%s\", must use each digit exactly once!",
                   byte_order);
            return PLCTAG_ERR_BAD_DATA;
        }

        taken[val] = 1;
    }

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag_id, "Done.");

    return PLCTAG_STATUS_OK;
}
