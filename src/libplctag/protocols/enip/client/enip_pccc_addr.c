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
 * PCCC logical-address parsing and PLC-5/SLC address encoding for the generic
 * ENIP engine. Adapted from the AB driver's pccc.c; behavior is unchanged, the
 * signatures use Bytes and sized integers per the coding guidelines.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>

#include <libplctag/lib/libplctag.h>
#include <libplctag/protocols/enip/client/enip_pccc_addr.h>
#include <platform.h>
#include <utils/bytes.h>
#include <utils/debug.h>

/* mnemonic field lookup row for structured PCCC types (timer, counter, ...) */
typedef struct {
    pccc_file_t file_type;
    const char *field_name;
    int32_t element_size_bytes;
    int32_t sub_element;
    bool is_bit;
    uint8_t bit;
} pccc_subelem_row_t;

static int32_t parse_pccc_file_type(const char **str, pccc_addr_t *address);
static int32_t parse_pccc_file_num(const char **str, pccc_addr_t *address);
static int32_t parse_pccc_elem_num(const char **str, pccc_addr_t *address);
static int32_t parse_pccc_subelem(const char **str, pccc_addr_t *address);
static int32_t parse_pccc_subelem_num(const char **str, pccc_addr_t *address);
static int32_t parse_pccc_subelem_mnemonic(const char **str, pccc_addr_t *address);
static int32_t parse_pccc_bit_num(const char **str, pccc_addr_t *address);
static bool encode_data(Bytes *cursor, int32_t val);

