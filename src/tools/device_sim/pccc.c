/***************************************************************************
 *   Copyright (C) 2026 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever    *
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
 * pccc.c — PCCC (Programmable Controller Communications Command) dispatcher.
 * Adapted from src/poc/ab_server_fiber/pccc.c.
 */

#include <stdint.h>

#include "platform.h"
#include "utils/arena.h"
#include "utils/bytes.h"
#include "utils/debug.h"
#include "device.h"
#include "pccc.h"

#define DEBUG_MOD DEBUG_MODULE_UTILS

/* ============================================================================
 * Constants
 * ============================================================================ */

static const uint8_t PCCC_RESP_PREFIX[11] = {0xcb, 0x00, 0x00, 0x00, 0x07, 0x3d, 0xf3, 0x45, 0x43, 0x50, 0x21};
static const uint8_t PCCC_CMD_PREFIX[2]   = {0x0f, 0x00};

#define PCCC_ERR_ADDR_NOT_USABLE ((uint8_t)0x06)
#define PCCC_ERR_FILE_WRONG_SIZE ((uint8_t)0x07)
#define PCCC_ERR_UNSUPPORTED_CMD ((uint8_t)0x0e)
#define PCCC_DATA_FILE_PREFIX    ((uint8_t)0x06)

#define PLC5_CMD_READ  ((uint8_t)0x01)
#define PLC5_CMD_WRITE ((uint8_t)0x00)
#define PLC5_CMD_RMW   ((uint8_t)0x26)
#define SLC_CMD_READ   ((uint8_t)0xa2)
#define SLC_CMD_WRITE  ((uint8_t)0xaa)
#define SLC_CMD_RMW    ((uint8_t)0xab)

#define PCCC_RESP_CMD  ((uint8_t)0x4f)

#define PCCC_CIP_HEADER_SIZE      ((size_t)13)
#define PCCC_MAX_TRANSFER_BYTES   ((size_t)240)

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

static Bytes pccc_error(Arena *a, uint8_t err, uint16_t seq_id);
static tag_def_t *find_tag_by_file_num(device_t *dev, size_t file_num);

static Bytes handle_plc5_read(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev);
static Bytes handle_plc5_write(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev);
static Bytes handle_plc5_rmw(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev);
static Bytes handle_slc_read(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev);
static Bytes handle_slc_write(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev);
static Bytes handle_slc_rmw(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev);

/* ============================================================================
 * Public functions
 * ============================================================================ */

