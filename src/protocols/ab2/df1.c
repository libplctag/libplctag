/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
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

#include <ab2/df1.h>
#include <ctype.h>
#include <lib/libplctag.h>
#include <util/debug.h>
#include <util/string.h>

/* tag name parsing. */
static int parse_df1_file_type(const char **str, df1_file_t *file_type);
static int parse_df1_file_num(const char **str, int *file_num);
static int parse_df1_elem_num(const char **str, int *elem_num);
static int parse_df1_subelem_num(const char **str, df1_file_t *file_type, int *subelem_num);
static int parse_df1_bit_num(const char **str, bool *is_bit, uint8_t *bit_num);


int df1_parse_logical_address(const char *name, df1_file_t *file_type, int *file_num, int *elem_num, int *subelem_num, bool *is_bit, uint8_t *bit_num)
{
    int rc = PLCTAG_STATUS_OK;
    const char *p = name;

    pdebug(DEBUG_DETAIL, "Starting to parse %s.", name);

    do {
        if((rc = parse_df1_file_type(&p, file_type)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for data-table type! Error %s!", plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_df1_file_num(&p, file_num)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for file number! Error %s!", plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_df1_elem_num(&p, elem_num)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for element number! Error %s!", plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_df1_subelem_num(&p, file_type, subelem_num)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for subelement number! Error %s!", plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_df1_bit_num(&p, is_bit, bit_num)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for subelement number! Error %s!", plc_tag_decode_error(rc));
            break;
        }
    } while(0);

    pdebug(DEBUG_DETAIL, "Done with name %s.", name);

    return rc;
}


int df1_element_size(df1_file_t file_type)
{
    int result = PLCTAG_ERR_UNSUPPORTED;

    switch(file_type) {
    case DF1_FILE_UNKNOWN:
        result = PLCTAG_ERR_UNSUPPORTED;
        break;
    case DF1_FILE_ASCII:
        result = 0;
        break; /* specific to the data file. */
    case DF1_FILE_BCD:
        result = 2;
        break;
    case DF1_FILE_BIT:
        result = 2;
        break;
    case DF1_FILE_BYTE: /* fake type for chars in strings. */
        result = 1;
        break;
    case DF1_FILE_CONTROL:
        result = 6;
        break;
    case DF1_FILE_COUNTER:
        result = 6;
        break;
    case DF1_FILE_FLOAT:
        result = 4;
        break;
    case DF1_FILE_INPUT:
        result = 2;
        break;
    case DF1_FILE_INT:
        result = 2;
        break;
    case DF1_FILE_LONG_INT:
        result = 4;
        break;
    case DF1_FILE_MESSAGE:
        result = 112;
        break;
    case DF1_FILE_OUTPUT:
        result = 2;
        break;
    case DF1_FILE_PID:
        result = 0;
        break; /* differs in size! */
    case DF1_FILE_STATUS:
        result = 2;
        break;
    case DF1_FILE_STRING:
        result = 84;
        break;
    case DF1_FILE_TIMER:
        result = 6;
        break;
    default:
        result = PLCTAG_ERR_UNSUPPORTED;
        break;
    }

    return result;
}


int parse_df1_file_type(const char **str, df1_file_t *file_type)
{
    int rc = PLCTAG_STATUS_OK;

    switch((*str)[0]) {
    case 'A':
    case 'a': /* ASCII */
        *file_type = DF1_FILE_ASCII;
        (*str)++;
        break;

    case 'B':
    case 'b': /* Bit or block transfer */
        if(isdigit((*str)[1])) {
            /* Bit */
            *file_type = DF1_FILE_BIT;
            (*str)++;
            break;
        } else {
            if((*str)[1] == 'T' || (*str)[1] == 't') {
                /* block transfer */
                *file_type = DF1_FILE_BLOCK_TRANSFER;
                (*str) += 2; /* skip past both characters */
            } else {
                *file_type = DF1_FILE_UNKNOWN;
                pdebug(DEBUG_WARN, "Bad format or unsupported logical address, expected B or BT!");
                rc = PLCTAG_ERR_BAD_PARAM;
            }
        }

        break;

    case 'C':
    case 'c': /* Counter */
        *file_type = DF1_FILE_COUNTER;
        (*str)++;
        break;

    case 'D':
    case 'd': /* BCD number */
        *file_type = DF1_FILE_BCD;
        (*str)++;
        break;

    case 'F':
    case 'f': /* Floating point Number */
        *file_type = DF1_FILE_FLOAT;
        (*str)++;
        break;

    case 'I':
    case 'i': /* Input */
        *file_type = DF1_FILE_INPUT;
        (*str)++;
        break;

    case 'L':
    case 'l':
        *file_type = DF1_FILE_LONG_INT;
        (*str)++;
        break;

    case 'M':
    case 'm': /* Message */
        if((*str)[1] == 'G' || (*str)[1] == 'g') {
            *file_type = DF1_FILE_MESSAGE;
            (*str) += 2; /* skip past both characters */
        } else {
            *file_type = DF1_FILE_UNKNOWN;
            pdebug(DEBUG_WARN, "Bad format or unsupported logical address, expected MG!");
            rc = PLCTAG_ERR_BAD_PARAM;
        }

        break;

    case 'N':
    case 'n': /* INT */
        *file_type = DF1_FILE_INT;
        (*str)++;
        break;

    case 'O':
    case 'o': /* Output */
        *file_type = DF1_FILE_OUTPUT;
        (*str)++;
        break;

    case 'P':
    case 'p': /* PID */
        if((*str)[1] == 'D' || (*str)[1] == 'd') {
            *file_type = DF1_FILE_PID;
            (*str) += 2; /* skip past both characters */
        } else {
            *file_type = DF1_FILE_UNKNOWN;
            pdebug(DEBUG_WARN, "Bad format or unsupported logical address, expected PD!");
            rc = PLCTAG_ERR_BAD_PARAM;
        }

        break;

    case 'R':
    case 'r': /* Control */
        *file_type = DF1_FILE_CONTROL;
        (*str)++;
        break;

    case 'S':
    case 's': /* Status, SFC or String */
        if(isdigit((*str)[1])) {
            /* Status */
            *file_type = DF1_FILE_STATUS;
            (*str)++;
            break;
        } else {
            if((*str)[1] == 'C' || (*str)[1] == 'c') {
                /* SFC */
                *file_type = DF1_FILE_SFC;
                (*str) += 2; /* skip past both characters */
            } else if((*str)[1] == 'T' || (*str)[1] == 't') {
                /* String */
                *file_type = DF1_FILE_STRING;
                (*str) += 2; /* skip past both characters */
            } else {
                *file_type = DF1_FILE_UNKNOWN;
                pdebug(DEBUG_WARN, "Bad format or unsupported logical address, expected string, SFC or status!");
                rc = PLCTAG_ERR_BAD_PARAM;
            }
        }

        break;

    case 'T':
    case 't': /* Timer */
        *file_type = DF1_FILE_TIMER;
        (*str)++;
        break;

    default:
        pdebug(DEBUG_WARN, "Bad format or unsupported logical address %s!", *str);
        *file_type = DF1_FILE_UNKNOWN;
        rc = PLCTAG_ERR_BAD_PARAM;
        break;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int parse_df1_file_num(const char **str, int *file_num)
{
    int tmp = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!str || !*str || !isdigit(**str)) {
        pdebug(DEBUG_WARN, "Expected data-table file number!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    while(**str && isdigit(**str) && tmp < 65535) {
        tmp *= 10;
        tmp += (int)((**str) - '0');
        (*str)++;
    }

    *file_num = tmp;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int parse_df1_elem_num(const char **str, int *elem_num)
{
    int tmp = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!str || !*str || **str != ':') {
        pdebug(DEBUG_WARN, "Expected data-table element number!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* step past the : character */
    (*str)++;

    while(**str && isdigit(**str) && tmp < 65535) {
        tmp *= 10;
        tmp += (int)((**str) - '0');
        (*str)++;
    }

    *elem_num = tmp;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int parse_df1_subelem_num(const char **str, df1_file_t *file_type, int *subelem_num)
{
    int tmp = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!str || !*str) {
        pdebug(DEBUG_WARN, "Called with bad string pointer!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * if we have a null character we are at the end of the name
     * and the subelement is not there.  That is not an error.
     */

    if((**str) == 0) {
        pdebug(DEBUG_DETAIL, "No subelement in this name.");
        *subelem_num = -1;
        return PLCTAG_STATUS_OK;
    }

    /* just return if we have a bit element here. */
    if((**str) == '.') {
        pdebug(DEBUG_DETAIL, "No subelement in this name, but there is a bit element.");
        *subelem_num = -1;
        return PLCTAG_STATUS_OK;
    }

    /* make sure the next character is '.' and nothing else. */
    if((**str) != '.') {
        pdebug(DEBUG_WARN, "Bad subelement field in logical address.");
        return PLCTAG_ERR_BAD_PARAM;
    }


    /* TODO

    - make all bit handling set the tag bit flag.
    - use the correct DF1 commands to mask the bit.
    - perhaps another vtable to allow for PLC-specific functions for
      request builders?
    - copy first and then refactor?
    - only allow bits on B, N and L fields or sub-elements
      that are integral types.   How to determine sub-element
      size?   Need that for typing and element size calculations.

    - Fix handling of connection refusal.   It appears that
      connections are marked as complete from the simulator
      when they should not be.

    - refactor to support multiple PCCC PLC types.   May
      be best to simply refactor the inner part of the
      request builders.

    */


    /* mnemonic. */

    /* step past the . character */
    (*str)++;

    /* this depends on the data-table file type.
     * Note that we switch the type of the file based on the field type.
     */
    switch(*file_type) {
    case DF1_FILE_BLOCK_TRANSFER:
        *file_type = DF1_FILE_INT;
        if(str_cmp_i(*str, "con") == 0) {
            *subelem_num = 0;
        } else if(str_cmp_i(*str, "rlen") == 0) {
            *subelem_num = 1;
        } else if(str_cmp_i(*str, "dlen") == 0) {
            *subelem_num = 2;
        } else if(str_cmp_i(*str, "df") == 0) {
            *subelem_num = 3;
        } else if(str_cmp_i(*str, "elem") == 0) {
            *subelem_num = 4;
        } else if(str_cmp_i(*str, "rgs") == 0) {
            *subelem_num = 5;
        } else {
            pdebug(DEBUG_WARN, "Unsupported block transfer mnemonic!");
            return PLCTAG_ERR_BAD_PARAM;
        }

        break;

    case DF1_FILE_COUNTER:
    case DF1_FILE_TIMER:
        *file_type = DF1_FILE_INT;
        if(str_cmp_i(*str, "con") == 0) {
            *subelem_num = 0;
        } else if(str_cmp_i(*str, "pre") == 0) {
            *subelem_num = 1;
        } else if(str_cmp_i(*str, "acc") == 0) {
            *subelem_num = 2;
        } else {
            pdebug(DEBUG_WARN, "Unsupported %s mnemonic!", (file_type == DF1_FILE_COUNTER ? "counter" : "timer"));
            return PLCTAG_ERR_BAD_PARAM;
        }

        break;

    case DF1_FILE_CONTROL:
        *file_type = DF1_FILE_INT;
        if(str_cmp_i(*str, "con") == 0) {
            *subelem_num = 0;
        } else if(str_cmp_i(*str, "len") == 0) {
            *subelem_num = 1;
        } else if(str_cmp_i(*str, "pos") == 0) {
            *subelem_num = 2;
        } else {
            pdebug(DEBUG_WARN, "Unsupported control mnemonic!");
            return PLCTAG_ERR_BAD_PARAM;
        }

        break;

    case DF1_FILE_PID:
        /* We cannot fix the type of the PID as it depends on the configuration! */
        *file_type = DF1_FILE_UNKNOWN;
        if(str_cmp_i(*str, "con") == 0) {
            *subelem_num = 0;
        } else if(str_cmp_i(*str, "sp") == 0) {
            *subelem_num = 2;
        } else if(str_cmp_i(*str, "kp") == 0) {
            *subelem_num = 4;
        } else if(str_cmp_i(*str, "ki") == 0) {
            *subelem_num = 6;
        } else if(str_cmp_i(*str, "kd") == 0) {
            *subelem_num = 8;
        } else if(str_cmp_i(*str, "pv") == 0) {
            *subelem_num = 26;
        } else {
            pdebug(DEBUG_WARN, "Unsupported PID mnemonic!");
            return PLCTAG_ERR_BAD_PARAM;
        }

        break;

    case DF1_FILE_MESSAGE:
        *file_type = DF1_FILE_INT;
        if(str_cmp_i(*str, "con") == 0) {
            *subelem_num = 0;
        } else if(str_cmp_i(*str, "err") == 0) {
            *subelem_num = 1;
        } else if(str_cmp_i(*str, "rlen") == 0) {
            *subelem_num = 2;
        } else if(str_cmp_i(*str, "dlen") == 0) {
            *subelem_num = 3;
        } else {
            pdebug(DEBUG_WARN, "Unsupported message mnemonic!");
            return PLCTAG_ERR_BAD_PARAM;
        }

        break;

    case DF1_FILE_STRING:
        if(str_cmp_i(*str, "len") == 0) {
            *file_type = DF1_FILE_INT;
            *subelem_num = 0;
        } else if(str_cmp_i(*str, "data") == 0) {
            *file_type = DF1_FILE_BYTE; /* not a real type, but used to indicate a character. */
            *subelem_num = 1;
        } else {
            pdebug(DEBUG_WARN, "Unsupported string mnemonic!");
            return PLCTAG_ERR_BAD_PARAM;
        }

        break;

    default:
        pdebug(DEBUG_WARN, "Unsupported mnemonic %s!", *str);
        return PLCTAG_ERR_BAD_PARAM;
        break;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int parse_df1_bit_num(const char **str, bool *is_bit, uint8_t *bit_num)
{
    int tmp = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!str || !*str) {
        pdebug(DEBUG_WARN, "Called with bad string pointer!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * if we have a null character we are at the end of the name
     * and the subelement is not there.  That is not an error.
     */

    if((**str) == 0) {
        pdebug(DEBUG_DETAIL, "No bit field in this name.");
        *is_bit = FALSE;
        return PLCTAG_STATUS_OK;
    }

    /*
     * We do have a character.  It must be . or / to be valid.
     * The . character is valid before a mnemonic for a field in a structured type.
     * The / character is valid before a bit number.
     */

    /* make sure the next character is '/' and nothing else. */
    if((**str) != '/') {
        pdebug(DEBUG_WARN, "Bad bit field in logical address.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* bit number. */

    /* step past the / character */
    (*str)++;

    /* FIXME - we do this a lot, should be a small routine. */
    tmp = 0;
    while(**str && isdigit(**str) && tmp < 32) { /* MAGIC - can we have a bit more than 32? */
        tmp *= 10;
        tmp += (int)((**str) - '0');
        (*str)++;
    }

    *bit_num = tmp;
    *is_bit = TRUE;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


const char *df1_decode_error(uint8_t status, uint16_t extended_status)
{
    unsigned int error = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    error = status;

    /* extended error? */
    if(error == 0xF0) {
        error = extended_status;
    }

    switch(error) {
    case 1:
        return "Error converting block address.";
        break;

    case 2:
        return "Less levels specified in address than minimum for any address.";
        break;

    case 3:
        return "More levels specified in address than system supports";
        break;

    case 4:
        return "Symbol not found.";
        break;

    case 5:
        return "Symbol is of improper format.";
        break;

    case 6:
        return "Address doesn't point to something usable.";
        break;

    case 7:
        return "File is wrong size.";
        break;

    case 8:
        return "Cannot complete request, situation has changed since the start of the command.";
        break;

    case 9:
        return "File is too large.";
        break;

    case 0x0A:
        return "Transaction size plus word address is too large.";
        break;

    case 0x0B:
        return "Access denied, improper privilege.";
        break;

    case 0x0C:
        return "Condition cannot be generated - resource is not available (some has upload active)";
        break;

    case 0x0D:
        return "Condition already exists - resource is already available.";
        break;

    case 0x0E:
        return "Command could not be executed PCCC decode error.";
        break;

    case 0x0F:
        return "Requester does not have upload or download access - no privilege.";
        break;

    case 0x10:
        return "Illegal command or format.";
        break;

    case 0x20:
        return "Host has a problem and will not communicate.";
        break;

    case 0x30:
        return "Remote node host is missing, disconnected, or shut down.";
        break;

    case 0x40:
        return "Host could not complete function due to hardware fault.";
        break;

    case 0x50:
        return "Addressing problem or memory protect rungs.";
        break;

    case 0x60:
        return "Function not allowed due to command protection selection.";
        break;

    case 0x70:
        return "Processor is in Program mode.";
        break;

    case 0x80:
        return "Compatibility mode file missing or communication zone problem.";
        break;

    case 0x90:
        return "Remote node cannot buffer command.";
        break;

    case 0xA0:
        return "Wait ACK (1775-KA buffer full).";
        break;

    case 0xB0:
        return "Remote node problem due to download.";
        break;

    case 0xC0:
        return "Wait ACK (1775-KA buffer full)."; /* why is this duplicate? */
        break;

    default:
        return "Unknown error response.";
        break;
    }


    return "Unknown error response.";
}
