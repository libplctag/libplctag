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
#include <libplctag/protocols/enip/server/device.h>
#include "pccc.h"
#include "pccc_defs.h"


/* ============================================================================
 * Constants
 * ============================================================================ */

static const uint8_t PCCC_RESP_PREFIX[11] = {0xcb, 0x00, 0x00, 0x00, 0x07, 0x3d, 0xf3, 0x45, 0x43, 0x50, 0x21};
static const uint8_t PCCC_CMD_PREFIX[2]   = {PCCC_TYPED_CMD, 0x00};

#define PCCC_ERR_ADDR_NOT_USABLE ((uint8_t)0x06)
#define PCCC_ERR_FILE_WRONG_SIZE ((uint8_t)0x07)
#define PCCC_ERR_UNSUPPORTED_CMD ((uint8_t)0x0e)
#define PCCC_DATA_FILE_PREFIX    ((uint8_t)0x06)

#define PCCC_RESP_CMD  ((uint8_t)0x4f)

#define PCCC_CIP_HEADER_SIZE      ((size_t)13)

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

static Bytes pccc_error(Arena *a, uint8_t err, uint16_t seq_id);
static tag_def_t *find_tag_by_file_num(device_t *dev, size_t file_num);
static bool pccc_locate(tag_def_t *tag, size_t start, size_t len, size_t max_len, uint8_t *err_out);
static tag_def_t *pccc_locate_plc5_range(Bytes cmd, device_t *dev, size_t *start_out, size_t *data_bytes_out,
                                         uint8_t *err_out);
static tag_def_t *pccc_locate_slc_range(Bytes cmd, device_t *dev, uint8_t *transfer_size_out, size_t *start_out,
                                        uint8_t *err_out);

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

    pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_DETAIL, 0,
           "pccc_dispatch: payload len=%zu.", payload.len);

    if(payload.len < PCCC_CIP_HEADER_SIZE + 4) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0, "PCCC: payload too short.");
        pccc_resp = pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, 0);
        goto build_response;
    }

    {
        Bytes pccc_pkt = bytes_slice(payload, PCCC_CIP_HEADER_SIZE, payload.len - PCCC_CIP_HEADER_SIZE);

        uint8_t prefix0 = 0, prefix1 = 0;
        Bytes rest = bytes_unpack(pccc_pkt, BYTES_LE, &prefix0, &prefix1, &seq_id, &cmd_byte);

        if(bytes_is_null(rest) || prefix0 != PCCC_CMD_PREFIX[0] || prefix1 != PCCC_CMD_PREFIX[1]) {
            pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0,
                   "PCCC: bad command prefix 0x%02x 0x%02x.",
                   (unsigned)prefix0, (unsigned)prefix1);
            pccc_resp = pccc_error(a, PCCC_ERR_UNSUPPORTED_CMD, 0);
            goto build_response;
        }

        pccc_cmd = rest;
        sess->pccc_seq_id = seq_id;

        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_DETAIL, 0,
               "PCCC seq=0x%04x cmd=0x%02x plc_type=%d.",
               (unsigned)seq_id, (unsigned)cmd_byte, (int)dev->plc_type);

        if(dev->plc_type == ENIP_PLC_PLC5) {
            switch(cmd_byte) {
                case PCCC_PLC5_READ_FNC:  pccc_resp = handle_plc5_read(a, pccc_cmd, seq_id, dev);  break;
                case PCCC_PLC5_WRITE_FNC: pccc_resp = handle_plc5_write(a, pccc_cmd, seq_id, dev); break;
                case PCCC_PLC5_RMW_FNC:   pccc_resp = handle_plc5_rmw(a, pccc_cmd, seq_id, dev);   break;
                default:
                    pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0,
                           "PCCC PLC/5: unknown cmd 0x%02x.", (unsigned)cmd_byte);
                    pccc_resp = pccc_error(a, PCCC_ERR_UNSUPPORTED_CMD, seq_id);
                    break;
            }
        } else if(dev->plc_type == ENIP_PLC_SLC || dev->plc_type == ENIP_PLC_MLGX) {
            switch(cmd_byte) {
                case PCCC_SLC_READ_FNC:  pccc_resp = handle_slc_read(a, pccc_cmd, seq_id, dev);  break;
                case PCCC_SLC_WRITE_FNC: pccc_resp = handle_slc_write(a, pccc_cmd, seq_id, dev); break;
                case PCCC_SLC_RMW_FNC:   pccc_resp = handle_slc_rmw(a, pccc_cmd, seq_id, dev);   break;
                default:
                    pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0,
                           "PCCC SLC: unknown cmd 0x%02x.", (unsigned)cmd_byte);
                    pccc_resp = pccc_error(a, PCCC_ERR_UNSUPPORTED_CMD, seq_id);
                    break;
            }
        } else {
            pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0,
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
    tag_def_t *found = NULL;
    critical_block(dev->tags_mutex) {
        tag_def_t *tag = dev->tags;
        while(tag) {
            if(tag->data_file_num == file_num) { found = tag; break; }
            tag = tag->next_tag;
        }
    }
    return found;
}


