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


#include "pccc.h"
#include "cip.h"
#include "fault.h"
#include "mutex.h"
#include "plc.h"
#include "utils.h"
#include "log.h"
#include <utils/byte_slice.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* the PCCC command inside Execute PCCC, and its reply: the command with bit 6 set. */
#define PCCC_CMD_EXECUTE ((uint8_t)0x0f)
#define PCCC_CMD_REPLY ((uint8_t)0x4f)
#define PCCC_REPLY_HEADER_SIZE ((int32_t)4) /* command, status, TNS */

/* reply status: 0 is success, 0xf0 means an extended status byte follows the TNS. */
#define PCCC_STS_OK ((uint8_t)0x00)
#define PCCC_STS_EXTENDED ((uint8_t)0xf0)

/* extended status codes. */
#define PCCC_ERR_ADDR_NOT_USABLE ((uint8_t)0x06)
#define PCCC_ERR_FILE_IS_WRONG_SIZE ((uint8_t)0x07)
#define PCCC_ERR_UNSUPPORTED_COMMAND ((uint8_t)0x0e)

/* function codes. */
#define PLC5_READ ((uint8_t)0x01)
#define PLC5_WRITE ((uint8_t)0x00)
#define PLC5_RMW ((uint8_t)0x26)
#define SLC_READ ((uint8_t)0xa2)
#define SLC_WRITE ((uint8_t)0xaa)
#define SLC_RMW ((uint8_t)0xab)

/*
 * The PCCC mapping inside a ControlLogix has its own typed read/write pair.  They carry a
 * PLC/5-style logical address but count in elements rather than words, and the data is wrapped
 * in PCCC type prefixes.  See eip_lgx_pccc.c in the library.
 */
#define LGX_READ ((uint8_t)0x68)
#define LGX_WRITE ((uint8_t)0x67)

/* PCCC typed-data type codes, as used in the DT byte below. */
#define PCCC_DATA_BIT_STRING ((uint8_t)0x02)
#define PCCC_DATA_BYTE_STRING ((uint8_t)0x03)
#define PCCC_DATA_INT ((uint8_t)0x04)
#define PCCC_DATA_REAL ((uint8_t)0x08)
#define PCCC_DATA_ARRAY ((uint8_t)0x09)

/* a PLC/5 logical address with levels one (file) and two (element); no sub-element. */
#define PLC5_ADDR_LEVELS ((uint8_t)0x06)

/* an address field over 254 is 0xFF and then the value as a u16 LE. */
#define PCCC_ADDR_WIDE ((uint8_t)0xff)

/*
 * A PLC/5 range read/write counts its transfer size and offset in 16-bit words -- see
 * plc5_pccc_read_cmd_req in the library's pccc.c, which sends tag->size / 2.  The SLC commands
 * count bytes instead, and the Logix-mapped ones count elements.
 */
#define PLC5_WORD_SIZE ((size_t)2)

/* the most data a reply can carry; the Logix-mapped reply leaves room for its type prefixes. */
#define PCCC_MAX_DATA ((size_t)240)
#define LGX_MAX_DATA ((size_t)230)

/* the SLC masked write is one 16-bit word: a mask and the data, two bytes each. */
#define SLC_RMW_SIZE ((size_t)2)

/*
 * A command handler reads the request after the function code and writes reply data into
 * *data, leaving *data at what is left.  It returns 0 or a PCCC extended status code.
 */
typedef uint8_t (*pccc_handler_t)(byte_slice_t in, byte_slice_t *data, plc_s *plc);

