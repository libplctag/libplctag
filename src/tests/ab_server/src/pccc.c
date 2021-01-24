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

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include "cip.h"
#include "eip.h"
#include "pccc.h"
#include "plc.h"
#include "slice.h"
#include "utils.h"

const uint8_t PCCC_PREFIX[] = { 0x0f, 0x00 };
const uint8_t PLC5_READ[] = { 0x01 };
const uint8_t PLC5_WRITE[] = { 0x00 };
const uint8_t SLC_READ[] = { 0xa2 };
const uint8_t SLC_WRITE[] = { 0xaa };

const uint8_t PCCC_RESP_PREFIX[] = { 0xcb, 0x00, 0x00, 0x00, 0x07, 0x3d, 0xf3, 0x45, 0x43, 0x50, 0x21 };

const uint8_t PCCC_ERR_ADDR_NOT_USABLE = (int8_t)0x06;
const uint8_t PCCC_ERR_FILE_IS_WRONG_SIZE = (int8_t)0x07;
const uint8_t PCCC_ERR_UNSUPPORTED_COMMAND = (uint8_t)0x0e;

// 4f f0 3c 96 06 - address does not point to something usable.
// 4f f0 fa da 07 - file is wrong size.
// 4f f0 a6 b3 0e - command could not be decoded

static slice_s handle_plc5_read_request(slice_s input, slice_s output, plc_s *plc);
static slice_s handle_plc5_write_request(slice_s input, slice_s output, plc_s *plc);
static slice_s handle_slc_read_request(slice_s input, slice_s output, plc_s *plc);
static slice_s handle_slc_write_request(slice_s input, slice_s output, plc_s *plc);
static slice_s make_pccc_error(slice_s output, uint8_t err_code, plc_s *plc);