/* Shared front half for all six wire handlers (3.g): bounds-check
 * [start, start+len) against tag's total byte size (elem_count*elem_size).
 * max_len, if nonzero, additionally caps len (PCCC_MAX_TRANSFER_BYTES for
 * the windowed read/write ops; 0 skips that check -- matches the RMW
 * handlers' pre-existing behavior, which never separately capped a request
 * already sized to one tag element). Sets *err_out to the CIP/PCCC error to
 * reply with and returns false on any bounds failure. */
static bool pccc_locate(tag_def_t *tag, size_t start, size_t len, size_t max_len, uint8_t *err_out) {
    size_t tag_size = tag->elem_count * tag->elem_size;
    size_t end = start + len;
    if(start >= tag_size || end > tag_size) {
        *err_out = PCCC_ERR_FILE_WRONG_SIZE;
        return false;
    }

    if(max_len != 0 && len > max_len) {
        *err_out = PCCC_ERR_FILE_WRONG_SIZE;
        return false;
    }

    return true;
}


/* PLC-5 Word Range Read/Write (FNC 0x01/0x00) shared front half: parse the
 * offset_words/transfer_size/file_prefix/file_num/file_element fields
 * common to both, validate file_prefix, and locate+bounds-check the tag via
 * pccc_locate. *start_out / *data_bytes_out are only meaningful when a tag is
 * returned. */
static tag_def_t *pccc_locate_plc5_range(Bytes cmd, device_t *dev, size_t *start_out, size_t *data_bytes_out,
                                         uint8_t *err_out) {
    uint16_t offset_words  = 0;
    uint16_t transfer_size = 0;
    uint8_t  file_prefix   = 0;
    uint8_t  file_num      = 0;
    uint8_t  file_element  = 0;

    if(bytes_is_null(bytes_unpack(cmd, BYTES_LE, BYTES_SKIP(1) /* fnc, already dispatched on */, &offset_words,
                                  &transfer_size, &file_prefix, &file_num, &file_element))) {
        *err_out = PCCC_ERR_FILE_WRONG_SIZE;
        return NULL;
    }

    if(file_prefix != PCCC_DATA_FILE_PREFIX) {
        *err_out = PCCC_ERR_ADDR_NOT_USABLE;
        return NULL;
    }

    tag_def_t *tag = find_tag_by_file_num(dev, file_num);
    if(!tag) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0, "PLC/5: tag file %u not found.", (unsigned)file_num);
        *err_out = PCCC_ERR_ADDR_NOT_USABLE;
        return NULL;
    }

    size_t start = (size_t)(offset_words * 2) + ((size_t)file_element * tag->elem_size);
    size_t data_bytes = (size_t)transfer_size * tag->elem_size;

    if(!pccc_locate(tag, start, data_bytes, PCCC_MAX_TRANSFER_BYTES, err_out)) { return NULL; }

    *start_out = start;
    *data_bytes_out = data_bytes;
    return tag;
}


/* SLC/MicroLogix Typed Logical Read/Write (FNC 0xA2/0xAA) shared front half:
 * parse the transfer_size/file_num/file_type/file_element/subelement fields
 * common to both, validate subelement==0 and the file_type match, and
 * locate+bounds-check the tag via pccc_locate. *start_out is only
 * meaningful when a tag is returned; *transfer_size_out is always set from
 * the wire (needed even on failure paths that log it). */
