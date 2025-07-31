/***************************************************************************
 *   Copyright (C) 2025 by Kyle Hayes                                      *
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
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
    uint8_t pccc_function;  /* FNC sub-function of command */
    uint16_le pccc_offset;  /* offset of requested in total request */
    uint16_le pccc_transfer_size; /* total number of words requested */
} END_PACK pccc_read_cmd_req;

START_PACK typedef struct {
    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
    uint8_t pccc_function;  /* FNC sub-function of command */
} END_PACK pccc_rmw_cmd_req;

START_PACK typedef struct {
    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
    uint8_t pccc_function;  /* FNC sub-function of command */
    uint16_le pccc_offset;  /* offset of requested in total request */
    uint16_le pccc_transfer_size; /* total number of words requested */
} END_PACK pccc_write_cmd_req;



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

    /* PCCC Command */
    uint8_t pccc_command;   /* CMD read, write etc. */
    uint8_t pccc_status;    /* STS 0x00 in request */
    uint16_le pccc_seq_num; /* TNS transaction/sequence id */
    uint8_t pccc_function;  /* FNC sub-function of command */
    uint16_le pccc_offset;  /* offset of requested in total request */
    uint16_le pccc_transfer_size; /* total number of words requested */
} END_PACK pccc_dhp_read_cmd_req;

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
    uint8_t pccc_function;  /* FNC sub-function of command */
} END_PACK pccc_dhp_rmw_cmd_req;

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
    uint8_t pccc_function;  /* FNC sub-function of command */
    uint16_le pccc_offset;  /* offset of requested in total request */
    uint16_le pccc_transfer_size; /* total number of words requested */
} END_PACK pccc_dhp_write_cmd_req;



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
static void encode_data(uint8_t *data, int *index, int val);
// static int encode_file_type(pccc_file_t file_type);


static int pccc_check_read_status(ab_tag_p tag);
static int pccc_check_write_status(ab_tag_p tag);
static int pccc_tag_write_bit_start(ab_tag_p tag);

