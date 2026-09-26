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

#include <libplctag/api/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/lib/tag_string.h>
#include <platform.h>
#include <utils/debug.h>

/**
 * @brief Get the total length of the string currently in the tag.
 *
 * @param tag
 * @param string_start_offset
 * @return int
 */
int get_string_total_length_unsafe(plc_tag_p tag, int string_start_offset) {
    int total_length = 0;

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Starting.");

    if(tag->byte_order->str_is_fixed_length) {
        total_length = (int)(tag->byte_order->str_max_capacity);
    } else {
        total_length = get_string_length_unsafe(tag, string_start_offset);

        /* a bad start offset shows up as a negative length.  Pass the error up, do not sum it. */
        if(total_length < 0) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Unable to get the string length, error %s!",
                   plc_tag_decode_error(total_length));
            return total_length;
        }
    }

    total_length += (int)(tag->byte_order->str_count_word_bytes) + (tag->byte_order->str_is_zero_terminated ? (int)1 : (int)0)
                    + (int)(tag->byte_order->str_pad_bytes);

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Done with length %d.", total_length);

    return total_length;
}


/*
 * get the string length depending on the PLC string type.
 *
 * This is called in other functions so is separated out.
 *
 * This must be called with the tag API mutex held!
 */

int get_string_length_unsafe(plc_tag_p tag, int offset) {
    int string_length = 0;

    /*
     * The offset comes from the application and reaches tag->data[] directly below,
     * so bounds check it here.  The count word is read first, so the buffer must hold
     * at least that many bytes at the offset.
     */
    if(!tag_range_is_valid(tag, offset, (int)tag->byte_order->str_count_word_bytes)) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "String start offset %d is out of bounds for a tag of %d bytes!",
               offset, tag->size);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    if(tag->byte_order->str_is_counted) {
        switch(tag->byte_order->str_count_word_bytes) {
            case 1: string_length = (int)(unsigned int)(tag->data[offset]); break;

            case 2:
                string_length = (int16_t)(uint16_t)(((uint16_t)(tag->data[offset + tag->byte_order->int16_order[0]]) << 0)
                                                    + ((uint16_t)(tag->data[offset + tag->byte_order->int16_order[1]]) << 8));
                break;

            case 4:
                string_length = (int32_t)(((uint32_t)(tag->data[offset + tag->byte_order->int32_order[0]]) << 0)
                                          + ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[1]]) << 8)
                                          + ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[2]]) << 16)
                                          + ((uint32_t)(tag->data[offset + tag->byte_order->int32_order[3]]) << 24));
                break;

            default:
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Unsupported string count word size, %d bytes!",
                       tag->byte_order->str_count_word_bytes);
                return 0; /* FIXME - this should be an error code. */
                break;
        }

        /*
         * The count word comes from the PLC.  On the wire it is a signed value: a DINT for
         * Logix strings and an INT for standard CIP strings, so a hostile or broken PLC can
         * return a negative count or one that claims more characters than the string can
         * hold.  Callers use this value to index and to size allocations, so reject bad
         * counts here rather than letting them out of this function.
         */
        if(string_length < 0) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "String count word at offset %d is negative, %d!", offset,
                   string_length);
            return PLCTAG_ERR_OUT_OF_BOUNDS;
        }

        /*
         * Both STRING and LOGIX_STRING are fixed-length: the character array is str_max_capacity
         * bytes no matter what the count word says.  When the string is fixed length that
         * capacity is the real limit, otherwise the only limit is the tag buffer itself.
         */
        if(tag->byte_order->str_is_fixed_length && (unsigned int)string_length > tag->byte_order->str_max_capacity) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                   "String count word %d at offset %d exceeds the string capacity of %u characters!", string_length, offset,
                   tag->byte_order->str_max_capacity);
            return PLCTAG_ERR_OUT_OF_BOUNDS;
        }

        /* the tag buffer is the outer bound in every case, fixed length or not. */
        if(!tag_range_is_valid(tag, offset + (int)(tag->byte_order->str_count_word_bytes), string_length)) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                   "String count word %d at offset %d is out of bounds for a tag of %d bytes!", string_length, offset, tag->size);
            return PLCTAG_ERR_OUT_OF_BOUNDS;
        }
    } else {
        if(tag->byte_order->str_is_zero_terminated) {
            /* slow, but hopefully correct. */

            /*
             * note that this will count the correct length of a string that runs up against
             * the end of the tag buffer.
             *
             * The string may sit in the middle of a larger UDT, so the end of the tag buffer
             * is only the outer bound.  If the string is fixed length then its own character
             * array ends well before that and the scan must stop there instead, otherwise a
             * PLC that omits the terminator makes us count the bytes of the next field.
             */
            int str_start = offset + (int)(tag->byte_order->str_count_word_bytes);
            int scan_end = tag->size;

            /* str_start is bounded by tag->size above, so the subtraction cannot overflow. */
            if(tag->byte_order->str_is_fixed_length
               && tag->byte_order->str_max_capacity <= (unsigned int)(tag->size - str_start)) {
                scan_end = str_start + (int)(tag->byte_order->str_max_capacity);
            }

            for(int i = str_start; i < scan_end; i++) {
                size_t char_index =
                    (((size_t)(unsigned int)string_length) ^ (tag->byte_order->str_is_byte_swapped)) /* byte swap if necessary */
                    + (size_t)(unsigned int)offset + (size_t)(unsigned int)(tag->byte_order->str_count_word_bytes);

                /*
                 * the byte swap can push the index one past the loop bound, so check the
                 * index we actually use, not the one we counted with.
                 */
                if(char_index >= (size_t)(unsigned int)scan_end) { break; }

                if(tag->data[char_index] == (uint8_t)0) {
                    /* found the end. */
                    break;
                }

                string_length++;
            }
        } else {
            /* it is not counted or zero terminated, so it is not supported. */
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                   "Unsupported string length type.   Must be counted or zero-terminated!");
            return 0; /* FIXME this should be an error code. */
        }
    }

    return string_length;
}