static uint8_t plc5_read(byte_slice_t in, byte_slice_t *data, plc_s *plc);
static uint8_t plc5_write(byte_slice_t in, byte_slice_t *data, plc_s *plc);
static uint8_t plc5_rmw(byte_slice_t in, byte_slice_t *data, plc_s *plc);
static uint8_t slc_read(byte_slice_t in, byte_slice_t *data, plc_s *plc);
static uint8_t slc_write(byte_slice_t in, byte_slice_t *data, plc_s *plc);
static uint8_t slc_rmw(byte_slice_t in, byte_slice_t *data, plc_s *plc);
static uint8_t lgx_read(byte_slice_t in, byte_slice_t *data, plc_s *plc);
static uint8_t lgx_write(byte_slice_t in, byte_slice_t *data, plc_s *plc);
static pccc_handler_t find_handler(plc_type_t plc_type, uint8_t fnc);
static byte_slice_t read_addr_field(byte_slice_t in, uint32_t *val);
static byte_slice_t read_plc5_header(byte_slice_t in, uint16_t *offset, uint16_t *transfer_size, uint8_t *levels, uint32_t *file,
                                     uint32_t *element);
static byte_slice_t read_slc_header(byte_slice_t in, uint8_t *transfer_size, uint32_t *file, uint32_t *file_type, uint32_t *element,
                                    uint32_t *sub_element);
static uint8_t find_plc5_file(byte_slice_t in, uint8_t levels, uint32_t file, plc_s *plc, tag_def_s **tag);
static uint8_t find_slc_file(byte_slice_t in, uint32_t file, uint32_t file_type, uint32_t sub_element, plc_s *plc, tag_def_s **tag);
static uint8_t check_range(tag_def_s *tag, size_t start, size_t end, size_t max_data);
static byte_slice_t write_dt_byte(byte_slice_t out, uint8_t data_type, size_t data_size);
static byte_slice_t read_dt_byte(byte_slice_t in, uint8_t *data_type, size_t *data_size);
static uint8_t lgx_pccc_data_type(tag_def_s *tag);

static const struct {
    plc_type_t plc_type;
    uint8_t fnc;
    pccc_handler_t handler;
} pccc_commands[] = {
    {PLC_PLC5, PLC5_READ, plc5_read},          {PLC_PLC5, PLC5_WRITE, plc5_write},
    {PLC_PLC5, PLC5_RMW, plc5_rmw},            {PLC_SLC, SLC_READ, slc_read},
    {PLC_SLC, SLC_WRITE, slc_write},           {PLC_SLC, SLC_RMW, slc_rmw},
    {PLC_MICROLOGIX, SLC_READ, slc_read},      {PLC_MICROLOGIX, SLC_WRITE, slc_write},
    {PLC_MICROLOGIX, SLC_RMW, slc_rmw},        {PLC_LGX_PCCC, LGX_READ, lgx_read},
    {PLC_LGX_PCCC, LGX_WRITE, lgx_write},
};


/*
 * Execute PCCC (0x4B on class 0x67).  The payload is the requestor ID (its own length byte, a
 * vendor ID and a serial number) and then the PCCC command: 0x0F, status, TNS, function code
 * and the function's own fields.  The reply echoes the requestor ID, then 0x4F, a status, the
 * TNS and the data, or 0xF0 and an extended status byte after the TNS.
 */