/* file type                    field   size    subelem is_bit  bit_num */
static pccc_subelem_row_t sub_element_lookup[] = {
    /* BT block transfer */
    {PCCC_FILE_BLOCK_TRANSFER, "con", 2, 0, false, 0},
    {PCCC_FILE_BLOCK_TRANSFER, "rlen", 2, 1, false, 0},
    {PCCC_FILE_BLOCK_TRANSFER, "dlen", 2, 2, false, 0},
    {PCCC_FILE_BLOCK_TRANSFER, "df", 2, 3, false, 0},
    {PCCC_FILE_BLOCK_TRANSFER, "elem", 2, 4, false, 0},
    {PCCC_FILE_BLOCK_TRANSFER, "rgs", 2, 5, false, 0},

    /* R Control */
    {PCCC_FILE_CONTROL, "con", 2, 0, false, 0},
    {PCCC_FILE_CONTROL, "len", 2, 1, false, 0},
    {PCCC_FILE_CONTROL, "pos", 2, 2, false, 0},

    /* C Counter */
    {PCCC_FILE_COUNTER, "con", 2, 0, false, 0},
    {PCCC_FILE_COUNTER, "cu", 2, 0, true, 15},
    {PCCC_FILE_COUNTER, "cd", 2, 0, true, 14},
    {PCCC_FILE_COUNTER, "dn", 2, 0, true, 13},
    {PCCC_FILE_COUNTER, "ov", 2, 0, true, 12},
    {PCCC_FILE_COUNTER, "un", 2, 0, true, 11},
    {PCCC_FILE_COUNTER, "pre", 2, 1, false, 0},
    {PCCC_FILE_COUNTER, "acc", 2, 2, false, 0},

    /* MG Message */
    {PCCC_FILE_MESSAGE, "con", 2, 0, false, 0},
    {PCCC_FILE_MESSAGE, "nr", 2, 0, true, 9},
    {PCCC_FILE_MESSAGE, "to", 2, 0, true, 8},
    {PCCC_FILE_MESSAGE, "en", 2, 0, true, 7},
    {PCCC_FILE_MESSAGE, "st", 2, 0, true, 6},
    {PCCC_FILE_MESSAGE, "dn", 2, 0, true, 5},
    {PCCC_FILE_MESSAGE, "er", 2, 0, true, 4},
    {PCCC_FILE_MESSAGE, "co", 2, 0, true, 3},
    {PCCC_FILE_MESSAGE, "ew", 2, 0, true, 2},
    {PCCC_FILE_MESSAGE, "err", 2, 1, false, 0},
    {PCCC_FILE_MESSAGE, "rlen", 2, 2, false, 0},
    {PCCC_FILE_MESSAGE, "dlen", 2, 3, false, 0},
    {PCCC_FILE_MESSAGE, "data", 104, 4, false, 0},

    /* PID */
    /* PD first control word */
    {PCCC_FILE_PID, "con", 2, 0, false, 0},
    {PCCC_FILE_PID, "en", 2, 0, true, 15},
    {PCCC_FILE_PID, "ct", 2, 0, true, 9},
    {PCCC_FILE_PID, "cl", 2, 0, true, 8},
    {PCCC_FILE_PID, "pvt", 2, 0, true, 7},
    {PCCC_FILE_PID, "do", 2, 0, true, 6},
    {PCCC_FILE_PID, "swm", 2, 0, true, 4},
    {PCCC_FILE_PID, "do", 2, 0, true, 2},
    {PCCC_FILE_PID, "mo", 2, 0, true, 1},
    {PCCC_FILE_PID, "pe", 2, 0, true, 0},

    /* second control word */
    {PCCC_FILE_PID, "ini", 2, 1, true, 12},
    {PCCC_FILE_PID, "spor", 2, 1, true, 11},
    {PCCC_FILE_PID, "oll", 2, 1, true, 10},
    {PCCC_FILE_PID, "olh", 2, 1, true, 9},
    {PCCC_FILE_PID, "ewd", 2, 1, true, 8},
    {PCCC_FILE_PID, "dvna", 2, 1, true, 3},
    {PCCC_FILE_PID, "dvpa", 2, 1, true, 2},
    {PCCC_FILE_PID, "pvla", 2, 1, true, 1},
    {PCCC_FILE_PID, "pvha", 2, 1, true, 0},

    /* main PID vars */
    {PCCC_FILE_PID, "sp", 4, 2, false, 0},
    {PCCC_FILE_PID, "kp", 4, 4, false, 0},
    {PCCC_FILE_PID, "ki", 4, 6, false, 0},
    {PCCC_FILE_PID, "kd", 4, 8, false, 0},

    {PCCC_FILE_PID, "bias", 4, 10, false, 0},
    {PCCC_FILE_PID, "maxs", 4, 12, false, 0},
    {PCCC_FILE_PID, "mins", 4, 14, false, 0},
    {PCCC_FILE_PID, "db", 4, 16, false, 0},
    {PCCC_FILE_PID, "so", 4, 18, false, 0},
    {PCCC_FILE_PID, "maxo", 4, 20, false, 0},
    {PCCC_FILE_PID, "mino", 4, 22, false, 0},
    {PCCC_FILE_PID, "upd", 4, 24, false, 0},

    {PCCC_FILE_PID, "pv", 4, 26, false, 0},

    {PCCC_FILE_PID, "err", 4, 28, false, 0},
    {PCCC_FILE_PID, "out", 4, 30, false, 0},
    {PCCC_FILE_PID, "pvh", 4, 32, false, 0},
    {PCCC_FILE_PID, "pvl", 4, 34, false, 0},
    {PCCC_FILE_PID, "dvp", 4, 36, false, 0},
    {PCCC_FILE_PID, "dvn", 4, 38, false, 0},

    {PCCC_FILE_PID, "pvdb", 4, 40, false, 0},
    {PCCC_FILE_PID, "dvdb", 4, 42, false, 0},
    {PCCC_FILE_PID, "maxi", 4, 44, false, 0},
    {PCCC_FILE_PID, "mini", 4, 46, false, 0},
    {PCCC_FILE_PID, "tie", 4, 48, false, 0},

    {PCCC_FILE_PID, "addr", 8, 48, false, 0},

    {PCCC_FILE_PID, "data", 56, 52, false, 0},

    /* ST String */
    {PCCC_FILE_STRING, "len", 2, 0, false, 0},
    {PCCC_FILE_STRING, "data", 82, 1, false, 0},

    /* SC SFC */
    {PCCC_FILE_SFC, "con", 2, 0, false, 0},
    {PCCC_FILE_SFC, "sa", 2, 0, true, 15},
    {PCCC_FILE_SFC, "fs", 2, 0, true, 14},
    {PCCC_FILE_SFC, "ls", 2, 0, true, 13},
    {PCCC_FILE_SFC, "ov", 2, 0, true, 12},
    {PCCC_FILE_SFC, "er", 2, 0, true, 11},
    {PCCC_FILE_SFC, "dn", 2, 0, true, 10},
    {PCCC_FILE_SFC, "pre", 2, 1, false, 0},
    {PCCC_FILE_SFC, "tim", 2, 2, false, 0},

    /* T timer */
    {PCCC_FILE_TIMER, "con", 2, 0, false, 0},
    {PCCC_FILE_TIMER, "en", 2, 0, true, 15},
    {PCCC_FILE_TIMER, "tt", 2, 0, true, 14},
    {PCCC_FILE_TIMER, "dn", 2, 0, true, 13},
    {PCCC_FILE_TIMER, "pre", 2, 1, false, 0},
    {PCCC_FILE_TIMER, "acc", 2, 2, false, 0}};


