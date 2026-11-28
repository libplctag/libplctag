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
#include <float.h>
#include <libplctag/lib/libplctag.h>
#include <libplctag/lib/tag.h>
#include <libplctag/protocols/ab/ab_common.h>
#include <libplctag/protocols/ab/error_codes.h>
#include <libplctag/protocols/ab/pccc.h>
#include <libplctag/protocols/ab/tag.h>
#include <limits.h>
#include <platform.h>
#include <stddef.h>
#include <string.h>
#include <utils/debug.h>


START_PACK typedef struct {
    /* PCCC Command Req Routing */
    uint8_t service_code;           /* ALWAYS 0x4B, Execute PCCC */
    uint8_t req_path_size;          /* ALWAYS 0x02, in 16-bit words */
    uint8_t req_path[4];            /* ALWAYS 0x20,0x67,0x24,0x01 for PCCC */
    uint8_t request_id_size;        /* ALWAYS 7 */
    uint16_le vendor_id;            /* Our CIP Vendor ID */
    uint32_le vendor_serial_number; /* Our CIP Vendor Serial Number */
} END_PACK cip_pccc_req;

START_PACK typedef struct {
    /* CIP Reply */
    uint8_t reply_code;     /* 0xCB Execute PCCC Reply */
    uint8_t reserved;       /* 0x00 in reply */
    uint8_t general_status; /* 0x00 for success */
    uint8_t status_size;    /* number of 16-bit words of extra status, 0 if success */

    /* PCCC matching info */
    uint8_t request_id_size;        /* ALWAYS 7 */
    uint16_le vendor_id;            /* Our CIP Vendor ID */
    uint32_le vendor_serial_number; /* Our CIP Vendor Serial Number */
} END_PACK cip_pccc_resp;


START_PACK typedef struct {
    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
    uint8_t pccc_function;  /* FNC sub-function of command */
} END_PACK pccc_cmd_req;


START_PACK typedef struct {
    /* PCCC Command */
    uint8_t pccc_command;         /* CMD read, write etc. */
    uint8_t pccc_status;          /* STS 0x00 in request */
    uint16_le pccc_seq_num;       /* TNS transaction/sequence id */
    uint8_t pccc_function;        /* FNC sub-function of command */
    uint16_le pccc_offset;        /* offset of requested in total request */
    uint16_le pccc_transfer_size; /* total number of words requested */
} END_PACK plc5_pccc_read_cmd_req;

START_PACK typedef struct {
    /* PCCC Command */
    uint8_t pccc_command;       /* CMD read, write etc. */
    uint8_t pccc_status;        /* STS 0x00 in request */
    uint16_le pccc_seq_num;     /* TNS transaction/sequence id */
    uint8_t pccc_function;      /* FNC sub-function of command */
    uint8_t pccc_transfer_size; /* total number of bytes requested */
} END_PACK slc_pccc_read_cmd_req;

START_PACK typedef struct {
    /* PCCC Command */
    uint8_t pccc_command;         /* CMD read, write etc. */
    uint8_t pccc_status;          /* STS 0x00 in request */
    uint16_le pccc_seq_num;       /* TNS transaction/sequence id */
    uint8_t pccc_function;        /* FNC sub-function of command */
    uint16_le pccc_offset;        /* offset of requested in total request */
    uint16_le pccc_transfer_size; /* total number of words to be written */
} END_PACK plc5_pccc_write_cmd_req;

START_PACK typedef struct {
    /* PCCC Command */
    uint8_t pccc_command;       /* CMD read, write etc. */
    uint8_t pccc_status;        /* STS 0x00 in request */
    uint16_le pccc_seq_num;     /* TNS transaction/sequence id */
    uint8_t pccc_function;      /* FNC sub-function of command */
    uint8_t pccc_transfer_size; /* total number of bytes to be written */
} END_PACK slc_pccc_write_cmd_req;


START_PACK typedef struct {
    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
    uint8_t pccc_function;  /* FNC sub-function of command */
} END_PACK pccc_rmw_cmd_req;


START_PACK typedef struct {
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
} END_PACK pccc_cmd_resp;


START_PACK typedef struct {
    /* DH+ Routing */
    uint16_le dest_link;
    uint16_le dest_node;
    uint16_le src_link;
    uint16_le src_node;
} END_PACK pccc_dhp_routing_header;

START_PACK typedef struct {
    /* DH+ Routing */
    uint16_le dest_link;
    uint16_le dest_node;
    uint16_le src_link;
    uint16_le src_node;

    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
} END_PACK pccc_dhp_cmd_resp;


// static int parse_pccc_logical_address(const char *name, pccc_file_t address->file_type, int *file_num, int *elem_num, int
// *sub_elem_num);
static int parse_pccc_file_type(const char **str, pccc_addr_t *address);
static int parse_pccc_file_num(const char **str, pccc_addr_t *address);
static int parse_pccc_elem_num(const char **str, pccc_addr_t *address);
static int parse_pccc_subelem(const char **str, pccc_addr_t *address);
static int parse_pccc_subelem_num(const char **str, pccc_addr_t *address);
static int parse_pccc_subelem_mnemonic(const char **str, pccc_addr_t *address);
static int parse_pccc_bit_num(const char **str, pccc_addr_t *address);
// static int encode_file_type(pccc_file_t file_type);


static int pccc_check_read_status(ab_tag_p tag);
static int pccc_check_write_status(ab_tag_p tag);
static int plc5_tag_write_bit_start(ab_tag_p tag);
static int slc_tag_write_bit_start(ab_tag_p tag);

static uint8_t *pccc_write_prefix(ab_tag_p tag, uint8_t *buf);
static size_t pccc_prefix_size(ab_tag_p tag);
static size_t pccc_reply_prefix_size(ab_tag_p tag);

/*
 * Public functions
 */


/*
 * The PLC/5, SLC, and MicroLogix have their own unique way of determining what you are
 * accessing.   All variables and data are in data-table files.  Each data-table contains
 * data of one type.
 *
 * The main ones are:
 *
 * File Type   Name    Structure/Size
 * ASCII       A       1 byte per character
 * Bit         B       word
 * Block-transfer BT   structure/6 words
 * Counter     C       structure/3 words
 * BCD         D       word
 * Float       F       32-bit float
 * Input       I       word
 * Output      O       word
 * Long Int    L       32-bit integer, two words
 * Message     MG      structure/56 words
 * Integer     N       word
 * PID         PD      structure/41 floats
 * Control     R       structure/3 words
 * Status      S       word
 * SFC         SC      structure/3 words
 * String      ST      structure/84 bytes
 * Timer       T       structure/3 words
 *
 * Logical address are formulated as follows:
 *
 * <data type> <file #> : <element #> <sub-element selector>
 *
 * The data type is the "Name" above.
 *
 * The file # is the data-table file number.
 *
 * The element number is the index (zero based) into the table.
 *
 * The sub-element selector is one of three things:
 *
 * 1) missing.   A logical address like N7:0 is perfectly fine and selects a full 16-bit integer.
 * 2) a named field in a timer, counter, control, PID, block transfer, message, or string.
 * 3) a bit within a word for integers or bits.
 *
 * The following are the supported named fields:
 *
 * Timer/Counter
 * Offset   Field
 * 0        con - control
 * 1        pre - preset
 * 2        acc - accumulated
 *
 * R/Control
 * Offset   Field
 * 0        con - control
 * 1        len - length
 * 2        pos - position
 *
 * PD/PID
 * Offset   Field
 * 0        con - control
 * 2        sp - SP
 * 4        kp - Kp
 * 6        ki - Ki
 * 8        kd - Kd
 * 26       pv - PV
 *
 * BT
 * Offset   Field
 * 0        con - control
 * 1        rlen - RLEN
 * 2        dlen - DLEN
 * 3        df - data file #
 * 4        elem - element #
 * 5        rgs - rack/grp/slot
 *
 * MG
 * Offset   Field
 * 0        con - control
 * 1        err - error
 * 2        rlen - RLEN
 * 3        dlen - DLEN
 *
 * ST
 * Offset   Field
 * 0        LEN - length
 * 2-84     DATA - characters of string
 */


/*
 * parse_pccc_logical_address
 *
 * Parse the address into a structure with all the fields broken out.  This
 * checks the validity of the address in a PLC-neutral way.
 */