byte_slice_t pccc_dispatch_request(uint8_t service, byte_slice_t payload, byte_slice_t out, plc_s *plc) {
    byte_slice_t in = payload;
    byte_slice_t requestor = {0};
    byte_slice_t reply = {0};
    byte_slice_t pccc_header = {0};
    byte_slice_t data = {0};
    byte_slice_t rest = {0};
    uint8_t requestor_len = 0;
    uint8_t cmd = 0;
    uint8_t sts = 0;
    uint8_t fnc = 0;
    uint16_t tns = 0;
    pccc_handler_t handler = NULL;
    uint8_t err = 0;

    log_info("PCCC request:");
    log_info_byte_slice(payload);

    byte_slice_read_u8(in, &requestor_len); /* the length counts its own byte */
    in = byte_slice_take(in, requestor_len, &requestor);
    in = byte_slice_read_u8(in, &cmd);
    in = byte_slice_read_u8(in, &sts);
    in = byte_slice_read_u16_le(in, &tns);
    in = byte_slice_read_u8(in, &fnc);

    if(byte_slice_has_error(in) || requestor_len == 0) {
        log_info("PCCC request is too short for its requestor ID and command header!");
        return cip_write_reply_header(out, service, CIP_ERR_INSUFFICIENT_DATA, false, 0);
    }

    if(cmd != PCCC_CMD_EXECUTE || sts != PCCC_STS_OK) {
        log_info("Unsupported PCCC command %02x with status %02x!", cmd, sts);
        err = PCCC_ERR_UNSUPPORTED_COMMAND;
    } else if(!(handler = find_handler(plc->plc_type, fnc))) {
        log_info("Unsupported PCCC function %02x for this PLC type!", fnc);
        err = PCCC_ERR_UNSUPPORTED_COMMAND;
    }

    reply = cip_write_reply_header(out, service, CIP_OK, false, 0);
    reply = byte_slice_write_bytes(reply, byte_slice_get_data(requestor), byte_slice_len(requestor));
    data = byte_slice_take(reply, PCCC_REPLY_HEADER_SIZE, &pccc_header);

    rest = data;
    if(!err) { err = handler(in, &rest, plc); }
    if(err) { rest = byte_slice_write_u8(data, err); }

    /* fault injection: corrupt the TNS, or answer a different CIP service.  See FAULT_PCCC_*. */
    if(fault_fires(plc, FAULT_PCCC_TNS)) { tns = (uint16_t)(tns ^ (uint16_t)0xFFFF); }

    pccc_header = byte_slice_write_u8(pccc_header, PCCC_CMD_REPLY);
    pccc_header = byte_slice_write_u8(pccc_header, err ? PCCC_STS_EXTENDED : PCCC_STS_OK);
    pccc_header = byte_slice_write_u16_le(pccc_header, tns);

    if(fault_fires(plc, FAULT_PCCC_REPLY)) { byte_slice_write_u8(out, (uint8_t)0xce); }

    return byte_slice_has_error(pccc_header) ? pccc_header : rest;
}


static pccc_handler_t find_handler(plc_type_t plc_type, uint8_t fnc) {
    for(size_t i = 0; i < sizeof(pccc_commands) / sizeof(pccc_commands[0]); i++) {
        if(pccc_commands[i].plc_type == plc_type && pccc_commands[i].fnc == fnc) { return pccc_commands[i].handler; }
    }

    return NULL;
}


/* one byte, or 0xFF and a u16 LE for values over 254. */
static byte_slice_t read_addr_field(byte_slice_t in, uint32_t *val) {
    uint8_t narrow = 0;
    uint16_t wide = 0;

    in = byte_slice_read_u8(in, &narrow);
    if(narrow != PCCC_ADDR_WIDE) {
        *val = narrow;
        return in;
    }

    in = byte_slice_read_u16_le(in, &wide);
    *val = wide;

    return in;
}


/* PLC/5 and Logix-mapped commands: offset, transfer size, then the level byte, file and element. */
static byte_slice_t read_plc5_header(byte_slice_t in, uint16_t *offset, uint16_t *transfer_size, uint8_t *levels, uint32_t *file,
                                     uint32_t *element) {
    in = byte_slice_read_u16_le(in, offset);
    in = byte_slice_read_u16_le(in, transfer_size);
    in = byte_slice_read_u8(in, levels);
    in = read_addr_field(in, file);
    return read_addr_field(in, element);
}


/* SLC commands: transfer size in bytes, then file, file type, element and sub-element. */
static byte_slice_t read_slc_header(byte_slice_t in, uint8_t *transfer_size, uint32_t *file, uint32_t *file_type, uint32_t *element,
                                    uint32_t *sub_element) {
    in = byte_slice_read_u8(in, transfer_size);
    in = read_addr_field(in, file);
    in = read_addr_field(in, file_type);
    in = read_addr_field(in, element);
    return read_addr_field(in, sub_element);
}


static uint8_t find_plc5_file(byte_slice_t in, uint8_t levels, uint32_t file, plc_s *plc, tag_def_s **tag) {
    if(byte_slice_has_error(in)) {
        log_info("PLC/5 command is too short for its header and address!");
        return PCCC_ERR_FILE_IS_WRONG_SIZE;
    }

    if(levels != PLC5_ADDR_LEVELS) {
        log_info("Unsupported PLC/5 address levels %02x!", levels);
        return PCCC_ERR_ADDR_NOT_USABLE;
    }

    for(*tag = plc->tags; *tag && (*tag)->data_file_num != file; *tag = (*tag)->next_tag) {}

    if(!*tag) {
        log_info("Unable to find tag with data file %u!", file);
        return PCCC_ERR_ADDR_NOT_USABLE;
    }

    return 0;
}