extern Bytes pccc_dispatch(Arena *a, Bytes payload, eip_session_t *sess, device_t *dev) {
    uint16_t seq_id  = 0;
    uint8_t  cmd_byte = 0;
    Bytes pccc_cmd  = {0};
    Bytes pccc_resp = {0};

    pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0,
           "pccc_dispatch: payload len=%zu.", payload.len);

    if(payload.len < PCCC_CIP_HEADER_SIZE + 4) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "PCCC: payload too short.");
        pccc_resp = pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, 0);
        goto build_response;
    }

    {
        Bytes pccc_pkt = bytes_slice(payload, PCCC_CIP_HEADER_SIZE, payload.len - PCCC_CIP_HEADER_SIZE);

        if(pccc_pkt.len < 4
           || pccc_pkt.data[0] != PCCC_CMD_PREFIX[0]
           || pccc_pkt.data[1] != PCCC_CMD_PREFIX[1]) {
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                   "PCCC: bad command prefix 0x%02x 0x%02x.",
                   (unsigned)pccc_pkt.data[0], (unsigned)pccc_pkt.data[1]);
            pccc_resp = pccc_error(a, PCCC_ERR_UNSUPPORTED_CMD, 0);
            goto build_response;
        }

        seq_id   = (uint16_t)(pccc_pkt.data[2] | ((uint16_t)pccc_pkt.data[3] << 8));
        cmd_byte = pccc_pkt.data[4];
        pccc_cmd = bytes_slice(pccc_pkt, 4, pccc_pkt.len - 4);
        sess->pccc_seq_id = seq_id;

        pdebug(DEBUG_MOD, PLCTAG_DEBUG_DETAIL, 0,
               "PCCC seq=0x%04x cmd=0x%02x plc_type=%d.",
               (unsigned)seq_id, (unsigned)cmd_byte, (int)dev->plc_type);

        if(dev->plc_type == PLC_PLC5) {
            switch(cmd_byte) {
                case PLC5_CMD_READ:  pccc_resp = handle_plc5_read(a, pccc_cmd, seq_id, dev);  break;
                case PLC5_CMD_WRITE: pccc_resp = handle_plc5_write(a, pccc_cmd, seq_id, dev); break;
                case PLC5_CMD_RMW:   pccc_resp = handle_plc5_rmw(a, pccc_cmd, seq_id, dev);   break;
                default:
                    pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                           "PCCC PLC/5: unknown cmd 0x%02x.", (unsigned)cmd_byte);
                    pccc_resp = pccc_error(a, PCCC_ERR_UNSUPPORTED_CMD, seq_id);
                    break;
            }
        } else if(dev->plc_type == PLC_SLC || dev->plc_type == PLC_MICROLOGIX) {
            switch(cmd_byte) {
                case SLC_CMD_READ:  pccc_resp = handle_slc_read(a, pccc_cmd, seq_id, dev);  break;
                case SLC_CMD_WRITE: pccc_resp = handle_slc_write(a, pccc_cmd, seq_id, dev); break;
                case SLC_CMD_RMW:   pccc_resp = handle_slc_rmw(a, pccc_cmd, seq_id, dev);   break;
                default:
                    pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                           "PCCC SLC: unknown cmd 0x%02x.", (unsigned)cmd_byte);
                    pccc_resp = pccc_error(a, PCCC_ERR_UNSUPPORTED_CMD, seq_id);
                    break;
            }
        } else {
            pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
                   "PCCC: unsupported plc_type %d.", (int)dev->plc_type);
            pccc_resp = pccc_error(a, PCCC_ERR_UNSUPPORTED_CMD, 0);
        }
    }

build_response:
    if(bytes_is_null(pccc_resp)) {
        pccc_resp = pccc_error(a, PCCC_ERR_UNSUPPORTED_CMD, sess->pccc_seq_id);
    }
    Bytes resp_prefix = bytes_from_buf(PCCC_RESP_PREFIX, sizeof(PCCC_RESP_PREFIX));
    return bytes_concat(a, resp_prefix, pccc_resp);
}

/* ============================================================================
 * Static functions
 * ============================================================================ */

static Bytes pccc_error(Arena *a, uint8_t err, uint16_t seq_id) {
    return bytes_pack(a, BYTES_LE, PCCC_RESP_CMD, (uint8_t)0xf0, seq_id, (int8_t)err);
}


static tag_def_t *find_tag_by_file_num(device_t *dev, size_t file_num) {
    tag_def_t *tag = dev->tags;
    while(tag && tag->data_file_num != file_num) { tag = tag->next_tag; }
    return tag;
}


static Bytes handle_plc5_read(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev) {
    uint16_t offset_words  = 0;
    uint16_t transfer_size = 0;
    uint8_t  file_prefix   = 0;
    uint8_t  file_num      = 0;
    uint8_t  file_element  = 0;
    size_t   start = 0;
    size_t   end   = 0;

    if(cmd.len < 8) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    offset_words  = (uint16_t)(cmd.data[1] | ((uint16_t)cmd.data[2] << 8));
    transfer_size = (uint16_t)(cmd.data[3] | ((uint16_t)cmd.data[4] << 8));
    file_prefix   = cmd.data[5];
    file_num      = cmd.data[6];
    file_element  = cmd.data[7];

    if(file_prefix != PCCC_DATA_FILE_PREFIX) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag_def_t *tag = find_tag_by_file_num(dev, file_num);
    if(!tag) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0, "PLC/5 read: tag file %u not found.", (unsigned)file_num);
        return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id);
    }

    size_t tag_size = tag->elem_count * tag->elem_size;
    start = (size_t)(offset_words * 2) + ((size_t)file_element * tag->elem_size);
    end   = start + ((size_t)transfer_size * tag->elem_size);

    if(start >= tag_size || end > tag_size) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    size_t data_bytes = (size_t)transfer_size * tag->elem_size;
    if(data_bytes > PCCC_MAX_TRANSFER_BYTES) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    Bytes hdr = bytes_pack(a, BYTES_LE, PCCC_RESP_CMD, (uint8_t)0, seq_id);
    Bytes resp;
    mutex_lock(tag->data_mutex);
    Bytes data = bytes_from_buf(tag->data + start, data_bytes);
    resp = bytes_concat(a, hdr, data);
    mutex_unlock(tag->data_mutex);
    return resp;
}