int32_t enip_pccc_parse_logical_address(const char *file_address, pccc_addr_t *address) {
    int32_t rc = PLCTAG_STATUS_OK;
    const char *p = file_address;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Starting.");

    do {
        if((rc = parse_pccc_file_type(&p, address)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to parse PCCC-style tag for data-table type! Error %s!",
                   plc_tag_decode_error(rc));
            break;
        }

        /* we allow the file number to be skipped if it is output or input */
        rc = parse_pccc_file_num(&p, address);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to parse PCCC-style tag for file number! Error %s!",
                   plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_pccc_elem_num(&p, address)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to parse PCCC-style tag for element number! Error %s!",
                   plc_tag_decode_error(rc));
            break;
        }

        /* a sub-element could be a mnemonic or a numeric entry. */
        if((rc = parse_pccc_subelem(&p, address)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to parse PCCC-style tag for element number! Error %s!",
                   plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_pccc_bit_num(&p, address)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unable to parse PCCC-style tag for subelement number! Error %s!",
                   plc_tag_decode_error(rc));
            break;
        }
    } while(0);

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


/*
 * Encode the logical address as a level encoding for use with PLC/5 PLCs.
 *
 * Byte Meaning
 * 0    level flags
 * 1-3  level one
 * 1-3  level two
 * 1-3  level three
 */

Bytes enip_pccc_encode_plc5_address(pccc_addr_t *address, Bytes dest) {
    uint8_t level_byte = 0;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Starting.");

    if(!address || bytes_is_null(dest)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Called with null address or buffer!");
        return bytes_null();
    }

    /* check for space: level byte + up to three 3-byte levels. */
    if(dest.len < (1 + 3 + 3 + 3)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Encoded PCCC logical address buffer is too small!");
        return bytes_null();
    }

    /* reserve the level byte at index 0; encode levels after it. */
    Bytes cursor = bytes_slice(dest, 1, dest.len - 1);

    /* do the required levels.  Remember we start at the low bit! */
    level_byte = 0x06; /* level one and two */

    /* add in the data file number. */
    if(!encode_data(&cursor, address->file)) { return bytes_null(); }

    /* add in the element number */
    if(!encode_data(&cursor, address->element)) { return bytes_null(); }

    /* check to see if we need to put in a subelement. */
    if(address->sub_element >= 0) {
        level_byte |= 0x08;

        if(!encode_data(&cursor, address->sub_element)) { return bytes_null(); }
    }

    /* store the encoded levels. */
    dest.data[0] = level_byte;

    size_t total = dest.len - cursor.len;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "PLC/5 encoded address:");
    pdebug_dump_bytes(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, dest.data, (int)total);

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Done.");

    return bytes_from_buf(dest.data, total);
}


/*
 * Encode the logical address as a file/type/element/subelement struct.
 *
 * element  Meaning
 * file     Data file #.
 * type     Data file type.
 * element  element # within data file.
 * sub      field/sub-element within data file for structured data.
 */

Bytes enip_pccc_encode_slc_address(pccc_addr_t *address, Bytes dest) {
    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Starting.");

    if(!address || bytes_is_null(dest)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Called with null address or buffer!");
        return bytes_null();
    }

    /* check for space. */
    if(dest.len < (3 + 1 + 3 + 3)) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Encoded SLC logical address buffer is too small!");
        return bytes_null();
    }

    if(address->file_type == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "SLC file type %d cannot be decoded!", address->file_type);
        return bytes_null();
    }

    Bytes cursor = dest;

    /* encode the file number */
    if(!encode_data(&cursor, address->file)) { return bytes_null(); }

    /* encode the data file type. */
    if(!encode_data(&cursor, (int32_t)address->file_type)) { return bytes_null(); }

    /* add in the element number */
    if(!encode_data(&cursor, address->element)) { return bytes_null(); }

    /* add in the sub-element number */
    if(!encode_data(&cursor, (address->sub_element < 0 ? 0 : address->sub_element))) { return bytes_null(); }

    size_t total = dest.len - cursor.len;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "SLC/Micrologix encoded address:");
    pdebug_dump_bytes(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, dest.data, (int)total);

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Done.");

    return bytes_from_buf(dest.data, total);
}


