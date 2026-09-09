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
#include "fault.h"
#include "cip.h"
#include "eip.h"
#include "plc.h"
#include "slice.h"
#include "utils.h"
#include "log.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>

const uint8_t PCCC_PREFIX[] = {0x0f, 0x00};
const uint8_t PLC5_READ[] = {0x01};
const uint8_t PLC5_WRITE[] = {0x00};
const uint8_t PLC5_RMW[] = {0x26};
const uint8_t SLC_READ[] = {0xa2};
const uint8_t SLC_WRITE[] = {0xaa};
const uint8_t SLC_RMW[] = {0xab};

/*
 * The PCCC mapping inside a ControlLogix has its own typed read/write pair.  They carry a
 * PLC/5-style logical address but count in elements rather than words, and the data is wrapped
 * in PCCC type prefixes.  See eip_lgx_pccc.c in the library.
 */
const uint8_t LGX_READ[] = {0x68};
const uint8_t LGX_WRITE[] = {0x67};

/* PCCC typed-data type codes, as used in the DT byte below. */
#define PCCC_DATA_BIT_STRING ((uint8_t)0x02)
#define PCCC_DATA_BYTE_STRING ((uint8_t)0x03)
#define PCCC_DATA_INT ((uint8_t)0x04)
#define PCCC_DATA_REAL ((uint8_t)0x08)
#define PCCC_DATA_ARRAY ((uint8_t)0x09)

const uint8_t PCCC_RESP_PREFIX[] = {0xcb, 0x00, 0x00, 0x00, 0x07, 0x3d, 0xf3, 0x45, 0x43, 0x50, 0x21};

/*
 * A PLC/5 range read/write counts its transfer size and offset in 16-bit words -- see
 * plc5_pccc_read_cmd_req in the library's pccc.c, which sends tag->size / 2.  The SLC command
 * right below counts bytes instead, which is why the two handlers differ.  Multiplying the
 * word count by tag->elem_size instead of by two happened to work for the 2-byte B3 and N7
 * files and asked for absurd amounts of data for anything wider.
 */
#define PLC5_WORD_SIZE ((size_t)2)

const uint8_t PCCC_ERR_ADDR_NOT_USABLE = (int8_t)0x06;
const uint8_t PCCC_ERR_FILE_IS_WRONG_SIZE = (int8_t)0x07;
const uint8_t PCCC_ERR_UNSUPPORTED_COMMAND = (uint8_t)0x0e;

// 4f f0 3c 96 06 - address does not point to something usable.
// 4f f0 fa da 07 - file is wrong size.
// 4f f0 a6 b3 0e - command could not be decoded

static slice_s handle_plc5_read_request(slice_s input, slice_s output, plc_s *plc);
static slice_s handle_plc5_write_request(slice_s input, slice_s output, plc_s *plc);
static slice_s handle_plc5_rmw_request(slice_s input, slice_s output, plc_s *plc);
static slice_s handle_slc_read_request(slice_s input, slice_s output, plc_s *plc);
static slice_s handle_slc_write_request(slice_s input, slice_s output, plc_s *plc);
static slice_s handle_slc_rmw_request(slice_s input, slice_s output, plc_s *plc);
static slice_s handle_lgx_read_request(slice_s input, slice_s output, plc_s *plc);
static slice_s handle_lgx_write_request(slice_s input, slice_s output, plc_s *plc);
static size_t encode_dt_byte(slice_s output, size_t offset, uint8_t data_type, size_t data_size);
static size_t decode_dt_byte(slice_s input, size_t offset, uint8_t *data_type, size_t *data_size);
static uint8_t lgx_pccc_data_type(tag_def_s *tag);
static slice_s make_pccc_log_error(slice_s output, uint8_t err_code, plc_s *plc);