int parse_pccc_logical_address(const char *file_address, pccc_addr_t *address) {
    int rc = PLCTAG_STATUS_OK;
    const char *p = file_address;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Starting.");

    do {
        if((rc = parse_pccc_file_type(&p, address)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Unable to parse PCCC-style tag for data-table type! Error %s!",
                   plc_tag_decode_error(rc));
            break;
        }

        /* we allow the file number to be skipped if it is output or input */
        rc = parse_pccc_file_num(&p, address);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Unable to parse PCCC-style tag for file number! Error %s!",
                   plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_pccc_elem_num(&p, address)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Unable to parse PCCC-style tag for element number! Error %s!",
                   plc_tag_decode_error(rc));
            break;
        }

        /* a sub-element could be a mnemonic or a numeric entry. */
        if((rc = parse_pccc_subelem(&p, address)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Unable to parse PCCC-style tag for element number! Error %s!",
                   plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_pccc_bit_num(&p, address)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Unable to parse PCCC-style tag for subelement number! Error %s!",
                   plc_tag_decode_error(rc));
            break;
        }
    } while(0);

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Starting.");

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

int plc5_encode_address(uint8_t *data, int *size, int buf_size, pccc_addr_t *address) {
    uint8_t level_byte = 0;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Starting.");

    if(!data || !size || !address) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Called with null data, or name or zero sized data!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* zero out the stored size. */
    *size = 0;

    /* check for space. */
    if(buf_size < (1 + 3 + 3 + 3)) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Encoded PCCC logical address buffer is too small!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* allocate space for the level byte */
    *size = 1;

    /* do the required levels.  Remember we start at the low bit! */
    level_byte = 0x06; /* level one and two */

    /* add in the data file number. */
    if(encode_data(data, size, buf_size, address->file) != PLCTAG_STATUS_OK) { return PLCTAG_ERR_TOO_SMALL; }

    /* add in the element number */
    if(encode_data(data, size, buf_size, address->element) != PLCTAG_STATUS_OK) { return PLCTAG_ERR_TOO_SMALL; }

    /* check to see if we need to put in a subelement. */
    if(address->sub_element >= 0) {
        level_byte |= 0x08;

        if(encode_data(data, size, buf_size, address->sub_element) != PLCTAG_STATUS_OK) { return PLCTAG_ERR_TOO_SMALL; }
    }

    /* store the encoded levels. */
    data[0] = level_byte;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "PLC/5 encoded address:");
    pdebug_dump_bytes(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, data, *size);

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
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

int slc_encode_address(uint8_t *data, int *size, int buf_size, pccc_addr_t *address) {
    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Starting.");

    if(!data || !size) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Called with null data, or name or zero sized data!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* check for space. */
    if(buf_size < (3 + 1 + 3 + 3)) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Encoded SLC logical address buffer is too small!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* zero out the size */
    *size = 0;

    if(address->file_type == 0) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "SLC file type %d cannot be decoded!", address->file_type);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* encode the file number */
    if(encode_data(data, size, buf_size, address->file) != PLCTAG_STATUS_OK) { return PLCTAG_ERR_TOO_SMALL; }

    /* encode the data file type. */
    if(encode_data(data, size, buf_size, (int)address->file_type) != PLCTAG_STATUS_OK) { return PLCTAG_ERR_TOO_SMALL; }

    /* add in the element number */
    if(encode_data(data, size, buf_size, address->element) != PLCTAG_STATUS_OK) { return PLCTAG_ERR_TOO_SMALL; }

    /* add in the sub-element number */
    if(encode_data(data, size, buf_size, (address->sub_element < 0 ? 0 : address->sub_element)) != PLCTAG_STATUS_OK) {
        return PLCTAG_ERR_TOO_SMALL;
    }

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "SLC/Micrologix encoded address:");
    pdebug_dump_bytes(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, data, *size);

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * The library status for a PCCC error.  Only the extended codes name a cause precisely enough to
 * report as more than a remote error; a data file the PLC does not have is one of them.
 */
int pccc_status_to_rc(uint8_t *error_ptr, size_t error_size) {
    if(error_size < 4 || error_ptr[0] != 0xF0) { return PLCTAG_ERR_REMOTE_ERR; }

    switch(error_ptr[3]) {
        case 0x04: /* symbol not found */
        case 0x06: /* address doesn't point to something usable, such as a file that does not exist */
            return PLCTAG_ERR_NOT_FOUND;

        case 0x07: /* file is wrong size */
        case 0x0A: /* transaction size plus word address is too large */
            return PLCTAG_ERR_OUT_OF_BOUNDS;

        case 0x0B: /* access denied */
        case 0x0F: /* no upload or download privilege */
            return PLCTAG_ERR_NOT_ALLOWED;

        default: return PLCTAG_ERR_REMOTE_ERR;
    }
}


const char *pccc_decode_error(uint8_t *error_ptr, size_t error_size) {
    uint8_t error = 0;

    /*
     * error_size is how many bytes the response actually has starting at error_ptr.  The
     * PLC picks the status byte and the response length independently, so a hostile PLC
     * can send 0xF0 in a response too short to hold the extended status that follows it.
     * Check before each read rather than trusting the status byte to imply the length.
     */
    if(error_size < 1) { return "PCCC response too short to contain a status byte!"; }

    error = *error_ptr;

    /* extended error? */
    if(error == 0xF0) {
        if(error_size < 4) { return "PCCC response too short to contain an extended status code!"; }

        error = *(error_ptr + 3);
    }

    switch(error) {
        case 1: return "Error converting block address."; break;

        case 2: return "Less levels specified in address than minimum for any address."; break;

        case 3: return "More levels specified in address than system supports"; break;

        case 4: return "Symbol not found."; break;

        case 5: return "Symbol is of improper format."; break;

        case 6: return "Address doesn't point to something usable."; break;

        case 7: return "File is wrong size."; break;

        case 8: return "Cannot complete request, situation has changed since the start of the command."; break;

        case 9: return "File is too large."; break;

        case 0x0A: return "Transaction size plus word address is too large."; break;

        case 0x0B: return "Access denied, improper privilege."; break;

        case 0x0C: return "Condition cannot be generated - resource is not available (some has upload active)"; break;

        case 0x0D: return "Condition already exists - resource is already available."; break;

        case 0x0E: return "Command could not be executed PCCC decode error."; break;

        case 0x0F: return "Requester does not have upload or download access - no privilege."; break;

        case 0x10: return "Illegal command or format."; break;

        case 0x20: return "Host has a problem and will not communicate."; break;

        case 0x30: return "Remote node host is missing, disconnected, or shut down."; break;

        case 0x40: return "Host could not complete function due to hardware fault."; break;

        case 0x50: return "Addressing problem or memory protect rungs."; break;

        case 0x60: return "Function not allowed due to command protection selection."; break;

        case 0x70: return "Processor is in Program mode."; break;

        case 0x80: return "Compatibility mode file missing or communication zone problem."; break;

        case 0x90: return "Remote node cannot buffer command."; break;

        case 0xA0: return "Wait ACK (1775-KA buffer full)."; break;

        case 0xB0: return "Remote node problem due to download."; break;

        case 0xC0:
            return "Wait ACK (1775-KA buffer full)."; /* why is this duplicate? */
            break;

        default: return "Unknown error response."; break;
    }


    return "Unknown error response.";
}


uint8_t *pccc_decode_dt_byte(uint8_t *data, int data_size, int *pccc_res_type, int *pccc_res_length) {
    uint8_t *data_start = data;
    uint32_t d_type;
    uint32_t d_size;

    /* check the data size.  If it is too small, then
     * we probably have an empty result.  The smallest valid
     * size is probably more than two bytes.  One for the DT
     * byte and another for the data itself.
     */
    if(data_size < 2) {
        *pccc_res_type = 0;
        *pccc_res_length = 0;
        return NULL;
    }

    /* get the type and data size parts */
    d_type = (((uint32_t)(*data) & (uint32_t)0xF0) >> (uint32_t)4);
    d_size = (*data) & 0x0F;

    /*
     * check the type.  If it is too large to hold in
     * the bottom three bits of the nybble, then the
     * top bit will be set and the bottom three will
     * hold the number of bytes that follows for the
     * type value.  We stop after 4 bytes.  Hopefully
     * that works.
     *
     * The extension bytes are little-endian, the same order encode_data()
     * uses for the equivalent escape in a logical address.
     */

    if(d_type & 0x08) {
        int size_bytes = d_type & 0x07;
        int shift = 0;

        /* the extension bytes must actually be present in the data we were given. */
        if(size_bytes > 4 || (int)(data - data_start) + size_bytes >= data_size) { return NULL; }

        d_type = 0;

        while(size_bytes--) {
            data++; /* we leave the pointer at the last read byte */
            d_type |= ((uint32_t)(*data)) << shift;
            shift += 8;
        }
    }

    /* same drill for the size */
    if(d_size & 0x08) {
        int size_bytes = d_size & 0x07;
        int shift = 0;

        /* the extension bytes must actually be present in the data we were given. */
        if(size_bytes > 4 || (int)(data - data_start) + size_bytes >= data_size) { return NULL; }

        d_size = 0;

        while(size_bytes--) {
            data++; /* we leave the pointer at the last read byte */
            d_size |= ((uint32_t)(*data)) << shift;
            shift += 8;
        }
    }

    *pccc_res_type = (int)d_type;
    *pccc_res_length = (int)d_size;

    /* point past the last byte read */
    data++;

    return data;
}