static int32_t parse_pccc_file_type(const char **str, pccc_addr_t *address) {
    int32_t rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_INFO, 0, "Starting.");

    switch((*str)[0]) {
        case 'A':
        case 'a': /* ASCII */
            pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found ASCII file.");
            address->file_type = PCCC_FILE_ASCII;
            address->element_size_bytes = 1;
            (*str)++;
            break;

        case 'B':
        case 'b': /* Bit or block transfer */
            if(isdigit((unsigned char)(*str)[1])) {
                /* Bit */
                pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found Bit file.");
                address->file_type = PCCC_FILE_BIT;
                address->element_size_bytes = 2;
                (*str)++;
                break;
            } else {
                if((*str)[1] == 'T' || (*str)[1] == 't') {
                    /* block transfer */
                    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found Block Transfer file.");
                    address->file_type = PCCC_FILE_BLOCK_TRANSFER;
                    address->element_size_bytes = 12;
                    (*str) += 2;
                } else {
                    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unknown file %s found!", *str);
                    address->file_type = PCCC_FILE_UNKNOWN;
                    address->element_size_bytes = 0;
                    rc = PLCTAG_ERR_BAD_PARAM;
                }
            }

            break;

        case 'C':
        case 'c': /* Counter */
            pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found Counter file.");
            address->file_type = PCCC_FILE_COUNTER;
            address->element_size_bytes = 6;
            (*str)++;
            break;

        case 'D':
        case 'd': /* BCD number */
            pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found BCD file.");
            address->file_type = PCCC_FILE_BCD;
            address->element_size_bytes = 2;
            (*str)++;
            break;

        case 'F':
        case 'f': /* Floating point Number */
            pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found Float/REAL file.");
            address->file_type = PCCC_FILE_FLOAT;
            address->element_size_bytes = 4;
            (*str)++;
            break;

        case 'I':
        case 'i': /* Input */
            pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found Input file.");
            address->file_type = PCCC_FILE_INPUT;
            address->file = 1; /* in case it is omitted */
            address->element_size_bytes = 2;
            (*str)++;
            break;

        case 'L':
        case 'l':
            pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found Long Int file.");
            address->file_type = PCCC_FILE_LONG_INT;
            address->element_size_bytes = 4;
            (*str)++;
            break;

        case 'M':
        case 'm': /* Message */
            if((*str)[1] == 'G' || (*str)[1] == 'g') {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found Message file.");
                address->file_type = PCCC_FILE_MESSAGE;
                address->element_size_bytes = 112;
                (*str) += 2; /* skip past both characters */
            } else {
                address->file_type = PCCC_FILE_UNKNOWN;
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unknown file %s found!", *str);
                rc = PLCTAG_ERR_BAD_PARAM;
            }
            break;

        case 'N':
        case 'n': /* INT */
            pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found Integer file.");
            address->file_type = PCCC_FILE_INT;
            address->element_size_bytes = 2;
            (*str)++;
            break;

        case 'O':
        case 'o': /* Output */
            /* FIXME - Check if 0x82 is correct instead of 0x8b */
            pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found Output file.");
            address->file_type = PCCC_FILE_OUTPUT;
            address->element_size_bytes = 2;
            address->file = 0; /* in case it is omitted */
            (*str)++;
            break;

        case 'P':
        case 'p': /* PID */
            if((*str)[1] == 'D' || (*str)[1] == 'd') {
                pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found PID file.");
                address->file_type = PCCC_FILE_PID;
                address->element_size_bytes = 164;
                (*str) += 2; /* skip past both characters */
            } else {
                address->file_type = PCCC_FILE_UNKNOWN;
                pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unknown file %s found!", *str);
                rc = PLCTAG_ERR_BAD_PARAM;
            }
            break;

        case 'R':
        case 'r': /* Control */
            pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found Control file.");
            address->file_type = PCCC_FILE_CONTROL;
            address->element_size_bytes = 6;
            (*str)++;
            break;

        case 'S':
        case 's': /* Status, SFC or String */
            if(isdigit((unsigned char)(*str)[1])) {
                /* Status */
                pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found Status file.");
                address->file_type = PCCC_FILE_STATUS;
                address->element_size_bytes = 2;
                (*str)++;
                break;
            } else {
                if((*str)[1] == 'C' || (*str)[1] == 'c') {
                    /* SFC */
                    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found SFC file.");
                    address->file_type = PCCC_FILE_SFC;
                    address->element_size_bytes = 6;
                    (*str) += 2; /* skip past both characters */
                } else if((*str)[1] == 'T' || (*str)[1] == 't') {
                    /* String */
                    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found String file.");
                    address->file_type = PCCC_FILE_STRING;
                    address->element_size_bytes = 84;
                    (*str) += 2; /* skip past both characters */
                } else {
                    address->file_type = PCCC_FILE_UNKNOWN;
                    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unknown file %s found!", *str);
                    rc = PLCTAG_ERR_BAD_PARAM;
                }
            }
            break;

        case 'T':
        case 't': /* Timer */
            pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found Timer file.");
            address->file_type = PCCC_FILE_TIMER;
            address->element_size_bytes = 6;
            (*str)++;
            break;

        default:
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Bad format or unsupported logical address %s!", *str);
            address->file_type = PCCC_FILE_UNKNOWN;
            address->element_size_bytes = 0;
            rc = PLCTAG_ERR_BAD_PARAM;
            break;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


static int32_t parse_pccc_file_num(const char **str, pccc_addr_t *address) {
    int32_t tmp = 0;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Starting.");

    if(!str || !*str) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Expected data-table file number!");
        address->file = -1;
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* if this is I or O, then we can skip the data file number. */
    if((address->file_type == PCCC_FILE_INPUT || address->file_type == PCCC_FILE_OUTPUT) && !isdigit((unsigned char)**str)) {
        /* skip the data file number */
        pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Data file number omitted for I or O data file.");
        return PLCTAG_STATUS_OK;
    }

    /* FIXME - why are we not using strtol here? We should also support octal. */
    while(**str && isdigit((unsigned char)**str) && tmp < 65535) {
        tmp *= 10;
        tmp += (int32_t)((**str) - '0');
        (*str)++;
    }

    address->file = tmp;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


static int32_t parse_pccc_elem_num(const char **str, pccc_addr_t *address) {
    int32_t tmp = 0;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Starting.");

    if(!str || !*str || **str != ':') {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Expected data-table element number!");
        address->element = -1;
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* step past the : character */
    (*str)++;

    while(**str && isdigit((unsigned char)**str) && tmp < 65535) {
        tmp *= 10;
        tmp += (int32_t)((**str) - '0');
        (*str)++;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found element %d.", tmp);

    address->element = tmp;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


static int32_t parse_pccc_subelem(const char **str, pccc_addr_t *address) {
    int32_t rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Starting.");

    if(!str || !*str) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Called with bad string pointer!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * if we have a null character we are at the end of the name
     * and the subelement is not there.  That is not an error.
     */

    if((**str) == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "No subelement in this name.");
        address->sub_element = -1;
        return PLCTAG_STATUS_OK;
    }

    /*
     * We do have a character.  It must be . or / to be valid.
     * The . character is valid before a mnemonic or number for a field in a structured type.
     * The / character is valid before a bit number.
     *
     * If we see a bit number, then punt out of this routine.
     */

    if((**str) == '/') {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "No subelement in this logical address.");
        address->sub_element = -1;
        return PLCTAG_STATUS_OK;
    }

    /* make sure the next character is . and nothing else. */
    if((**str) != '.') {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Bad subelement field in logical address.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* step past the . character */
    (*str)++;

    /* try a number. */
    rc = parse_pccc_subelem_num(str, address);
    if(rc == PLCTAG_STATUS_OK) {
        /* we found a numeric sub-element */
        pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found numeric sub-element %d.", address->sub_element);
        return rc;
    }

    if(rc == PLCTAG_ERR_NO_MATCH) {
        /* not a numeric sub-element, try matching a mnemonic. */
        return parse_pccc_subelem_mnemonic(str, address);
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


static int32_t parse_pccc_subelem_num(const char **str, pccc_addr_t *address) {
    int32_t tmp = 0;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Starting.");

    /* is it a numeric sub-element? */
    if(!isdigit((unsigned char)**str)) {
        /* nope, it is not. */
        pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Not a numeric sub-element.");
        return PLCTAG_ERR_NO_MATCH;
    }

    while(**str && isdigit((unsigned char)**str) && tmp < 65535) {
        tmp *= 10;
        tmp += (int32_t)((**str) - '0');
        (*str)++;
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Found sub-element %d.", tmp);

    address->sub_element = tmp;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


static int32_t parse_pccc_subelem_mnemonic(const char **str, pccc_addr_t *address) {
    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Starting.");

    if(!str || !*str) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Called with bad string pointer!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * The caller (parse_pccc_subelem) has already consumed the leading '.',
     * so *str points directly at the mnemonic.  A null here means a trailing
     * '.' with no field, which is not an error.
     */

    if((**str) == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "No subelement in this name.");
        address->sub_element = -1;
        return PLCTAG_STATUS_OK;
    }

    /* search for a match. */
    for(size_t i = 0; i < (sizeof(sub_element_lookup) / sizeof(sub_element_lookup[0])); i++) {
        if(sub_element_lookup[i].file_type == address->file_type
           && str_cmp_i_n(*str, sub_element_lookup[i].field_name, str_length(sub_element_lookup[i].field_name)) == 0) {
            pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Matched file type %x and field mnemonic \"%.*s\".", address->file_type,
                   str_length(sub_element_lookup[i].field_name), *str);

            address->is_bit = sub_element_lookup[i].is_bit;
            address->bit = sub_element_lookup[i].bit;
            address->sub_element = sub_element_lookup[i].sub_element;
            address->element_size_bytes = sub_element_lookup[i].element_size_bytes;

            /* bump past the field name */
            (*str) += str_length(sub_element_lookup[i].field_name);

            return PLCTAG_STATUS_OK;
        }
    }

    pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unsupported field mnemonic %s for type %x!", *str, address->file_type);

    return PLCTAG_ERR_BAD_PARAM;
}


static int32_t parse_pccc_bit_num(const char **str, pccc_addr_t *address) {
    int32_t tmp = 0;
    int32_t max_bit = 0;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Starting.");

    if(!str || !*str) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Called with bad string pointer!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * if we have a null character we are at the end of the name
     * and the subelement is not there.  That is not an error.
     */

    if((**str) == 0) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "No bit number in this name.");
        return PLCTAG_STATUS_OK;
    }

    /* make sure the next character is /. */
    if((**str) != '/') {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Bad bit number in logical address.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* make sure that the data type is B, N, L or S. */
    switch(address->file_type) {
        case PCCC_FILE_BIT: max_bit = 15; break;
        case PCCC_FILE_INT: max_bit = 15; break;
        case PCCC_FILE_LONG_INT: max_bit = 31; break;
        case PCCC_FILE_STATUS: max_bit = 16; break;
        default:
            pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Unsupported file type %x!", address->file);
            return PLCTAG_ERR_BAD_PARAM;
    }

    /* step past the / character */
    (*str)++;

    /* FIXME - we do this a lot, should be a small routine. */
    while(**str && isdigit((unsigned char)**str) && tmp < 65535) {
        tmp *= 10;
        tmp += (int32_t)((**str) - '0');
        (*str)++;
    }

    if(tmp < 0 || tmp > max_bit) {
        pdebug(DEBUG_MODULE_ENIP, DEBUG_WARN, 0, "Error processing bit number.  Must be between 0 and %d inclusive, found %d!",
               max_bit, tmp);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    address->is_bit = true;
    address->bit = (uint8_t)tmp;

    pdebug(DEBUG_MODULE_ENIP, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/* Encode one PCCC address level (a byte, or 0xFF + a u16le for a value that
 * does not fit a byte), advancing *cursor to the unfilled remainder. Returns
 * false (leaving *cursor unchanged) if *cursor has no room. */
static bool encode_data(Bytes *cursor, int32_t val) {
    Bytes next = (val <= 254) ? bytes_pack_into(*cursor, BYTES_LE, (uint8_t)val)
                              : bytes_pack_into(*cursor, BYTES_LE, (uint8_t)0xFF, (uint16_t)val);
    if(bytes_is_null(next)) { return false; }
    *cursor = next;
    return true;
}