slice_s dispatch_pccc_request(slice_s input, slice_s output, plc_s *plc) {
    slice_s pccc_input;
    slice_s pccc_output;
    log_info("Got packet:");
    log_info_slice(input);

    if(slice_len(input) < 20) { /* FIXME - 13 + 7 */
        log_info("Packet too short!");
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* split off the PCCC packet. */
    pccc_input = slice_from_slice(input, 13, slice_len(input) - 13);
    pccc_output = slice_from_slice(output, sizeof(PCCC_RESP_PREFIX), slice_len(output) - sizeof(PCCC_RESP_PREFIX));

    /* copy the response prefix. */
    for(size_t i = 0; i < sizeof(PCCC_RESP_PREFIX); i++) { slice_set_uint8(output, i, PCCC_RESP_PREFIX[i]); }

    log_info("PCCC packet:");
    log_info_slice(pccc_input);

    if(slice_match_data_prefix(pccc_input, PCCC_PREFIX, sizeof(PCCC_PREFIX))) {
        slice_s pccc_command;

        log_info("Matched valid PCCC prefix.");

        plc->pccc_seq_id = slice_get_uint16_le(pccc_input, 2);

        pccc_command = slice_from_slice(pccc_input, 4, slice_len(pccc_input) - 4);

        /* match the command. */
        if(plc->plc_type == PLC_PLC5 && slice_match_data_prefix(pccc_command, PLC5_READ, sizeof(PLC5_READ))) {
            pccc_output = handle_plc5_read_request(pccc_command, pccc_output, plc);
        } else if(plc->plc_type == PLC_PLC5 && slice_match_data_prefix(pccc_command, PLC5_WRITE, sizeof(PLC5_WRITE))) {
            pccc_output = handle_plc5_write_request(pccc_command, pccc_output, plc);
        } else if(plc->plc_type == PLC_PLC5 && slice_match_data_prefix(pccc_command, PLC5_RMW, sizeof(PLC5_RMW))) {
            pccc_output = handle_plc5_rmw_request(pccc_command, pccc_output, plc);
        } else if((plc->plc_type == PLC_SLC || plc->plc_type == PLC_MICROLOGIX)
                  && slice_match_data_prefix(pccc_command, SLC_READ, sizeof(SLC_READ))) {
            pccc_output = handle_slc_read_request(pccc_command, pccc_output, plc);
        } else if((plc->plc_type == PLC_SLC || plc->plc_type == PLC_MICROLOGIX)
                  && slice_match_data_prefix(pccc_command, SLC_WRITE, sizeof(SLC_WRITE))) {
            pccc_output = handle_slc_write_request(pccc_command, pccc_output, plc);
        } else if((plc->plc_type == PLC_SLC || plc->plc_type == PLC_MICROLOGIX)
                  && slice_match_data_prefix(pccc_command, SLC_RMW, sizeof(SLC_RMW))) {
            pccc_output = handle_slc_rmw_request(pccc_command, pccc_output, plc);
        } else if(plc->plc_type == PLC_LGX_PCCC && slice_match_data_prefix(pccc_command, LGX_READ, sizeof(LGX_READ))) {
            pccc_output = handle_lgx_read_request(pccc_command, pccc_output, plc);
        } else if(plc->plc_type == PLC_LGX_PCCC && slice_match_data_prefix(pccc_command, LGX_WRITE, sizeof(LGX_WRITE))) {
            pccc_output = handle_lgx_write_request(pccc_command, pccc_output, plc);
        } else {
            log_info("Unsupported PCCC command!");
            pccc_output = make_pccc_log_error(pccc_output, PCCC_ERR_UNSUPPORTED_COMMAND, plc);
        }
    } else {
        slice_s prefix = slice_make(&(PCCC_PREFIX[0]), sizeof(PCCC_PREFIX));
        log_info("Invalid PCCC prefix!");
        log_info("Expected:");
        log_info_slice(prefix);
        log_info("Got:");
        log_info_slice(pccc_input);
        pccc_output = make_pccc_log_error(pccc_output, PCCC_ERR_UNSUPPORTED_COMMAND, plc);
    }

    /*
     * Fault injection.  Every handler above writes the reply code at offset 0 and the
     * transaction number at offset 2 of its response, so both corruptions belong here rather
     * than repeated in each of the six handlers.
     */
    if(!slice_has_err(pccc_output) && slice_len(pccc_output) >= 4) {
        /*
         * The reply code the client checks is the CIP-layer one at the front of
         * PCCC_RESP_PREFIX, 0xcb, not the PCCC command echo at the start of the PCCC payload.
         * It is the PCCC-layer echo of the service that was asked for, and everything after it
         * shifts if the target answers a different service.
         */
        if(fault_fires(plc, FAULT_PCCC_REPLY)) { slice_set_uint8(output, 0, (uint8_t)0xce); }

        if(fault_fires(plc, FAULT_PCCC_TNS)) {
            slice_set_uint16_le(pccc_output, 2, (uint16_t)(plc->pccc_seq_id ^ (uint16_t)0xFFFF));
        }
    }

    return slice_from_slice(output, 0, 11 + slice_len(pccc_output));
}


slice_s handle_plc5_read_request(slice_s input, slice_s output, plc_s *plc) {
    uint16_t offset = 0;
    size_t start_byte_offset = 0;
    uint16_t transfer_size = 0;
    size_t end_byte_offset = 0;
    size_t tag_size = 0;
    size_t data_file_num = 0;
    size_t data_file_element = 0;
    uint8_t data_file_prefix = 0;
    tag_def_s *tag = plc->tags;

    log_info("Got packet:");
    log_info_slice(input);

    offset = slice_get_uint16_le(input, 1);
    transfer_size = slice_get_uint16_le(input, 3);

    /* decode the data file. */
    data_file_prefix = slice_get_uint8(input, 5);

    /* check the data file prefix. */
    if(data_file_prefix != 0x06) {
        log_info("Unexpected data file prefix byte %d!", data_file_prefix);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* get data file number. */
    data_file_num = slice_get_uint8(input, 6);

    /* get the data element number. */
    data_file_element = slice_get_uint8(input, 7);

    /* find the tag. */
    while(tag && tag->data_file_num != data_file_num) { tag = tag->next_tag; }

    if(!tag) {
        log_info("Unable to find tag with data file %u!", data_file_num);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* now we can check the start and end offsets. */
    tag_size = tag->elem_count * tag->elem_size;
    start_byte_offset = (offset * PLC5_WORD_SIZE) + (data_file_element * tag->elem_size);
    end_byte_offset = start_byte_offset + (transfer_size * PLC5_WORD_SIZE);

    if(start_byte_offset >= tag_size) {
        log_info("Starting offset, %u, is greater than tag size, %d!", (unsigned int)start_byte_offset, (unsigned int)tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    if(end_byte_offset > tag_size) {
        log_info("Ending offset, %u, is greater than tag size, %d!", (unsigned int)end_byte_offset, (unsigned int)tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* check the amount of data requested. */
    if((end_byte_offset - start_byte_offset) > 240) {
        log_info("Request asks for too much data, %u bytes, for response packet!",
                 (unsigned int)(end_byte_offset - start_byte_offset));
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    log_info("Transfer size %u (in words), tag elem size %u, bytes to transfer %d.", transfer_size, tag->elem_size,
             transfer_size * PLC5_WORD_SIZE);

    /* build the response. */
    slice_set_uint8(output, 0, 0x4f);
    slice_set_uint8(output, 1, 0); /* no error */
    slice_set_uint16_le(output, 2, plc->pccc_seq_id);

    for(size_t i = 0; i < (transfer_size * PLC5_WORD_SIZE); i++) {
        log_info("setting byte %d to value %d.", 4 + i, tag->data[start_byte_offset + i]);
        slice_set_uint8(output, 4 + i, tag->data[start_byte_offset + i]);
    }

    log_info("Output slice length %d.", slice_len(slice_from_slice(output, 0, 4 + (transfer_size * PLC5_WORD_SIZE))));

    return slice_from_slice(output, 0, 4 + (transfer_size * PLC5_WORD_SIZE));
}


slice_s handle_plc5_write_request(slice_s input, slice_s output, plc_s *plc) {
    uint16_t offset = 0;
    size_t start_byte_offset = 0;
    uint16_t transfer_size = 0;
    size_t end_byte_offset = 0;
    size_t tag_size = 0;
    size_t data_start_byte_offset = 0;
    size_t data_len = 0;
    size_t data_file_num = 0;
    size_t data_file_element = 0;
    uint8_t data_file_prefix = 0;
    tag_def_s *tag = plc->tags;

    log_info("Got packet:");
    log_info_slice(input);

    offset = slice_get_uint16_le(input, 1);
    transfer_size = slice_get_uint16_le(input, 3);

    /* decode the data file. */
    data_file_prefix = slice_get_uint8(input, 5);

    /* check the data file prefix. */
    if(data_file_prefix != 0x06) {
        log_info("Unexpected data file prefix byte %d!", data_file_prefix);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* get data file number. */
    data_file_num = slice_get_uint8(input, 6);

    /* get the data element number. */
    data_file_element = slice_get_uint8(input, 7);

    /* find the tag. */
    while(tag && tag->data_file_num != data_file_num) { tag = tag->next_tag; }

    if(!tag) {
        log_info("Unable to find tag with data file %u!", data_file_num);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /*
     * we have the tag, now write the data.   The size of the write
     * needs to be less than the tag size.
     */

    tag_size = tag->elem_count * tag->elem_size;
    start_byte_offset = (offset * PLC5_WORD_SIZE) + (data_file_element * tag->elem_size);
    end_byte_offset = start_byte_offset + (transfer_size * PLC5_WORD_SIZE);

    if(start_byte_offset >= tag_size) {
        log_info("Starting offset, %u, is greater than tag size, %d!", (unsigned int)start_byte_offset, (unsigned int)tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    if(end_byte_offset > tag_size) {
        log_info("Ending offset, %u, is greater than tag size, %d!", (unsigned int)end_byte_offset, (unsigned int)tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    data_start_byte_offset = 8;
    data_len = slice_len(input) - 8;

    if(data_len != (transfer_size * PLC5_WORD_SIZE)) {
        log_info("Data in packet is not the same length, %u, as the requested transfer, %d!", data_len,
                 (transfer_size * PLC5_WORD_SIZE));
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* copy the data into the tag. */
    for(size_t i = 0; i < (transfer_size * PLC5_WORD_SIZE); i++) {
        log_info("setting byte %d to value %d.", start_byte_offset + i, slice_get_uint8(input, data_start_byte_offset + i));
        tag->data[start_byte_offset + i] = slice_get_uint8(input, data_start_byte_offset + i);
    }

    log_info("Transfer size %u (in words), tag elem size %u, bytes to transfer %d.", transfer_size, tag->elem_size,
             transfer_size * PLC5_WORD_SIZE);

    /* build the response. */
    slice_set_uint8(output, 0, 0x4f);
    slice_set_uint8(output, 1, 0); /* no error */
    slice_set_uint16_le(output, 2, plc->pccc_seq_id);

    return slice_from_slice(output, 0, 4);
}


slice_s handle_plc5_rmw_request(slice_s input, slice_s output, plc_s *plc) {
    size_t start_byte_offset = 0;
    size_t tag_size = 0;
    size_t data_file_num = 0;
    size_t data_file_element = 0;
    uint8_t data_file_prefix = 0;
    size_t elem_size = 0;
    size_t and_mask_offset = 0;
    size_t or_mask_offset = 0;
    tag_def_s *tag = plc->tags;

    log_info("Got packet:");
    log_info_slice(input);

    /* decode the data file. */
    data_file_prefix = slice_get_uint8(input, 1);

    /* check the data file prefix. */
    if(data_file_prefix != 0x06) {
        log_info("Unexpected data file prefix byte %d!", data_file_prefix);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* get data file number. */
    data_file_num = slice_get_uint8(input, 2);

    /* get the data element number. */
    data_file_element = slice_get_uint8(input, 3);

    /* find the tag. */
    while(tag && tag->data_file_num != data_file_num) { tag = tag->next_tag; }

    if(!tag) {
        log_info("Unable to find tag with data file %u!", data_file_num);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* Calculate element size and offsets. */
    elem_size = tag->elem_size;
    tag_size = tag->elem_count * tag->elem_size;
    start_byte_offset = data_file_element * tag->elem_size;  // MAGIC - 3 is offset of first mask byte in RMW packet

    log_info("Element size %zu, start byte offset %zu, tag size %zu.", elem_size, start_byte_offset, tag_size);

    if(start_byte_offset >= tag_size) {
        log_info("Starting offset, %zu, is greater than tag size, %zu!", start_byte_offset, tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    if(start_byte_offset + elem_size > tag_size) {
        log_info("Ending offset, %zu, is greater than tag size, %zu!", start_byte_offset + elem_size, tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* Verify packet has enough data: cmd(1) + prefix(1) + file(1) + element(1) + AND masks + OR masks */
    and_mask_offset = 4;
    or_mask_offset = and_mask_offset + elem_size;

    if(slice_len(input) < or_mask_offset + elem_size) {
        log_info("Packet too short for AND and OR masks!");
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* Apply the AND/OR masks to the data. */
    log_info("Applying AND/OR masks to tag data.");
    for(size_t i = 0; i < elem_size; i++) {
        uint8_t and_mask = slice_get_uint8(input, and_mask_offset + i);
        uint8_t or_mask = slice_get_uint8(input, or_mask_offset + i);

        log_info("Byte %zu: AND mask=%02x, OR mask=%02x, old value=%02x", i, and_mask, or_mask, tag->data[start_byte_offset + i]);

        tag->data[start_byte_offset + i] = (tag->data[start_byte_offset + i] & and_mask) | or_mask;

        log_info("Byte %zu: new value=%02x", i, tag->data[start_byte_offset + i]);
    }

    log_info("RMW operation complete.");

    /* build the response. */
    slice_set_uint8(output, 0, 0x4f);
    slice_set_uint8(output, 1, 0); /* no error */
    slice_set_uint16_le(output, 2, plc->pccc_seq_id);

    return slice_from_slice(output, 0, 4);
}


slice_s handle_slc_read_request(slice_s input, slice_s output, plc_s *plc) {
    size_t start_byte_offset = 0;
    uint8_t transfer_size = 0;
    size_t end_byte_offset = 0;
    size_t tag_size = 0;
    size_t data_file_num = 0;
    size_t data_file_type = 0;
    size_t data_file_element = 0;
    size_t data_file_subelement = 0;
    tag_def_s *tag = plc->tags;

    log_info("Got packet:");
    log_info_slice(input);

    /*
     * a2 - SLC-type read.
     * <size> - size in bytes to read.
     * <file num> - data file number.
     * <file type> - data file type.
     * <file element> - data file element.
     * <file subelement> - data file subelement.
     */

    transfer_size = slice_get_uint8(input, 1);
    data_file_num = slice_get_uint8(input, 2);
    data_file_type = slice_get_uint8(input, 3);
    data_file_element = slice_get_uint8(input, 4);
    data_file_subelement = slice_get_uint8(input, 5);

    if(data_file_subelement != 0) {
        log_info("Data file subelement is unsupported!");
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* find the tag. */
    while(tag && tag->data_file_num != data_file_num) { tag = tag->next_tag; }

    if(!tag) {
        log_info("Unable to find tag with data file %u!", data_file_num);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    if(tag->tag_type != data_file_type) {
        log_info("Data file type requested, %u, does not match file type of tag, %d!", data_file_type, tag->tag_type);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* now we can check the start and end offsets. */
    tag_size = tag->elem_count * tag->elem_size;
    start_byte_offset = (data_file_element * tag->elem_size);
    end_byte_offset = start_byte_offset + transfer_size;

    log_info("Start byte offset %u, end byte offset %u.", (unsigned int)start_byte_offset, (unsigned int)end_byte_offset);

    if(start_byte_offset >= tag_size) {
        log_info("Starting offset, %u, is greater than tag size, %u!", (unsigned int)start_byte_offset, (unsigned int)tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    if(end_byte_offset > tag_size) {
        log_info("Ending offset, %u, is greater than tag size, %u!", (unsigned int)end_byte_offset, (unsigned int)tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* check the amount of data requested. */
    if(transfer_size > 240) {
        log_info("Request asks for too much data, %u bytes, for response packet!", (unsigned int)transfer_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    log_info("Transfer size %u (in bytes), tag elem size %u.", transfer_size, tag->elem_size);

    /* build the response. */
    slice_set_uint8(output, 0, 0x4f);
    slice_set_uint8(output, 1, 0); /* no error */
    slice_set_uint16_le(output, 2, plc->pccc_seq_id);

    for(size_t i = 0; i < transfer_size; i++) {
        log_info("setting byte %d to value %d.", 4 + i, tag->data[start_byte_offset + i]);
        slice_set_uint8(output, 4 + i, tag->data[start_byte_offset + i]);
    }

    log_info("Output slice length %d.", slice_len(slice_from_slice(output, 0, (size_t)4 + (size_t)transfer_size)));

    return slice_from_slice(output, 0, (size_t)4 + (size_t)transfer_size);
}


slice_s handle_slc_write_request(slice_s input, slice_s output, plc_s *plc) {
    size_t start_byte_offset = 0;
    uint8_t transfer_size = 0;
    size_t end_byte_offset = 0;
    size_t tag_size = 0;
    size_t data_file_num = 0;
    size_t data_file_type = 0;
    size_t data_file_element = 0;
    size_t data_file_subelement = 0;
    size_t data_len = 0;
    size_t data_start_byte_offset = 0;
    tag_def_s *tag = plc->tags;

    log_info("Got packet:");
    log_info_slice(input);

    /*
     * aa - SLC-type write.
     * <size> - size in bytes to write.
     * <file num> - data file number.
     * <file type> - data file type.
     * <file element> - data file element.
     * <file subelement> - data file subelement.
     * ... data ... - data to write.
     */

    transfer_size = slice_get_uint8(input, 1);
    data_file_num = slice_get_uint8(input, 2);
    data_file_type = slice_get_uint8(input, 3);
    data_file_element = slice_get_uint8(input, 4);
    data_file_subelement = slice_get_uint8(input, 5);

    if(data_file_subelement != 0) {
        log_info("Data file subelement is unsupported!");
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* find the tag. */
    while(tag && tag->data_file_num != data_file_num) { tag = tag->next_tag; }

    if(!tag) {
        log_info("Unable to find tag with data file %u!", data_file_num);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    if(tag->tag_type != data_file_type) {
        log_info("Data file type requested, %u, does not match file type of tag, %d!", data_file_type, tag->tag_type);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* now we can check the start and end offsets. */
    tag_size = tag->elem_count * tag->elem_size;
    start_byte_offset = (data_file_element * tag->elem_size);
    end_byte_offset = start_byte_offset + transfer_size;

    log_info("Start byte offset %u, end byte offset %u.", (unsigned int)start_byte_offset, (unsigned int)end_byte_offset);

    if(start_byte_offset >= tag_size) {
        log_info("Starting offset, %u, is greater than tag size, %d!", (unsigned int)start_byte_offset, (unsigned int)tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    if(end_byte_offset > tag_size) {
        log_info("Ending offset, %u, is greater than tag size, %d!", (unsigned int)end_byte_offset, (unsigned int)tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* check the amount of data requested. */
    if(transfer_size > 240) {
        log_info("Request asks for too much data, %u bytes, for response packet!", (unsigned int)transfer_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    log_info("Transfer size %u (in bytes), tag elem size %u.", transfer_size, tag->elem_size);

    data_start_byte_offset = 6;
    data_len = slice_len(input) - data_start_byte_offset;

    if(data_len != transfer_size) {
        log_info("Data in packet is not the same length, %u, as the requested transfer, %d!", data_len, transfer_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* copy the data into the tag. */
    for(size_t i = 0; i < transfer_size; i++) {
        log_info("setting byte %d to value %d.", start_byte_offset + i, slice_get_uint8(input, data_start_byte_offset + i));
        tag->data[start_byte_offset + i] = slice_get_uint8(input, data_start_byte_offset + i);
    }

    log_info("Transfer size %u, tag elem size %u.", transfer_size, tag->elem_size);

    /* build the response. */
    slice_set_uint8(output, 0, 0x4f);
    slice_set_uint8(output, 1, 0); /* no error */
    slice_set_uint16_le(output, 2, plc->pccc_seq_id);

    return slice_from_slice(output, 0, 4);
}


slice_s handle_slc_rmw_request(slice_s input, slice_s output, plc_s *plc) {
    uint8_t transfer_size = 0;
    size_t start_byte_offset = 0;
    size_t tag_size = 0;
    size_t data_file_num = 0;
    size_t data_file_type = 0;
    size_t data_file_element = 0;
    size_t data_file_subelement = 0;
    size_t mask_offset = 0;
    size_t data_offset = 0;
    tag_def_s *tag = plc->tags;

    log_info("Got packet:");
    log_info_slice(input);

    /*
     * ab - SLC-type RMW.
     * <size> - size in bytes to write (must be 2 for 16-bit mask).
     * <file num> - data file number.
     * <file type> - data file type.
     * <file element> - data file element.
     * <file subelement> - data file subelement.
     * <mask bytes> - 2 bytes indicating which bits change.
     * <data bytes> - 2 bytes with new values for masked bits.
     */

    transfer_size = slice_get_uint8(input, 1);
    data_file_num = slice_get_uint8(input, 2);
    data_file_type = slice_get_uint8(input, 3);
    data_file_element = slice_get_uint8(input, 4);
    data_file_subelement = slice_get_uint8(input, 5);

    log_info("Transfer size %u, file type %zu, element %zu.", transfer_size, data_file_type, data_file_element);

    /* SLC RMW only supports 16-bit (2-byte) elements. */
    if(transfer_size != 2) {
        log_info("Transfer size must be 2 bytes for SLC RMW, got %u!", transfer_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    if(data_file_subelement != 0) {
        log_info("Data file subelement is unsupported!");
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* find the tag. */
    while(tag && tag->data_file_num != data_file_num) { tag = tag->next_tag; }

    if(!tag) {
        log_info("Unable to find tag with data file %u!", data_file_num);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    if(tag->tag_type != data_file_type) {
        log_info("Data file type requested, %zu, does not match file type of tag, %d!", data_file_type, tag->tag_type);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* Check element size is 2 bytes. */
    if(tag->elem_size != 2) {
        log_info("Tag element size %zu is not 2 bytes for RMW!", tag->elem_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* Calculate offsets. */
    tag_size = tag->elem_count * tag->elem_size;
    start_byte_offset = data_file_element * tag->elem_size;

    log_info("Start byte offset %zu, tag size %zu.", start_byte_offset, tag_size);

    if(start_byte_offset >= tag_size) {
        log_info("Starting offset, %zu, is greater than tag size, %zu!", start_byte_offset, tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    if(start_byte_offset + 2 > tag_size) {
        log_info("Ending offset, %zu, is greater than tag size, %zu!", start_byte_offset + 2, tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* Verify packet has mask and data bytes. */
    mask_offset = 6;
    data_offset = 8;

    if(slice_len(input) < data_offset + 2) {
        log_info("Packet too short for mask and data!");
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* Apply the mask and data to the tag. */
    log_info("Applying mask and data to tag.");
    for(size_t i = 0; i < 2; i++) {
        uint8_t mask = slice_get_uint8(input, mask_offset + i);
        uint8_t new_data = slice_get_uint8(input, data_offset + i);

        log_info("Byte %d: mask=%02x, new_data=%02x, old value=%02x", i, mask, new_data, tag->data[start_byte_offset + i]);

        /* Preserve bits not in the mask, apply new data for bits in the mask. */
        tag->data[start_byte_offset + i] = (uint8_t)((tag->data[start_byte_offset + i] & ~mask) | (new_data & mask));

        log_info("Byte %zu: new value=%02x", i, tag->data[start_byte_offset + i]);
    }

    log_info("RMW operation complete.");

    /* build the response. */
    slice_set_uint8(output, 0, 0x4f);
    slice_set_uint8(output, 1, 0); /* no error */
    slice_set_uint16_le(output, 2, plc->pccc_seq_id);

    return slice_from_slice(output, 0, 4);
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
size_t encode_dt_byte(slice_s output, size_t offset, uint8_t data_type, size_t data_size) {
    size_t index = offset + 1;
    uint8_t type_nybble = 0;
    uint8_t size_nybble = 0;

    if(data_type <= 0x07) {
        type_nybble = data_type;
    } else {
        type_nybble = (uint8_t)0x09;
        slice_set_uint8(output, index++, data_type);
    }

    if(data_size <= 0x07) {
        size_nybble = (uint8_t)data_size;
    } else {
        size_nybble = (uint8_t)0x09;
        slice_set_uint8(output, index++, (uint8_t)data_size);
    }

    slice_set_uint8(output, offset, (uint8_t)((type_nybble << 4) | size_nybble));

    return index - offset;
}


/* returns the number of bytes consumed, or zero if the prefix is malformed or truncated. */
size_t decode_dt_byte(slice_s input, size_t offset, uint8_t *data_type, size_t *data_size) {
    size_t index = offset + 1;
    uint8_t dt_byte = 0;
    uint8_t type_nybble = 0;
    uint8_t size_nybble = 0;

    if(offset >= slice_len(input)) { return 0; }

    dt_byte = slice_get_uint8(input, offset);
    type_nybble = (uint8_t)((dt_byte & (uint8_t)0xF0) >> 4);
    size_nybble = (uint8_t)(dt_byte & (uint8_t)0x0F);

    *data_type = type_nybble;
    *data_size = size_nybble;

    if(type_nybble & (uint8_t)0x08) {
        if((type_nybble & (uint8_t)0x07) != 1 || index >= slice_len(input)) { return 0; }
        *data_type = slice_get_uint8(input, index++);
    }

    if(size_nybble & (uint8_t)0x08) {
        if((size_nybble & (uint8_t)0x07) != 1 || index >= slice_len(input)) { return 0; }
        *data_size = slice_get_uint8(input, index++);
    }

    return index - offset;
}


uint8_t lgx_pccc_data_type(tag_def_s *tag) {
    switch(tag->tag_type) {
        case TAG_PCCC_TYPE_BIT: return PCCC_DATA_BIT_STRING; break;
        case TAG_PCCC_TYPE_REAL: return PCCC_DATA_REAL; break;
        case TAG_PCCC_TYPE_STRING: return PCCC_DATA_BYTE_STRING; break;
        /* N and L files are both signed integers; the DT byte size tells them apart. */
        default: return PCCC_DATA_INT; break;
    }
}


slice_s handle_lgx_read_request(slice_s input, slice_s output, plc_s *plc) {
    uint16_t offset = 0;
    uint16_t transfer_size = 0;
    size_t start_byte_offset = 0;
    size_t end_byte_offset = 0;
    size_t transfer_bytes = 0;
    size_t tag_size = 0;
    size_t data_file_num = 0;
    size_t data_file_element = 0;
    size_t dt_len = 0;
    uint8_t data_file_prefix = 0;
    tag_def_s *tag = plc->tags;

    log_info("Got packet:");
    log_info_slice(input);

    /*
     * 68 - PCCC-mapped Logix typed read.
     * <offset> - 16-bit element offset into the data file.
     * <transfer size> - 16-bit count of elements to transfer.
     * 06 <file num> <file element> - PLC/5-style logical address.
     * <transfer size> - the element count again.
     */

    offset = slice_get_uint16_le(input, 1);
    transfer_size = slice_get_uint16_le(input, 3);

    data_file_prefix = slice_get_uint8(input, 5);

    if(data_file_prefix != 0x06) {
        log_info("Unexpected data file prefix byte %d!", data_file_prefix);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    data_file_num = slice_get_uint8(input, 6);
    data_file_element = slice_get_uint8(input, 7);

    /* find the tag. */
    while(tag && tag->data_file_num != data_file_num) { tag = tag->next_tag; }

    if(!tag) {
        log_info("Unable to find tag with data file %u!", (unsigned int)data_file_num);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    tag_size = tag->elem_count * tag->elem_size;
    start_byte_offset = ((size_t)offset + data_file_element) * tag->elem_size;
    transfer_bytes = (size_t)transfer_size * tag->elem_size;
    end_byte_offset = start_byte_offset + transfer_bytes;

    if(start_byte_offset >= tag_size) {
        log_info("Starting offset, %u, is greater than tag size, %u!", (unsigned int)start_byte_offset, (unsigned int)tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    if(end_byte_offset > tag_size) {
        log_info("Ending offset, %u, is greater than tag size, %u!", (unsigned int)end_byte_offset, (unsigned int)tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* the type prefixes below add a few bytes, so leave more headroom than the raw-data cases. */
    if(transfer_bytes > 230) {
        log_info("Request asks for too much data, %u bytes, for response packet!", (unsigned int)transfer_bytes);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* build the response. */
    slice_set_uint8(output, 0, 0x4f);
    slice_set_uint8(output, 1, 0); /* no error */
    slice_set_uint16_le(output, 2, plc->pccc_seq_id);

    /* a typed read answers with an array prefix covering the whole transfer, then the element type. */
    dt_len = encode_dt_byte(output, 4, PCCC_DATA_ARRAY, transfer_bytes);
    dt_len += encode_dt_byte(output, 4 + dt_len, lgx_pccc_data_type(tag), tag->elem_size);

    for(size_t i = 0; i < transfer_bytes; i++) {
        slice_set_uint8(output, 4 + dt_len + i, tag->data[start_byte_offset + i]);
    }

    log_info("Transfer size %u (in elements), tag elem size %u, %u type prefix bytes.", transfer_size,
             (unsigned int)tag->elem_size, (unsigned int)dt_len);

    return slice_from_slice(output, 0, 4 + dt_len + transfer_bytes);
}


slice_s handle_lgx_write_request(slice_s input, slice_s output, plc_s *plc) {
    uint16_t offset = 0;
    uint16_t transfer_size = 0;
    size_t start_byte_offset = 0;
    size_t end_byte_offset = 0;
    size_t transfer_bytes = 0;
    size_t tag_size = 0;
    size_t data_file_num = 0;
    size_t data_file_element = 0;
    size_t data_start_byte_offset = 0;
    size_t data_len = 0;
    size_t dt_len = 0;
    size_t dt_data_size = 0;
    uint8_t dt_data_type = 0;
    uint8_t data_file_prefix = 0;
    tag_def_s *tag = plc->tags;

    log_info("Got packet:");
    log_info_slice(input);

    /*
     * 67 - PCCC-mapped Logix typed write.  Same header as the read above, but the element count
     * at the end is replaced by the type prefixes the client got back from its first read,
     * followed by the data itself.
     */

    offset = slice_get_uint16_le(input, 1);
    transfer_size = slice_get_uint16_le(input, 3);

    data_file_prefix = slice_get_uint8(input, 5);

    if(data_file_prefix != 0x06) {
        log_info("Unexpected data file prefix byte %d!", data_file_prefix);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    data_file_num = slice_get_uint8(input, 6);
    data_file_element = slice_get_uint8(input, 7);

    /* find the tag. */
    while(tag && tag->data_file_num != data_file_num) { tag = tag->next_tag; }

    if(!tag) {
        log_info("Unable to find tag with data file %u!", (unsigned int)data_file_num);
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    tag_size = tag->elem_count * tag->elem_size;
    start_byte_offset = ((size_t)offset + data_file_element) * tag->elem_size;
    transfer_bytes = (size_t)transfer_size * tag->elem_size;
    end_byte_offset = start_byte_offset + transfer_bytes;

    if(start_byte_offset >= tag_size) {
        log_info("Starting offset, %u, is greater than tag size, %u!", (unsigned int)start_byte_offset, (unsigned int)tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    if(end_byte_offset > tag_size) {
        log_info("Ending offset, %u, is greater than tag size, %u!", (unsigned int)end_byte_offset, (unsigned int)tag_size);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* step over the type prefixes: an array header, if present, then the element header. */
    dt_len = decode_dt_byte(input, 8, &dt_data_type, &dt_data_size);

    if(dt_len == 0) {
        log_info("Unable to decode the data type prefix!");
        return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    if(dt_data_type == PCCC_DATA_ARRAY) {
        size_t elem_dt_len = decode_dt_byte(input, 8 + dt_len, &dt_data_type, &dt_data_size);

        if(elem_dt_len == 0) {
            log_info("Unable to decode the array element data type prefix!");
            return make_pccc_log_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
        }

        dt_len += elem_dt_len;
    }

    data_start_byte_offset = 8 + dt_len;

    if(slice_len(input) < data_start_byte_offset) {
        log_info("Packet too short to contain any data!");
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    data_len = slice_len(input) - data_start_byte_offset;

    /*
     * The client pads the embedded PCCC request out to an even length, so one trailing byte
     * beyond the transfer is expected and is not part of the data.
     */
    if(data_len != transfer_bytes && data_len != transfer_bytes + 1) {
        log_info("Data in packet is not the same length, %u, as the requested transfer, %u!", (unsigned int)data_len,
                 (unsigned int)transfer_bytes);
        return make_pccc_log_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    for(size_t i = 0; i < transfer_bytes; i++) {
        tag->data[start_byte_offset + i] = slice_get_uint8(input, data_start_byte_offset + i);
    }

    /* build the response. */
    slice_set_uint8(output, 0, 0x4f);
    slice_set_uint8(output, 1, 0); /* no error */
    slice_set_uint16_le(output, 2, plc->pccc_seq_id);

    return slice_from_slice(output, 0, 4);
}


slice_s make_pccc_log_error(slice_s output, uint8_t err_code, plc_s *plc) {
    // 4f f0 3c 96 06
    slice_s err_resp = slice_from_slice(output, 0, 5);

    if(!slice_has_err(err_resp)) {
        slice_set_uint8(err_resp, 0, (uint8_t)0x4f);
        slice_set_uint8(err_resp, 1, (uint8_t)0xf0);
        slice_set_uint16_le(err_resp, 2, plc->pccc_seq_id);
        slice_set_uint8(err_resp, 4, err_code);
    }

    return err_resp;
}