static uint8_t find_slc_file(byte_slice_t in, uint32_t file, uint32_t file_type, uint32_t sub_element, plc_s *plc, tag_def_s **tag) {
    if(byte_slice_has_error(in)) {
        log_info("SLC command is too short for its header and address!");
        return PCCC_ERR_FILE_IS_WRONG_SIZE;
    }

    if(sub_element != 0) {
        log_info("Data file subelement is unsupported!");
        return PCCC_ERR_ADDR_NOT_USABLE;
    }

    for(*tag = plc->tags; *tag && (*tag)->data_file_num != file; *tag = (*tag)->next_tag) {}

    if(!*tag) {
        log_info("Unable to find tag with data file %u!", file);
        return PCCC_ERR_ADDR_NOT_USABLE;
    }

    if((*tag)->tag_type != file_type) {
        log_info("Data file type requested, %u, does not match file type of tag, %u!", file_type, (unsigned int)(*tag)->tag_type);
        return PCCC_ERR_ADDR_NOT_USABLE;
    }

    return 0;
}


static uint8_t check_range(tag_def_s *tag, size_t start, size_t end, size_t max_data) {
    size_t tag_size = tag->elem_count * tag->elem_size;

    log_info("Byte offsets %zu to %zu of %zu.", start, end, tag_size);

    if(start >= tag_size || end > tag_size) {
        log_info("Byte offsets %zu to %zu run past the tag's %zu bytes!", start, end, tag_size);
        return PCCC_ERR_FILE_IS_WRONG_SIZE;
    }

    if(end - start > max_data) {
        log_info("Transfer of %zu bytes does not fit in one reply!", end - start);
        return PCCC_ERR_FILE_IS_WRONG_SIZE;
    }

    return 0;
}


static uint8_t plc5_read(byte_slice_t in, byte_slice_t *data, plc_s *plc) {
    uint16_t offset = 0;
    uint16_t transfer_size = 0;
    uint8_t levels = 0;
    uint32_t file = 0;
    uint32_t element = 0;
    tag_def_s *tag = NULL;
    size_t start = 0;
    size_t end = 0;
    uint8_t err = 0;

    in = read_plc5_header(in, &offset, &transfer_size, &levels, &file, &element);
    if((err = find_plc5_file(in, levels, file, plc, &tag))) { return err; }

    start = (offset * PLC5_WORD_SIZE) + (element * tag->elem_size);
    end = start + (transfer_size * PLC5_WORD_SIZE);
    if((err = check_range(tag, start, end, PCCC_MAX_DATA))) { return err; }

    critical_block(tag->data_mutex) { *data = byte_slice_write_bytes(*data, tag->data + start, (int32_t)(end - start)); }

    return 0;
}


static uint8_t plc5_write(byte_slice_t in, byte_slice_t *data, plc_s *plc) {
    uint16_t offset = 0;
    uint16_t transfer_size = 0;
    uint8_t levels = 0;
    uint32_t file = 0;
    uint32_t element = 0;
    tag_def_s *tag = NULL;
    size_t start = 0;
    size_t end = 0;
    uint8_t err = 0;

    (void)data;

    in = read_plc5_header(in, &offset, &transfer_size, &levels, &file, &element);
    if((err = find_plc5_file(in, levels, file, plc, &tag))) { return err; }

    start = (offset * PLC5_WORD_SIZE) + (element * tag->elem_size);
    end = start + (transfer_size * PLC5_WORD_SIZE);
    if((err = check_range(tag, start, end, SIZE_MAX))) { return err; }

    if((size_t)byte_slice_len(in) != end - start) {
        log_info("Write carries %d bytes but the transfer is %zu!", byte_slice_len(in), end - start);
        return PCCC_ERR_FILE_IS_WRONG_SIZE;
    }

    critical_block(tag->data_mutex) { memcpy(tag->data + start, byte_slice_get_data(in), end - start); }

    return 0;
}