/*
 * Helper functions
 */


/*
 * FIXME TODO - refactor this into a table-driven function.
 */

int parse_pccc_file_type(const char **str, pccc_addr_t *address) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, 0, "Starting.");

    switch((*str)[0]) {
        case 'A':
        case 'a': /* ASCII */
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found ASCII file.");
            address->file_type = PCCC_FILE_ASCII;
            address->element_size_bytes = 1;
            (*str)++;
            break;

        case 'B':
        case 'b': /* Bit or block transfer */
            if(isdigit((*str)[1])) {
                /* Bit */
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found Bit file.");
                address->file_type = PCCC_FILE_BIT;
                address->element_size_bytes = 2;
                (*str)++;
                break;
            } else {
                if((*str)[1] == 'T' || (*str)[1] == 't') {
                    /* block transfer */
                    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found Block Transfer file.");
                    address->file_type = PCCC_FILE_BLOCK_TRANSFER;
                    address->element_size_bytes = 12;
                    (*str) += 2;
                } else {
                    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Unknown file %s found!", *str);
                    address->file_type = PCCC_FILE_UNKNOWN;
                    address->element_size_bytes = 0;
                    rc = PLCTAG_ERR_BAD_PARAM;
                }
            }

            break;

        case 'C':
        case 'c': /* Counter */
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found Counter file.");
            address->file_type = PCCC_FILE_COUNTER;
            address->element_size_bytes = 6;
            (*str)++;
            break;

        case 'D':
        case 'd': /* BCD number */
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found BCD file.");
            address->file_type = PCCC_FILE_BCD;
            address->element_size_bytes = 2;
            (*str)++;
            break;

        case 'F':
        case 'f': /* Floating point Number */
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found Float/REAL file.");
            address->file_type = PCCC_FILE_FLOAT;
            address->element_size_bytes = 4;
            (*str)++;
            break;

        case 'I':
        case 'i': /* Input */
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found Input file.");
            address->file_type = PCCC_FILE_INPUT;
            address->file = 1; /* in case it is omitted */
            address->element_size_bytes = 2;
            (*str)++;
            break;

        case 'L':
        case 'l':
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found Long Int file.");
            address->file_type = PCCC_FILE_LONG_INT;
            address->element_size_bytes = 4;
            (*str)++;
            break;

        case 'M':
        case 'm': /* Message */
            if((*str)[1] == 'G' || (*str)[1] == 'g') {
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found Message file.");
                address->file_type = PCCC_FILE_MESSAGE;
                address->element_size_bytes = 112;
                (*str) += 2; /* skip past both characters */
            } else {
                address->file_type = PCCC_FILE_UNKNOWN;
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Unknown file %s found!", *str);
                rc = PLCTAG_ERR_BAD_PARAM;
            }
            break;

        case 'N':
        case 'n': /* INT */
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found Integer file.");
            address->file_type = PCCC_FILE_INT;
            address->element_size_bytes = 2;
            (*str)++;
            break;

        case 'O':
        case 'o': /* Output */
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found Output file.");
            address->file_type = PCCC_FILE_OUTPUT;
            address->element_size_bytes = 2;
            address->file = 0; /* in case it is omitted */
            (*str)++;
            break;

        case 'P':
        case 'p': /* PID */
            if((*str)[1] == 'D' || (*str)[1] == 'd') {
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found PID file.");
                address->file_type = PCCC_FILE_PID;
                address->element_size_bytes = 164;
                (*str) += 2; /* skip past both characters */
            } else {
                address->file_type = PCCC_FILE_UNKNOWN;
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Unknown file %s found!", *str);
                rc = PLCTAG_ERR_BAD_PARAM;
            }
            break;

        case 'R':
        case 'r': /* Control */
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found Control file.");
            address->file_type = PCCC_FILE_CONTROL;
            address->element_size_bytes = 6;
            (*str)++;
            break;

        case 'S':
        case 's': /* Status, SFC or String */
            if(isdigit((*str)[1])) {
                /* Status */
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found Status file.");
                address->file_type = PCCC_FILE_STATUS;
                address->element_size_bytes = 2;
                (*str)++;
                break;
            } else {
                if((*str)[1] == 'C' || (*str)[1] == 'c') {
                    /* SFC */
                    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found SFC file.");
                    address->file_type = PCCC_FILE_SFC;
                    address->element_size_bytes = 6;
                    (*str) += 2; /* skip past both characters */
                } else if((*str)[1] == 'T' || (*str)[1] == 't') {
                    /* String */
                    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found String file.");
                    address->file_type = PCCC_FILE_STRING;
                    address->element_size_bytes = 84;
                    (*str) += 2; /* skip past both characters */
                } else {
                    address->file_type = PCCC_FILE_UNKNOWN;
                    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Unknown file %s found!", *str);
                    rc = PLCTAG_ERR_BAD_PARAM;
                }
            }
            break;

        case 'T':
        case 't': /* Timer */
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found Timer file.");
            address->file_type = PCCC_FILE_TIMER;
            address->element_size_bytes = 6;
            (*str)++;
            break;

        default:
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Bad format or unsupported logical address %s!", *str);
            address->file_type = PCCC_FILE_UNKNOWN;
            address->element_size_bytes = 0;
            rc = PLCTAG_ERR_BAD_PARAM;
            break;
    }

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


int parse_pccc_file_num(const char **str, pccc_addr_t *address) {
    int tmp = 0;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Starting.");

    if(!str || !*str) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Expected data-table file number!");
        address->file = -1;
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* if this is I or O, then we can skip the data file number. */
    if((address->file_type == PCCC_FILE_INPUT || address->file_type == PCCC_FILE_OUTPUT) && !isdigit(**str)) {
        /* skip the data file number */
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Data file number omitted for I or O data file.");
        return PLCTAG_STATUS_OK;
    }

    /* FIXME - why are we not using strtol here? We should also support octal. */
    while(**str && isdigit(**str) && tmp < 65535) {
        tmp *= 10;
        tmp += (int)((**str) - '0');
        (*str)++;
    }

    address->file = tmp;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


int parse_pccc_elem_num(const char **str, pccc_addr_t *address) {
    int tmp = 0;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Starting.");

    if(!str || !*str || **str != ':') {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Expected data-table element number!");
        address->element = -1;
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* step past the : character */
    (*str)++;

    while(**str && isdigit(**str) && tmp < 65535) {
        tmp *= 10;
        tmp += (int)((**str) - '0');
        (*str)++;
    }

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found element %d.", tmp);

    address->element = tmp;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


int parse_pccc_subelem(const char **str, pccc_addr_t *address) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Starting.");

    if(!str || !*str) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Called with bad string pointer!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * if we have a null character we are at the end of the name
     * and the subelement is not there.  That is not an error.
     */

    if((**str) == 0) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "No subelement in this name.");
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
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "No subelement in this logical address.");
        address->sub_element = -1;
        return PLCTAG_STATUS_OK;
    }

    /* make sure the next character is . and nothing else. */
    if((**str) != '.') {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Bad subelement field in logical address.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* step past the . character */
    (*str)++;

    /* try a number. */
    rc = parse_pccc_subelem_num(str, address);
    if(rc == PLCTAG_STATUS_OK) {
        /* we found a numeric sub-element */
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found numeric sub-element %d.", address->sub_element);
        return rc;
    }

    if(rc == PLCTAG_ERR_NO_MATCH) {
        /* not a numeric sub-element, try matching a mnemonic. */
        return parse_pccc_subelem_mnemonic(str, address);
    }

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Done.");

    return rc;
}


int parse_pccc_subelem_num(const char **str, pccc_addr_t *address) {
    int tmp = 0;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Starting.");

    /* is it a numeric sub-element? */
    if(!isdigit(**str)) {
        /* nope, it is not. */
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Not a numeric sub-element.");
        return PLCTAG_ERR_NO_MATCH;
    }

    while(**str && isdigit(**str) && tmp < 65535) {
        tmp *= 10;
        tmp += (int)((**str) - '0');
        (*str)++;
    }

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Found sub-element %d.", tmp);

    address->sub_element = tmp;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}

