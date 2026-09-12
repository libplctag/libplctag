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

#include <libplctag/protocols/cip/cip.h>

#include <ctype.h>
#include <errno.h>
#include <libplctag/lib/libplctag.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <utils/debug.h>
#include <utils/str.h>


/* the tag-name parser is recursive descent over three segment kinds. */
static int skip_whitespace(const char *name, int *name_index);
static int parse_bit_segment(cip_tag_name_t *ctx, const char *name, int *name_index);
static int parse_symbolic_segment(cip_tag_name_t *ctx, const char *name, int *encoded_index, int *name_index);
static int parse_numeric_segment(cip_tag_name_t *ctx, const char *name, int *encoded_index, int *name_index);


/*
 * The EBNF is:
 *
 * tag ::= SYMBOLIC_SEG ( tag_seg )* ( bit_seg )?
 *
 * tag_seg ::= '.' SYMBOLIC_SEG
 *             '[' array_seg ']'
 *
 * bit_seg ::= '.' [0-9]+
 *
 * array_seg ::= NUMERIC_SEG ( ',' NUMERIC_SEG )*
 *
 * SYMBOLIC_SEG ::= [a-zA-Z]([a-zA-Z0-9_]*)
 *
 * NUMERIC_SEG ::= [0-9]+
 *
 */


int cip_encode_tag_name(cip_tag_name_t *ctx, const char *name) {
    int rc = PLCTAG_STATUS_OK;
    int encoded_index = 0;
    int name_index = 0;
    int name_len = str_length(name);

    /* zero out the CIP encoded name size. Byte zero in the encoded name. */
    ctx->encoded_name[encoded_index] = 0;
    encoded_index++;

    /* names must start with a symbolic segment. */
    if(parse_symbolic_segment(ctx, name, &encoded_index, &name_index) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id, "Unable to parse initial symbolic segment in tag name %s!", name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    while(name_index < name_len && encoded_index < MAX_TAG_NAME) {
        /* try to parse the different parts of the name. */
        if(name[name_index] == '.') {
            name_index++;
            /* could be a name segment or could be a bit identifier. */
            if(parse_symbolic_segment(ctx, name, &encoded_index, &name_index) != PLCTAG_STATUS_OK) {
                /* try a bit identifier. */
                if(parse_bit_segment(ctx, name, &name_index) == PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, ctx->tag_id, "Found bit identifier %u.", ctx->bit);
                    break;
                } else {
                    pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id,
                           "Expected a symbolic segment or a bit identifier at position %d in tag name %s", name_index, name);
                    return PLCTAG_ERR_BAD_PARAM;
                }
            } else {
                pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, ctx->tag_id, "Found symbolic segment ending at %d", name_index);
            }
        } else if(name[name_index] == '[') {
            int num_dimensions = 0;
            /* must be an array so look for comma separated numeric segments. */
            do {
                name_index++;
                num_dimensions++;

                skip_whitespace(name, &name_index);
                rc = parse_numeric_segment(ctx, name, &encoded_index, &name_index);
                skip_whitespace(name, &name_index);
            } while(rc == PLCTAG_STATUS_OK && name[name_index] == ',' && num_dimensions < 3);

            /* pass up the real reason rather than reporting it as a bad bracket below. */
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id, "Unable to parse numeric segment at %d in tag name %s!",
                       name_index, name);
                return rc;
            }

            /* must terminate with a closing ']' */
            if(name[name_index] != ']') {
                pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id,
                       "Bad tag name format, expected closing array bracket at %d in tag name %s!", name_index, name);
                return PLCTAG_ERR_BAD_PARAM;
            }

            /* step past the closing bracket. */
            name_index++;
        } else {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id, "Unexpected character at position %d in name string %s!",
                   name_index, name);
            break;
        }
    }

    /*
     * The loop above stops as soon as the encoded name fills the buffer, which leaves the rest of
     * the name unparsed and looks exactly like a malformed name to the check below.  Say what
     * actually happened instead, and with the error code that fits it.
     */
    if(name_index < name_len && encoded_index >= MAX_TAG_NAME) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id, "Encoded tag name is too long at position %d in the tag name!",
               name_index);
        return PLCTAG_ERR_TOO_LARGE;
    }

    if(name_index != name_len) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id,
               "Bad tag name format.  Tag must end with a bit identifier if one is present.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* set the word count. */
    ctx->encoded_name[0] = (uint8_t)((encoded_index - 1) / 2);
    ctx->encoded_name_size = encoded_index;

    return PLCTAG_STATUS_OK;
}