/*
 * Level byte, file and element, then an AND mask and an OR mask of one element each.  Nothing
 * follows the masks, so a longer packet is a malformed one and a real PLC refuses it.
 */
static uint8_t plc5_rmw(byte_slice_t in, byte_slice_t *data, plc_s *plc) {
    uint8_t levels = 0;
    uint32_t file = 0;
    uint32_t element = 0;
    tag_def_s *tag = NULL;
    byte_slice_t and_mask = {0};
    byte_slice_t or_mask = {0};
    size_t start = 0;
    uint8_t err = 0;

    (void)data;

    in = byte_slice_read_u8(in, &levels);
    in = read_addr_field(in, &file);
    in = read_addr_field(in, &element);
    if((err = find_plc5_file(in, levels, file, plc, &tag))) { return err; }

    start = element * tag->elem_size;
    if((err = check_range(tag, start, start + tag->elem_size, SIZE_MAX))) { return err; }

    in = byte_slice_take(in, (int32_t)tag->elem_size, &and_mask);
    in = byte_slice_take(in, (int32_t)tag->elem_size, &or_mask);

    if(byte_slice_has_error(byte_slice_end(in))) {
        log_info("PLC/5 RMW request does not end with exactly two %zu byte masks!", tag->elem_size);
        return PCCC_ERR_FILE_IS_WRONG_SIZE;
    }

    critical_block(tag->data_mutex) {
        for(size_t i = 0; i < tag->elem_size; i++) {
            uint8_t *byte = &tag->data[start + i];

            *byte = (uint8_t)((*byte & byte_slice_get_data(and_mask)[i]) | byte_slice_get_data(or_mask)[i]);
        }
    }

    return 0;
}


static uint8_t slc_read(byte_slice_t in, byte_slice_t *data, plc_s *plc) {
    uint8_t transfer_size = 0;
    uint32_t file = 0;
    uint32_t file_type = 0;
    uint32_t element = 0;
    uint32_t sub_element = 0;
    tag_def_s *tag = NULL;
    size_t start = 0;
    uint8_t err = 0;

    in = read_slc_header(in, &transfer_size, &file, &file_type, &element, &sub_element);
    if((err = find_slc_file(in, file, file_type, sub_element, plc, &tag))) { return err; }

    start = element * tag->elem_size;
    if((err = check_range(tag, start, start + transfer_size, PCCC_MAX_DATA))) { return err; }

    critical_block(tag->data_mutex) { *data = byte_slice_write_bytes(*data, tag->data + start, transfer_size); }

    return 0;
}


static uint8_t slc_write(byte_slice_t in, byte_slice_t *data, plc_s *plc) {
    uint8_t transfer_size = 0;
    uint32_t file = 0;
    uint32_t file_type = 0;
    uint32_t element = 0;
    uint32_t sub_element = 0;
    tag_def_s *tag = NULL;
    size_t start = 0;
    uint8_t err = 0;

    (void)data;

    in = read_slc_header(in, &transfer_size, &file, &file_type, &element, &sub_element);
    if((err = find_slc_file(in, file, file_type, sub_element, plc, &tag))) { return err; }

    start = element * tag->elem_size;
    if((err = check_range(tag, start, start + transfer_size, PCCC_MAX_DATA))) { return err; }

    if(byte_slice_len(in) != transfer_size) {
        log_info("Write carries %d bytes but the transfer is %u!", byte_slice_len(in), transfer_size);
        return PCCC_ERR_FILE_IS_WRONG_SIZE;
    }

    critical_block(tag->data_mutex) { memcpy(tag->data + start, byte_slice_get_data(in), transfer_size); }

    return 0;
}


/*
 * The SLC masked write: the header, then a two byte mask and two bytes of data, and nothing
 * after them.  A packet one byte too long used to be accepted, with the mask and data read
 * from the wrong offsets, while a real MicroLogix rejected it -- which is how a client bug
 * reached hardware with the simulator green.  A real PLC answers "file is wrong size".
 */