static const struct {
    pccc_file_t file_type;
    const char *field_name;
    int element_size_bytes;
    int sub_element;
    uint8_t is_bit;
    uint8_t bit;
} sub_element_lookup[] = {
    /* file type                    field   size    subelem is_bit  bit_num */

    /* BT block transfer */
    {PCCC_FILE_BLOCK_TRANSFER, "con", 2, 0, 0, 0},
    {PCCC_FILE_BLOCK_TRANSFER, "rlen", 2, 1, 0, 0},
    {PCCC_FILE_BLOCK_TRANSFER, "dlen", 2, 2, 0, 0},
    {PCCC_FILE_BLOCK_TRANSFER, "df", 2, 3, 0, 0},
    {PCCC_FILE_BLOCK_TRANSFER, "elem", 2, 4, 0, 0},
    {PCCC_FILE_BLOCK_TRANSFER, "rgs", 2, 5, 0, 0},

    /* R Control */
    {PCCC_FILE_CONTROL, "con", 2, 0, 0, 0},
    {PCCC_FILE_CONTROL, "len", 2, 1, 0, 0},
    {PCCC_FILE_CONTROL, "pos", 2, 2, 0, 0},

    /* C Counter */
    {PCCC_FILE_COUNTER, "con", 2, 0, 0, 0},
    {PCCC_FILE_COUNTER, "cu", 2, 0, 1, 15},
    {PCCC_FILE_COUNTER, "cd", 2, 0, 1, 14},
    {PCCC_FILE_COUNTER, "dn", 2, 0, 1, 13},
    {PCCC_FILE_COUNTER, "ov", 2, 0, 1, 12},
    {PCCC_FILE_COUNTER, "un", 2, 0, 1, 11},
    {PCCC_FILE_COUNTER, "pre", 2, 1, 0, 0},
    {PCCC_FILE_COUNTER, "acc", 2, 2, 0, 0},

    /* MG Message */
    {PCCC_FILE_MESSAGE, "con", 2, 0, 0, 0},
    {PCCC_FILE_MESSAGE, "nr", 2, 0, 1, 9},
    {PCCC_FILE_MESSAGE, "to", 2, 0, 1, 8},
    {PCCC_FILE_MESSAGE, "en", 2, 0, 1, 7},
    {PCCC_FILE_MESSAGE, "st", 2, 0, 1, 6},
    {PCCC_FILE_MESSAGE, "dn", 2, 0, 1, 5},
    {PCCC_FILE_MESSAGE, "er", 2, 0, 1, 4},
    {PCCC_FILE_MESSAGE, "co", 2, 0, 1, 3},
    {PCCC_FILE_MESSAGE, "ew", 2, 0, 1, 2},
    {PCCC_FILE_MESSAGE, "err", 2, 1, 0, 0},
    {PCCC_FILE_MESSAGE, "rlen", 2, 2, 0, 0},
    {PCCC_FILE_MESSAGE, "dlen", 2, 3, 0, 0},
    {PCCC_FILE_MESSAGE, "data", 104, 4, 0, 0},

    /* PID */
    /* PD first control word */
    {PCCC_FILE_PID, "con", 2, 0, 0, 0},
    {PCCC_FILE_PID, "en", 2, 0, 1, 15},
    {PCCC_FILE_PID, "ct", 2, 0, 1, 9},
    {PCCC_FILE_PID, "cl", 2, 0, 1, 8},
    {PCCC_FILE_PID, "pvt", 2, 0, 1, 7},
    {PCCC_FILE_PID, "do", 2, 0, 1, 6},
    {PCCC_FILE_PID, "swm", 2, 0, 1, 4},
    {PCCC_FILE_PID, "do", 2, 0, 1, 2},
    {PCCC_FILE_PID, "mo", 2, 0, 1, 1},
    {PCCC_FILE_PID, "pe", 2, 0, 1, 0},

    /* second control word */
    {PCCC_FILE_PID, "ini", 2, 1, 1, 12},
    {PCCC_FILE_PID, "spor", 2, 1, 1, 11},
    {PCCC_FILE_PID, "oll", 2, 1, 1, 10},
    {PCCC_FILE_PID, "olh", 2, 1, 1, 9},
    {PCCC_FILE_PID, "ewd", 2, 1, 1, 8},
    {PCCC_FILE_PID, "dvna", 2, 1, 1, 3},
    {PCCC_FILE_PID, "dvpa", 2, 1, 1, 2},
    {PCCC_FILE_PID, "pvla", 2, 1, 1, 1},
    {PCCC_FILE_PID, "pvha", 2, 1, 1, 0},

    /* main PID vars */
    {PCCC_FILE_PID, "sp", 4, 2, 0, 0},
    {PCCC_FILE_PID, "kp", 4, 4, 0, 0},
    {PCCC_FILE_PID, "ki", 4, 6, 0, 0},
    {PCCC_FILE_PID, "kd", 4, 8, 0, 0},

    {PCCC_FILE_PID, "bias", 4, 10, 0, 0},
    {PCCC_FILE_PID, "maxs", 4, 12, 0, 0},
    {PCCC_FILE_PID, "mins", 4, 14, 0, 0},
    {PCCC_FILE_PID, "db", 4, 16, 0, 0},
    {PCCC_FILE_PID, "so", 4, 18, 0, 0},
    {PCCC_FILE_PID, "maxo", 4, 20, 0, 0},
    {PCCC_FILE_PID, "mino", 4, 22, 0, 0},
    {PCCC_FILE_PID, "upd", 4, 24, 0, 0},

    {PCCC_FILE_PID, "pv", 4, 26, 0, 0},

    {PCCC_FILE_PID, "err", 4, 28, 0, 0},
    {PCCC_FILE_PID, "out", 4, 30, 0, 0},
    {PCCC_FILE_PID, "pvh", 4, 32, 0, 0},
    {PCCC_FILE_PID, "pvl", 4, 34, 0, 0},
    {PCCC_FILE_PID, "dvp", 4, 36, 0, 0},
    {PCCC_FILE_PID, "dvn", 4, 38, 0, 0},

    {PCCC_FILE_PID, "pvdb", 4, 40, 0, 0},
    {PCCC_FILE_PID, "dvdb", 4, 42, 0, 0},
    {PCCC_FILE_PID, "maxi", 4, 44, 0, 0},
    {PCCC_FILE_PID, "mini", 4, 46, 0, 0},
    {PCCC_FILE_PID, "tie", 4, 48, 0, 0},

    {PCCC_FILE_PID, "addr", 8, 48, 0, 0},

    {PCCC_FILE_PID, "data", 56, 52, 0, 0},

    /* ST String */
    {PCCC_FILE_STRING, "len", 2, 0, 0, 0},
    {PCCC_FILE_STRING, "data", 82, 1, 0, 0},

    /* SC SFC */
    {PCCC_FILE_SFC, "con", 2, 0, 0, 0},
    {PCCC_FILE_SFC, "sa", 2, 0, 1, 15},
    {PCCC_FILE_SFC, "fs", 2, 0, 1, 14},
    {PCCC_FILE_SFC, "ls", 2, 0, 1, 13},
    {PCCC_FILE_SFC, "ov", 2, 0, 1, 12},
    {PCCC_FILE_SFC, "er", 2, 0, 1, 11},
    {PCCC_FILE_SFC, "dn", 2, 0, 1, 10},
    {PCCC_FILE_SFC, "pre", 2, 1, 0, 0},
    {PCCC_FILE_SFC, "tim", 2, 2, 0, 0},

    /* T timer */
    {PCCC_FILE_TIMER, "con", 2, 0, 0, 0},
    {PCCC_FILE_TIMER, "en", 2, 0, 1, 15},
    {PCCC_FILE_TIMER, "tt", 2, 0, 1, 14},
    {PCCC_FILE_TIMER, "dn", 2, 0, 1, 13},
    {PCCC_FILE_TIMER, "pre", 2, 1, 0, 0},
    {PCCC_FILE_TIMER, "acc", 2, 2, 0, 0}};


int parse_pccc_subelem_mnemonic(const char **str, pccc_addr_t *address) {
    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Starting.");

    if(!str || !*str) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Called with bad string pointer!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * if we have a null character we are at the end of the name
     * and the subelement is not there.  That is not an error.
     */

    if((**str) == 0) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "No subelement in this name.");
        address->sub_element = -1;
        return PLCTAG_STATUS_OK;
    }

    /* parse_pccc_subelem() has already stepped past the '.'; *str is at the mnemonic (#688). */
    /* search for a match. */
    for(size_t i = 0; i < (sizeof(sub_element_lookup) / sizeof(sub_element_lookup[0])); i++) {
        if(sub_element_lookup[i].file_type == address->file_type
           && str_cmp_i_n(*str, sub_element_lookup[i].field_name, str_length(sub_element_lookup[i].field_name)) == 0) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Matched file type %x and field mnemonic \"%.*s\".", address->file_type,
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

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Unsupported field mnemonic %s for type %x!", *str, address->file_type);

    return PLCTAG_ERR_BAD_PARAM;
}