static Bytes handle_plc5_write(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev) {
    uint16_t offset_words  = 0;
    uint16_t transfer_size = 0;
    uint8_t  file_prefix   = 0;
    uint8_t  file_num      = 0;
    uint8_t  file_element  = 0;
    size_t   start = 0;
    size_t   end   = 0;

    if(cmd.len < 9) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    offset_words  = (uint16_t)(cmd.data[1] | ((uint16_t)cmd.data[2] << 8));
    transfer_size = (uint16_t)(cmd.data[3] | ((uint16_t)cmd.data[4] << 8));
    file_prefix   = cmd.data[5];
    file_num      = cmd.data[6];
    file_element  = cmd.data[7];

    if(file_prefix != PCCC_DATA_FILE_PREFIX) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag_def_t *tag = find_tag_by_file_num(dev, file_num);
    if(!tag) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    size_t tag_size = tag->elem_count * tag->elem_size;
    start = (size_t)(offset_words * 2) + ((size_t)file_element * tag->elem_size);
    end   = start + ((size_t)transfer_size * tag->elem_size);

    if(start >= tag_size || end > tag_size) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    size_t data_bytes = (size_t)transfer_size * tag->elem_size;
    if(data_bytes > PCCC_MAX_TRANSFER_BYTES) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    Bytes write_data = bytes_slice(cmd, 8, data_bytes);
    if(bytes_is_null(write_data)) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    mutex_lock(tag->data_mutex);
    mem_copy(tag->data + start, write_data.data, (int)data_bytes);
    mutex_unlock(tag->data_mutex);

    return bytes_pack(a, BYTES_LE, PCCC_RESP_CMD, (uint8_t)0, seq_id);
}


static Bytes handle_plc5_rmw(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev) {
    uint8_t file_prefix  = 0;
    uint8_t file_num     = 0;
    uint8_t file_element = 0;
    size_t  start = 0;

    if(cmd.len < 4) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    file_prefix  = cmd.data[1];
    file_num     = cmd.data[2];
    file_element = cmd.data[3];

    if(file_prefix != PCCC_DATA_FILE_PREFIX) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag_def_t *tag = find_tag_by_file_num(dev, file_num);
    if(!tag) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    size_t tag_size = tag->elem_count * tag->elem_size;
    start = (size_t)file_element * tag->elem_size;

    if(start + tag->elem_size > tag_size) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    size_t mask_offset = 4;
    if(cmd.len < mask_offset + tag->elem_size * 2) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    mutex_lock(tag->data_mutex);
    for(size_t i = 0; i < tag->elem_size; i++) {
        uint8_t and_mask = cmd.data[mask_offset + i];
        uint8_t or_mask  = cmd.data[mask_offset + tag->elem_size + i];
        tag->data[start + i] = (uint8_t)((tag->data[start + i] & and_mask) | or_mask);
    }
    mutex_unlock(tag->data_mutex);

    return bytes_pack(a, BYTES_LE, PCCC_RESP_CMD, (uint8_t)0, seq_id);
}