static int skip_whitespace(const char *name, int *name_index) {
    while(name[*name_index] == ' ') { (*name_index)++; }

    return PLCTAG_STATUS_OK;
}


/*
 * A bit segment is simply an integer from 0 to 63 (inclusive). */
static int parse_bit_segment(cip_tag_name_t *ctx, const char *name, int *name_index) {
    const char *p, *q;
    long val;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, ctx->tag_id, "Starting with name index=%d.", *name_index);

    p = &name[*name_index];
    q = p;

    val = strtol((char *)p, (char **)&q, 10);

    /* sanity checks. */
    if(p == q) {
        /* no number. */
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id,
               "Expected bit identifier or symbolic segment at position %d in tag name %s!", *name_index, name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if((val < 0) || (val >= 65536)) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id, "Bit identifier must be between 0 and 255, inclusive, was %d!",
               (int)val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(ctx->elem_count != 1) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id, "Bit tags must have only one element!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* bump name_index. */
    *name_index += (int)(q - p);
    ctx->is_bit = 1;
    ctx->bit = (int)val;

    return PLCTAG_STATUS_OK;
}


static int parse_symbolic_segment(cip_tag_name_t *ctx, const char *name, int *encoded_index, int *name_index) {
    int encoded_i = *encoded_index;
    int name_i = *name_index;
    int name_start = name_i;
    int seg_len_index = 0;
    int seg_len = 0;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, ctx->tag_id, "Starting with name index=%d and encoded name index=%d.", name_i,
           encoded_i);

    /* a symbolic segment must start with an alphabetic character or @, then can have digits or underscores. */
    if(!isalpha(name[name_i]) && name[name_i] != ':' && name[name_i] != '_' && name[name_i] != '@') {
        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, ctx->tag_id, "tag name at position %d is not the start of a symbolic segment.",
               name_i);
        return PLCTAG_ERR_NO_MATCH;
    }

    /*
     * The segment type, the length byte and the first character are written before the
     * bounded loop below, so make sure all three fit.  The caller only checks the encoded
     * index between segments, not within one.
     */
    if(encoded_i > MAX_TAG_NAME - 3) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id, "Encoded tag name is too long at position %d in the tag name!",
               name_i);
        return PLCTAG_ERR_TOO_LARGE;
    }

    /* start building the encoded symbolic segment. */
    ctx->encoded_name[encoded_i] = 0x91; /* start of symbolic segment. */
    encoded_i++;
    seg_len_index = encoded_i;
    ctx->encoded_name[seg_len_index] = 0; /* the loop below counts up from zero, do not trust what was here. */
    ctx->encoded_name[seg_len_index]++;
    encoded_i++;

    /* store the first character of the name. */
    ctx->encoded_name[encoded_i] = (uint8_t)name[name_i];
    encoded_i++;
    name_i++;

    /* get the rest of the name. */
    while((isalnum(name[name_i]) || name[name_i] == ':' || name[name_i] == '_') && (encoded_i < (MAX_TAG_NAME - 1))) {
        /*
         * The segment length is a single byte, so a symbolic segment cannot hold more than 255
         * characters.  Without this the counter below wraps back to zero and the PLC gets a
         * zero-length segment followed by the rest of the name as stray path bytes.
         */
        if(ctx->encoded_name[seg_len_index] == 0xFF) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id,
                   "Symbolic segment starting at position %d in the tag name is longer than the maximum of 255 characters!",
                   name_start);
            return PLCTAG_ERR_TOO_LARGE;
        }

        ctx->encoded_name[encoded_i] = (uint8_t)name[name_i];
        encoded_i++;
        ctx->encoded_name[seg_len_index]++;
        name_i++;
    }

    seg_len = ctx->encoded_name[seg_len_index];

    /* finish up the encoded name.   Space for the name must be a multiple of two bytes long. */
    if((ctx->encoded_name[seg_len_index] & 0x01) && (encoded_i < MAX_TAG_NAME)) {
        ctx->encoded_name[encoded_i] = 0;
        encoded_i++;
    }

    *encoded_index = encoded_i;
    *name_index = name_i;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, ctx->tag_id, "Parsed symbolic segment \"%.*s\" in tag name.", seg_len,
           &name[name_start]);

    return PLCTAG_STATUS_OK;
}