int parse_pccc_bit_num(const char **str, pccc_addr_t *address) {
    int tmp = 0;
    int max_bit = 0;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Starting.");

    if(!str || !*str) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Called with bad string pointer!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * if we have a null character we are at the end of the name
     * and the subelement is not there.  That is not an error.
     */

    if((**str) == 0) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "No bit number in this name.");
        return PLCTAG_STATUS_OK;
    }

    /* make sure the next character is /. */
    if((**str) != '/') {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Bad bit number in logical address.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* make sure that the data type is B, N, L or S. */
    switch(address->file_type) {
        case PCCC_FILE_BIT: max_bit = 15; break;
        case PCCC_FILE_INT: max_bit = 15; break;
        case PCCC_FILE_LONG_INT: max_bit = 31; break;
        case PCCC_FILE_STATUS: max_bit = 16; break;
        default:
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Unsupported file type %x!", address->file);
            return PLCTAG_ERR_BAD_PARAM;
    }

    /* step past the / character */
    (*str)++;

    /* FIXME - we do this a lot, should be a small routine. */
    while(**str && isdigit(**str) && tmp < 65535) {
        tmp *= 10;
        tmp += (int)((**str) - '0');
        (*str)++;
    }

    if(tmp < 0 || tmp > max_bit) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Error processing bit number.  Must be between 0 and %d inclusive, found %d!",
               max_bit, tmp);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    address->is_bit = (uint8_t)1;
    address->bit = (uint8_t)tmp;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Done.");

    return PLCTAG_STATUS_OK;
}


/*
 * Append one PCCC logical-address datum at *index: one byte for values that fit, or a 0xFF
 * escape plus a little-endian 16-bit value for the rest.  buf_size is the capacity of data[]
 * so the write is bounded here rather than relying on the caller's up-front size check to have
 * accounted for every datum -- the same shape of bug as the ab_server stack write (Risk 7).
 */
int encode_data(uint8_t *data, int *index, int buf_size, int val) {
    int needed = (val <= 254) ? 1 : 3;

    if(*index < 0 || needed > buf_size - *index) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "Encoded PCCC address needs %d more bytes than the %d byte buffer holds!",
               needed, buf_size);
        return PLCTAG_ERR_TOO_SMALL;
    }

    if(val <= 254) {
        data[*index] = (uint8_t)val;
        *index = *index + 1;
    } else {
        data[*index] = (uint8_t)0xff;
        data[*index + 1] = (uint8_t)(val & 0xff);
        data[*index + 2] = (uint8_t)((val >> 8) & 0xff);
        *index = *index + 3;
    }

    return PLCTAG_STATUS_OK;
}


//===============================================================
// PCCC Tag Functions
//===============================================================


/*
 * pccc_check_response_header
 *
 * Every PCCC response handler casts the request buffer to a chain of headers and
 * then reads fields out of it.  The generic check in check_request_status() only
 * knows about the CIP response headers, so validate the PCCC-specific chain here,
 * in one place, before any handler dereferences it.
 *
 * Two things are checked: that the buffer is long enough for the headers the handler
 * is about to walk, and that the response is an answer to the request we sent.  The
 * PCCC layer identifies a request by its TNS, and DH+ additionally by the node the
 * reply came from -- a DH+ gateway multiplexes several PLCs over one CIP connection,
 * so without the node check any of them can answer for any other.
 *
 * DH+ responses arrive behind a connected CPF header, everything else behind an
 * unconnected one.
 */
int pccc_check_response_header(ab_tag_p tag, uint8_t **pccc_reply) {
    int min_size = 0;
    pccc_cmd_resp *pccc_cmd = NULL;
    bool is_dhp = false;

    if(!tag || !tag->req) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, (tag ? tag->tag_id : 0), "Called without a request in flight!");
        return PLCTAG_ERR_NULL_PTR;
    }

    is_dhp = (tag->session->is_dhp != 0);

    if(is_dhp) {
        min_size = (int)sizeof(pccc_dhp_cmd_resp);
    } else {
        min_size = (int)((int)(pccc_reply_prefix_size(tag) + sizeof(pccc_cmd_resp)));
    }

    if(tag->req->request_size < min_size) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
               "Response of %d bytes is too short to hold a %s response header of %d bytes!", tag->req->request_size,
               (is_dhp ? "DH+ PCCC" : "PCCC"), min_size);
        return PLCTAG_ERR_TOO_SMALL;
    }

    if(is_dhp) {
        pccc_dhp_cmd_resp *dhp_resp = (pccc_dhp_cmd_resp *)(tag->req->data);

        /* we sent this to dhp_dest from node zero, so the answer has to come back the other way. */
        if(!tag->session) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Called without a session!");
            return PLCTAG_ERR_NULL_PTR;
        }

        if(le2h16(dhp_resp->src_node) != tag->session->dhp_dest || le2h16(dhp_resp->dest_node) != 0) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                   "DH+ response is from node %u to node %u but we sent to node %u from node 0!",
                   (unsigned int)le2h16(dhp_resp->src_node), (unsigned int)le2h16(dhp_resp->dest_node),
                   (unsigned int)tag->session->dhp_dest);
            return PLCTAG_ERR_BAD_DATA;
        }

        /* the DH+ command response has no CIP PCCC header, so the TNS is all that is left. */
        pccc_cmd = (pccc_cmd_resp *)(&dhp_resp->pccc_command);
    } else {
        cip_pccc_resp *cip_pccc = (cip_pccc_resp *)(tag->req->data);

        /*
         * The reply code is the PCCC-layer echo of the service we asked for.  Everything after
         * it shifts if the target answered a different service, so check it before the fields
         * that follow are read.
         */
        if(cip_pccc->reply_code != (AB_EIP_CMD_PCCC_EXECUTE | AB_EIP_CMD_CIP_OK)) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Unexpected PCCC reply code %02x, expected %02x!",
                   (unsigned int)cip_pccc->reply_code, (unsigned int)(AB_EIP_CMD_PCCC_EXECUTE | AB_EIP_CMD_CIP_OK));
            return PLCTAG_ERR_BAD_DATA;
        }

        /*
         * The requester ID is echoed verbatim from our request.  It is how a PCCC target tells
         * one requester from another, so a reply carrying somebody else's is not ours.
         */
        if(cip_pccc->request_id_size != 7 || le2h16(cip_pccc->vendor_id) != AB_EIP_VENDOR_ID
           || le2h32(cip_pccc->vendor_serial_number) != AB_EIP_VENDOR_SN) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "PCCC response requester ID (%u, %04x, %08x) is not ours!",
                   (unsigned int)cip_pccc->request_id_size, (unsigned int)le2h16(cip_pccc->vendor_id),
                   (unsigned int)le2h32(cip_pccc->vendor_serial_number));
            return PLCTAG_ERR_BAD_DATA;
        }

        if(cip_pccc->general_status != AB_EIP_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "PCCC command failed, response code: (%d) %s",
                   cip_pccc->general_status,
                   decode_cip_error_short(&cip_pccc->general_status,
                                          cip_error_data_size(&cip_pccc->general_status, tag->req->data + tag->req->request_size)));
            return PLCTAG_ERR_REMOTE_ERR;
        }

        pccc_cmd = (pccc_cmd_resp *)(cip_pccc + 1);
    }

    if(le2h16(pccc_cmd->pccc_seq_num) != tag->req_pccc_seq_num) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Response has TNS %04x but we sent %04x!",
               (unsigned int)le2h16(pccc_cmd->pccc_seq_num), (unsigned int)tag->req_pccc_seq_num);
        return PLCTAG_ERR_BAD_DATA;
    }

    if(pccc_reply) { *pccc_reply = (uint8_t *)pccc_cmd; }

    return PLCTAG_STATUS_OK;
}


/*
 * Write what goes in front of the PCCC command: Execute PCCC (0x4B) with our requester ID
 * through CIP, or the DH+ routing words through a DH+ bridge.  Returns where the command starts.
 */
uint8_t *pccc_write_prefix(ab_tag_p tag, uint8_t *buf) {
    if(tag->session->is_dhp) {
        pccc_dhp_routing_header *dhp = (pccc_dhp_routing_header *)buf;

        dhp->dest_link = h2le16(0);
        dhp->dest_node = h2le16(tag->session->dhp_dest);
        dhp->src_link = h2le16(0);
        dhp->src_node = h2le16(0);

        return (uint8_t *)(dhp + 1);
    } else {
        cip_pccc_req *cip_pccc = (cip_pccc_req *)buf;

        cip_pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE;
        cip_pccc->req_path_size = 2;
        cip_pccc->req_path[0] = 0x20;
        cip_pccc->req_path[1] = 0x67;
        cip_pccc->req_path[2] = 0x24;
        cip_pccc->req_path[3] = 0x01;
        cip_pccc->request_id_size = 7;
        cip_pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);
        cip_pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN);

        return (uint8_t *)(cip_pccc + 1);
    }
}


size_t pccc_prefix_size(ab_tag_p tag) {
    return tag->session->is_dhp ? sizeof(pccc_dhp_routing_header) : sizeof(cip_pccc_req);
}