static int pccc_dhp_check_read_status(ab_tag_p tag);
static int pccc_dhp_check_write_status(ab_tag_p tag);
static int pccc_dhp_tag_write_bit_start(ab_tag_p tag);

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

    pdebug(DEBUG_DETAIL, "Starting.");

    do {
        if((rc = parse_pccc_file_type(&p, address)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for data-table type! Error %s!", plc_tag_decode_error(rc));
            break;
        }

        /* we allow the file number to be skipped if it is output or input */
        rc = parse_pccc_file_num(&p, address);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for file number! Error %s!", plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_pccc_elem_num(&p, address)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for element number! Error %s!", plc_tag_decode_error(rc));
            break;
        }

        /* a sub-element could be a mnemonic or a numeric entry. */
        if((rc = parse_pccc_subelem(&p, address)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for element number! Error %s!", plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_pccc_bit_num(&p, address)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for subelement number! Error %s!", plc_tag_decode_error(rc));
            break;
        }
    } while(0);

    pdebug(DEBUG_DETAIL, "Starting.");

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

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!data || !size || !address) {
        pdebug(DEBUG_WARN, "Called with null data, or name or zero sized data!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* zero out the stored size. */
    *size = 0;

    /* check for space. */
    if(buf_size < (1 + 3 + 3 + 3)) {
        pdebug(DEBUG_WARN, "Encoded PCCC logical address buffer is too small!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* allocate space for the level byte */
    *size = 1;

    /* do the required levels.  Remember we start at the low bit! */
    level_byte = 0x06; /* level one and two */

    /* add in the data file number. */
    encode_data(data, size, address->file);

    /* add in the element number */
    encode_data(data, size, address->element);

    /* check to see if we need to put in a subelement. */
    if(address->sub_element >= 0) {
        level_byte |= 0x08;

        encode_data(data, size, address->sub_element);
    }

    /* store the encoded levels. */
    data[0] = level_byte;

    pdebug(DEBUG_DETAIL, "PLC/5 encoded address:");
    pdebug_dump_bytes(DEBUG_DETAIL, data, *size);

    pdebug(DEBUG_DETAIL, "Done.");

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
    pdebug(DEBUG_DETAIL, "Starting.");

    if(!data || !size) {
        pdebug(DEBUG_WARN, "Called with null data, or name or zero sized data!");
        return PLCTAG_ERR_NULL_PTR;
    }

    /* check for space. */
    if(buf_size < (3 + 1 + 3 + 3)) {
        pdebug(DEBUG_WARN, "Encoded SLC logical address buffer is too small!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* zero out the size */
    *size = 0;

    if(address->file_type == 0) {
        pdebug(DEBUG_WARN, "SLC file type %d cannot be decoded!", address->file_type);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* encode the file number */
    encode_data(data, size, address->file);

    /* encode the data file type. */
    encode_data(data, size, (int)address->file_type);

    /* add in the element number */
    encode_data(data, size, address->element);

    /* add in the sub-element number */
    encode_data(data, size, (address->sub_element < 0 ? 0 : address->sub_element));

    pdebug(DEBUG_DETAIL, "SLC/Micrologix encoded address:");
    pdebug_dump_bytes(DEBUG_DETAIL, data, *size);

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


uint8_t pccc_calculate_bcc(uint8_t *data, int size) {
    int bcc = 0;
    int i;

    for(i = 0; i < size; i++) { bcc += data[i]; }

    /* we want the twos-compliment of the lowest 8 bits. */
    bcc = -bcc;

    /* only the lowest 8 bits */
    return (uint8_t)(bcc & 0xFF);
}


/* Calculate AB's version of CRC-16.  We use a precalculated
 * table for simplicity.   Note that modern processors execute
 * so many instructions per second, that using a table, even
 * this small, is probably slower.
 */

uint16_t CRC16Bytes[] = {
    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241, 0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481,
    0x0440, 0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40, 0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0,
    0x0880, 0xC841, 0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40, 0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01,
    0x1DC0, 0x1C80, 0xDC41, 0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641, 0xD201, 0x12C0, 0x1380, 0xD341,
    0x1100, 0xD1C1, 0xD081, 0x1040, 0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240, 0x3600, 0xF6C1, 0xF781,
    0x3740, 0xF501, 0x35C0, 0x3480, 0xF441, 0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41, 0xFA01, 0x3AC0,
    0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840, 0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41, 0xEE01,
    0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40, 0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
    0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041, 0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281,
    0x6240, 0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441, 0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0,
    0x6E80, 0xAE41, 0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840, 0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01,
    0x7BC0, 0x7A80, 0xBA41, 0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40, 0xB401, 0x74C0, 0x7580, 0xB541,
    0x7700, 0xB7C1, 0xB681, 0x7640, 0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041, 0x5000, 0x90C1, 0x9181,
    0x5140, 0x9301, 0x53C0, 0x5280, 0x9241, 0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440, 0x9C01, 0x5CC0,
    0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40, 0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841, 0x8801,
    0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40, 0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
    0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641, 0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081,
    0x4040};


uint16_t pccc_calculate_crc16(uint8_t *data, int size) {
    uint16_t running_crc = 0;
    int i;

    /* for each byte in the data... */
    for(i = 0; i < size; i++) {
        /* calculate the running byte.  This is a lot like
         * a CBC.  You keep the running value as you go along
         * and the table is precalculated to have all the right
         * 256 values.
         */

        /* mask the CRC and XOR with the data byte */
        uint8_t running_byte = (uint8_t)(running_crc & 0x00FF) ^ data[i];

        /* calculate the next CRC value by shifting and XORing with
         * the value we get from a table lookup using the running
         * byte as an index.  This chains the data forward as we
         * calculate the CRC.
         */
        running_crc = (running_crc >> 8) ^ CRC16Bytes[running_byte];
    }

    return running_crc;
}


const char *pccc_decode_error(uint8_t *error_ptr) {
    uint8_t error = *error_ptr;

    /* extended error? */
    if(error == 0xF0) { error = *(error_ptr + 3); }

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


/*
 * FIXME This does not check for data overruns!
 */

uint8_t *pccc_decode_dt_byte(uint8_t *data, int data_size, int *pccc_res_type, int *pccc_res_length) {
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

    /* check the type.  If it is too large to hold in
     * the bottom three bits of the nybble, then the
     * top bit will be set and the bottom three will
     * hold the number of bytes that follows for the
     * type value.  We stop after 4 bytes.  Hopefully
     * that works.
     */

    if(d_type & 0x08) {
        int size_bytes = d_type & 0x07;

        if(size_bytes > 4) { return NULL; }

        d_type = 0;

        while(size_bytes--) {
            data++; /* we leave the pointer at the last read byte */
            d_type <<= 8;
            d_type |= *data;
        }
    }

    /* same drill for the size */
    if(d_size & 0x08) {
        int size_bytes = d_size & 0x07;

        if(size_bytes > 4) { return NULL; }

        d_size = 0;

        while(size_bytes--) {
            data++; /* we leave the pointer at the last read byte */
            d_size <<= 8;
            d_size |= *data;
        }
    }

    *pccc_res_type = (int)d_type;
    *pccc_res_length = (int)d_size;

    /* point past the last byte read */
    data++;

    return data;
}


int pccc_encode_dt_byte(uint8_t *data, int buf_size, uint32_t data_type, uint32_t data_size) {
    uint8_t *dt_byte = data;
    uint8_t d_byte;
    uint8_t t_byte;
    int size_bytes;

    /* increment past the dt_byte */
    data++;
    buf_size--;

    /* if data type fits in 3 bits then
     * just use the dt_byte.
     */

    if(data_type <= 0x07) {
        t_byte = (uint8_t)data_type;
        data_type = 0;
    } else {
        size_bytes = 0;

        while((data_type & 0xFF) && data_size) {
            *data = data_type & 0xFF;
            data_type >>= 8;
            size_bytes++;
            buf_size--;
            data++;
        }

        t_byte = (uint8_t)(0x08 | size_bytes);
    }

    if(data_size <= 0x07) {
        d_byte = (uint8_t)data_size;
        data_size = 0;
    } else {
        size_bytes = 0;

        while((data_size & 0xFF) && data_size) {
            *data = data_size & 0xFF;
            data_size >>= 8;
            size_bytes++;
            buf_size--;
            data++;
        }

        d_byte = (uint8_t)(0x08 | size_bytes);
    }

    *dt_byte = (uint8_t)((t_byte << 4) | d_byte);

    /* did we succeed? */
    if(buf_size == 0 || data_type != 0 || data_size != 0) { return 0; }


    return (int)(data - dt_byte);
}


/*
 * Helper functions
 */


/*
 * FIXME TODO - refactor this into a table-driven function.
 */

int parse_pccc_file_type(const char **str, pccc_addr_t *address) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    switch((*str)[0]) {
        case 'A':
        case 'a': /* ASCII */
            pdebug(DEBUG_DETAIL, "Found ASCII file.");
            address->file_type = PCCC_FILE_ASCII;
            address->element_size_bytes = 1;
            (*str)++;
            break;

        case 'B':
        case 'b': /* Bit or block transfer */
            if(isdigit((*str)[1])) {
                /* Bit */
                pdebug(DEBUG_DETAIL, "Found Bit file.");
                address->file_type = PCCC_FILE_BIT;
                address->element_size_bytes = 2;
                (*str)++;
                break;
            } else {
                if((*str)[1] == 'T' || (*str)[1] == 't') {
                    /* block transfer */
                    pdebug(DEBUG_DETAIL, "Found Block Transfer file.");
                    address->file_type = PCCC_FILE_BLOCK_TRANSFER;
                    address->element_size_bytes = 12;
                    (*str) += 2;
                } else {
                    pdebug(DEBUG_WARN, "Unknown file %s found!", *str);
                    address->file_type = PCCC_FILE_UNKNOWN;
                    address->element_size_bytes = 0;
                    rc = PLCTAG_ERR_BAD_PARAM;
                }
            }

            break;

        case 'C':
        case 'c': /* Counter */
            pdebug(DEBUG_DETAIL, "Found Counter file.");
            address->file_type = PCCC_FILE_COUNTER;
            address->element_size_bytes = 6;
            (*str)++;
            break;

        case 'D':
        case 'd': /* BCD number */
            pdebug(DEBUG_DETAIL, "Found BCD file.");
            address->file_type = PCCC_FILE_BCD;
            address->element_size_bytes = 2;
            (*str)++;
            break;

        case 'F':
        case 'f': /* Floating point Number */
            pdebug(DEBUG_DETAIL, "Found Float/REAL file.");
            address->file_type = PCCC_FILE_FLOAT;
            address->element_size_bytes = 4;
            (*str)++;
            break;

        case 'I':
        case 'i': /* Input */
            pdebug(DEBUG_DETAIL, "Found Input file.");
            address->file_type = PCCC_FILE_INPUT;
            address->file = 1; /* in case it is omitted */
            address->element_size_bytes = 2;
            (*str)++;
            break;

        case 'L':
        case 'l':
            pdebug(DEBUG_DETAIL, "Found Long Int file.");
            address->file_type = PCCC_FILE_LONG_INT;
            address->element_size_bytes = 4;
            (*str)++;
            break;

        case 'M':
        case 'm': /* Message */
            if((*str)[1] == 'G' || (*str)[1] == 'g') {
                pdebug(DEBUG_DETAIL, "Found Message file.");
                address->file_type = PCCC_FILE_MESSAGE;
                address->element_size_bytes = 112;
                (*str) += 2; /* skip past both characters */
            } else {
                address->file_type = PCCC_FILE_UNKNOWN;
                pdebug(DEBUG_WARN, "Unknown file %s found!", *str);
                rc = PLCTAG_ERR_BAD_PARAM;
            }
            break;

        case 'N':
        case 'n': /* INT */
            pdebug(DEBUG_DETAIL, "Found Integer file.");
            address->file_type = PCCC_FILE_INT;
            address->element_size_bytes = 2;
            (*str)++;
            break;

        case 'O':
        case 'o': /* Output */
            /* FIXME - Check if 0x82 is correct instead of 0x8b */
            pdebug(DEBUG_DETAIL, "Found Output file.");
            address->file_type = PCCC_FILE_OUTPUT;
            address->element_size_bytes = 2;
            address->file = 0; /* in case it is omitted */
            (*str)++;
            break;

        case 'P':
        case 'p': /* PID */
            if((*str)[1] == 'D' || (*str)[1] == 'd') {
                pdebug(DEBUG_DETAIL, "Found PID file.");
                address->file_type = PCCC_FILE_PID;
                address->element_size_bytes = 164;
                (*str) += 2; /* skip past both characters */
            } else {
                address->file_type = PCCC_FILE_UNKNOWN;
                pdebug(DEBUG_WARN, "Unknown file %s found!", *str);
                rc = PLCTAG_ERR_BAD_PARAM;
            }
            break;

        case 'R':
        case 'r': /* Control */
            pdebug(DEBUG_DETAIL, "Found Control file.");
            address->file_type = PCCC_FILE_CONTROL;
            address->element_size_bytes = 6;
            (*str)++;
            break;

        case 'S':
        case 's': /* Status, SFC or String */
            if(isdigit((*str)[1])) {
                /* Status */
                pdebug(DEBUG_DETAIL, "Found Status file.");
                address->file_type = PCCC_FILE_STATUS;
                address->element_size_bytes = 2;
                (*str)++;
                break;
            } else {
                if((*str)[1] == 'C' || (*str)[1] == 'c') {
                    /* SFC */
                    pdebug(DEBUG_DETAIL, "Found SFC file.");
                    address->file_type = PCCC_FILE_SFC;
                    address->element_size_bytes = 6;
                    (*str) += 2; /* skip past both characters */
                } else if((*str)[1] == 'T' || (*str)[1] == 't') {
                    /* String */
                    pdebug(DEBUG_DETAIL, "Found String file.");
                    address->file_type = PCCC_FILE_STRING;
                    address->element_size_bytes = 84;
                    (*str) += 2; /* skip past both characters */
                } else {
                    address->file_type = PCCC_FILE_UNKNOWN;
                    pdebug(DEBUG_WARN, "Unknown file %s found!", *str);
                    rc = PLCTAG_ERR_BAD_PARAM;
                }
            }
            break;

        case 'T':
        case 't': /* Timer */
            pdebug(DEBUG_DETAIL, "Found Timer file.");
            address->file_type = PCCC_FILE_TIMER;
            address->element_size_bytes = 6;
            (*str)++;
            break;

        default:
            pdebug(DEBUG_WARN, "Bad format or unsupported logical address %s!", *str);
            address->file_type = PCCC_FILE_UNKNOWN;
            address->element_size_bytes = 0;
            rc = PLCTAG_ERR_BAD_PARAM;
            break;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int parse_pccc_file_num(const char **str, pccc_addr_t *address) {
    int tmp = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!str || !*str) {
        pdebug(DEBUG_WARN, "Expected data-table file number!");
        address->file = -1;
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* if this is I or O, then we can skip the data file number. */
    if((address->file_type == PCCC_FILE_INPUT || address->file_type == PCCC_FILE_OUTPUT) && !isdigit(**str)) {
        /* skip the data file number */
        pdebug(DEBUG_DETAIL, "Data file number omitted for I or O data file.");
        return PLCTAG_STATUS_OK;
    }

    /* FIXME - why are we not using strtol here? We should also support octal. */
    while(**str && isdigit(**str) && tmp < 65535) {
        tmp *= 10;
        tmp += (int)((**str) - '0');
        (*str)++;
    }

    address->file = tmp;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int parse_pccc_elem_num(const char **str, pccc_addr_t *address) {
    int tmp = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!str || !*str || **str != ':') {
        pdebug(DEBUG_WARN, "Expected data-table element number!");
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

    pdebug(DEBUG_DETAIL, "Found element %d.", tmp);

    address->element = tmp;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


int parse_pccc_subelem(const char **str, pccc_addr_t *address) {
    int rc = PLCTAG_STATUS_OK;

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
        pdebug(DEBUG_DETAIL, "No subelement in this logical address.");
        address->sub_element = -1;
        return PLCTAG_STATUS_OK;
    }

    /* make sure the next character is . and nothing else. */
    if((**str) != '.') {
        pdebug(DEBUG_WARN, "Bad subelement field in logical address.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* step past the . character */
    (*str)++;

    /* try a number. */
    rc = parse_pccc_subelem_num(str, address);
    if(rc == PLCTAG_STATUS_OK) {
        /* we found a numeric sub-element */
        pdebug(DEBUG_DETAIL, "Found numeric sub-element %d.", address->sub_element);
        return rc;
    }

    if(rc == PLCTAG_ERR_NO_MATCH) {
        /* not a numeric sub-element, try matching a mnemonic. */
        return parse_pccc_subelem_mnemonic(str, address);
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int parse_pccc_subelem_num(const char **str, pccc_addr_t *address) {
    int tmp = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* is it a numeric sub-element? */
    if(!isdigit(**str)) {
        /* nope, it is not. */
        pdebug(DEBUG_DETAIL, "Not a numeric sub-element.");
        return PLCTAG_ERR_NO_MATCH;
    }

    while(**str && isdigit(**str) && tmp < 65535) {
        tmp *= 10;
        tmp += (int)((**str) - '0');
        (*str)++;
    }

    pdebug(DEBUG_DETAIL, "Found sub-element %d.", tmp);

    address->sub_element = tmp;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}

// NOLINTNEXTLINE
struct {
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
        address->sub_element = -1;
        return PLCTAG_STATUS_OK;
    }

    /*
     * We do have a character.  It must be . or / to be valid.
     * The . character is valid before a mnemonic for a field in a structured type.
     * The / character is valid before a bit number.
     *
     * If we see a bit number, then punt out of this routine.
     */

    if((**str) == '/') {
        pdebug(DEBUG_DETAIL, "No subelement in this logical address.");
        address->sub_element = -1;
        return PLCTAG_STATUS_OK;
    }

    /* make sure the next character is either / or . and nothing else. */
    if((**str) != '.') {
        pdebug(DEBUG_WARN, "Bad subelement field in logical address.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* step past the . character */
    (*str)++;

    /* search for a match. */
    for(size_t i = 0; i < (sizeof(sub_element_lookup) / sizeof(sub_element_lookup[0])); i++) {
        if(sub_element_lookup[i].file_type == address->file_type
           && str_cmp_i_n(*str, sub_element_lookup[i].field_name, str_length(sub_element_lookup[i].field_name)) == 0) {
            pdebug(DEBUG_DETAIL, "Matched file type %x and field mnemonic \"%.*s\".", address->file_type,
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

    pdebug(DEBUG_WARN, "Unsupported field mnemonic %s for type %x!", *str, address->file_type);

    return PLCTAG_ERR_BAD_PARAM;
}


int parse_pccc_bit_num(const char **str, pccc_addr_t *address) {
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
        pdebug(DEBUG_DETAIL, "No bit number in this name.");
        return PLCTAG_STATUS_OK;
    }

    /* make sure the next character is /. */
    if((**str) != '/') {
        pdebug(DEBUG_WARN, "Bad bit number in logical address.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* make sure that the data size is two bytes only. */
    if(address->element_size_bytes != 2) {
        pdebug(DEBUG_WARN, "Single bit selection only works on word-sized data.");
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

    if(tmp < 0 || tmp > 15) {
        pdebug(DEBUG_WARN, "Error processing bit number.  Must be between 0 and 15 inclusive, found %d!", tmp);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    address->is_bit = (uint8_t)1;
    address->bit = (uint8_t)tmp;

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


void encode_data(uint8_t *data, int *index, int val) {
    if(val <= 254) {
        data[*index] = (uint8_t)val;
        *index = *index + 1;
    } else {
        data[*index] = (uint8_t)0xff;
        data[*index + 1] = (uint8_t)(val & 0xff);
        data[*index + 2] = (uint8_t)((val >> 8) & 0xff);
        *index = *index + 3;
    }
}




//===============================================================
// PCCC Tag Functions
//===============================================================


/*
 * tag_status
 *
 * get the tag status.
 */
int pccc_tag_status(ab_tag_p tag) {
    if(!tag->session) {
        /* this is not OK.  This is fatal! */
        return PLCTAG_ERR_CREATE;
    }

    if(tag->read_in_progress) { return PLCTAG_STATUS_PENDING; }

    if(tag->write_in_progress) { return PLCTAG_STATUS_PENDING; }

    return tag->status;
}


int pccc_tag_tickler(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    rc = check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->read_in_progress) {
        pdebug(DEBUG_SPEW, "Read in progress.");
        rc = pccc_check_read_status(tag);
        tag->status = (int8_t)rc;

        /* check to see if the read finished. */
        if(!tag->read_in_progress) {
            /* first read done */
            if(tag->first_read) {
                tag->first_read = 0;
                tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, PLCTAG_STATUS_OK);
            }

            tag->read_complete = 1;
        }

        return rc;
    }

    if(tag->write_in_progress) {
        pdebug(DEBUG_SPEW, "Write in progress.");
        rc = pccc_check_write_status(tag);
        tag->status = (int8_t)rc;

        /* check to see if the write finished. */
        if(!tag->write_in_progress) { tag->write_complete = 1; }

        return rc;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return tag->status;
}

/*
 * tag_read_start
 *
 * Start a PCCC tag read (PLC5).
 */

int pccc_tag_read_start(ab_tag_p tag, uint8_t pccc_func_code) {
    int rc = PLCTAG_STATUS_OK;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));
    ab_request_p req = NULL;
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    int cip_payload_space = session_get_available_cip_payload_space(tag->session);

    pdebug(DEBUG_INFO, "Starting");

    /* pseudo-exception block for error handling */
    do {
        /* check for busy */
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_WARN, "Read or write operation already in flight!");
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        tag->read_in_progress = 1;

        /* calculate response overhead and available payload space */
        int response_overhead = sizeof(cip_pccc_resp) + sizeof(pccc_cmd_resp);
        int response_payload_space = cip_payload_space - response_overhead;

        if(response_payload_space < 0) {
            pdebug(DEBUG_WARN, "PLC5 read response overhead (%d bytes) exceeds session payload space (%d bytes).",
                   response_overhead, cip_payload_space);
            tag->read_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        if(response_payload_space < tag->size) {
            pdebug(DEBUG_WARN,
                   "Tag size (%d bytes) exceeds available response data space (%d bytes). PCCC does not support fragmentation.",
                   tag->size, response_payload_space);
            tag->read_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* calculate request overhead */
        int request_overhead =   (int)sizeof(cip_pccc_req)
                               + (int)sizeof(pccc_read_cmd_req)
                               + tag->encoded_name_size
                               + 1;

        pdebug(DEBUG_INFO, "PLC5 request overhead: CIP/PCCC header size %zu, PCCC read command header size %zu, encoded_name=%d, data_size=1, total=%d bytes",
               sizeof(cip_pccc_req), sizeof(pccc_read_cmd_req), tag->encoded_name_size, request_overhead);

        int request_payload_space = cip_payload_space - request_overhead;

        if(request_payload_space < 0) {
            pdebug(
                DEBUG_DETAIL,
                "PLC5 read request overhead (%d bytes) exceeds session payload space (%d bytes). Tag name too long or PCCC packet limit exceeded.",
                request_overhead, cip_payload_space);
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* create the request */
        rc = session_create_request(tag->session, tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to get new request.  rc=%d", rc);
            break;
        }

        pdebug(DEBUG_INFO, "Request created. Request capacity: %d bytes", req->request_capacity);

        if(request_overhead > req->request_capacity) {
            pdebug(DEBUG_DETAIL, "PCCC request overhead (%d bytes) exceeds request capacity (%d bytes).", request_overhead,
                   req->request_capacity);
            rc_dec(req);
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* point the struct pointers to the buffer */
        eip_cpf_uc_header *cip_req = (eip_cpf_uc_header *)(req->data);
        cip_pccc_req *cip_pccc = (cip_pccc_req *)(cip_req + 1);
        pccc_read_cmd_req *pccc_cmd = (pccc_read_cmd_req *)(cip_pccc + 1);
        embed_start = (uint8_t *)(&cip_pccc->service_code);

        /* fill in CIP/PCCC header fields */
        cip_pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE;
        cip_pccc->req_path_size = 2;
        cip_pccc->req_path[0] = 0x20;
        cip_pccc->req_path[1] = 0x67;
        cip_pccc->req_path[2] = 0x24;
        cip_pccc->req_path[3] = 0x01;

        cip_pccc->request_id_size = 7;
        cip_pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);
        cip_pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN);

        /* fill in PCCC command fields */
        pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        pccc_cmd->pccc_status = 0;
        pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
        pccc_cmd->pccc_function = pccc_func_code;
        pccc_cmd->pccc_offset = h2le16(0);
        pccc_cmd->pccc_transfer_size = h2le16((uint16_t)(tag->size / 2)); /* size in words */

        /* point data pointer just past the fixed data fields */
        data = (uint8_t *)(pccc_cmd + 1);

        /* copy encoded tag name into the request */
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        /* add data size byte */
        *data = (uint8_t)(tag->size);
        data++;

        /* debug: request full data length */
        ptrdiff_t calculated_request_size = (ptrdiff_t)(data - req->data);
        pdebug(DEBUG_DETAIL, "PCCC request full data length: %td bytes.", calculated_request_size);

        /* debug: CIP data length */
        ptrdiff_t cip_request_size = (ptrdiff_t)(data - embed_start);
        pdebug(DEBUG_DETAIL, "PCCC request CIP data length: %td bytes.", cip_request_size);

        /* fill in Common Packet Format fields */
        cip_req->cpf_item_count = h2le16(2);
        cip_req->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI);
        cip_req->cpf_nai_item_length = h2le16(0);
        cip_req->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI);
        cip_req->cpf_udi_item_length = h2le16((uint16_t)cip_request_size);

        pdebug(DEBUG_DETAIL, "PCCC request CPF UDI item length: %u bytes.", le2h16(cip_req->cpf_udi_item_length));

        cip_req->router_timeout = h2le16(1);
        cip_req->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND);

        /* set request size */
        req->request_size = (int)calculated_request_size;
        pdebug(DEBUG_DETAIL, "PCCC request size set to %d bytes.", req->request_size);

        /* debug: dump request data */
        pdebug(DEBUG_DETAIL, "PCCC request data:");
        pdebug_dump_bytes(DEBUG_DETAIL, req->data, (int)calculated_request_size);

        /* add request to session */
        rc = session_add_request(tag->session, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to add request to session! rc=%d", rc);
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* set tag->req if successful, else clean up */
    if(rc == PLCTAG_STATUS_OK) {
        critical_block(tag->api_mutex) {
            if(tag->req) {
                pdebug(DEBUG_WARN, "Request already set! This should not happen!");
                rc = PLCTAG_ERR_BAD_DATA;
            } else {
                pdebug(DEBUG_INFO, "Setting request for tag %d", tag->tag_id);
                tag->req = req;
                rc = PLCTAG_STATUS_PENDING;
            }
        }
    } else {
        pdebug(DEBUG_WARN, "Failed to generate new read request rc=%s", plc_tag_decode_error(rc));
        tag->read_in_progress = 0;
        req = rc_dec(req);
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");
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

    pdebug(DEBUG_SPEW, "Starting");

    /* get the header pointers */
    eip_cpf_uc_header *eip_cpf = (eip_cpf_uc_header *)(tag->req->data);
    cip_pccc_resp *cip_pccc = (cip_pccc_resp *)(eip_cpf + 1);
    pccc_cmd_resp *pccc_cmd = (pccc_cmd_resp *)(cip_pccc + 1);

    uint8_t *data = (uint8_t *)(pccc_cmd + 1);
    uint8_t *data_end = tag->req->data + tag->req->request_size;

    /* fake exceptions */
    do {
        if(cip_pccc->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: (%d) %s", cip_pccc->general_status,
                   decode_cip_error_long((uint8_t *)&(cip_pccc->general_status)));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc_cmd->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d - %s", pccc_cmd->pccc_status,
                   pccc_decode_error(&pccc_cmd->pccc_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /* did we get the right amount of data? */
        if((data_end - data) != tag->size) {
            if((int)(data_end - data) > tag->size) {
                pdebug(DEBUG_WARN, "Too much data received!  Expected %d bytes but got %d bytes!", tag->size,
                       (int)(data_end - data));
                rc = PLCTAG_ERR_TOO_LARGE;
            } else {
                pdebug(DEBUG_WARN, "Too little data received!  Expected %d bytes but got %d bytes!", tag->size,
                       (int)(data_end - data));
                rc = PLCTAG_ERR_TOO_SMALL;
            }
            break;
        }

        /* copy data into the tag. */
        mem_copy(tag->data, data, (int)(data_end - data));

        tag->read_in_progress = 0;
        tag->read_complete = 1;

        rc = PLCTAG_STATUS_OK;
    } while(0);

    ab_tag_abort_request(tag);

    pdebug(DEBUG_SPEW, "Done with status %s.", plc_tag_decode_error(rc));

    return rc;
}


int pccc_tag_write_start(ab_tag_p tag, uint8_t pccc_func_code) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    size_t overhead, data_per_packet;

    pdebug(DEBUG_INFO, "Starting.");

    if(tag->is_bit) {
        return pccc_tag_write_bit_start(tag);
    }

    do {
        /* check for busy */
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_WARN, "Read (%d) or write (%d) operation already in flight!", tag->read_in_progress, tag->write_in_progress);
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        tag->write_in_progress = 1;

        /* How much overhead? */
        overhead =   sizeof(cip_pccc_req)
                   + sizeof(pccc_write_cmd_req)
                   + (size_t)(unsigned int)tag->encoded_name_size
                   + 1;

        int session_payload_space = session_get_available_cip_payload_space(tag->session);

        if(session_payload_space <= 0) {
            pdebug(DEBUG_WARN, "Unable to get valid payload space from session. Available payload: %d bytes", session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        data_per_packet = (size_t)session_payload_space - overhead;

        if(data_per_packet <= 0) {
            pdebug(DEBUG_WARN, "Unable to send request.  Packet overhead, %d bytes, is too large for available payload, %d bytes!",
                   overhead, session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        if(data_per_packet < (size_t)tag->size) {
            pdebug(DEBUG_WARN, "Tag size is %d, write overhead is %d, and write data per packet is %zu.", tag->size, overhead,
                   data_per_packet);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* get a request buffer */
        rc = session_create_request(tag->session, tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to get new request.  rc=%d", rc);
            tag->write_in_progress = 0;
            break;
        }

        /* point the struct pointers to the buffer */
        eip_cpf_uc_header *cip_req = (eip_cpf_uc_header *)(req->data);
        cip_pccc_req *cip_pccc = (cip_pccc_req *)(cip_req + 1);
        pccc_write_cmd_req *pccc_cmd = (pccc_write_cmd_req *)(cip_pccc + 1);
        embed_start = (uint8_t *)(&cip_pccc->service_code);

        /* fill in CIP/PCCC header fields */
        cip_pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE;
        cip_pccc->req_path_size = 2;
        cip_pccc->req_path[0] = 0x20;
        cip_pccc->req_path[1] = 0x67;
        cip_pccc->req_path[2] = 0x24;
        cip_pccc->req_path[3] = 0x01;

        cip_pccc->request_id_size = (uint8_t)(sizeof(cip_pccc->request_id_size) + sizeof(cip_pccc->vendor_id) + sizeof(cip_pccc->vendor_serial_number));
        cip_pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);
        cip_pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN);

        /* fill in PCCC command fields */
        pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        pccc_cmd->pccc_status = 0;
        pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
        pccc_cmd->pccc_function = pccc_func_code;
        pccc_cmd->pccc_offset = h2le16(0);
        pccc_cmd->pccc_transfer_size = h2le16((uint16_t)(tag->size / 2)); // size is in bytes, PCCC transfer size is in words

        /* point data pointer just past the fixed data fields */
        data = (uint8_t *)(pccc_cmd + 1);

        /* copy encoded tag name into the request */
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        /* now copy the data to write */
        mem_copy(data, tag->data, tag->size);
        data += tag->size;

        /* debug: request full data length */
        ptrdiff_t calculated_request_size = (ptrdiff_t)(data - req->data);
        pdebug(DEBUG_DETAIL, "PCCC write request full data length: %td bytes.", calculated_request_size);

        /* debug: dump request data */
        pdebug(DEBUG_DETAIL, "PCCC write request data:");
        pdebug_dump_bytes(DEBUG_DETAIL, req->data, (int)calculated_request_size);

        /* debug: CIP data length */
        ptrdiff_t cip_request_size = (ptrdiff_t)(data - embed_start);
        pdebug(DEBUG_DETAIL, "PCCC write request CIP data length: %td bytes.", cip_request_size);

        /* fill in Common Packet Format fields */
        cip_req->cpf_item_count = h2le16(2);
        cip_req->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI);
        cip_req->cpf_nai_item_length = h2le16(0);
        cip_req->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI);
        cip_req->cpf_udi_item_length = h2le16((uint16_t)cip_request_size);

        pdebug(DEBUG_DETAIL, "PCCC write request CPF UDI item length: %u bytes.", le2h16(cip_req->cpf_udi_item_length));

        cip_req->router_timeout = h2le16(1);
        cip_req->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND);

        /* set request size */
        req->request_size = (int)calculated_request_size;
        pdebug(DEBUG_DETAIL, "PCCC write request size set to %d bytes.", req->request_size);

        /* add request to session */
        rc = session_add_request(tag->session, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to add write request to session! rc=%d", rc);
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* set tag->req if successful, else clean up */
    if(rc == PLCTAG_STATUS_OK) {
        critical_block(tag->api_mutex) {
            if(tag->req) {
                pdebug(DEBUG_WARN, "Request already set! This should not happen!");
                rc = PLCTAG_ERR_BAD_DATA;
            } else {
                pdebug(DEBUG_INFO, "Setting write request for tag %d", tag->tag_id);
                tag->req = req;
                rc = PLCTAG_STATUS_PENDING;
            }
        }
    } else {
        pdebug(DEBUG_WARN, "Failed to generate new write request rc=%s", plc_tag_decode_error(rc));
        req = rc_dec(req);
        tag->write_in_progress = 0;
        tag->write_complete = 1;
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");
    return rc;
}

int pccc_tag_write_bit_start(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    size_t overhead, data_per_packet;

    pdebug(DEBUG_INFO, "Starting.");

    do {
        /* check for busy */
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_WARN, "Read (%d) or write (%d) operation already in flight!", tag->read_in_progress, tag->write_in_progress);
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        tag->write_in_progress = 1;

        /* How much overhead? */
        overhead =   sizeof(cip_pccc_req)
                   + sizeof(pccc_write_cmd_req)
                   + (size_t)(unsigned int)tag->encoded_name_size
                   + 1;

        int session_payload_space = session_get_available_cip_payload_space(tag->session);

        if(session_payload_space <= 0) {
            pdebug(DEBUG_WARN, "Unable to get valid payload space from session. Available payload: %d bytes", session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        data_per_packet = (size_t)session_payload_space - overhead;

        if(data_per_packet <= 0) {
            pdebug(DEBUG_WARN, "Unable to send request.  Packet overhead, %d bytes, is too large for available payload, %d bytes!",
                   overhead, session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        if(data_per_packet < (size_t)tag->size) {
            pdebug(DEBUG_WARN, "Tag size is %d, write overhead is %d, and write data per packet is %zu.", tag->size, overhead,
                   data_per_packet);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* get a request buffer */
        rc = session_create_request(tag->session, tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to get new request.  rc=%d", rc);
            tag->write_in_progress = 0;
            break;
        }

        /* point the struct pointers to the buffer */
        eip_cpf_uc_header *cip_req = (eip_cpf_uc_header *)(req->data);
        cip_pccc_req *cip_pccc = (cip_pccc_req *)(cip_req + 1);
        pccc_rmw_cmd_req *pccc_cmd = (pccc_rmw_cmd_req *)(cip_pccc + 1);
        embed_start = (uint8_t *)(&cip_pccc->service_code);

        /* fill in CIP/PCCC header fields */
        cip_pccc->service_code = AB_EIP_CMD_PCCC_EXECUTE;
        cip_pccc->req_path_size = 2;
        cip_pccc->req_path[0] = 0x20;
        cip_pccc->req_path[1] = 0x67;
        cip_pccc->req_path[2] = 0x24;
        cip_pccc->req_path[3] = 0x01;

        cip_pccc->request_id_size = (uint8_t)(sizeof(cip_pccc->request_id_size) + sizeof(cip_pccc->vendor_id) + sizeof(cip_pccc->vendor_serial_number));
        cip_pccc->vendor_id = h2le16(AB_EIP_VENDOR_ID);
        cip_pccc->vendor_serial_number = h2le32(AB_EIP_VENDOR_SN);

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
                pdebug(DEBUG_DETAIL, "adding reset mask byte %d: %x", i, *data);
                data++;
            } else {
                *data = (uint8_t)0xFF;
                pdebug(DEBUG_DETAIL, "adding reset mask byte %d: %x", i, *data);
                data++;
            }
        }
        /* OR/set mask */
        for(int i = 0; i < tag->elem_size; i++) {
            if((tag->bit / 8) == i) {
                *data = tag->data[i] & (uint8_t)(1 << (tag->bit % 8));
                pdebug(DEBUG_DETAIL, "adding set mask byte %d: %x", i, *data);
                data++;
            } else {
                *data = (uint8_t)0x00;
                pdebug(DEBUG_DETAIL, "adding set mask byte %d: %x", i, *data);
                data++;
            }
        }

        /* debug: request full data length */
        ptrdiff_t calculated_request_size = (ptrdiff_t)(data - req->data);
        pdebug(DEBUG_DETAIL, "PCCC write request full data length: %td bytes.", calculated_request_size);

        /* debug: dump request data */
        pdebug(DEBUG_DETAIL, "PCCC write request data:");
        pdebug_dump_bytes(DEBUG_DETAIL, req->data, (int)calculated_request_size);

        /* debug: CIP data length */
        ptrdiff_t cip_request_size = (ptrdiff_t)(data - embed_start);
        pdebug(DEBUG_DETAIL, "PCCC write request CIP data length: %td bytes.", cip_request_size);

        /* fill in Common Packet Format fields */
        cip_req->cpf_item_count = h2le16(2);
        cip_req->cpf_nai_item_type = h2le16(AB_EIP_ITEM_NAI);
        cip_req->cpf_nai_item_length = h2le16(0);
        cip_req->cpf_udi_item_type = h2le16(AB_EIP_ITEM_UDI);
        cip_req->cpf_udi_item_length = h2le16((uint16_t)cip_request_size);

        pdebug(DEBUG_DETAIL, "PCCC write request CPF UDI item length: %u bytes.", le2h16(cip_req->cpf_udi_item_length));

        cip_req->router_timeout = h2le16(1);
        cip_req->encap_command = h2le16(AB_EIP_UNCONNECTED_SEND);

        /* set request size */
        req->request_size = (int)calculated_request_size;
        pdebug(DEBUG_DETAIL, "PCCC write request size set to %d bytes.", req->request_size);

        /* add request to session */
        rc = session_add_request(tag->session, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to add write request to session! rc=%d", rc);
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* set tag->req if successful, else clean up */
    if(rc == PLCTAG_STATUS_OK) {
        critical_block(tag->api_mutex) {
            if(tag->req) {
                pdebug(DEBUG_WARN, "Request already set! This should not happen!");
                rc = PLCTAG_ERR_BAD_DATA;
            } else {
                pdebug(DEBUG_INFO, "Setting write request for tag %d", tag->tag_id);
                tag->req = req;
                rc = PLCTAG_STATUS_PENDING;
            }
        }
    } else {
        pdebug(DEBUG_WARN, "Failed to generate new write request rc=%s", plc_tag_decode_error(rc));
        req = rc_dec(req);
        tag->write_in_progress = 0;
        tag->write_complete = 1;
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");
    return rc;
}


/*
 * check_write_status
 *
 * Fragments are not supported.
 */
int pccc_check_write_status(ab_tag_p tag) {
    pccc_resp *pccc;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    /* the request reference is valid. */

    pccc = (pccc_resp *)(tag->req->data);

    /* fake exception */
    do {
        if(pccc->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d", pccc->general_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d - %s", pccc->pccc_status,
                   pccc_decode_error(&pccc->pccc_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        tag->write_in_progress = 0;
        tag->write_complete = 1;

        rc = PLCTAG_STATUS_OK;
    } while(0);

    ab_tag_abort_request(tag);

    pdebug(DEBUG_SPEW, "Done with status %s.", plc_tag_decode_error(rc));

    /* Success! */
    return rc;
}


//==================================================================
// DH+ PCCC-specific functions
//==================================================================


/*
 * tag_status
 *
 * PCCC/DH+-specific status.  This functions as a "tickler" routine
 * to check on the completion of async requests.
 */
int pccc_dhp_tag_status(ab_tag_p tag) {
    if(!tag->session) {
        /* this is not OK.  This is fatal! */
        return PLCTAG_ERR_CREATE;
    }

    if(tag->read_in_progress) { return PLCTAG_STATUS_PENDING; }

    if(tag->write_in_progress) { return PLCTAG_STATUS_PENDING; }

    return tag->status;
}


int pccc_dhp_tag_tickler(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    rc = check_request_status(tag);
    if(rc != PLCTAG_STATUS_OK) { return rc; }

    if(tag->read_in_progress) {
        pdebug(DEBUG_SPEW, "Read in progress.");
        rc = pccc_dhp_check_read_status(tag);
        tag->status = (int8_t)rc;

        /* check to see if the read finished. */
        if(!tag->read_in_progress) {
            /* read done so create done. */
            if(tag->first_read) {
                tag->first_read = 0;
                tag_raise_event((plc_tag_p)tag, PLCTAG_EVENT_CREATED, (int8_t)rc);
            }

            tag->read_complete = 1;
        }

        return rc;
    }

    if(tag->write_in_progress) {
        pdebug(DEBUG_SPEW, "Write in progress.");
        rc = pccc_dhp_check_write_status(tag);
        tag->status = (int8_t)rc;

        /* check to see if the write finished. */
        if(!tag->write_in_progress) { tag->write_complete = 1; }

        return rc;
    }

    pdebug(DEBUG_SPEW, "Done.");

    return tag->status;
}


/*
 * tag_read_start
 *
 * Start a PCCC tag read (PLC5).
 */

int pccc_dhp_tag_read_start(ab_tag_p tag, uint8_t pccc_func_code) {
    int rc = PLCTAG_STATUS_OK;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));
    ab_request_p req = NULL;
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    int cip_payload_space = session_get_available_cip_payload_space(tag->session);

    pdebug(DEBUG_INFO, "Starting");

    /* pseudo-exception block for error handling */
    do {
        /* check for busy */
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_WARN, "Read or write operation already in flight!");
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        tag->read_in_progress = 1;

        /* calculate response overhead and available payload space */
        int response_overhead = sizeof(pccc_dhp_cmd_resp);
        int response_payload_space = cip_payload_space - response_overhead;

        if(response_payload_space < 0) {
            pdebug(DEBUG_WARN, "PLC5 read response overhead (%d bytes) exceeds session payload space (%d bytes).",
                   response_overhead, cip_payload_space);
            tag->read_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        if(response_payload_space < tag->size) {
            pdebug(DEBUG_WARN,
                   "Tag size (%d bytes) exceeds available response data space (%d bytes). DH+ PCCC does not support fragmentation.",
                   tag->size, response_payload_space);
            tag->read_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* calculate request overhead */
        int request_overhead =   (int)sizeof(pccc_dhp_read_cmd_req)
                               + tag->encoded_name_size
                               + 1;

        pdebug(DEBUG_INFO, "PLC5 request overhead: PCCC read command header size %zu, encoded_name=%d, data_size=1, total=%d bytes",
               sizeof(pccc_dhp_read_cmd_req), tag->encoded_name_size, request_overhead);

        int request_payload_space = cip_payload_space - request_overhead;

        if(request_payload_space < 0) {
            pdebug(
                DEBUG_DETAIL,
                "PLC5 read request overhead (%d bytes) exceeds session payload space (%d bytes). Tag name too long or PCCC packet limit exceeded.",
                request_overhead, cip_payload_space);
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* create the request */
        rc = session_create_request(tag->session, tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to get new request.  rc=%d", rc);
            break;
        }

        pdebug(DEBUG_INFO, "Request created. Request capacity: %d bytes", req->request_capacity);

        if(request_overhead > req->request_capacity) {
            pdebug(DEBUG_DETAIL, "DH+ PCCC request overhead (%d bytes) exceeds request capacity (%d bytes).", request_overhead,
                   req->request_capacity);
            rc_dec(req);
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* point the struct pointers to the buffer */
        eip_cpf_co_header *cip_req = (eip_cpf_co_header *)(req->data);
        pccc_dhp_read_cmd_req *pccc_cmd = (pccc_dhp_read_cmd_req *)(cip_req + 1);

        embed_start = (uint8_t *)(cip_req + 1);

        /* fill in DH+ fields */
        pccc_cmd->dest_link = h2le16(0);
        pccc_cmd->dest_node = h2le16(tag->session->dhp_dest);
        pccc_cmd->src_link = h2le16(0);
        pccc_cmd->src_node = h2le16(0);

        /* fill in PCCC command fields */
        pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        pccc_cmd->pccc_status = 0;
        pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
        pccc_cmd->pccc_function = pccc_func_code;
        pccc_cmd->pccc_offset = h2le16(0);
        pccc_cmd->pccc_transfer_size = h2le16((uint16_t)(tag->size / 2));

        /* point data pointer just past the fixed data fields */
        data = (uint8_t *)(pccc_cmd + 1);

        pdebug(DEBUG_DETAIL, "DH+ PCCC request PCCC data 1:");
        pdebug_dump_bytes(DEBUG_DETAIL, req->data,
                       (int)(data - req->data));

        /* copy encoded tag name into the request */
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        pdebug(DEBUG_DETAIL, "DH+ PCCC request PCCC data 2:");
        pdebug_dump_bytes(DEBUG_DETAIL, req->data,
                       (int)(data - req->data));

        /* add data size byte */
        *data = (uint8_t)(tag->size);
        data++;

        pdebug(DEBUG_DETAIL, "DH+ PCCC request PCCC data 3:");
        pdebug_dump_bytes(DEBUG_DETAIL, req->data,
                       (int)(data - req->data));

        /* debug: request full data length */
        ptrdiff_t calculated_request_size = (ptrdiff_t)(data - req->data);
        pdebug(DEBUG_DETAIL, "DH+ PCCC request full data length: %td bytes.", calculated_request_size);

        /* debug: CIP data length */
        ptrdiff_t cip_request_size = (ptrdiff_t)(data - embed_start);
        pdebug(DEBUG_DETAIL, "DH+ PCCC request CIP data length: %td bytes.", cip_request_size);

        /* fill in Common Packet Format fields */
        cip_req->cpf_item_count = h2le16(2);
        cip_req->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);
        cip_req->cpf_cai_item_length = h2le16(4);
        cip_req->cpf_targ_conn_id = h2le32(tag->session->targ_connection_id);
        cip_req->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);
        cip_req->cpf_conn_seq_num = h2le16(conn_seq_id);
        cip_req->cpf_cdi_item_length = h2le16((uint16_t)((size_t)cip_request_size + sizeof(cip_req->cpf_conn_seq_num)));

        pdebug(DEBUG_DETAIL, "DH+ PCCC request CPF CDI item length: %u bytes.", le2h16(cip_req->cpf_cdi_item_length));

        cip_req->router_timeout = h2le16(1);
        cip_req->encap_command = h2le16(AB_EIP_CONNECTED_SEND);

        /* set request size */
        req->request_size = (int)calculated_request_size;
        pdebug(DEBUG_DETAIL, "DH+ PCCC request size set to %d bytes.", req->request_size);

        /* debug: dump request data */
        pdebug(DEBUG_DETAIL, "DH+ PCCC request data:");
        pdebug_dump_bytes(DEBUG_DETAIL, req->data, (int)calculated_request_size);

        /* add request to session */
        rc = session_add_request(tag->session, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to add request to session! rc=%d", rc);
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* set tag->req if successful, else clean up */
    if(rc == PLCTAG_STATUS_OK) {
        critical_block(tag->api_mutex) {
            if(tag->req) {
                pdebug(DEBUG_WARN, "Request already set! This should not happen!");
                rc = PLCTAG_ERR_BAD_DATA;
            } else {
                pdebug(DEBUG_INFO, "Setting request for tag %d", tag->tag_id);
                tag->req = req;
                rc = PLCTAG_STATUS_PENDING;
            }
        }
    } else {
        pdebug(DEBUG_WARN, "Failed to generate new read request rc=%s", plc_tag_decode_error(rc));
        tag->read_in_progress = 0;
        req = rc_dec(req);
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");
    return rc;
}



/*
 * check_read_status
 *
 * NOTE that we can have only one outstanding request because PCCC
 * does not support fragments.
 */
int pccc_dhp_check_read_status(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting");

    /* get the header pointers */
    eip_cpf_co_header *eip_cpf = (eip_cpf_co_header *)(tag->req->data);
    pccc_dhp_cmd_resp *pccc_cmd = (pccc_dhp_cmd_resp *)(eip_cpf + 1);

    uint8_t *data = (uint8_t *)(pccc_cmd + 1);
    uint8_t *data_end = tag->req->data + tag->req->request_size;

    /* fake exceptions */
    do {
        if(pccc_cmd->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d - %s", pccc_cmd->pccc_status,
                   pccc_decode_error(&pccc_cmd->pccc_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        /* did we get the right amount of data? */
        if((data_end - data) != tag->size) {
            if((int)(data_end - data) > tag->size) {
                pdebug(DEBUG_WARN, "Too much data received!  Expected %d bytes but got %d bytes!", tag->size,
                       (int)(data_end - data));
                rc = PLCTAG_ERR_TOO_LARGE;
            } else {
                pdebug(DEBUG_WARN, "Too little data received!  Expected %d bytes but got %d bytes!", tag->size,
                       (int)(data_end - data));
                rc = PLCTAG_ERR_TOO_SMALL;
            }
            break;
        }

        /* copy data into the tag. */
        mem_copy(tag->data, data, (int)(data_end - data));

        tag->read_in_progress = 0;
        tag->read_complete = 1;

        rc = PLCTAG_STATUS_OK;
    } while(0);

    ab_tag_abort_request(tag);

    pdebug(DEBUG_SPEW, "Done with status %s.", plc_tag_decode_error(rc));

    return rc;
}


int pccc_dhp_tag_write_start(ab_tag_p tag, uint8_t pccc_func_code) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    size_t overhead, data_per_packet;

    pdebug(DEBUG_INFO, "Starting.");

    if(tag->is_bit) {
        return pccc_dhp_tag_write_bit_start(tag);
    }

    do {
        /* check for busy */
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_WARN, "Read (%d) or write (%d) operation already in flight!", tag->read_in_progress, tag->write_in_progress);
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        tag->write_in_progress = 1;

        /* How much overhead? */
        overhead =   sizeof(eip_cpf_co_header)
                   + sizeof(pccc_dhp_write_cmd_req)
                   + (size_t)(unsigned int)tag->encoded_name_size
                   + 1;

        int session_payload_space = session_get_available_cip_payload_space(tag->session);

        if(session_payload_space <= 0) {
            pdebug(DEBUG_WARN, "Unable to get valid payload space from session. Available payload: %d bytes", session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        data_per_packet = (size_t)session_payload_space - overhead;

        if(data_per_packet <= 0) {
            pdebug(DEBUG_WARN, "Unable to send request.  Packet overhead, %d bytes, is too large for available payload, %d bytes!",
                   overhead, session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        if(data_per_packet < (size_t)tag->size) {
            pdebug(DEBUG_WARN, "Tag size is %d, write overhead is %d, and write data per packet is %zu.", tag->size, overhead,
                   data_per_packet);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* get a request buffer */
        rc = session_create_request(tag->session, tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to get new request.  rc=%d", rc);
            tag->write_in_progress = 0;
            break;
        }

        /* point the struct pointers to the buffer */
        eip_cpf_co_header *cip_req = (eip_cpf_co_header *)(req->data);
        pccc_dhp_write_cmd_req *pccc_cmd = (pccc_dhp_write_cmd_req *)(cip_req + 1);
        embed_start = (uint8_t *)(cip_req + 1);

        /* fill in DH+ fields */
        pccc_cmd->dest_link = h2le16(0);
        pccc_cmd->dest_node = h2le16(tag->session->dhp_dest);
        pccc_cmd->src_link = h2le16(0);
        pccc_cmd->src_node = h2le16(0);

        /* fill in PCCC command fields */
        pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        pccc_cmd->pccc_status = 0;
        pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
        pccc_cmd->pccc_function = pccc_func_code;
        pccc_cmd->pccc_offset = h2le16(0);
        pccc_cmd->pccc_transfer_size = h2le16((uint16_t)(tag->size / 2));

        /* point data pointer just past the fixed data fields */
        data = (uint8_t *)(pccc_cmd + 1);

        /* copy encoded tag name into the request */
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        /* now copy the data to write */
        mem_copy(data, tag->data, tag->size);
        data += tag->size;

        /* debug: request full data length */
        ptrdiff_t calculated_request_size = (ptrdiff_t)(data - req->data);
        pdebug(DEBUG_DETAIL, "DH+ PCCC write request full data length: %td bytes.", calculated_request_size);

        /* debug: dump request data */
        pdebug(DEBUG_DETAIL, "DH+ PCCC write request data:");
        pdebug_dump_bytes(DEBUG_DETAIL, req->data, (int)calculated_request_size);

        /* debug: CIP data length */
        ptrdiff_t cip_request_size = (ptrdiff_t)(data - embed_start);
        pdebug(DEBUG_DETAIL, "DH+ PCCC write request CIP data length: %td bytes.", cip_request_size);

        /* fill in Common Packet Format fields */
        cip_req->cpf_item_count = h2le16(2);
        cip_req->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);
        cip_req->cpf_cai_item_length = h2le16(4);
        cip_req->cpf_targ_conn_id = h2le32(tag->session->targ_connection_id);
        cip_req->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);
        cip_req->cpf_conn_seq_num = h2le16(conn_seq_id);
        cip_req->cpf_cdi_item_length = h2le16((uint16_t)((size_t)cip_request_size + sizeof(cip_req->cpf_conn_seq_num)));

        pdebug(DEBUG_DETAIL, "DH+ PCCC request CPF CDI item length: %u bytes.", le2h16(cip_req->cpf_cdi_item_length));

        cip_req->router_timeout = h2le16(1);
        cip_req->encap_command = h2le16(AB_EIP_CONNECTED_SEND);

        pdebug(DEBUG_DETAIL, "DH+ PCCC write request CPF UDI item length: %u bytes.", le2h16(cip_req->cpf_cdi_item_length));

        /* set request size */
        req->request_size = (int)calculated_request_size;
        pdebug(DEBUG_DETAIL, "DH+ PCCC write request size set to %d bytes.", req->request_size);

        /* add request to session */
        rc = session_add_request(tag->session, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to add write request to session! rc=%d", rc);
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* set tag->req if successful, else clean up */
    if(rc == PLCTAG_STATUS_OK) {
        critical_block(tag->api_mutex) {
            if(tag->req) {
                pdebug(DEBUG_WARN, "Request already set! This should not happen!");
                rc = PLCTAG_ERR_BAD_DATA;
            } else {
                pdebug(DEBUG_INFO, "Setting write request for tag %d", tag->tag_id);
                tag->req = req;
                rc = PLCTAG_STATUS_PENDING;
            }
        }
    } else {
        pdebug(DEBUG_WARN, "Failed to generate new write request rc=%s", plc_tag_decode_error(rc));
        req = rc_dec(req);
        tag->write_in_progress = 0;
        tag->write_complete = 1;
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");
    return rc;
}

int pccc_dhp_tag_write_bit_start(ab_tag_p tag) {
    int rc = PLCTAG_STATUS_OK;
    ab_request_p req = NULL;
    uint16_t conn_seq_id = (uint16_t)(session_get_new_seq_id(tag->session));
    uint8_t *data = NULL;
    uint8_t *embed_start = NULL;
    size_t overhead = 0;
    int data_per_packet = 0;

    pdebug(DEBUG_INFO, "Starting.");

    do {
        /* check for busy */
        if(tag->read_in_progress || tag->write_in_progress) {
            pdebug(DEBUG_WARN, "Read (%d) or write (%d) operation already in flight!", tag->read_in_progress, tag->write_in_progress);
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        tag->write_in_progress = 1;

        /* How much overhead? */
        overhead =   sizeof(eip_cpf_co_header)
                   + sizeof(pccc_dhp_rmw_cmd_req)
                   + (size_t)tag->encoded_name_size
                   + (size_t)(tag->elem_size * 2); /* AND/OR masks */

        int session_payload_space = session_get_available_cip_payload_space(tag->session);

        if(session_payload_space <= 0) {
            pdebug(DEBUG_WARN, "Unable to get valid payload space from session. Available payload: %d bytes", session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        data_per_packet = session_payload_space - (int)overhead;

        if(data_per_packet < 0) {
            pdebug(DEBUG_WARN, "Unable to send request.  Packet overhead, %zu bytes, is too large for available payload, %d bytes!",
                   overhead, session_payload_space);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* check if we can fit the AND/OR masks in the packet */
        if(data_per_packet < (int)(tag->elem_size * 2)) {
            pdebug(DEBUG_WARN, "Tag elem_size is %d, write overhead is %zu, and write data per packet is %zu.", tag->elem_size, overhead,
                   data_per_packet);
            tag->write_in_progress = 0;
            rc = PLCTAG_ERR_TOO_LARGE;
            break;
        }

        /* get a request buffer */
        rc = session_create_request(tag->session, tag->tag_id, &req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to get new request.  rc=%d", rc);
            tag->write_in_progress = 0;
            break;
        }

        /* stack the struct pointers as in tag_read_start */
        eip_cpf_co_header *cip_req = (eip_cpf_co_header *)(req->data);
        pccc_dhp_rmw_cmd_req *pccc_cmd = (pccc_dhp_rmw_cmd_req *)(cip_req + 1);
        embed_start = (uint8_t *)(cip_req + 1);

        /* point data pointer just past the fixed data fields */
        data = (uint8_t *)(pccc_cmd + 1);

        /* copy encoded tag name into the request */
        mem_copy(data, tag->encoded_name, tag->encoded_name_size);
        data += tag->encoded_name_size;

        /* AND/reset mask */
        for(int i = 0; i < tag->elem_size; i++) {
            if((tag->bit / 8) == i) {
                uint8_t mask = (uint8_t)(1 << (tag->bit % 8));
                if(tag->data[i] & mask) {
                    *data = (uint8_t)0xFF;
                } else {
                    *data = (uint8_t)~mask;
                }
                pdebug(DEBUG_DETAIL, "adding reset mask byte %d: %x", i, *data);
                data++;
            } else {
                *data = (uint8_t)0xFF;
                pdebug(DEBUG_DETAIL, "adding reset mask byte %d: %x", i, *data);
                data++;
            }
        }
        /* OR/set mask */
        for(int i = 0; i < tag->elem_size; i++) {
            if((tag->bit / 8) == i) {
                *data = tag->data[i] & (uint8_t)(1 << (tag->bit % 8));
                pdebug(DEBUG_DETAIL, "adding set mask byte %d: %x", i, *data);
                data++;
            } else {
                *data = (uint8_t)0x00;
                pdebug(DEBUG_DETAIL, "adding set mask byte %d: %x", i, *data);
                data++;
            }
        }

        /* debug: request full data length */
        ptrdiff_t calculated_request_size = (ptrdiff_t)(data - req->data);
        pdebug(DEBUG_DETAIL, "DH+ PCCC bit write request full data length: %td bytes.", calculated_request_size);

        /* debug: dump request data */
        pdebug(DEBUG_DETAIL, "DH+ PCCC bit write request data:");
        pdebug_dump_bytes(DEBUG_DETAIL, req->data, (int)calculated_request_size);

        /* debug: CIP data length */
        ptrdiff_t cip_request_size = (ptrdiff_t)(data - embed_start);
        pdebug(DEBUG_DETAIL, "DH+ PCCC bit write request CIP data length: %td bytes.", cip_request_size);

        /* fill in DH+ fields */
        pccc_cmd->dest_link = h2le16(0);
        pccc_cmd->dest_node = h2le16(tag->session->dhp_dest);
        pccc_cmd->src_link = h2le16(0);
        pccc_cmd->src_node = h2le16(0);

        /* fill in PCCC command fields */
        pccc_cmd->pccc_command = AB_EIP_PCCC_TYPED_CMD;
        pccc_cmd->pccc_status = 0;
        pccc_cmd->pccc_seq_num = h2le16(conn_seq_id);
        pccc_cmd->pccc_function = AB_EIP_PLC5_RMW_FUNC;

        /* fill in Common Packet Format fields */
        cip_req->cpf_item_count = h2le16(2);
        cip_req->cpf_cai_item_type = h2le16(AB_EIP_ITEM_CAI);
        cip_req->cpf_cai_item_length = h2le16(4);
        cip_req->cpf_targ_conn_id = h2le32(tag->session->targ_connection_id);
        cip_req->cpf_cdi_item_type = h2le16(AB_EIP_ITEM_CDI);
        cip_req->cpf_conn_seq_num = h2le16(conn_seq_id);
        cip_req->cpf_cdi_item_length = h2le16((uint16_t)((size_t)cip_request_size + sizeof(cip_req->cpf_conn_seq_num)));

        pdebug(DEBUG_DETAIL, "DH+ PCCC bit write request CPF CDI item length: %u bytes.", le2h16(cip_req->cpf_cdi_item_length));

        cip_req->router_timeout = h2le16(1);
        cip_req->encap_command = h2le16(AB_EIP_CONNECTED_SEND);

        /* set request size */
        req->request_size = (int)calculated_request_size;
        pdebug(DEBUG_DETAIL, "DH+ PCCC bit write request size set to %d bytes.", req->request_size);

        /* add request to session */
        rc = session_add_request(tag->session, req);
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to add bit write request to session! rc=%d", rc);
            break;
        }

        rc = PLCTAG_STATUS_OK;
    } while(0);

    /* set tag->req if successful, else clean up */
    if(rc == PLCTAG_STATUS_OK) {
        critical_block(tag->api_mutex) {
            if(tag->req) {
                pdebug(DEBUG_WARN, "Request already set! This should not happen!");
                rc = PLCTAG_ERR_BAD_DATA;
            } else {
                pdebug(DEBUG_INFO, "Setting bit write request for tag %d", tag->tag_id);
                tag->req = req;
                rc = PLCTAG_STATUS_PENDING;
            }
        }
    } else {
        pdebug(DEBUG_WARN, "Failed to generate new bit write request rc=%s", plc_tag_decode_error(rc));
        req = rc_dec(req);
        tag->write_in_progress = 0;
        tag->write_complete = 1;
        ab_tag_abort_request(tag);
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");
    return rc;
}


/*
 * check_write_status
 *
 * Fragments are not supported.
 */
int pccc_dhp_check_write_status(ab_tag_p tag) {
    pccc_resp *pccc;
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_SPEW, "Starting.");

    /* the request reference is valid. */

    pccc = (pccc_resp *)(tag->req->data);

    /* fake exception */
    do {
        if(pccc->general_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d", pccc->general_status);
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        if(pccc->pccc_status != AB_EIP_OK) {
            pdebug(DEBUG_WARN, "PCCC command failed, response code: %d - %s", pccc->pccc_status,
                   pccc_decode_error(&pccc->pccc_status));
            rc = PLCTAG_ERR_REMOTE_ERR;
            break;
        }

        tag->write_in_progress = 0;
        tag->write_complete = 1;

        rc = PLCTAG_STATUS_OK;
    } while(0);

    ab_tag_abort_request(tag);

    pdebug(DEBUG_SPEW, "Done with status %s.", plc_tag_decode_error(rc));

    /* Success! */
    return rc;
}