static int parse_numeric_segment(cip_tag_name_t *ctx, const char *name, int *encoded_index, int *name_index) {
    const char *p, *q;
    long val;

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, ctx->tag_id, "Starting with name index=%d and encoded name index=%d.", *name_index,
           *encoded_index);

    p = &name[*name_index];
    q = p;

    errno = 0;
    val = strtol((char *)p, (char **)&q, 10);

    /* sanity checks. */
    if(p == q) {
        /* no number. */
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id, "Expected numeric segment at position %d in tag name %s!",
               *name_index, name);
        return PLCTAG_ERR_BAD_PARAM;
    }

    if(val < 0) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id, "Numeric segment must be greater than or equal to zero, was %d!",
               (int)val);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * strtol() returns a long, which is wider than the 32 bits the largest segment encoding
     * holds on many platforms.  Reject anything that would be silently truncated rather than
     * quietly addressing a different array element than the caller asked for.
     *
     * Where long is only 32 bits the range check alone is not enough: strtol() saturates at
     * LONG_MAX == INT32_MAX and sets ERANGE, so an overflowing value slips through as the
     * largest legal segment.
     */
    if(errno == ERANGE || val > (long)INT32_MAX) {
        pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id, "Numeric segment must be less than or equal to %ld!",
               (long)INT32_MAX);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * Work out how much room the encoding needs and check it before writing anything.  The
     * caller only checks the encoded index once per segment group, but an array reference
     * such as "[a,b,c]" calls this three times, so the index can run well past the end of
     * the buffer between those checks.
     */
    {
        int segment_size = (val > 0xFFFF ? 6 : (val > 0xFF ? 4 : 2));

        if(*encoded_index > MAX_TAG_NAME - segment_size) {
            pdebug(DEBUG_MODULE_CIP, DEBUG_WARN, ctx->tag_id,
                   "Encoded tag name is too long to hold a numeric segment at position %d in the tag name!", *name_index);
            return PLCTAG_ERR_TOO_LARGE;
        }
    }

    /* bump name_index. */
    *name_index += (int)(q - p);

    /* encode the segment. */
    if(val > 0xFFFF) {
        ctx->encoded_name[*encoded_index] = (uint8_t)0x2A; /* 4-byte segment value. */
        (*encoded_index)++;

        ctx->encoded_name[*encoded_index] = (uint8_t)0; /* padding. */
        (*encoded_index)++;

        ctx->encoded_name[*encoded_index] = (uint8_t)val & 0xFF;
        (*encoded_index)++;
        ctx->encoded_name[*encoded_index] = (uint8_t)((val >> 8) & 0xFF);
        (*encoded_index)++;
        ctx->encoded_name[*encoded_index] = (uint8_t)((val >> 16) & 0xFF);
        (*encoded_index)++;
        ctx->encoded_name[*encoded_index] = (uint8_t)((val >> 24) & 0xFF);
        (*encoded_index)++;

        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, ctx->tag_id, "Parsed 4-byte numeric segment of value %u.", (uint32_t)val);
    } else if(val > 0xFF) {
        ctx->encoded_name[*encoded_index] = (uint8_t)0x29; /* 2-byte segment value. */
        (*encoded_index)++;

        ctx->encoded_name[*encoded_index] = (uint8_t)0; /* padding. */
        (*encoded_index)++;

        ctx->encoded_name[*encoded_index] = (uint8_t)val & 0xFF;
        (*encoded_index)++;
        ctx->encoded_name[*encoded_index] = (uint8_t)((val >> 8) & 0xFF);
        (*encoded_index)++;

        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, ctx->tag_id, "Parsed 2-byte numeric segment of value %u.", (uint32_t)val);
    } else {
        ctx->encoded_name[*encoded_index] = (uint8_t)0x28; /* 1-byte segment value. */
        (*encoded_index)++;

        ctx->encoded_name[*encoded_index] = (uint8_t)val & 0xFF;
        (*encoded_index)++;

        pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, ctx->tag_id, "Parsed 1-byte numeric segment of value %u.", (uint32_t)val);
    }

    pdebug(DEBUG_MODULE_CIP, DEBUG_DETAIL, ctx->tag_id, "Done with name index=%d and encoded name index=%d.", *name_index,
           *encoded_index);

    return PLCTAG_STATUS_OK;
}


struct cip_type_lookup_entry_t {
    int is_found;
    int type_data_length;
    int instance_data_length;
};