slice_s dispatch_pccc_request(slice_s input, slice_s output, plc_s *plc)
{
    slice_s pccc_input;
    slice_s pccc_output;
    info("Got packet:");
    slice_dump(input);

    if(slice_len(input) < 20) { /* FIXME - 13 + 7 */
        info("Packet too short!");
        return make_pccc_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* split off the PCCC packet. */
    pccc_input = slice_from_slice(input, 13, slice_len(input)-13);
    pccc_output = slice_from_slice(output, sizeof(PCCC_RESP_PREFIX), slice_len(output)-sizeof(PCCC_RESP_PREFIX));

    /* copy the response prefix. */
    for(size_t i=0; i < sizeof(PCCC_RESP_PREFIX); i++) {
        slice_set_uint8(output, i, PCCC_RESP_PREFIX[i]);
    }

    info("PCCC packet:");
    slice_dump(pccc_input);

    if(slice_match_bytes(pccc_input, PCCC_PREFIX, sizeof(PCCC_PREFIX))) {
        slice_s pccc_command;

        info("Matched valid PCCC prefix.");

        plc->pccc_seq_id = slice_get_uint16_le(pccc_input, 2);

        pccc_command = slice_from_slice(pccc_input, 4, slice_len(pccc_input) - 4);

        /* match the command. */
        if(plc->plc_type == PLC_PLC5 && slice_match_bytes(pccc_command, PLC5_READ, sizeof(PLC5_READ))) {
            pccc_output = handle_plc5_read_request(pccc_command, pccc_output, plc);
        } else if(plc->plc_type == PLC_PLC5 && slice_match_bytes(pccc_command, PLC5_WRITE, sizeof(PLC5_WRITE))) {
            pccc_output = handle_plc5_write_request(pccc_command, pccc_output, plc);
        } else if((plc->plc_type == PLC_SLC || plc->plc_type == PLC_MICROLOGIX) && slice_match_bytes(pccc_command, SLC_READ, sizeof(SLC_READ))) {
            pccc_output = handle_slc_read_request(pccc_command, pccc_output, plc);
        } else if((plc->plc_type == PLC_SLC || plc->plc_type == PLC_MICROLOGIX) && slice_match_bytes(pccc_command, SLC_WRITE, sizeof(SLC_WRITE))) {
            pccc_output = handle_slc_write_request(pccc_command, pccc_output, plc);
        } else {
            info("Unsupported PCCC command!");
            pccc_output = make_pccc_error(pccc_output, PCCC_ERR_UNSUPPORTED_COMMAND, plc);
        }
    }

    return slice_from_slice(output, 0, 11 + slice_len(pccc_output));
}


slice_s handle_plc5_read_request(slice_s input, slice_s output, plc_s *plc)
{
    uint16_t offset = 0;
    size_t start_byte_offset = 0;
    uint16_t transfer_size = 0;
    size_t end_byte_offset = 0;
    size_t tag_size = 0;
    size_t data_file_num = 0;
    size_t data_file_element = 0;
    uint8_t data_file_prefix = 0;
    tag_def_s *tag = plc->tags;

    info("Got packet:");
    slice_dump(input);

    offset = slice_get_uint16_le(input, 1);
    transfer_size = slice_get_uint16_le(input, 3);

    /* decode the data file. */
    data_file_prefix = slice_get_uint8(input, 5);

    /* check the data file prefix. */
    if(data_file_prefix != 0x06) {
        info("Unexpected data file prefix byte %d!", data_file_prefix);
        return make_pccc_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* get data file number. */
    data_file_num = slice_get_uint8(input, 6);

    /* get the data element number. */
    data_file_element = slice_get_uint8(input, 7);

    /* find the tag. */
    while(tag && tag->data_file_num != data_file_num) {
        tag = tag->next_tag;
    }

    if(!tag) {
        info("Unable to find tag with data file %u!", data_file_num);
        return make_pccc_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* now we can check the start and end offsets. */
    tag_size = tag->elem_count * tag->elem_size;
    start_byte_offset = offset + (data_file_element * tag->elem_size);
    end_byte_offset = start_byte_offset + (transfer_size * tag->elem_size);

    if(start_byte_offset >= tag_size) {
        info("Starting offset, %u, is greater than tag size, %d!", (unsigned int)start_byte_offset, (unsigned int)tag_size);
        return make_pccc_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    if(end_byte_offset > tag_size) {
        info("Ending offset, %u, is greater than tag size, %d!", (unsigned int)end_byte_offset, (unsigned int)tag_size);
        return make_pccc_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* check the amount of data requested. */
    if((end_byte_offset - start_byte_offset) > 240) {
        info("Request asks for too much data, %u bytes, for response packet!", (unsigned int)(end_byte_offset - start_byte_offset));
        return make_pccc_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    info("Transfer size %u, tag elem size %u, bytes to transfer %d.", transfer_size, tag->elem_size, transfer_size * tag->elem_size);

    /* build the response. */
    slice_set_uint8(output, 0, 0x4f);
    slice_set_uint8(output, 1, 0); /* no error */
    slice_set_uint16_le(output, 2, plc->pccc_seq_id);

    for(size_t i = 0; i < (transfer_size * tag->elem_size); i++) {
        info("setting byte %d to value %d.", 4 + i, tag->data[start_byte_offset + i]);
        slice_set_uint8(output, 4 + i, tag->data[start_byte_offset + i]);
    }

    info("Output slice length %d.", slice_len(slice_from_slice(output, 0, 4 + (transfer_size * tag->elem_size))));

    return slice_from_slice(output, 0, 4 + (transfer_size * tag->elem_size));
}


slice_s handle_plc5_write_request(slice_s input, slice_s output, plc_s *plc)
{
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

    info("Got packet:");
    slice_dump(input);

    offset = slice_get_uint16_le(input, 1);
    transfer_size = slice_get_uint16_le(input, 3);

    /* decode the data file. */
    data_file_prefix = slice_get_uint8(input, 5);

    /* check the data file prefix. */
    if(data_file_prefix != 0x06) {
        info("Unexpected data file prefix byte %d!", data_file_prefix);
        return make_pccc_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* get data file number. */
    data_file_num = slice_get_uint8(input, 6);

    /* get the data element number. */
    data_file_element = slice_get_uint8(input, 7);

    /* find the tag. */
    while(tag && tag->data_file_num != data_file_num) {
        tag = tag->next_tag;
    }

    if(!tag) {
        info("Unable to find tag with data file %u!", data_file_num);
        return make_pccc_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /*
     * we have the tag, now write the data.   The size of the write
     * needs to be less than the tag size.
     */

    tag_size = tag->elem_count * tag->elem_size;
    start_byte_offset = offset + (data_file_element * tag->elem_size);
    end_byte_offset = start_byte_offset + (transfer_size * tag->elem_size);

    if(start_byte_offset >= tag_size) {
        info("Starting offset, %u, is greater than tag size, %d!", (unsigned int)start_byte_offset, (unsigned int)tag_size);
        return make_pccc_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    if(end_byte_offset > tag_size) {
        info("Ending offset, %u, is greater than tag size, %d!", (unsigned int)end_byte_offset, (unsigned int)tag_size);
        return make_pccc_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    data_start_byte_offset = 8;
    data_len = slice_len(input) - 8;

    if(data_len != (transfer_size * tag->elem_size)) {
        info("Data in packet is not the same length, %u, as the requested transfer, %d!", data_len, (transfer_size * tag->elem_size));
        return make_pccc_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* copy the data into the tag. */
    for(size_t i = 0; i < (transfer_size * tag->elem_size); i++) {
        info("setting byte %d to value %d.", start_byte_offset + i, slice_get_uint8(input, data_start_byte_offset + i));
        tag->data[start_byte_offset + i] = slice_get_uint8(input, data_start_byte_offset + i);
    }

    info("Transfer size %u, tag elem size %u, bytes to transfer %d.", transfer_size, tag->elem_size, transfer_size * tag->elem_size);

    /* build the response. */
    slice_set_uint8(output, 0, 0x4f);
    slice_set_uint8(output, 1, 0); /* no error */
    slice_set_uint16_le(output, 2, plc->pccc_seq_id);

    return slice_from_slice(output, 0, 4);
}


slice_s handle_slc_read_request(slice_s input, slice_s output, plc_s *plc)
{
    size_t start_byte_offset = 0;
    uint8_t transfer_size = 0;
    size_t end_byte_offset = 0;
    size_t tag_size = 0;
    size_t data_file_num = 0;
    size_t data_file_type = 0;
    size_t data_file_element = 0;
    size_t data_file_subelement = 0;
    tag_def_s *tag = plc->tags;

    info("Got packet:");
    slice_dump(input);

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
        info("Data file subelement is unsupported!");
        return make_pccc_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* find the tag. */
    while(tag && tag->data_file_num != data_file_num) {
        tag = tag->next_tag;
    }

    if(!tag) {
        info("Unable to find tag with data file %u!", data_file_num);
        return make_pccc_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    if(tag->tag_type != data_file_type) {
        info("Data file type requested, %u, does not match file type of tag, %d!", data_file_type, tag->tag_type);
        return make_pccc_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* now we can check the start and end offsets. */
    tag_size = tag->elem_count * tag->elem_size;
    start_byte_offset = (data_file_element * tag->elem_size);
    end_byte_offset = start_byte_offset + transfer_size;

    info("Start byte offset %u, end byte offset %u.", (unsigned int)start_byte_offset, (unsigned int)end_byte_offset);

    if(start_byte_offset >= tag_size) {
        info("Starting offset, %u, is greater than tag size, %u!", (unsigned int)start_byte_offset, (unsigned int)tag_size);
        return make_pccc_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    if(end_byte_offset > tag_size) {
        info("Ending offset, %u, is greater than tag size, %u!", (unsigned int)end_byte_offset, (unsigned int)tag_size);
        return make_pccc_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* check the amount of data requested. */
    if(transfer_size > 240) {
        info("Request asks for too much data, %u bytes, for response packet!", (unsigned int)transfer_size);
        return make_pccc_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    info("Transfer size %u (in bytes), tag elem size %u.", transfer_size, tag->elem_size);

    /* build the response. */
    slice_set_uint8(output, 0, 0x4f);
    slice_set_uint8(output, 1, 0); /* no error */
    slice_set_uint16_le(output, 2, plc->pccc_seq_id);

    for(size_t i = 0; i < transfer_size; i++) {
        info("setting byte %d to value %d.", 4 + i, tag->data[start_byte_offset + i]);
        slice_set_uint8(output, 4 + i, tag->data[start_byte_offset + i]);
    }

    info("Output slice length %d.", slice_len(slice_from_slice(output, 0, (size_t)4 + (size_t)transfer_size)));

    return slice_from_slice(output, 0, (size_t)4 + (size_t)transfer_size);
}



slice_s handle_slc_write_request(slice_s input, slice_s output, plc_s *plc)
{
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

    info("Got packet:");
    slice_dump(input);

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
        info("Data file subelement is unsupported!");
        return make_pccc_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* find the tag. */
    while(tag && tag->data_file_num != data_file_num) {
        tag = tag->next_tag;
    }

    if(!tag) {
        info("Unable to find tag with data file %u!", data_file_num);
        return make_pccc_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    if(tag->tag_type != data_file_type) {
        info("Data file type requested, %u, does not match file type of tag, %d!", data_file_type, tag->tag_type);
        return make_pccc_error(output, PCCC_ERR_ADDR_NOT_USABLE, plc);
    }

    /* now we can check the start and end offsets. */
    tag_size = tag->elem_count * tag->elem_size;
    start_byte_offset = (data_file_element * tag->elem_size);
    end_byte_offset = start_byte_offset + transfer_size;

    info("Start byte offset %u, end byte offset %u.", (unsigned int)start_byte_offset, (unsigned int)end_byte_offset);

    if(start_byte_offset >= tag_size) {
        info("Starting offset, %u, is greater than tag size, %d!", (unsigned int)start_byte_offset, (unsigned int)tag_size);
        return make_pccc_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    if(end_byte_offset > tag_size) {
        info("Ending offset, %u, is greater than tag size, %d!", (unsigned int)end_byte_offset, (unsigned int)tag_size);
        return make_pccc_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* check the amount of data requested. */
    if(transfer_size > 240) {
        info("Request asks for too much data, %u bytes, for response packet!", (unsigned int)transfer_size);
        return make_pccc_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    info("Transfer size %u (in bytes), tag elem size %u.", transfer_size, tag->elem_size);

    data_start_byte_offset = 6;
    data_len = slice_len(input) - data_start_byte_offset;

    if(data_len != transfer_size) {
        info("Data in packet is not the same length, %u, as the requested transfer, %d!", data_len, transfer_size);
        return make_pccc_error(output, PCCC_ERR_FILE_IS_WRONG_SIZE, plc);
    }

    /* copy the data into the tag. */
    for(size_t i = 0; i < transfer_size; i++) {
        info("setting byte %d to value %d.", start_byte_offset + i, slice_get_uint8(input, data_start_byte_offset + i));
        tag->data[start_byte_offset + i] = slice_get_uint8(input, data_start_byte_offset + i);
    }

    info("Transfer size %u, tag elem size %u.", transfer_size, tag->elem_size);

    /* build the response. */
    slice_set_uint8(output, 0, 0x4f);
    slice_set_uint8(output, 1, 0); /* no error */
    slice_set_uint16_le(output, 2, plc->pccc_seq_id);

    return slice_from_slice(output, 0, 4);
}




slice_s make_pccc_error(slice_s output, uint8_t err_code, plc_s *plc)
{
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

