/***************************************************************************
 *   Copyright (C) 2015 by OmanTek                                         *
 *   Author Kyle Hayes  kylehayes@omantek.com                              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include <string.h>
#include <ctype.h>
#include <lib/libplctag.h>
#include <lib/tag.h>
#include <platform.h>
#include <ab/ab_common.h>
#include <ab/pccc.h>
#include <util/debug.h>



static int parse_pccc_logical_address(const char *name, pccc_file_t *file_type, int *file_num, int *elem_num, int *sub_elem_num);
static int parse_pccc_file_type(const char **str, pccc_file_t *file_type);
static int parse_pccc_file_num(const char **str, int *file_num);
static int parse_pccc_elem_num(const char **str, int *elem_num);
static int parse_pccc_subelem_num(const char **str, pccc_file_t file_type, int *subelem_num);
static void encode_data(uint8_t *data, int *index, int val);
static int encode_file_type(pccc_file_t file_type);




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
 */


/*
 * Encode the logical address as a level encoding for use with PLC/5 PLCs.
 *
 * Byte Meaning
 * 0    level flags
 * 1-3  level one
 * 1-3  level two
 * 1-3  level three
 */

int plc5_encode_tag_name(uint8_t *data, int *size, pccc_file_t *file_type, const char *name, int max_tag_name_size)
{
    int rc = PLCTAG_STATUS_OK;
    uint8_t level_byte = 0;
    int file_num = 0;
    int elem_num = 0;
    int subelem_num = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!data || !size || !name) {
        pdebug(DEBUG_WARN, "Called with null data, or name or zero sized data!");
        return PLCTAG_ERR_NULL_PTR;
    }

    *size = 0;
    *file_type = PCCC_FILE_UNKNOWN;

    if((rc = parse_pccc_logical_address(name, file_type, &file_num, &elem_num, &subelem_num)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to parse PCCC logical addresss!");
        return rc;
    }

    /* check for space. */
    if(max_tag_name_size < (1 + 3 + 3 + 3)) {
        pdebug(DEBUG_WARN,"Encoded PCCC logical address buffer is too small!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* allocate space for the level byte */
    *size = 1;

    /* do the required levels.  Remember we start at the low bit! */
    level_byte = 0x06; /* level one and two */

    /* add in the data file number. */
    encode_data(data, size, file_num);

    /* add in the element number */
    encode_data(data, size, elem_num);

    /* check to see if we need to put in a subelement. */
    if(subelem_num >= 0) {
        level_byte |= 0x08;

        encode_data(data, size, subelem_num);
    }

    /* store the encoded levels. */
    data[0] = level_byte;

    pdebug(DEBUG_DETAIL,"Done.");

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

int slc_encode_tag_name(uint8_t *data, int *size, pccc_file_t *file_type, const char *name, int max_tag_name_size)
{
    int rc = PLCTAG_STATUS_OK;
    int file_num = 0;
    int encoded_file_type = 0;
    int elem_num = 0;
    int subelem_num = 0;

    pdebug(DEBUG_DETAIL, "Starting.");

    if(!data || !size || !name) {
        pdebug(DEBUG_WARN, "Called with null data, or name or zero sized data!");
        return PLCTAG_ERR_NULL_PTR;
    }

    *size = 0;
    *file_type = PCCC_FILE_UNKNOWN;

    if((rc = parse_pccc_logical_address(name, file_type, &file_num, &elem_num, &subelem_num)) != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to parse SLC logical addresss!");
        return rc;
    }

    /* check for space. */
    if(max_tag_name_size < (3 + 1 + 3 + 3)) {
        pdebug(DEBUG_WARN,"Encoded SLC logical address buffer is too small!");
        return PLCTAG_ERR_TOO_SMALL;
    }

    encoded_file_type = encode_file_type(*file_type);
    if(encoded_file_type == 0) {
        pdebug(DEBUG_WARN,"SLC file type %d cannot be decoded!", *file_type);
        return PLCTAG_ERR_BAD_PARAM;
    }

    /* encode the file number */
    encode_data(data, size, file_num);

    /* encode the data file type. */
    encode_data(data, size, encoded_file_type);

    /* add in the element number */
    encode_data(data, size, elem_num);

    /* add in the sub-element number */
    encode_data(data, size, (subelem_num < 0 ? 0 : subelem_num));

    pdebug(DEBUG_DETAIL,"Done.");

    return PLCTAG_STATUS_OK;
}







uint8_t pccc_calculate_bcc(uint8_t *data,int size)
{
    int bcc = 0;
    int i;

    for(i = 0; i < size; i++)
        bcc += data[i];

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
    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
    0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
    0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
    0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
    0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
    0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
    0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
    0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
    0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
    0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
    0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
    0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
    0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
    0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
    0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
    0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
    0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
    0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
    0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
    0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
    0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
    0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
    0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
    0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
    0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
    0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
    0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
    0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
    0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
    0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
    0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
    0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
};


uint16_t pccc_calculate_crc16(uint8_t *data, int size)
{
    uint16_t running_crc = 0;
    int i;

    /* for each byte in the data... */
    for(i=0; i < size; i++) {
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






const char *pccc_decode_error(int error)
{
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
        return "Wait ACK (1775-KA buffer full).";  /* why is this duplicate? */
        break;

    default:
        return "Unknown error response.";
        break;
    }


    return "Unknown error response.";
}





/*
 * FIXME This does not check for data overruns!
 */

uint8_t *pccc_decode_dt_byte(uint8_t *data,int data_size, int *pccc_res_type, int *pccc_res_length)
{
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
    d_type = (((*data) & 0xF0)>>4);
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

        if(size_bytes > 4) {
            return NULL;
        }

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

        if(size_bytes > 4) {
            return NULL;
        }

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





int pccc_encode_dt_byte(uint8_t *data,int buf_size, uint32_t data_type, uint32_t data_size)
{
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
        data_type =0;
    } else {
        size_bytes=0;

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
    if(buf_size == 0 || data_type != 0 || data_size != 0)
        return 0;


    return (int)(data - dt_byte);
}





/*
 * Helper functions
 */




static int parse_pccc_logical_address(const char *name, pccc_file_t *file_type, int *file_num, int *elem_num, int *subelem_num)
{
    int rc = PLCTAG_STATUS_OK;
    const char *p = name;

    pdebug(DEBUG_DETAIL, "Starting.");

    do {
        if((rc = parse_pccc_file_type(&p, file_type)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for data-table type! Error %s!", plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_pccc_file_num(&p, file_num)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for file number! Error %s!", plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_pccc_elem_num(&p, elem_num)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for element number! Error %s!", plc_tag_decode_error(rc));
            break;
        }

        if((rc = parse_pccc_subelem_num(&p, *file_type, subelem_num)) != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to parse PCCC-style tag for subelement number! Error %s!", plc_tag_decode_error(rc));
            break;
        }
    } while(0);

    pdebug(DEBUG_DETAIL, "Starting.");

    return rc;
}



int parse_pccc_file_type(const char **str, pccc_file_t *file_type)
{
    int rc = PLCTAG_STATUS_OK;

    switch((*str)[0]) {
    case 'A':
    case 'a': /* ASCII */
        *file_type = PCCC_FILE_ASCII;
        (*str)++;
        break;

    case 'B':
    case 'b': /* Bit or block transfer */
        if(isdigit((*str)[1])) {
            /* Bit */
            *file_type = PCCC_FILE_BIT;
            (*str)++;
            break;
        } else {
            if((*str)[1] == 'T' || (*str)[1] == 't') {
                /* block transfer */
                *file_type = PCCC_FILE_BLOCK_TRANSFER;
                (*str) += 2;  /* skip past both characters */
            } else {
                *file_type = PCCC_FILE_UNKNOWN;
                pdebug(DEBUG_WARN, "Bad format or unsupported logical address, expected B or BT!");
                rc = PLCTAG_ERR_BAD_PARAM;
            }
        }

        break;

    case 'C':
    case 'c': /* Counter */
        *file_type = PCCC_FILE_COUNTER;
        (*str)++;
        break;

    case 'D':
    case 'd': /* BCD number */
        *file_type = PCCC_FILE_BCD;
        (*str)++;
        break;

    case 'F':
    case 'f': /* Floating point Number */
        *file_type = PCCC_FILE_FLOAT;
        (*str)++;
        break;

    case 'I':
    case 'i': /* Input */
        *file_type = PCCC_FILE_INPUT;
        (*str)++;
        break;

    case 'M':
    case 'm': /* Message */
        if((*str)[1] == 'G' || (*str)[1] == 'g') {
            *file_type = PCCC_FILE_MESSAGE;
            (*str) += 2;  /* skip past both characters */
        } else {
            *file_type = PCCC_FILE_UNKNOWN;
            pdebug(DEBUG_WARN, "Bad format or unsupported logical address, expected MG!");
            rc = PLCTAG_ERR_BAD_PARAM;
        }

        break;

    case 'N':
    case 'n': /* Input */
        *file_type = PCCC_FILE_INT;
        (*str)++;
        break;

    case 'O':
    case 'o': /* Output */
        *file_type = PCCC_FILE_OUTPUT;
        (*str)++;
        break;

    case 'P':
    case 'p': /* PID */
        if((*str)[1] == 'D' || (*str)[1] == 'd') {
            *file_type = PCCC_FILE_PID;
            (*str) += 2;  /* skip past both characters */
        } else {
            *file_type = PCCC_FILE_UNKNOWN;
            pdebug(DEBUG_WARN, "Bad format or unsupported logical address, expected PD!");
            rc = PLCTAG_ERR_BAD_PARAM;
        }

        break;

    case 'R':
    case 'r': /* Control */
        *file_type = PCCC_FILE_CONTROL;
        (*str)++;
        break;

    case 'S':
    case 's': /* Status, SFC or String */
        if(isdigit((*str)[1])) {
            /* Status */
            *file_type = PCCC_FILE_STATUS;
            (*str)++;
            break;
        } else {
            if((*str)[1] == 'C' || (*str)[1] == 'c') {
                /* SFC */
                *file_type = PCCC_FILE_SFC;
                (*str) += 2;  /* skip past both characters */
            } else if((*str)[1] == 'T' || (*str)[1] == 't') {
                /* String */
                *file_type = PCCC_FILE_STRING;
                (*str) += 2;  /* skip past both characters */
            } else {
                *file_type = PCCC_FILE_UNKNOWN;
                pdebug(DEBUG_WARN, "Bad format or unsupported logical address, expected string, SFC or status!");
                rc = PLCTAG_ERR_BAD_PARAM;
            }
        }

        break;

    case 'T':
    case 't': /* Timer */
        *file_type = PCCC_FILE_TIMER;
        (*str)++;
        break;

    default:
        pdebug(DEBUG_WARN, "Bad format or unsupported logical address %s!", *str);
        *file_type = PCCC_FILE_UNKNOWN;
        rc = PLCTAG_ERR_BAD_PARAM;
        break;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}



int parse_pccc_file_num(const char **str, int *file_num)
{
    int tmp = 0;

    pdebug(DEBUG_DETAIL,"Starting.");

    if(!str || !*str || !isdigit(**str)) {
        pdebug(DEBUG_WARN,"Expected data-table file number!");
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



int parse_pccc_elem_num(const char **str, int *elem_num)
{
    int tmp = 0;

    pdebug(DEBUG_DETAIL,"Starting.");

    if(!str || !*str || **str != ':') {
        pdebug(DEBUG_WARN,"Expected data-table element number!");
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



int parse_pccc_subelem_num(const char **str, pccc_file_t file_type, int *subelem_num)
{
    int tmp = 0;

    pdebug(DEBUG_DETAIL,"Starting.");

    if(!str || !*str) {
        pdebug(DEBUG_WARN,"Called with bad string pointer!");
        return PLCTAG_ERR_BAD_PARAM;
    }

    /*
     * if we have a null character we are at the end of the name
     * and the subelement is not there.  That is not an error.
     */

    if( (**str) == 0) {
        pdebug(DEBUG_DETAIL, "No subelement in this name.");
        *subelem_num = -1;
        return PLCTAG_STATUS_OK;
    }

    /*
     * We do have a character.  It must be . or / to be valid.
     * The . character is valid before a mnemonic for a field in a structured type.
     * The / character is valid before a bit number.
     */

    /* make sure the next character is either / or . and nothing else. */
    if((**str) != '/' && (**str) != '.') {
        pdebug(DEBUG_WARN, "Bad subelement field in logical address.");
        return PLCTAG_ERR_BAD_PARAM;
    }

    if((**str) == '/') {
        /* bit number. */

        /* step past the / character */
        (*str)++;

        /* FIXME - we do this a lot, should be a small routine. */
        while(**str && isdigit(**str) && tmp < 65535) {
            tmp *= 10;
            tmp += (int)((**str) - '0');
            (*str)++;
        }

        *subelem_num = tmp;

        pdebug(DEBUG_DETAIL, "Done.");

        return PLCTAG_STATUS_OK;
    } else {
        /* mnemonic. */

        /* step past the . character */
        (*str)++;

        /* this depends on the data-table file type. */
        switch(file_type) {
        case PCCC_FILE_BLOCK_TRANSFER:
            if(str_cmp_i(*str,"con") == 0) {
                *subelem_num = 0;
            } else if(str_cmp_i(*str,"rlen") == 0) {
                *subelem_num = 1;
            } else if(str_cmp_i(*str,"dlen") == 0) {
                *subelem_num = 2;
            } else if(str_cmp_i(*str,"df") == 0) {
                *subelem_num = 3;
            } else if(str_cmp_i(*str,"elem") == 0) {
                *subelem_num = 4;
            } else if(str_cmp_i(*str,"rgs") == 0) {
                *subelem_num = 5;
            } else {
                pdebug(DEBUG_WARN,"Unsupported block transfer mnemonic!");
                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        case PCCC_FILE_COUNTER:
        case PCCC_FILE_TIMER:
            if(str_cmp_i(*str,"con") == 0) {
                *subelem_num = 0;
            } else if(str_cmp_i(*str,"pre") == 0) {
                *subelem_num = 1;
            } else if(str_cmp_i(*str,"acc") == 0) {
                *subelem_num = 2;
            } else {
                pdebug(DEBUG_WARN,"Unsupported %s mnemonic!", (file_type == PCCC_FILE_COUNTER ? "counter" : "timer"));
                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        case PCCC_FILE_CONTROL:
            if(str_cmp_i(*str,"con") == 0) {
                *subelem_num = 0;
            } else if(str_cmp_i(*str,"len") == 0) {
                *subelem_num = 1;
            } else if(str_cmp_i(*str,"pos") == 0) {
                *subelem_num = 2;
            } else {
                pdebug(DEBUG_WARN,"Unsupported control mnemonic!");
                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        case PCCC_FILE_PID:
            if(str_cmp_i(*str,"con") == 0) {
                *subelem_num = 0;
            } else if(str_cmp_i(*str,"sp") == 0) {
                *subelem_num = 2;
            } else if(str_cmp_i(*str,"kp") == 0) {
                *subelem_num = 4;
            } else if(str_cmp_i(*str,"ki") == 0) {
                *subelem_num = 6;
            } else if(str_cmp_i(*str,"kd") == 0) {
                *subelem_num = 8;
            } else if(str_cmp_i(*str,"pv") == 0) {
                *subelem_num = 26;
            } else {
                pdebug(DEBUG_WARN,"Unsupported PID mnemonic!");
                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        case PCCC_FILE_MESSAGE:
            if(str_cmp_i(*str,"con") == 0) {
                *subelem_num = 0;
            } else if(str_cmp_i(*str,"err") == 0) {
                *subelem_num = 1;
            } else if(str_cmp_i(*str,"rlen") == 0) {
                *subelem_num = 2;
            } else if(str_cmp_i(*str,"dlen") == 0) {
                *subelem_num = 3;
            } else {
                pdebug(DEBUG_WARN,"Unsupported message mnemonic!");
                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        case PCCC_FILE_STRING:
            if(str_cmp_i(*str,"len") == 0) {
                *subelem_num = 0;
            } else if(str_cmp_i(*str,"data") == 0) {
                *subelem_num = 1;
            } else {
                pdebug(DEBUG_WARN,"Unsupported string mnemonic!");
                return PLCTAG_ERR_BAD_PARAM;
            }

            break;

        default:
            pdebug(DEBUG_WARN, "Unsupported mnemonic %s!", *str);
            return PLCTAG_ERR_BAD_PARAM;
            break;
        }
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return PLCTAG_STATUS_OK;
}


void encode_data(uint8_t *data, int *index, int val)
{
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





int encode_file_type(pccc_file_t file_type)
{
    switch(file_type) {
        case PCCC_FILE_ASCII: return 0x8e; break;
        case PCCC_FILE_BIT: return 0x85; break;
        case PCCC_FILE_BLOCK_TRANSFER: break;
        case PCCC_FILE_COUNTER: return 0x87; break;
        case PCCC_FILE_BCD: return 0x8f; break;
        case PCCC_FILE_FLOAT: return 0x8a; break;
        case PCCC_FILE_INPUT: return 0x8c; break;
        case PCCC_FILE_MESSAGE: break;
        case PCCC_FILE_INT: return 0x89; break;
        case PCCC_FILE_OUTPUT: return 0x8b; break;
        case PCCC_FILE_PID: break;
        case PCCC_FILE_CONTROL: return 0x88; break;
        case PCCC_FILE_STATUS: return 0x84; break;
        case PCCC_FILE_SFC: break;
        case PCCC_FILE_STRING: return 0x8d; break;
        case PCCC_FILE_TIMER: return 0x86; break;
        default:
             return 0x00;
             break;
    }

    return 0x00;
}