static tag_def_t *pccc_locate_slc_range(Bytes cmd, device_t *dev, uint8_t *transfer_size_out, size_t *start_out,
                                        uint8_t *err_out) {
    uint8_t file_num      = 0;
    uint8_t file_type     = 0;
    uint8_t file_element  = 0;
    uint8_t subelement    = 0;

    if(bytes_is_null(bytes_unpack(cmd, BYTES_LE, BYTES_SKIP(1) /* fnc, already dispatched on */, transfer_size_out,
                                  &file_num, &file_type, &file_element, &subelement))) {
        *err_out = PCCC_ERR_FILE_WRONG_SIZE;
        return NULL;
    }

    if(subelement != 0) {
        *err_out = PCCC_ERR_ADDR_NOT_USABLE;
        return NULL;
    }

    tag_def_t *tag = find_tag_by_file_num(dev, file_num);
    if(!tag) {
        *err_out = PCCC_ERR_ADDR_NOT_USABLE;
        return NULL;
    }

    if((uint16_t)tag->tag_type != (uint16_t)file_type) {
        pdebug(DEBUG_MODULE_ENIP, PLCTAG_DEBUG_WARN, 0, "SLC: file type mismatch got 0x%02x expected 0x%04x.",
               (unsigned)file_type, (unsigned)tag->tag_type);
        *err_out = PCCC_ERR_ADDR_NOT_USABLE;
        return NULL;
    }

    size_t start = (size_t)file_element * tag->elem_size;

    if(!pccc_locate(tag, start, *transfer_size_out, PCCC_MAX_TRANSFER_BYTES, err_out)) { return NULL; }

    *start_out = start;
    return tag;
}


static Bytes handle_plc5_read(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev) {
    size_t start = 0, data_bytes = 0;
    uint8_t err = 0;

    tag_def_t *tag = pccc_locate_plc5_range(cmd, dev, &start, &data_bytes, &err);
    if(!tag) { return pccc_error(a, err, seq_id); }

    Bytes hdr = bytes_pack(a, BYTES_LE, PCCC_RESP_CMD, (uint8_t)0, seq_id);

    Bytes data_buf = bytes_alloc(a, data_bytes);
    if(bytes_is_null(data_buf)) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    mutex_lock(tag->data_mutex);
    bytes_pack_into(data_buf, BYTES_LE, bytes_from_buf(tag->data + start, data_bytes));
    mutex_unlock(tag->data_mutex);

    if(tag->read_cb) {
        tag->read_cb(dev->sim, tag->name, data_buf.data, (uint32_t)data_bytes, tag->user_data);
    }

    return bytes_concat(a, hdr, data_buf);
}


static Bytes handle_plc5_write(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev) {
    size_t start = 0, data_bytes = 0;
    uint8_t err = 0;

    tag_def_t *tag = pccc_locate_plc5_range(cmd, dev, &start, &data_bytes, &err);
    if(!tag) { return pccc_error(a, err, seq_id); }

    Bytes write_data = bytes_slice(cmd, 8, data_bytes);
    if(bytes_is_null(write_data)) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    mutex_lock(tag->data_mutex);
    bytes_pack_into(bytes_from_buf(tag->data + start, data_bytes), BYTES_LE, write_data);
    mutex_unlock(tag->data_mutex);

    if(tag->write_cb) {
        tag->write_cb(dev->sim, tag->name, (void *)write_data.data, (uint32_t)data_bytes, tag->user_data);
    }

    return bytes_pack(a, BYTES_LE, PCCC_RESP_CMD, (uint8_t)0, seq_id);
}


static Bytes handle_plc5_rmw(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev) {
    uint8_t file_prefix  = 0;
    uint8_t file_num     = 0;
    uint8_t file_element = 0;

    if(bytes_is_null(bytes_unpack(cmd, BYTES_LE, BYTES_SKIP(1) /* fnc, already dispatched on */, &file_prefix, &file_num,
                                  &file_element))) {
        return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id);
    }

    if(file_prefix != PCCC_DATA_FILE_PREFIX) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag_def_t *tag = find_tag_by_file_num(dev, file_num);
    if(!tag) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    size_t start = (size_t)file_element * tag->elem_size;
    uint8_t err = 0;
    if(!pccc_locate(tag, start, tag->elem_size, 0, &err)) { return pccc_error(a, err, seq_id); }

    size_t mask_offset = 4;
    Bytes and_mask = bytes_slice(cmd, mask_offset, tag->elem_size);
    Bytes or_mask  = bytes_slice(cmd, mask_offset + tag->elem_size, tag->elem_size);
    if(bytes_is_null(and_mask) || bytes_is_null(or_mask)) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    uint8_t rmw_scratch[8];
    mutex_lock(tag->data_mutex);
    for(size_t i = 0; i < tag->elem_size; i++) {
        tag->data[start + i] = (uint8_t)((tag->data[start + i] & and_mask.data[i]) | or_mask.data[i]);
    }
    if(tag->write_cb) {
        bytes_pack_into(bytes_from_buf(rmw_scratch, sizeof(rmw_scratch)), BYTES_LE, bytes_from_buf(tag->data + start, tag->elem_size));
    }
    mutex_unlock(tag->data_mutex);

    if(tag->write_cb) {
        tag->write_cb(dev->sim, tag->name, rmw_scratch, (uint32_t)tag->elem_size, tag->user_data);
    }

    return bytes_pack(a, BYTES_LE, PCCC_RESP_CMD, (uint8_t)0, seq_id);
}