size_t pccc_reply_prefix_size(ab_tag_p tag) {
    return tag->session->is_dhp ? sizeof(pccc_dhp_routing_header) : sizeof(cip_pccc_resp);
}


/*
 * tag_status
 *
 * get the tag status.
 */
int pccc_tag_status(plc_tag_p tag_arg) {
    ab_tag_p tag = (ab_tag_p)tag_arg;
    if(!tag->session) {
        /* this is not OK.  This is fatal! */
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, 0, "returning PLCTAG_ERR_CREATE (no session)");
        return PLCTAG_ERR_CREATE;
    }

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, 0, "read_in_progress=1, returning PENDING");
        return PLCTAG_STATUS_PENDING;
    }

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, 0, "write_in_progress=1, returning PENDING");
        return PLCTAG_STATUS_PENDING;
    }

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, 0, "returning tag->status=%d (%s)", tag->status, plc_tag_decode_error(tag->status));
    return tag->status;
}


int pccc_tag_tickler(plc_tag_p tag_arg) {
    ab_tag_p tag = (ab_tag_p)tag_arg;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_SPEW, 0, "Starting.");

    rc = check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "check_request_status returned %s.", plc_tag_decode_error(rc));
        return rc;
    }

    if(tag->read_in_progress) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_SPEW, 0, "Read in progress.");
        rc = pccc_check_read_status(tag);
        tag->status = (int8_t)rc;

        /* check to see if the read finished. */
        if(!tag->read_in_progress) {
            /* first read done */
            if(tag->first_read) {
                tag->first_read = 0;
                tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)rc);
            }

            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Read complete with status %s.", plc_tag_decode_error(tag->status));

            tag->read_complete = 1;
        }

        return rc;
    }

    if(tag->write_in_progress) {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Write in progress.");
        rc = pccc_check_write_status(tag);
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "pccc_check_write_status returned %d (%s)", rc, plc_tag_decode_error(rc));
        tag->status = (int8_t)rc;
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "set tag->status=%d, write_in_progress=%d, write_complete=%d", tag->status,
               tag->write_in_progress, tag->write_complete);

        /* check to see if the write finished. */
        if(!tag->write_in_progress) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, 0, "Write complete with status %s.", plc_tag_decode_error(tag->status));

            tag->write_complete = 1;
        }

        return rc;
    }

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_SPEW, 0, "Done.");

    return tag->status;
}

/*
 * tag_read_start
 *
 * Start a PCCC tag read (PLC5).
 */

int pccc_tag_read_start(plc_tag_p tag_arg) {
    ab_tag_p tag = (ab_tag_p)tag_arg;
    int rc = PLCTAG_STATUS_OK;
    uint16_t conn_seq_id = session_get_new_seq_id(tag->session);
    ab_request_p req = NULL;
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    int cip_payload_space = session_get_available_cip_payload_space(tag->session);

    /* remember the TNS so pccc_check_response_header() can match the reply to this request. */
    tag->req_pccc_seq_num = conn_seq_id;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Starting");

    /* pseudo-exception block for error handling */
    do {
        /* check for busy */
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Read or write operation already in flight!");
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        tag->read_in_progress = 1;

        /* calculate response overhead and available payload space */
        int response_overhead = (int)(pccc_reply_prefix_size(tag) + sizeof(pccc_cmd_resp));
        int response_payload_space = cip_payload_space - response_overhead;

        if(response_payload_space < 0) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                   "PLC5 read response overhead (%d bytes) exceeds session payload space (%d bytes).", response_overhead,
                   cip_payload_space);
            tag->read_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        if(response_payload_space < tag->size) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                   "Tag size (%d bytes) exceeds available response data space (%d bytes). PCCC does not support fragmentation.",
                   tag->size, response_payload_space);
            tag->read_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* calculate request overhead */
        int request_overhead =
            (int)pccc_prefix_size(tag)
            + (tag->plc_type == AB_PLC_PLC5 ? (int)sizeof(plc5_pccc_read_cmd_req) : (int)sizeof(slc_pccc_read_cmd_req))
            + tag->encoded_name_size + 1;

        pdebug(
            DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id,
            "PLC5 request overhead: CIP/PCCC header size %zu, PCCC read command header size %zu, encoded_name=%d, data_size=1, total=%d bytes",
            pccc_prefix_size(tag), sizeof(plc5_pccc_read_cmd_req), tag->encoded_name_size, request_overhead);

        int request_payload_space = cip_payload_space - request_overhead;

        if(request_payload_space < 0) {
            pdebug(
                DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id,
                "PLC5 read request overhead (%d bytes) exceeds session payload space (%d bytes). Tag name too long or PCCC packet limit exceeded.",
                request_overhead, cip_payload_space);
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* create the request */
        rc = session_reuse_request(tag->session, ab_tag_take_spare_request(tag), tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Unable to get new request.  rc=%d", rc);
            break;
        }

        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Request created. Request capacity: %d bytes",
               req->request_capacity);

        if(request_overhead > req->request_capacity) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id,
                   "PCCC request overhead (%d bytes) exceeds request capacity (%d bytes).", request_overhead,
                   req->request_capacity);
            rc_dec(req);
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* point the struct pointers to the buffer */
        uint8_t *cmd_start = pccc_write_prefix(tag, req->data);
        embed_start = req->data;

        if(tag->plc_type == AB_PLC_PLC5) {
            plc5_pccc_read_cmd_req *pccc_cmd = (plc5_pccc_read_cmd_req *)cmd_start;

            /* fill in PCCC command fields */
            pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
            pccc_cmd->pccc_status = 0;
            pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
            pccc_cmd->pccc_function = AB_EIP_PLC5_RANGE_READ_FUNC;
            pccc_cmd->pccc_offset = h2le16(0);
            pccc_cmd->pccc_transfer_size = h2le16((uint16_t)(tag->size / 2)); /* size in words */

            /* point data pointer just past the fixed data fields */
            data = (uint8_t *)(pccc_cmd + 1);
        } else {
            slc_pccc_read_cmd_req *pccc_cmd = (slc_pccc_read_cmd_req *)cmd_start;

            /* fill in PCCC command fields */
            pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
            pccc_cmd->pccc_status = 0;
            pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
            pccc_cmd->pccc_function = AB_EIP_SLC_RANGE_READ_FUNC;
            pccc_cmd->pccc_transfer_size = (uint8_t)tag->size; /* size in bytes */

            /* point data pointer just past the fixed data fields */
            data = (uint8_t *)(pccc_cmd + 1);
        }

        /* copy encoded tag name into the request */
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        if(tag->plc_type == AB_PLC_PLC5) {
            /* add data size byte */
            *data = (uint8_t)(tag->size);
            data++;
        }

        /* debug: request full data length */
        ptrdiff_t calculated_request_size = (ptrdiff_t)(data - req->data);
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC request full data length: %td bytes.",
               calculated_request_size);

        /* debug: CIP data length */
        ptrdiff_t cip_request_size = (ptrdiff_t)(data - embed_start);
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC request CIP data length: %td bytes.", cip_request_size);




        /* set request size */
        req->request_size = (int)cip_request_size;
        req->connected = (tag->session->is_dhp != 0); /* DH+ goes over the bridge's connection. */
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC request size set to %d bytes.", req->request_size);

        /* debug: dump request data */
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC request data:");
        pdebug_dump_bytes(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, req->data, (int)calculated_request_size);

        /* add request to session */
        rc = session_add_request(tag->session, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Unable to add request to session! rc=%d", rc);
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* set tag->req if successful, else clean up */
    if(rc == PLCTAG_STATUS_OK) {
        critical_block(&tag->api_mutex) {
            if(tag->req) {
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Request already set! This should not happen!");
                rc = PLCTAG_ERR_BAD_DATA;
            } else {
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Setting request for tag %d", tag->tag_id);
                tag->req = req;
                rc = PLCTAG_STATUS_PENDING;
            }
        }
    } else {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Failed to generate new read request rc=%s",
               plc_tag_decode_error(rc));
        tag->read_in_progress = 0;
        req = rc_dec(req);
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Done.");
    return rc;
}


/*
 * check_read_status
 *
 * NOTE that we can have only one outstanding request because PCCC
 * does not support fragments.
 */