static struct cip_type_lookup_entry_t cip_type_lookup[] = {
    /* 0x00 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x01 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x02 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x03 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x04 */ {PLCTAG_STATUS_OK, 2, 2},    /* UINT_BCD: OMRON-specific */
    /* 0x05 */ {PLCTAG_STATUS_OK, 2, 4},    /* UDINT_BCD: OMRON-specific */
    /* 0x06 */ {PLCTAG_STATUS_OK, 2, 8},    /* ULINT_BCD: OMRON-specific */
    /* 0x07 */ {PLCTAG_STATUS_OK, 2, 4},    /* ENUM: OMRON-specific */
    /* 0x08 */ {PLCTAG_STATUS_OK, 2, 8},    /* DATE_NSEC: OMRON-specific */
    /* 0x09 */ {PLCTAG_STATUS_OK, 2, 8},    /* TIME_NSEC: OMRON-specific, Time in nanoseconds */
    /* 0x0a */ {PLCTAG_STATUS_OK, 2, 8},    /* DATE_AND_TIME_NSEC: OMRON-specific, Date/Time in nanoseconds*/
    /* 0x0b */ {PLCTAG_STATUS_OK, 2, 8},    /* TIME_OF_DAY_NSEC: OMRON-specific */
    /* 0x0c */ {PLCTAG_ERR_NO_MATCH, 0, 0}, /* ???? UNION: Omron-specific */
    /* 0x0d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x0e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x0f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x10 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x11 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x12 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x13 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x14 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x15 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x16 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x17 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x18 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x19 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x1a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x1b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x1c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x1d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x1e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x1f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x20 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x21 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x22 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x23 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x24 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x25 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x26 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x27 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x28 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x29 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x2a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x2b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x2c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x2d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x2e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x2f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x30 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x31 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x32 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x33 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x34 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x35 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x36 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x37 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x38 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x39 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x3a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x3b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x3c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x3d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x3e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x3f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x40 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x41 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x42 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x43 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x44 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x45 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x46 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x47 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x48 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x49 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x4a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x4b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x4c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x4d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x4e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x4f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x50 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x51 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x52 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x53 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x54 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x55 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x56 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x57 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x58 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x59 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x5a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x5b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x5c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x5d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x5e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x5f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x60 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x61 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x62 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x63 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x64 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x65 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x66 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x67 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x68 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x69 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x6a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x6b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x6c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x6d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x6e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x6f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x70 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x71 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x72 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x73 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x74 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x75 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x76 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x77 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x78 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x79 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x7a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x7b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x7c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x7d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x7e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x7f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x80 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x81 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x82 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x83 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x84 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x85 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x86 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x87 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x88 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x89 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x8a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x8b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x8c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x8d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x8e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x8f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x90 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x91 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x92 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x93 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x94 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x95 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x96 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x97 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x98 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x99 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x9a */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x9b */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x9c */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x9d */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x9e */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0x9f */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xa0 */ {PLCTAG_STATUS_OK, 4, 0},    /* Data is an abbreviated struct type, i.e. a CRC of the actual type descriptor */
    /* 0xa1 */ {PLCTAG_STATUS_OK, 4, 0},    /* Data is an abbreviated array type. The limits are left off */
    /* 0xa2 */ {PLCTAG_ERR_NO_MATCH, 0, 0}, /* Data is a struct type descriptor, marked no match because we do not know how to
                                               parse it */
    /* 0xa3 */ {PLCTAG_ERR_NO_MATCH, 0, 0}, /* Data is an array type descriptor, marked no match because we do not know how to
                                               parse it */
    /* 0xa4 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xa5 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xa6 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xa7 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xa8 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xa9 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xaa */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xab */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xac */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xad */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xae */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xaf */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb0 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb1 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb2 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb3 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb4 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb5 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb6 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb7 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb8 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xb9 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xba */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xbb */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xbc */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xbd */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xbe */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xbf */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xc0 */ {PLCTAG_STATUS_OK, 2, 8},  /* DT: DT value, 64 bit */
    /* 0xc1 */ {PLCTAG_STATUS_OK, 2, 1},  /* BOOL: Boolean value, 1 bit */
    /* 0xc2 */ {PLCTAG_STATUS_OK, 2, 1},  /* SINT: Signed 8–bit integer value */
    /* 0xc3 */ {PLCTAG_STATUS_OK, 2, 2},  /* INT: Signed 16–bit integer value */
    /* 0xc4 */ {PLCTAG_STATUS_OK, 2, 4},  /* DINT: Signed 32–bit integer value */
    /* 0xc5 */ {PLCTAG_STATUS_OK, 2, 8},  /* LINT: Signed 64–bit integer value */
    /* 0xc6 */ {PLCTAG_STATUS_OK, 2, 1},  /* USINT: Unsigned 8–bit integer value */
    /* 0xc7 */ {PLCTAG_STATUS_OK, 2, 2},  /* UINT: Unsigned 16–bit integer value */
    /* 0xc8 */ {PLCTAG_STATUS_OK, 2, 4},  /* UDINT: Unsigned 32–bit integer value */
    /* 0xc9 */ {PLCTAG_STATUS_OK, 2, 8},  /* ULINT: Unsigned 64–bit integer value */
    /* 0xca */ {PLCTAG_STATUS_OK, 2, 4},  /* REAL: 32–bit floating point value, IEEE format */
    /* 0xcb */ {PLCTAG_STATUS_OK, 2, 8},  /* LREAL: 64–bit floating point value, IEEE format */
    /* 0xcc */ {PLCTAG_STATUS_OK, 2, 4},  /* STIME: System Time Synchronous time value */
    /* 0xcd */ {PLCTAG_STATUS_OK, 2, 2},  /* DATE: Date value */
    /* 0xce */ {PLCTAG_STATUS_OK, 2, 4},  /* TIME_OF_DAY: Time of day value */
    /* 0xcf */ {PLCTAG_STATUS_OK, 2, 8},  /* DATE_AND_TIME: Date and time of day value */
    /* 0xd0 */ {PLCTAG_STATUS_OK, 2, 84}, /* STRING: Character string, 2 byte count word, 1 byte per character */
    /* 0xd1 */ {PLCTAG_STATUS_OK, 2, 1},  /* BYTE: 8-bit bit string */
    /* 0xd2 */ {PLCTAG_STATUS_OK, 2, 2},  /* WORD: 16-bit bit string */
    /* 0xd3 */ {PLCTAG_STATUS_OK, 2, 4},  /* DWORD: 32-bit bit string */
    /* 0xd4 */ {PLCTAG_STATUS_OK, 2, 8},  /* LWORD: 64-bit bit string */
    /* 0xd5 */ {PLCTAG_STATUS_OK, 2, 0},  /* STRING2: Wide string, 2-byte count, 2 bytes per character, utf-16-le */
    /* 0xd6 */ {PLCTAG_STATUS_OK, 2, 4},  /* FTIME: High resolution duration value */
    /* 0xd7 */ {PLCTAG_STATUS_OK, 2, 8},  /* TIME: Medium resolution duration value */
    /* 0xd8 */ {PLCTAG_STATUS_OK, 2, 2},  /* ITIME: Low resolution duration value */
    /* 0xd9 */ {PLCTAG_STATUS_OK, 2, 0},  /* STRINGN: N-byte per char character string */
    /* 0xda */ {PLCTAG_STATUS_OK, 2, 0},  /* SHORT_STRING: 1 byte per character and 1 byte length */
    /* 0xdb */ {PLCTAG_STATUS_OK, 2, 4},  /* TIME: Duration in milliseconds */
    /* 0xdc */ {PLCTAG_STATUS_OK, 2, 0},  /* EPATH: CIP path segment(s) */
    /* 0xdd */ {PLCTAG_STATUS_OK, 2, 2},  /* ENGUNIT: Engineering units */
    /* 0xde */ {PLCTAG_STATUS_OK, 2, 0},  /* STRINGI: International character string (encoding?) */
    /* 0xdf */ {PLCTAG_STATUS_OK, 2, 8},  /* LTIME: Large time value */
    /* 0xe0 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe1 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe2 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe3 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe4 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe5 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe6 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe7 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe8 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xe9 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xea */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xeb */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xec */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xed */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xee */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xef */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf0 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf1 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf2 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf3 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf4 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf5 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf6 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf7 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf8 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xf9 */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xfa */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xfb */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xfc */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xfd */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xfe */ {PLCTAG_ERR_NO_MATCH, 0, 0},
    /* 0xff */ {PLCTAG_ERR_NO_MATCH, 0, 0},
};


int cip_lookup_encoded_type_size(uint8_t type_byte, int *type_size) {
    *type_size = cip_type_lookup[type_byte].type_data_length;
    return cip_type_lookup[type_byte].is_found;
}


int cip_lookup_data_element_size(uint8_t type_byte, int *element_size) {
    *element_size = cip_type_lookup[type_byte].instance_data_length;
    return cip_type_lookup[type_byte].is_found;
}