static Bytes handle_slc_read(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev) {
    uint8_t transfer_size = 0;
    size_t start = 0;
    uint8_t err = 0;

    tag_def_t *tag = pccc_locate_slc_range(cmd, dev, &transfer_size, &start, &err);
    if(!tag) { return pccc_error(a, err, seq_id); }

    Bytes hdr = bytes_pack(a, BYTES_LE, PCCC_RESP_CMD, (uint8_t)0, seq_id);

    Bytes data_buf = bytes_alloc(a, transfer_size);
    if(bytes_is_null(data_buf)) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    mutex_lock(tag->data_mutex);
    bytes_pack_into(data_buf, BYTES_LE, bytes_from_buf(tag->data + start, transfer_size));
    mutex_unlock(tag->data_mutex);

    if(tag->read_cb) {
        tag->read_cb(dev->sim, tag->name, data_buf.data, (uint32_t)transfer_size, tag->user_data);
    }

    return bytes_concat(a, hdr, data_buf);
}


static Bytes handle_slc_write(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev) {
    uint8_t transfer_size = 0;
    size_t start = 0;
    uint8_t err = 0;

    tag_def_t *tag = pccc_locate_slc_range(cmd, dev, &transfer_size, &start, &err);
    if(!tag) { return pccc_error(a, err, seq_id); }

    Bytes write_data = bytes_slice(cmd, 6, transfer_size);
    if(bytes_is_null(write_data)) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    mutex_lock(tag->data_mutex);
    bytes_pack_into(bytes_from_buf(tag->data + start, transfer_size), BYTES_LE, write_data);
    mutex_unlock(tag->data_mutex);

    if(tag->write_cb) {
        tag->write_cb(dev->sim, tag->name, (void *)write_data.data, (uint32_t)transfer_size, tag->user_data);
    }

    return bytes_pack(a, BYTES_LE, PCCC_RESP_CMD, (uint8_t)0, seq_id);
}


static Bytes handle_slc_rmw(Arena *a, Bytes cmd, uint16_t seq_id, device_t *dev) {
    uint8_t transfer_size = 0;
    uint8_t file_num      = 0;
    uint8_t file_type     = 0;
    uint8_t file_element  = 0;
    uint8_t subelement    = 0;

    if(bytes_is_null(bytes_unpack(cmd, BYTES_LE, BYTES_SKIP(1) /* fnc, already dispatched on */, &transfer_size, &file_num,
                                  &file_type, &file_element, &subelement))) {
        return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id);
    }

    if(transfer_size != 2) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }
    if(subelement != 0) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag_def_t *tag = find_tag_by_file_num(dev, file_num);
    if(!tag) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    if((uint16_t)tag->tag_type != (uint16_t)file_type || tag->elem_size != 2) {
        return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id);
    }

    size_t start = (size_t)file_element * tag->elem_size;
    uint8_t err = 0;
    if(!pccc_locate(tag, start, 2, 0, &err)) { return pccc_error(a, err, seq_id); }

    Bytes mask_bytes = bytes_slice(cmd, 6, 2);
    Bytes new_bytes  = bytes_slice(cmd, 8, 2);
    if(bytes_is_null(mask_bytes) || bytes_is_null(new_bytes)) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    uint8_t slc_rmw_scratch[2];
    mutex_lock(tag->data_mutex);
    for(size_t i = 0; i < 2; i++) {
        uint8_t mask     = mask_bytes.data[i];
        uint8_t new_data = new_bytes.data[i];
        tag->data[start + i] = (uint8_t)((tag->data[start + i] & (uint8_t)~mask) | (new_data & mask));
    }
    if(tag->write_cb) {
        bytes_pack_into(bytes_from_buf(slc_rmw_scratch, sizeof(slc_rmw_scratch)), BYTES_LE, bytes_from_buf(tag->data + start, 2));
    }
    mutex_unlock(tag->data_mutex);

    if(tag->write_cb) {
        tag->write_cb(dev->sim, tag->name, slc_rmw_scratch, (uint32_t)2, tag->user_data);
    }

    return bytes_pack(a, BYTES_LE, PCCC_RESP_CMD, (uint8_t)0, seq_id);
}