int pccc_check_read_status(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    pccc_cmd_resp *pccc_cmd = NULL;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_SPEW, tag->tag_id, "Starting");

    rc = pccc_check_response_header(tag, (uint8_t **)&pccc_cmd);
    if(rc != PLCTAG_STATUS_OK) {
        ab_tag_abort_request(tag);
        tag->read_in_progress = 0;
        tag->read_complete = 1;
        return rc;
    }

    uint8_t *data = (uint8_t *)(pccc_cmd + 1);
    uint8_t *data_end = tag->req->data + tag->req->request_size;

    /* fake exceptions */
    do {
        if(pccc_cmd->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "PCCC command failed, response code: %d - %s",
                   pccc_cmd->pccc_status,
                   pccc_decode_error(&pccc_cmd->pccc_status, cip_error_data_size(&pccc_cmd->pccc_status, data_end)));
            rc = pccc_status_to_rc(&pccc_cmd->pccc_status, cip_error_data_size(&pccc_cmd->pccc_status, data_end));
            break;
        }

        /* did we get the right amount of data? */
        if((data_end - data) != (ptrdiff_t)tag->size) {
            if((data_end - data) > (ptrdiff_t)tag->size) {
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                       "Too much data received!  Expected %d bytes but got %d bytes!", tag->size, (int)(data_end - data));
                rc = PLCTAG_ERR_TOO_LARGE;
            } else {
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                       "Too little data received!  Expected %d bytes but got %d bytes!", tag->size, (int)(data_end - data));
                rc = PLCTAG_ERR_TOO_SMALL;
            }
            break;
        }

        /* copy data into the tag. */
        mem_copy(tag->data, data, (int)(data_end - data));

        rc = PLCTAG_STATUS_OK;
    } while(0);

    ab_tag_abort_request(tag);

    /* Read is complete whether it succeeded or failed */
    tag->read_in_progress = 0;
    tag->read_complete = 1;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_SPEW, tag->tag_id, "Done with status %s.", plc_tag_decode_error(rc));

    return rc;
}


int pccc_tag_write_start(plc_tag_p tag_arg) {
    ab_tag_p tag = (ab_tag_p)tag_arg;
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;
    uint16_t conn_seq_id = session_get_new_seq_id(tag->session);
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    size_t overhead, data_per_packet;

    /* remember the TNS so pccc_check_response_header() can match the reply to this request. */
    tag->req_pccc_seq_num = conn_seq_id;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Starting.");

    if(tag->is_bit) {
        if(tag->plc_type == AB_PLC_PLC5) {
            return plc5_tag_write_bit_start(tag);
        } else {
            return slc_tag_write_bit_start(tag);
        }
    }

    do {
        /* check for busy */
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Read (%d) or write (%d) operation already in flight!",
                   tag->read_in_progress, tag->write_in_progress);
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        tag->write_in_progress = 1;

        /* How much overhead? */
        overhead = pccc_prefix_size(tag)
                   + (tag->plc_type == AB_PLC_PLC5 ? sizeof(plc5_pccc_write_cmd_req) : sizeof(slc_pccc_write_cmd_req))
                   + (size_t)(unsigned int)tag->encoded_name_size + 1;

        int session_payload_space = session_get_available_cip_payload_space(tag->session);

        if(session_payload_space <= 0) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                   "Unable to get valid payload space from session. Available payload: %d bytes", session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /*
         * The payload space comes from the connection size the PLC negotiated in the
         * ForwardOpen response, so it can be smaller than our overhead.  Compare before
         * subtracting: data_per_packet is unsigned, so an underflow here would wrap to a
         * huge value that passes every check below.
         */
        if((size_t)session_payload_space <= overhead) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                   "Unable to send request.  Packet overhead, %zu bytes, is too large for available payload, %d bytes!", overhead,
                   session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        data_per_packet = (size_t)session_payload_space - overhead;

        if(data_per_packet < (size_t)tag->size) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                   "Tag size is %d, write overhead is %zu, and write data per packet is %zu.", tag->size, overhead,
                   data_per_packet);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* get a request buffer */
        rc = session_reuse_request(tag->session, ab_tag_take_spare_request(tag), tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Unable to get new request.  rc=%d", rc);
            tag->write_in_progress = 0;
            break;
        }

        /* point the struct pointers to the buffer */
        uint8_t *cmd_start = pccc_write_prefix(tag, req->data);
        embed_start = req->data;

        /* fill in PCCC command fields */
        if(tag->plc_type == AB_PLC_PLC5) {
            plc5_pccc_write_cmd_req *pccc_cmd = (plc5_pccc_write_cmd_req *)cmd_start;

            pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
            pccc_cmd->pccc_status = 0;
            pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
            pccc_cmd->pccc_function = AB_EIP_PLC5_RANGE_WRITE_FUNC;
            pccc_cmd->pccc_offset = h2le16(0);
            pccc_cmd->pccc_transfer_size = h2le16((uint16_t)(tag->size / 2));  // size is in bytes, PCCC transfer size is in words

            /* point data pointer just past the fixed data fields */
            data = (uint8_t *)(pccc_cmd + 1);
        } else {
            slc_pccc_write_cmd_req *pccc_cmd = (slc_pccc_write_cmd_req *)cmd_start;

            pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
            pccc_cmd->pccc_status = 0;
            pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
            pccc_cmd->pccc_function = AB_EIP_SLC_RANGE_WRITE_FUNC;
            pccc_cmd->pccc_transfer_size = (uint8_t)tag->size; /* size in bytes */

            /* point data pointer just past the fixed data fields */
            data = (uint8_t *)(pccc_cmd + 1);
        }

        /* copy encoded tag name into the request */
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        /* now copy the data to write */
        mem_copy(data, tag->data, tag->size);
        data += tag->size;

        /* debug: request full data length */
        ptrdiff_t calculated_request_size = (ptrdiff_t)(data - req->data);
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC write request full data length: %td bytes.",
               calculated_request_size);

        /* debug: dump request data */
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC write request data:");
        pdebug_dump_bytes(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, req->data, (int)calculated_request_size);

        /* debug: CIP data length */
        ptrdiff_t cip_request_size = (ptrdiff_t)(data - embed_start);
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC write request CIP data length: %td bytes.",
               cip_request_size);




        /* set request size */
        req->request_size = (int)cip_request_size;
        req->connected = (tag->session->is_dhp != 0); /* DH+ goes over the bridge's connection. */
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC write request size set to %d bytes.", req->request_size);

        /* add request to session */
        rc = session_add_request(tag->session, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Unable to add write request to session! rc=%d", rc);
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* set tag->req if successful, else clean up */
    if(rc == PLCTAG_STATUS_OK) {
        critical_block(&tag->api_mutex) {
            if(tag->req) {
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Request already set! This should not happen!");
                rc = PLCTAG_ERR_BAD_DATA;
            } else {
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Setting write request for tag %d", tag->tag_id);
                tag->req = req;
                rc = PLCTAG_STATUS_PENDING;
            }
        }
    } else {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Failed to generate new write request rc=%s",
               plc_tag_decode_error(rc));
        req = rc_dec(req);
        tag->write_in_progress = 0;
        tag->write_complete = 1;
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Done.");
    return rc;
}