static uint8_t slc_rmw(byte_slice_t in, byte_slice_t *data, plc_s *plc) {
    uint8_t transfer_size = 0;
    uint32_t file = 0;
    uint32_t file_type = 0;
    uint32_t element = 0;
    uint32_t sub_element = 0;
    tag_def_s *tag = NULL;
    byte_slice_t mask = {0};
    byte_slice_t new_bits = {0};
    size_t start = 0;
    uint8_t err = 0;

    (void)data;

    in = read_slc_header(in, &transfer_size, &file, &file_type, &element, &sub_element);
    if((err = find_slc_file(in, file, file_type, sub_element, plc, &tag))) { return err; }

    if(transfer_size != SLC_RMW_SIZE || tag->elem_size != SLC_RMW_SIZE) {
        log_info("SLC RMW is one 2 byte word; transfer size %u, element size %zu!", transfer_size, tag->elem_size);
        return PCCC_ERR_FILE_IS_WRONG_SIZE;
    }

    start = element * tag->elem_size;
    if((err = check_range(tag, start, start + SLC_RMW_SIZE, SIZE_MAX))) { return err; }

    in = byte_slice_take(in, (int32_t)SLC_RMW_SIZE, &mask);
    in = byte_slice_take(in, (int32_t)SLC_RMW_SIZE, &new_bits);

    if(byte_slice_has_error(byte_slice_end(in))) {
        log_info("SLC RMW request does not end with exactly a 2 byte mask and 2 bytes of data!");
        return PCCC_ERR_FILE_IS_WRONG_SIZE;
    }

    /* bits outside the mask keep their value; bits inside it take the new data. */
    critical_block(tag->data_mutex) {
        for(size_t i = 0; i < SLC_RMW_SIZE; i++) {
            uint8_t m = byte_slice_get_data(mask)[i];
            uint8_t *byte = &tag->data[start + i];

            *byte = (uint8_t)((*byte & (uint8_t)~m) | (byte_slice_get_data(new_bits)[i] & m));
        }
    }

    return 0;
}


/*
 * PCCC typed data is prefixed with a "DT byte": the high nybble is the type and the low nybble
 * the size.  A nybble with bit 3 set means its low three bits are instead a count of following
 * bytes holding the real value.
 *
 * ponytail: only single extension bytes are generated and accepted.  A PCCC packet cannot
 * exceed 244 bytes, so nothing wider is reachable, and the library's own encoder and decoder
 * disagree on the byte order of wider values anyway (pccc.c:719 vs pccc.c:666).
 */
static byte_slice_t write_dt_byte(byte_slice_t out, uint8_t data_type, size_t data_size) {
    uint8_t type_nybble = (data_type <= 0x07) ? data_type : (uint8_t)0x09;
    uint8_t size_nybble = (data_size <= 0x07) ? (uint8_t)data_size : (uint8_t)0x09;

    out = byte_slice_write_u8(out, (uint8_t)((type_nybble << 4) | size_nybble));
    if(data_type > 0x07) { out = byte_slice_write_u8(out, data_type); }
    if(data_size > 0x07) { out = byte_slice_write_u8(out, (uint8_t)data_size); }

    return out;
}


static byte_slice_t read_dt_byte(byte_slice_t in, uint8_t *data_type, size_t *data_size) {
    uint8_t dt_byte = 0;
    uint8_t type_nybble = 0;
    uint8_t size_nybble = 0;
    uint8_t ext = 0;

    in = byte_slice_read_u8(in, &dt_byte);
    type_nybble = (uint8_t)(dt_byte >> 4);
    size_nybble = (uint8_t)(dt_byte & (uint8_t)0x0F);
    *data_type = type_nybble;
    *data_size = size_nybble;

    if(type_nybble & (uint8_t)0x08) {
        if((type_nybble & (uint8_t)0x07) != 1) { return byte_slice_fail(in, PLCTAG_ERR_UNSUPPORTED); }
        in = byte_slice_read_u8(in, &ext);
        *data_type = ext;
    }

    if(size_nybble & (uint8_t)0x08) {
        if((size_nybble & (uint8_t)0x07) != 1) { return byte_slice_fail(in, PLCTAG_ERR_UNSUPPORTED); }
        in = byte_slice_read_u8(in, &ext);
        *data_size = ext;
    }

    return in;
}