static Bytes handle_slc_read(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev) {
    uint8_t transfer_size = 0;
    uint8_t file_num      = 0;
    uint8_t file_type     = 0;
    uint8_t file_element  = 0;
    uint8_t subelement    = 0;
    size_t  start = 0;
    size_t  end   = 0;

    if(cmd.len < 6) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    transfer_size = cmd.data[1];
    file_num      = cmd.data[2];
    file_type     = cmd.data[3];
    file_element  = cmd.data[4];
    subelement    = cmd.data[5];

    if(subelement != 0) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag_def_t *tag = find_tag_by_file_num(dev, file_num);
    if(!tag) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    if((uint16_t)tag->tag_type != (uint16_t)file_type) {
        pdebug(DEBUG_MOD, PLCTAG_DEBUG_WARN, 0,
               "SLC read: file type mismatch got 0x%02x expected 0x%04x.",
               (unsigned)file_type, (unsigned)tag->tag_type);
        return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id);
    }

    size_t tag_size = tag->elem_count * tag->elem_size;
    start = (size_t)file_element * tag->elem_size;
    end   = start + transfer_size;

    if(start >= tag_size || end > tag_size) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }
    if(transfer_size > PCCC_MAX_TRANSFER_BYTES) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    Bytes hdr = bytes_pack(a, BYTES_LE, PCCC_RESP_CMD, (uint8_t)0, seq_id);
    Bytes resp;
    mutex_lock(tag->data_mutex);
    Bytes data = bytes_from_buf(tag->data + start, transfer_size);
    resp = bytes_concat(a, hdr, data);
    mutex_unlock(tag->data_mutex);
    return resp;
}


static Bytes handle_slc_write(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev) {
    uint8_t transfer_size = 0;
    uint8_t file_num      = 0;
    uint8_t file_type     = 0;
    uint8_t file_element  = 0;
    uint8_t subelement    = 0;
    size_t  start = 0;
    size_t  end   = 0;

    if(cmd.len < 7) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    transfer_size = cmd.data[1];
    file_num      = cmd.data[2];
    file_type     = cmd.data[3];
    file_element  = cmd.data[4];
    subelement    = cmd.data[5];

    if(subelement != 0) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag_def_t *tag = find_tag_by_file_num(dev, file_num);
    if(!tag) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    if((uint16_t)tag->tag_type != (uint16_t)file_type) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    size_t tag_size = tag->elem_count * tag->elem_size;
    start = (size_t)file_element * tag->elem_size;
    end   = start + transfer_size;

    if(start >= tag_size || end > tag_size) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }
    if(transfer_size > PCCC_MAX_TRANSFER_BYTES) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    Bytes write_data = bytes_slice(cmd, 6, transfer_size);
    if(bytes_is_null(write_data)) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    mutex_lock(tag->data_mutex);
    mem_copy(tag->data + start, write_data.data, (int)transfer_size);
    mutex_unlock(tag->data_mutex);

    return bytes_pack(a, BYTES_LE, PCCC_RESP_CMD, (uint8_t)0, seq_id);
}


static Bytes handle_slc_rmw(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev) {
    uint8_t transfer_size = 0;
    uint8_t file_num      = 0;
    uint8_t file_type     = 0;
    uint8_t file_element  = 0;
    uint8_t subelement    = 0;
    size_t  start = 0;

    if(cmd.len < 10) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    transfer_size = cmd.data[1];
    file_num      = cmd.data[2];
    file_type     = cmd.data[3];
    file_element  = cmd.data[4];
    subelement    = cmd.data[5];

    if(transfer_size != 2) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }
    if(subelement != 0) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag_def_t *tag = find_tag_by_file_num(dev, file_num);
    if(!tag) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    if((uint16_t)tag->tag_type != (uint16_t)file_type || tag->elem_size != 2) {
        return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id);
    }

    size_t tag_size = tag->elem_count * tag->elem_size;
    start = (size_t)file_element * tag->elem_size;
    if(start + 2 > tag_size) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    mutex_lock(tag->data_mutex);
    for(size_t i = 0; i < 2; i++) {
        uint8_t mask     = cmd.data[6 + i];
        uint8_t new_data = cmd.data[8 + i];
        tag->data[start + i] = (uint8_t)((tag->data[start + i] & (uint8_t)~mask) | (new_data & mask));
    }
    mutex_unlock(tag->data_mutex);

    return bytes_pack(a, BYTES_LE, PCCC_RESP_CMD, (uint8_t)0, seq_id);
}
