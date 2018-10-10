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

/**************************************************************************
 * CHANGE LOG                                                             *
 *                                                                        *
 * 2013-11-19  KRH - Created file.                                        *
 **************************************************************************/

#include <string.h>
#include <ctype.h>
#include <lib/libplctag.h>
#include <lib/tag.h>
#include <platform.h>
#include <ab/ab_common.h>
#include <ab/pccc.h>
#include <util/debug.h>


/*
 * convert a string of digits to a number and encode that.  Only numbers as high as 65535 (?) can
 * be encoded like this.
 */

static const char *parse_pccc_name_number(const char *name, uint8_t *data, int *size)
{
    uint32_t tmp = 0;

    if(!name || !*name || !isdigit(*name)) {
        return NULL;
    }

    while(*name && isdigit(*name) && tmp < 65535) {
        tmp *= (uint32_t)10;
        tmp += (uint32_t)(*name - '0');
        name++;
    }

    if(tmp <= 254) {
        data[*size] = (uint8_t)tmp;
        *size = *size + 1;
    } else {
        data[*size] = (uint8_t)0xff;
        data[*size + 1] = (uint8_t)(tmp & 0xff);
        data[*size + 2] = (uint8_t)((tmp >> 8) & 0xff);
        *size = *size + 3;
    }

    return name;
}

/*
 * Encode the name as a level encoding.
 *
 * Byte Meaning
 * 0    level flags
 * 1-3  level one
 * 1-3  level two
 * 1-3  level three
 */

int pccc_encode_tag_name(uint8_t *data, int *size, const char *name, int max_tag_name_size)
{
    const char *tmp = name;
    uint8_t *level_byte = data;

    if(!data || !size || !name) {
        return 0;
    }

    *size = 0;

    if(!strlen(name)) {
        return 0;
    }

    if(max_tag_name_size < 10) {
        return 0;
    }

    /* get the data type */
    tmp = name;

    //pdebug(DEBUG_DETAIL,"starting to parse name %s",tmp);

    /* skip to the first number */
    while(*tmp && !isdigit(*tmp)) {
        //pdebug(DEBUG_DETAIL,"skipping character '%c'",*tmp);
        tmp++;
    }

    /* allocate space for the level byte */
    *size = 1;

    /* the next thing is the file number, and we parse that into up to three bytes. */
    tmp = parse_pccc_name_number(tmp, data, size);

    if(!tmp) {
        /* oops, bad parse! */
        *size = 0;
        return 0;
    }

    *level_byte |= 0x02;

    if(*tmp != ':') {
        /* bad parse! */
        pdebug(DEBUG_WARN,"bad parse of name, failed to fine : after data file number.");
        *size = 0;
        return 0;
    }

    /* bump past the : character */
    ++tmp;

    /* now read the element number */
    tmp = parse_pccc_name_number(tmp, data, size);
    if(!tmp) {
        /* oops, bad parse! */
        *size = 0;
        return 0;
    }

    *level_byte |= 0x04;

    /*
     * if there is a trailing part, it is something like .ACC, so convert that
     * into the third level, sub-element, of the address/name.
     */

    if(strlen(tmp) > 0 && (*tmp == '/' || *tmp == '.')) {
        uint8_t sub_element=255;

        /* bump past the / or . character */
        ++tmp;

        /*
         * If there is remaining data, it might be a special
         * field name.  Or it might be a bit number.
         *
         * The fields are:
         *
         * Timer/Counter
         * Offset   Field
         * 0        con - control
         * 1        pre - preset
         * 2        acc - accumulated
         *
         * Control
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
<<<<<<< HEAD
         * The following are NOT handled correctly!
         *
=======
>>>>>>> Added additional handleing to PCCC/DF1/CSP tags.
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

        /* test the remaining part of the name */
        if(!str_cmp_i(tmp,"con")) {
            sub_element = 0;
        } else if(!str_cmp_i(tmp,"pre")) {
            sub_element = 1;
        } else if(!str_cmp_i(tmp,"acc")) {
            sub_element = 2;
        } else if(!str_cmp_i(tmp, "len")) {
            sub_element = 1;
        } else if(!str_cmp_i(tmp, "pos")) {
            sub_element = 2;
        } else if(!str_cmp_i(tmp, "sp")) {
            sub_element = 2;
        } else if(!str_cmp_i(tmp, "kp")) {
            sub_element = 4;
        } else if(!str_cmp_i(tmp, "ki")) {
            sub_element = 6;
        } else if(!str_cmp_i(tmp, "kd")) {
            sub_element = 8;
        } else if(!str_cmp_i(tmp, "pv")) {
            sub_element = 26;
        } else if(!str_cmp_i(tmp, "rlen")) {
            sub_element = 1;
        } else if(!str_cmp_i(tmp, "dlen")) {
            sub_element = 2;
        } else if(!str_cmp_i(tmp, "df")) {
            sub_element = 3;
        } else if(!str_cmp_i(tmp, "elem")) {
            sub_element = 4;
        } else if(!str_cmp_i(tmp, "rgs")) {
            sub_element = 5;
        } else if(!str_cmp_i(tmp, "err")) {
            sub_element = 1;
            /* FIXME - missing RLEN and DLEN for MG! */
        } else {
            /* FIXME - what to do here? */
            tmp = parse_pccc_name_number(tmp, data, size);
            if(!tmp) {
                /* oops, bad parse! */
                pdebug(DEBUG_WARN, "Unable to correctly parse PLC/5-style name! %s", name);
                *size = 0;
                return 0;
            }

            /* we do have a fourth element */
            *level_byte |= 0x08;

            /* guard against the code below */
            sub_element = 255;
        }

        if(sub_element != 255) {
            data[*size] = sub_element;
            *size = *size + 1;
            *level_byte |= 0x08;
        }
    }

    return 1;
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
    uint8_t running_byte = 0;
    int i;

    /* for each byte in the data... */
    for(i=0; i < size; i++) {
        /* calculate the running byte.  This is a lot like
         * a CBC.  You keep the running value as you go along
         * and the table is precalculated to have all the right
         * 256 values.
         */

        /* mask the CRC and XOR with the data byte */
        running_byte = (uint8_t)(running_crc & 0x00FF) ^ data[i];

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
        return "Shutdown could not be executed.";
        break;

    case 0x0F:
        return "Requester does not have upload or download access - no privilege.";
        break;

    case 0x10:
        return "Histogram overflow.";
        break;

    case 0x11:
        return "Illegal data type.";
        break;

    case 0x12:
        return "Bad parameter.";
        break;

    case 0x13:
        return "Address reference exists to deleted data table.";
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