static uint8_t lgx_pccc_data_type(tag_def_s *tag) {
    switch(tag->tag_type) {
        case TAG_PCCC_TYPE_BIT: return PCCC_DATA_BIT_STRING;
        case TAG_PCCC_TYPE_REAL: return PCCC_DATA_REAL;
        case TAG_PCCC_TYPE_STRING: return PCCC_DATA_BYTE_STRING;
        /* N and L files are both signed integers; the DT byte size tells them apart. */
        default: return PCCC_DATA_INT;
    }
}


/*
 * 0x68: offset and transfer size in elements, the PLC/5 address, then the element count again.
 * The reply data is an array type prefix covering the whole transfer, the element type, then
 * the data.
 */
static uint8_t lgx_read(byte_slice_t in, byte_slice_t *data, plc_s *plc) {
    uint16_t offset = 0;
    uint16_t transfer_size = 0;
    uint8_t levels = 0;
    uint32_t file = 0;
    uint32_t element = 0;
    tag_def_s *tag = NULL;
    size_t start = 0;
    size_t end = 0;
    uint8_t err = 0;

    in = read_plc5_header(in, &offset, &transfer_size, &levels, &file, &element);
    if((err = find_plc5_file(in, levels, file, plc, &tag))) { return err; }

    start = ((size_t)offset + element) * tag->elem_size;
    end = start + (size_t)transfer_size * tag->elem_size;
    if((err = check_range(tag, start, end, LGX_MAX_DATA))) { return err; }

    *data = write_dt_byte(*data, PCCC_DATA_ARRAY, end - start);
    *data = write_dt_byte(*data, lgx_pccc_data_type(tag), tag->elem_size);

    critical_block(tag->data_mutex) { *data = byte_slice_write_bytes(*data, tag->data + start, (int32_t)(end - start)); }

    return 0;
}


/*
 * 0x67: the read's header, then the type prefixes the client got back from its first read
 * (an array prefix, if present, then the element prefix) and the data.  The client pads the
 * embedded PCCC request to an even length, so one trailing byte beyond the data is allowed.
 */
static uint8_t lgx_write(byte_slice_t in, byte_slice_t *data, plc_s *plc) {
    uint16_t offset = 0;
    uint16_t transfer_size = 0;
    uint8_t levels = 0;
    uint32_t file = 0;
    uint32_t element = 0;
    uint8_t dt_type = 0;
    size_t dt_size = 0;
    tag_def_s *tag = NULL;
    size_t start = 0;
    size_t end = 0;
    uint8_t err = 0;

    (void)data;

    in = read_plc5_header(in, &offset, &transfer_size, &levels, &file, &element);
    if((err = find_plc5_file(in, levels, file, plc, &tag))) { return err; }

    start = ((size_t)offset + element) * tag->elem_size;
    end = start + (size_t)transfer_size * tag->elem_size;
    if((err = check_range(tag, start, end, SIZE_MAX))) { return err; }

    in = read_dt_byte(in, &dt_type, &dt_size);
    if(dt_type == PCCC_DATA_ARRAY) { in = read_dt_byte(in, &dt_type, &dt_size); }

    if(byte_slice_has_error(in)) {
        log_info("Unable to decode the data type prefix!");
        return PCCC_ERR_ADDR_NOT_USABLE;
    }

    if((size_t)byte_slice_len(in) != end - start && (size_t)byte_slice_len(in) != end - start + 1) {
        log_info("Write carries %d bytes but the transfer is %zu!", byte_slice_len(in), end - start);
        return PCCC_ERR_FILE_IS_WRONG_SIZE;
    }

    critical_block(tag->data_mutex) { memcpy(tag->data + start, byte_slice_get_data(in), end - start); }

    return 0;
}