int get_new_string_total_length_unsafe(plc_tag_p tag, const char *string_val) {
    int rc = PLCTAG_STATUS_OK;
    unsigned int string_size_in_buffer = 0;

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Starting.");

    do {
        int string_length = str_length(string_val);

        /* if this is a fixed-size string, use that data. */
        if(tag->byte_order->str_is_fixed_length) {
            if(tag->byte_order->str_total_length) {
                string_size_in_buffer = tag->byte_order->str_total_length;
                pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id,
                       "String is fixed size, so use the total length %d as the size in the buffer.",
                       tag->byte_order->str_total_length);
                break;
            } else {
                pdebug(
                    DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
                    "Unsupported configuration.  You must set the total string length if you set the flag for string is fixed size!");
                rc = PLCTAG_ERR_BAD_CONFIG;
                break;
            }
        }

        /* add the incoming string size. */
        string_size_in_buffer = (unsigned int)string_length;
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id,
               "String size in buffer is at least %u after the incoming string length %u.", string_size_in_buffer, string_length);

        /* OK the string will fit, now lets add the count word if any. */
        if(tag->byte_order->str_count_word_bytes) {
            string_size_in_buffer += tag->byte_order->str_count_word_bytes;
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "String size in buffer is %u after adding count word size, %u.",
                   string_size_in_buffer, tag->byte_order->str_count_word_bytes);
        }

        /* any terminator byte? */
        if(tag->byte_order->str_is_zero_terminated) {
            string_size_in_buffer += 1;
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id,
                   "String is zero terminated so the string size in the tag buffer is at least %u.", string_size_in_buffer);
        }

        /* any pad bytes? */
        if(tag->byte_order->str_pad_bytes) {
            string_size_in_buffer += tag->byte_order->str_pad_bytes;
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id,
                   "String has %u padding bytes so the string size in the tag buffer is at least %u.",
                   tag->byte_order->str_pad_bytes, string_size_in_buffer);
        }

        /* bytes reordered?  If so, we need an even string size in the buffer. */
        if(tag->byte_order->str_is_byte_swapped) {
            if(string_length & 0x01) {
                string_size_in_buffer += 1;
                pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "String is byte swapped so length is now %u.",
                       string_size_in_buffer);
            }
        }

        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Final string size in the tag buffer is %u bytes.",
               string_size_in_buffer);

    } while(0);

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Done with size %d.", string_size_in_buffer);
        return (int)string_size_in_buffer;
    } else {
        pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id,
               "Error %s found while calculating the new string size in the tag buffer.", plc_tag_decode_error(rc));
        return rc;
    }
}