int plc5_tag_write_bit_start(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;
    uint16_t conn_seq_id = session_get_new_seq_id(tag->session);
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    size_t overhead, data_per_packet;

    /* remember the TNS so pccc_check_response_header() can match the reply to this request. */
    tag->req_pccc_seq_num = conn_seq_id;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Starting.");

    do {
        /* check for busy */
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Read (%d) or write (%d) operation already in flight!",
                   tag->read_in_progress, tag->write_in_progress);
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        tag->write_in_progress = 1;

        /* How much overhead? */
        overhead = pccc_prefix_size(tag) + sizeof(plc5_pccc_write_cmd_req) + (size_t)(unsigned int)tag->encoded_name_size + 1;

        int session_payload_space = session_get_available_cip_payload_space(tag->session);

        if(session_payload_space <= 0) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                   "Unable to get valid payload space from session. Available payload: %d bytes", session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /*
         * The payload space comes from the connection size the PLC negotiated in the
         * ForwardOpen response, so it can be smaller than our overhead.  Compare before
         * subtracting: data_per_packet is unsigned, so an underflow here would wrap to a
         * huge value that passes every check below.
         */
        if((size_t)session_payload_space <= overhead) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                   "Unable to send request.  Packet overhead, %zu bytes, is too large for available payload, %d bytes!", overhead,
                   session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        data_per_packet = (size_t)session_payload_space - overhead;

        if(data_per_packet < (size_t)tag->size) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                   "Tag size is %d, write overhead is %zu, and write data per packet is %zu.", tag->size, overhead,
                   data_per_packet);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* get a request buffer */
        rc = session_reuse_request(tag->session, ab_tag_take_spare_request(tag), tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Unable to get new request.  rc=%d", rc);
            tag->write_in_progress = 0;
            break;
        }

        /* point the struct pointers to the buffer */
        uint8_t *cmd_start = pccc_write_prefix(tag, req->data);
        embed_start = req->data;
        pccc_rmw_cmd_req *pccc_cmd = (pccc_rmw_cmd_req *)cmd_start;

        /* fill in PCCC command fields */
        pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        pccc_cmd->pccc_status = 0;
        pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
        pccc_cmd->pccc_function = AB_EIP_PLC5_RMW_FUNC;

        /* point data pointer just past the fixed data fields */
        data = (uint8_t *)(pccc_cmd + 1);

        /* copy encoded tag name into the request */
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        /* now make the mask data */
        /* AND/reset mask */
        for(int i = 0; i < tag->elem_size; i++) {
            if((tag->bit / 8) == i) {
                uint8_t mask = (uint8_t)(1 << (tag->bit % 8));
                if(tag->data[i] & mask) {
                    *data = (uint8_t)0xFF;
                } else {
                    *data = (uint8_t)~mask;
                }
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "adding reset mask byte %d: %x", i, *data);
                data++;
            } else {
                *data = (uint8_t)0xFF;
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "adding reset mask byte %d: %x", i, *data);
                data++;
            }
        }
        /* OR/set mask */
        for(int i = 0; i < tag->elem_size; i++) {
            if((tag->bit / 8) == i) {
                *data = tag->data[i] & (uint8_t)(1 << (tag->bit % 8));
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "adding set mask byte %d: %x", i, *data);
                data++;
            } else {
                *data = (uint8_t)0x00;
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "adding set mask byte %d: %x", i, *data);
                data++;
            }
        }

        /* debug: request full data length */
        ptrdiff_t calculated_request_size = (ptrdiff_t)(data - req->data);
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC write request full data length: %td bytes.",
               calculated_request_size);

        /* debug: dump request data */
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC write request data:");
        pdebug_dump_bytes(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, req->data, (int)calculated_request_size);

        /* debug: CIP data length */
        ptrdiff_t cip_request_size = (ptrdiff_t)(data - embed_start);
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC write request CIP data length: %td bytes.",
               cip_request_size);




        /* set request size */
        req->request_size = (int)cip_request_size;
        req->connected = (tag->session->is_dhp != 0); /* DH+ goes over the bridge's connection. */
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC write request size set to %d bytes.", req->request_size);

        /* add request to session */
        rc = session_add_request(tag->session, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Unable to add write request to session! rc=%d", rc);
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* set tag->req if successful, else clean up */
    if(rc == PLCTAG_STATUS_OK) {
        critical_block(&tag->api_mutex) {
            if(tag->req) {
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Request already set! This should not happen!");
                rc = PLCTAG_ERR_BAD_DATA;
            } else {
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Setting write request for tag %d", tag->tag_id);
                tag->req = req;
                rc = PLCTAG_STATUS_PENDING;
            }
        }
    } else {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Failed to generate new write request rc=%s",
               plc_tag_decode_error(rc));
        req = rc_dec(req);
        tag->write_in_progress = 0;
        tag->write_complete = 1;
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Done.");
    return rc;
}


int slc_tag_write_bit_start(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;
    uint16_t conn_seq_id = session_get_new_seq_id(tag->session);
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    size_t overhead, data_per_packet;

    /* remember the TNS so pccc_check_response_header() can match the reply to this request. */
    tag->req_pccc_seq_num = conn_seq_id;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Starting.");

    do {
        /* check for busy */
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Read (%d) or write (%d) operation already in flight!",
                   tag->read_in_progress, tag->write_in_progress);
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        /* the mask is only 16 bits. */
        if(tag->size != 2 || tag->elem_size != 2) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Tag (%d bytes) and element size (%d bytes) must be 2 bytes!",
                   tag->size, tag->elem_size);
            rc = PLCTAG_ERR_UNSUPPORTED;
            break;
        }

        tag->write_in_progress = 1;

        /* How much overhead? */
        overhead = pccc_prefix_size(tag) + sizeof(slc_pccc_write_cmd_req) + (size_t)(unsigned int)tag->encoded_name_size + 1;

        int session_payload_space = session_get_available_cip_payload_space(tag->session);

        if(session_payload_space <= 0) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                   "Unable to get valid payload space from session. Available payload: %d bytes", session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /*
         * The payload space comes from the connection size the PLC negotiated in the
         * ForwardOpen response, so it can be smaller than our overhead.  Compare before
         * subtracting: data_per_packet is unsigned, so an underflow here would wrap to a
         * huge value that passes every check below.
         */
        if((size_t)session_payload_space <= overhead) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                   "Unable to send request.  Packet overhead, %zu bytes, is too large for available payload, %d bytes!", overhead,
                   session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        data_per_packet = (size_t)session_payload_space - overhead;

        if(data_per_packet < (size_t)tag->size) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id,
                   "Tag size is %d, write overhead is %zu, and write data per packet is %zu.", tag->size, overhead,
                   data_per_packet);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* get a request buffer */
        rc = session_reuse_request(tag->session, ab_tag_take_spare_request(tag), tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Unable to get new request.  rc=%d", rc);
            tag->write_in_progress = 0;
            break;
        }

        /* point the struct pointers to the buffer */
        uint8_t *cmd_start = pccc_write_prefix(tag, req->data);
        embed_start = req->data;
        pccc_rmw_cmd_req *pccc_cmd = (pccc_rmw_cmd_req *)cmd_start;

        /* fill in PCCC command fields */
        pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        pccc_cmd->pccc_status = 0;
        pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
        pccc_cmd->pccc_function = AB_EIP_SLC_RANGE_WRITE_MASK_FUNC;

        /* point data pointer just past the fixed data fields */
        data = (uint8_t *)(pccc_cmd + 1);

        /* Add the transfer size. */
        *data = (uint8_t)(tag->size & 0xFF);
        data++;

        /* copy encoded tag name into the request */
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        /* The mask has bits set where the data will change the data file. */
        for(int i = 0; i < tag->elem_size; i++) {
            if((tag->bit / 8) == i) {
                *data = (uint8_t)(1 << (tag->bit % 8));
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "adding mask byte %d: %x", i, *data);
                data++;
            } else {
                *data = (uint8_t)0x00;
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "adding mask byte %d: %x", i, *data);
                data++;
            }
        }

        /* the set mask */
        for(int i = 0; i < tag->elem_size; i++) {
            *data = tag->data[i];
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "adding set byte %d: %x", i, *data);
            data++;
        }

        /* debug: request full data length */
        ptrdiff_t calculated_request_size = (ptrdiff_t)(data - req->data);
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC write request full data length: %td bytes.",
               calculated_request_size);

        /* debug: dump request data */
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC write request data:");
        pdebug_dump_bytes(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, req->data, (int)calculated_request_size);

        /* debug: CIP data length */
        ptrdiff_t cip_request_size = (ptrdiff_t)(data - embed_start);
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC write request CIP data length: %td bytes.",
               cip_request_size);




        /* set request size */
        req->request_size = (int)cip_request_size;
        req->connected = (tag->session->is_dhp != 0); /* DH+ goes over the bridge's connection. */
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_DETAIL, tag->tag_id, "PCCC write request size set to %d bytes.", req->request_size);

        /* add request to session */
        rc = session_add_request(tag->session, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Unable to add write request to session! rc=%d", rc);
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* set tag->req if successful, else clean up */
    if(rc == PLCTAG_STATUS_OK) {
        critical_block(&tag->api_mutex) {
            if(tag->req) {
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Request already set! This should not happen!");
                rc = PLCTAG_ERR_BAD_DATA;
            } else {
                pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Setting write request for tag %d", tag->tag_id);
                tag->req = req;
                rc = PLCTAG_STATUS_PENDING;
            }
        }
    } else {
        pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "Failed to generate new write request rc=%s",
               plc_tag_decode_error(rc));
        req = rc_dec(req);
        tag->write_in_progress = 0;
        tag->write_complete = 1;
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_INFO, tag->tag_id, "Done.");
    return rc;
}


/*
 * check_write_status
 *
 * Fragments are not supported.
 */
int pccc_check_write_status(ab_tag_p tag) {
    pccc_cmd_resp *pccc = NULL;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_SPEW, tag->tag_id, "Starting.");

    rc = pccc_check_response_header(tag, (uint8_t **)&pccc);
    if(rc != PLCTAG_STATUS_OK) {
        ab_tag_abort_request(tag);
        tag->write_in_progress = 0;
        return rc;
    }

    uint8_t *data_end = tag->req->data + tag->req->request_size;

    /* fake exception */
    do {
        if(pccc->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_WARN, tag->tag_id, "PCCC command failed, response code: %d - %s",
                   pccc->pccc_status, pccc_decode_error(&pccc->pccc_status, cip_error_data_size(&pccc->pccc_status, data_end)));
            rc = pccc_status_to_rc(&pccc->pccc_status, cip_error_data_size(&pccc->pccc_status, data_end));
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    ab_tag_abort_request(tag);

    /* Write is complete whether it succeeded or failed */
    tag->write_in_progress = 0;
    tag->write_complete = 1;

    pdebug(DEBUG_MODULE_AB_PCCC, DEBUG_SPEW, tag->tag_id, "Done with status %s.", plc_tag_decode_error(rc));

    /* Success! */
    return rc;
}