int resize_tag_buffer_unsafe(plc_tag_p tag, int new_size) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Starting.");

    do {
        uint8_t *new_data = NULL;

        pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Changing the tag buffer size from %d to %d.", tag->size, new_size);

        new_data = mem_realloc(tag->data, (int)new_size);
        if(!new_data) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Unable to allocate new tag data buffer!");
            rc = PLCTAG_ERR_NO_MEM;
            tag->status = (int8_t)rc;
            break;
        }

        tag->data = new_data;
        tag->size = new_size;
    } while(0);

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Done with status %s.", plc_tag_decode_error(rc));

    return rc;
}


int resize_tag_buffer_at_offset_unsafe(plc_tag_p tag, int old_split_index, int new_split_index) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Starting.");

    do {
        pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Move old index %d to new index %d.", old_split_index,
               new_split_index);

        /* double check the data. */
        if(old_split_index < 0 || old_split_index > tag->size) {
            /* not good. */
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Old split index %d is outside tag data, %d bytes!",
                   old_split_index, tag->size);
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        if(new_split_index < 0) {
            /* not good. */
            pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "New split index %d is outside tag data!", old_split_index);
            rc = PLCTAG_ERR_OUT_OF_BOUNDS;
            break;
        }

        if(new_split_index == old_split_index) {
            /* nothing to do! */
            pdebug(DEBUG_MODULE_LIB, DEBUG_INFO, tag->tag_id, "Tag new size is the same as the tag old size so nothing to do.");
            break;
        }

        /* are we shrinking or growing? */
        if(new_split_index < old_split_index) {
            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Shrinking tag buffer by %d bytes",
                   old_split_index - new_split_index);

            /* shrinking.  We must move the existing data down to the new end point of the string. */
            void *old_split_ptr = tag->data + old_split_index;
            void *new_split_ptr = tag->data + new_split_index;
            int amount_to_move = tag->size - old_split_index;
            int new_tag_size = tag->size - (old_split_index - new_split_index);

            /* amount_to_move will be positive or zero because of the above if check. */
            mem_move(new_split_ptr, old_split_ptr, amount_to_move);

            rc = resize_tag_buffer_unsafe(tag, new_tag_size);
            break;
        }

        /* are we shrinking or growing? */
        if(new_split_index > old_split_index) {
            void *old_split_ptr = NULL;
            void *new_split_ptr = NULL;

            pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Growing tag buffer by %d bytes",
                   new_split_index - old_split_index);

            /* growing.  We must move the existing data up to the new end point of the string. */
            int amount_to_move = tag->size - old_split_index;
            int new_tag_size = tag->size + (new_split_index - old_split_index);

            /* resize the buffer now. */
            rc = resize_tag_buffer_unsafe(tag, new_tag_size);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_LIB, DEBUG_WARN, tag->tag_id, "Unable to resize the tag buffer!");
                break;
            }

            /* the tag data pointer may have changed, now calculate the two pointers. */
            old_split_ptr = tag->data + old_split_index;
            new_split_ptr = tag->data + new_split_index;

            /* amount_to_move will be positive or zero because of the above if check. */
            mem_move(new_split_ptr, old_split_ptr, amount_to_move);
        }
    } while(0);

    pdebug(DEBUG_MODULE_LIB, DEBUG_DETAIL, tag->tag_id, "Done with status %s.", plc_tag_decode_error(rc));

    return rc;
}
