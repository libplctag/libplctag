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
 *
 * Called from cip.c when the CIP PCCC Execute service (0x4B) is received.
 *
 * The full CIP payload passed in has the following layout:
 *
 *   Bytes 0-12   CIP header (service, path, PCCC execute obj path, PCCC prefix)
 *   Bytes 13+    PCCC request payload
 *
 * PCCC prefix (bytes 13-16):
 *   0x0f 0x00   command + STS
 *   uint16 LE   sequence ID
 *   byte        PCCC command
 *   ...         PCCC data
 *
 * Response structure:
 *   11 bytes fixed prefix  (PCCC_RESP_PREFIX)
 *   PCCC response payload  (4+ bytes depending on command)
 *
 * PCCC commands supported:
 *   PLC/5:      0x01 read, 0x00 write, 0x26 RMW
 *   SLC/MLX:    0xA2 read, 0xAA write, 0xAB RMW
 */

#include <stdint.h>
#include <string.h>

#include "arena.h"
#include "bytes.h"
#include "log.h"
#include "pccc.h"
#include "plc.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Fixed 11-byte prefix prepended to every PCCC response. */
static const uint8_t PCCC_RESP_PREFIX[11] = {0xcb, 0x00, 0x00, 0x00, 0x07, 0x3d, 0xf3, 0x45, 0x43, 0x50, 0x21};

/* Expected 2-byte PCCC command prefix (cmd=0x0f, sts=0x00). */
static const uint8_t PCCC_CMD_PREFIX[2] = {0x0f, 0x00};

/* PCCC error codes */
#define PCCC_ERR_ADDR_NOT_USABLE ((uint8_t)0x06)
#define PCCC_ERR_FILE_WRONG_SIZE ((uint8_t)0x07)
#define PCCC_ERR_UNSUPPORTED_CMD ((uint8_t)0x0e)

/* PCCC data file address prefix byte. */
#define PCCC_DATA_FILE_PREFIX ((uint8_t)0x06)

/* PCCC read/write commands */
#define PLC5_CMD_READ ((uint8_t)0x01)
#define PLC5_CMD_WRITE ((uint8_t)0x00)
#define PLC5_CMD_RMW ((uint8_t)0x26)
#define SLC_CMD_READ ((uint8_t)0xa2)
#define SLC_CMD_WRITE ((uint8_t)0xaa)
#define SLC_CMD_RMW ((uint8_t)0xab)

/* PCCC response command byte (success). */
#define PCCC_RESP_CMD ((uint8_t)0x4f)

/* Offset into the full CIP payload where the PCCC packet starts. */
#define PCCC_CIP_HEADER_SIZE ((size_t)13)

/* Max bytes per PCCC read/write (PLC/5 and SLC specification limit). */
#define PCCC_MAX_TRANSFER_BYTES ((size_t)240)

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

static Bytes pccc_error(Arena *a, uint8_t err, uint16_t seq_id);
static tag_def_t *find_tag_by_file_num(plc_config_t *cfg, size_t file_num);

static Bytes handle_plc5_read(Arena *a, Bytes cmd, uint16_t seq_id, plc_config_t *cfg);
static Bytes handle_plc5_write(Arena *a, Bytes cmd, uint16_t seq_id, plc_config_t *cfg);
static Bytes handle_plc5_rmw(Arena *a, Bytes cmd, uint16_t seq_id, plc_config_t *cfg);
static Bytes handle_slc_read(Arena *a, Bytes cmd, uint16_t seq_id, plc_config_t *cfg);
static Bytes handle_slc_write(Arena *a, Bytes cmd, uint16_t seq_id, plc_config_t *cfg);
static Bytes handle_slc_rmw(Arena *a, Bytes cmd, uint16_t seq_id, plc_config_t *cfg);

/* ============================================================================
 * Public functions
 * ============================================================================ */

extern Bytes pccc_dispatch(Arena *a, Bytes payload, eip_session_t *sess, plc_config_t *cfg) {
    uint16_t seq_id = 0;
    uint8_t cmd_byte = 0;
    Bytes pccc_cmd = {0};
    Bytes pccc_resp = {0};
    Bytes resp_prefix = {0};

    pdlog(LOG_MODULE_PCCC, LOG_LEVEL_DETAIL, "pccc_dispatch: payload len=%zu", payload.len);

    /* Strip the 13-byte CIP header to get to the PCCC packet. */
    if(payload.len < PCCC_CIP_HEADER_SIZE + 4) {
        pdlog(LOG_MODULE_PCCC, LOG_LEVEL_WARN, "PCCC: payload too short");
        pccc_resp = pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, 0);
        goto build_response;
    }

    Bytes pccc_pkt = bytes_slice(payload, PCCC_CIP_HEADER_SIZE, payload.len - PCCC_CIP_HEADER_SIZE);

    /* Verify PCCC prefix: first two bytes must be 0x0f 0x00. */
    if(pccc_pkt.len < 4 || pccc_pkt.data[0] != PCCC_CMD_PREFIX[0] || pccc_pkt.data[1] != PCCC_CMD_PREFIX[1]) {
        pdlog(LOG_MODULE_PCCC, LOG_LEVEL_WARN, "PCCC: bad command prefix 0x%02x 0x%02x", pccc_pkt.data[0], pccc_pkt.data[1]);
        pccc_resp = pccc_error(a, PCCC_ERR_UNSUPPORTED_CMD, 0);
        goto build_response;
    }

    /* Extract sequence ID and command byte. */
    seq_id = (uint16_t)(pccc_pkt.data[2] | ((uint16_t)pccc_pkt.data[3] << 8));
    cmd_byte = pccc_pkt.data[4];
    pccc_cmd = bytes_slice(pccc_pkt, 4, pccc_pkt.len - 4);

    sess->pccc_seq_id = seq_id;

    pdlog(LOG_MODULE_PCCC, LOG_LEVEL_DETAIL, "PCCC seq=0x%04x cmd=0x%02x plc_type=%d", seq_id, cmd_byte, cfg->plc_type);

    if(cfg->plc_type == PLC_PLC5) {
        switch(cmd_byte) {
            case PLC5_CMD_READ: pccc_resp = handle_plc5_read(a, pccc_cmd, seq_id, cfg); break;
            case PLC5_CMD_WRITE: pccc_resp = handle_plc5_write(a, pccc_cmd, seq_id, cfg); break;
            case PLC5_CMD_RMW: pccc_resp = handle_plc5_rmw(a, pccc_cmd, seq_id, cfg); break;
            default:
                pdlog(LOG_MODULE_PCCC, LOG_LEVEL_WARN, "PCCC PLC/5: unknown cmd 0x%02x", cmd_byte);
                pccc_resp = pccc_error(a, PCCC_ERR_UNSUPPORTED_CMD, seq_id);
                break;
        }
    } else if(cfg->plc_type == PLC_SLC || cfg->plc_type == PLC_MICROLOGIX) {
        switch(cmd_byte) {
            case SLC_CMD_READ: pccc_resp = handle_slc_read(a, pccc_cmd, seq_id, cfg); break;
            case SLC_CMD_WRITE: pccc_resp = handle_slc_write(a, pccc_cmd, seq_id, cfg); break;
            case SLC_CMD_RMW: pccc_resp = handle_slc_rmw(a, pccc_cmd, seq_id, cfg); break;
            default:
                pdlog(LOG_MODULE_PCCC, LOG_LEVEL_WARN, "PCCC SLC: unknown cmd 0x%02x", cmd_byte);
                pccc_resp = pccc_error(a, PCCC_ERR_UNSUPPORTED_CMD, seq_id);
                break;
        }
    } else {
        pdlog(LOG_MODULE_PCCC, LOG_LEVEL_WARN, "PCCC: unsupported plc_type %d", cfg->plc_type);
        pccc_resp = pccc_error(a, PCCC_ERR_UNSUPPORTED_CMD, 0);
    }

build_response:
    if(bytes_is_null(pccc_resp)) { pccc_resp = pccc_error(a, PCCC_ERR_UNSUPPORTED_CMD, sess->pccc_seq_id); }

    resp_prefix = bytes_from_buf(PCCC_RESP_PREFIX, sizeof(PCCC_RESP_PREFIX));
    return bytes_concat(a, resp_prefix, pccc_resp);
}

/* ============================================================================
 * Static functions
 * ============================================================================ */

/*
 * Build a 5-byte PCCC error response (prepended prefix not included here).
 */
static Bytes pccc_error(Arena *a, uint8_t err, uint16_t seq_id) {
    return bytes_pack_fmt(a, "<BBHb", PCCC_RESP_CMD, (uint8_t)0xf0, seq_id, (int8_t)err);
}


static tag_def_t *find_tag_by_file_num(plc_config_t *cfg, size_t file_num) {
    tag_def_t *tag = cfg->tags;
    while(tag && tag->data_file_num != file_num) { tag = tag->next_tag; }
    return tag;
}


/*
 * PLC/5 read:
 *   cmd(1) offset_words(2) unk(1) transfer_size_words(2) prefix(1) file_num(1) element(1)
 */
static Bytes handle_plc5_read(Arena *a, Bytes cmd, uint16_t seq_id, plc_config_t *cfg) {
    uint16_t offset_words = 0;
    uint16_t transfer_size = 0;
    uint8_t file_prefix = 0;
    uint8_t file_num = 0;
    uint8_t file_element = 0;
    tag_def_t *tag = NULL;
    size_t start = 0;
    size_t end = 0;
    size_t tag_size = 0;

    if(cmd.len < 8) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    offset_words = (uint16_t)(cmd.data[1] | ((uint16_t)cmd.data[2] << 8));
    transfer_size = (uint16_t)(cmd.data[3] | ((uint16_t)cmd.data[4] << 8));
    file_prefix = cmd.data[5];
    file_num = cmd.data[6];
    file_element = cmd.data[7];

    if(file_prefix != PCCC_DATA_FILE_PREFIX) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag = find_tag_by_file_num(cfg, file_num);
    if(!tag) {
        pdlog(LOG_MODULE_PCCC, LOG_LEVEL_WARN, "PLC/5 read: tag file %u not found", file_num);
        return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id);
    }

    tag_size = tag->elem_count * tag->elem_size;
    start = (size_t)(offset_words * 2) + ((size_t)file_element * tag->elem_size);
    end = start + ((size_t)transfer_size * tag->elem_size);

    if(start >= tag_size || end > tag_size) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    size_t data_bytes = transfer_size * tag->elem_size;
    if(data_bytes > PCCC_MAX_TRANSFER_BYTES) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    Bytes hdr = bytes_pack_fmt(a, "<BBH", PCCC_RESP_CMD, (uint8_t)0, seq_id);
    Bytes data = bytes_from_buf(tag->data + start, data_bytes);
    return bytes_concat(a, hdr, data);
}


/*
 * PLC/5 write:
 *   cmd(1) offset_words(2) unk(1) transfer_size_words(2) prefix(1) file_num(1) element(1) data...
 */
static Bytes handle_plc5_write(Arena *a, Bytes cmd, uint16_t seq_id, plc_config_t *cfg) {
    uint16_t offset_words = 0;
    uint16_t transfer_size = 0;
    uint8_t file_prefix = 0;
    uint8_t file_num = 0;
    uint8_t file_element = 0;
    tag_def_t *tag = NULL;
    size_t start = 0;
    size_t end = 0;
    size_t tag_size = 0;

    if(cmd.len < 9) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    offset_words = (uint16_t)(cmd.data[1] | ((uint16_t)cmd.data[2] << 8));
    transfer_size = (uint16_t)(cmd.data[3] | ((uint16_t)cmd.data[4] << 8));
    file_prefix = cmd.data[5];
    file_num = cmd.data[6];
    file_element = cmd.data[7];

    if(file_prefix != PCCC_DATA_FILE_PREFIX) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag = find_tag_by_file_num(cfg, file_num);
    if(!tag) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag_size = tag->elem_count * tag->elem_size;
    start = (size_t)(offset_words * 2) + ((size_t)file_element * tag->elem_size);
    end = start + ((size_t)transfer_size * tag->elem_size);

    if(start >= tag_size || end > tag_size) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    size_t data_bytes = transfer_size * tag->elem_size;
    if(data_bytes > PCCC_MAX_TRANSFER_BYTES) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    Bytes write_data = bytes_slice(cmd, 8, data_bytes);
    if(bytes_is_null(write_data)) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    memcpy(tag->data + start, write_data.data, data_bytes);

    return bytes_pack_fmt(a, "<BBH", PCCC_RESP_CMD, (uint8_t)0, seq_id);
}


/*
 * PLC/5 RMW (Read-Modify-Write):
 *   cmd(1) prefix(1) file_num(1) element(1) AND_mask[elem_size] OR_mask[elem_size]
 */
static Bytes handle_plc5_rmw(Arena *a, Bytes cmd, uint16_t seq_id, plc_config_t *cfg) {
    uint8_t file_prefix = 0;
    uint8_t file_num = 0;
    uint8_t file_element = 0;
    tag_def_t *tag = NULL;
    size_t start = 0;
    size_t tag_size = 0;

    if(cmd.len < 4) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    file_prefix = cmd.data[1];
    file_num = cmd.data[2];
    file_element = cmd.data[3];

    if(file_prefix != PCCC_DATA_FILE_PREFIX) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag = find_tag_by_file_num(cfg, file_num);
    if(!tag) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag_size = tag->elem_count * tag->elem_size;
    start = (size_t)file_element * tag->elem_size;

    if(start + tag->elem_size > tag_size) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    /* AND mask at cmd[4..4+elem_size), OR mask at cmd[4+elem_size..4+2*elem_size). */
    size_t mask_offset = 4;
    if(cmd.len < mask_offset + tag->elem_size * 2) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    for(size_t i = 0; i < tag->elem_size; i++) {
        uint8_t and_mask = cmd.data[mask_offset + i];
        uint8_t or_mask = cmd.data[mask_offset + tag->elem_size + i];
        tag->data[start + i] = (uint8_t)((tag->data[start + i] & and_mask) | or_mask);
    }

    return bytes_pack_fmt(a, "<BBH", PCCC_RESP_CMD, (uint8_t)0, seq_id);
}


/*
 * SLC/Micrologix read:
 *   cmd(1) size_bytes(1) file_num(1) file_type(1) element(1) subelement(1)
 */
static Bytes handle_slc_read(Arena *a, Bytes cmd, uint16_t seq_id, plc_config_t *cfg) {
    uint8_t transfer_size = 0;
    uint8_t file_num = 0;
    uint8_t file_type = 0;
    uint8_t file_element = 0;
    uint8_t subelement = 0;
    tag_def_t *tag = NULL;
    size_t start = 0;
    size_t end = 0;
    size_t tag_size = 0;

    if(cmd.len < 6) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    transfer_size = cmd.data[1];
    file_num = cmd.data[2];
    file_type = cmd.data[3];
    file_element = cmd.data[4];
    subelement = cmd.data[5];

    if(subelement != 0) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag = find_tag_by_file_num(cfg, file_num);
    if(!tag) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    if((uint16_t)tag->tag_type != (uint16_t)file_type) {
        pdlog(LOG_MODULE_PCCC, LOG_LEVEL_WARN, "SLC read: file type mismatch got 0x%02x expected 0x%04x", file_type,
              tag->tag_type);
        return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id);
    }

    tag_size = tag->elem_count * tag->elem_size;
    start = (size_t)file_element * tag->elem_size;
    end = start + transfer_size;

    if(start >= tag_size || end > tag_size) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    if(transfer_size > PCCC_MAX_TRANSFER_BYTES) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    Bytes hdr = bytes_pack_fmt(a, "<BBH", PCCC_RESP_CMD, (uint8_t)0, seq_id);
    Bytes data = bytes_from_buf(tag->data + start, transfer_size);
    return bytes_concat(a, hdr, data);
}


/*
 * SLC/Micrologix write:
 *   cmd(1) size_bytes(1) file_num(1) file_type(1) element(1) subelement(1) data...
 */
static Bytes handle_slc_write(Arena *a, Bytes cmd, uint16_t seq_id, plc_config_t *cfg) {
    uint8_t transfer_size = 0;
    uint8_t file_num = 0;
    uint8_t file_type = 0;
    uint8_t file_element = 0;
    uint8_t subelement = 0;
    tag_def_t *tag = NULL;
    size_t start = 0;
    size_t end = 0;
    size_t tag_size = 0;

    if(cmd.len < 7) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    transfer_size = cmd.data[1];
    file_num = cmd.data[2];
    file_type = cmd.data[3];
    file_element = cmd.data[4];
    subelement = cmd.data[5];

    if(subelement != 0) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag = find_tag_by_file_num(cfg, file_num);
    if(!tag) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    if((uint16_t)tag->tag_type != (uint16_t)file_type) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag_size = tag->elem_count * tag->elem_size;
    start = (size_t)file_element * tag->elem_size;
    end = start + transfer_size;

    if(start >= tag_size || end > tag_size) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    if(transfer_size > PCCC_MAX_TRANSFER_BYTES) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    Bytes write_data = bytes_slice(cmd, 6, transfer_size);
    if(bytes_is_null(write_data)) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    memcpy(tag->data + start, write_data.data, transfer_size);

    return bytes_pack_fmt(a, "<BBH", PCCC_RESP_CMD, (uint8_t)0, seq_id);
}


/*
 * SLC/Micrologix RMW:
 *   cmd(1) size(1)=2 file_num(1) file_type(1) element(1) subelement(1)
 *   mask(2) new_data(2)
 *
 * Mask selects which bits change; new_data provides values for those bits.
 */
static Bytes handle_slc_rmw(Arena *a, Bytes cmd, uint16_t seq_id, plc_config_t *cfg) {
    uint8_t transfer_size = 0;
    uint8_t file_num = 0;
    uint8_t file_type = 0;
    uint8_t file_element = 0;
    uint8_t subelement = 0;
    tag_def_t *tag = NULL;
    size_t start = 0;
    size_t tag_size = 0;

    if(cmd.len < 10) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    transfer_size = cmd.data[1];
    file_num = cmd.data[2];
    file_type = cmd.data[3];
    file_element = cmd.data[4];
    subelement = cmd.data[5];

    /* SLC RMW only supports 2-byte (16-bit) elements. */
    if(transfer_size != 2) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    if(subelement != 0) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    tag = find_tag_by_file_num(cfg, file_num);
    if(!tag) { return pccc_error(a, PCCC_ERR_ADDR_NOT_USABLE, seq_id); }

    if((uint16_t)tag->tag_type != (uint16_t)file_type || tag->elem_size != 2) {
        return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id);
    }

    tag_size = tag->elem_count * tag->elem_size;
    start = (size_t)file_element * tag->elem_size;

    if(start + 2 > tag_size) { return pccc_error(a, PCCC_ERR_FILE_WRONG_SIZE, seq_id); }

    /* mask bytes at cmd[6..8), new data bytes at cmd[8..10). */
    for(size_t i = 0; i < 2; i++) {
        uint8_t mask = cmd.data[6 + i];
        uint8_t new_data = cmd.data[8 + i];
        tag->data[start + i] = (uint8_t)((tag->data[start + i] & (uint8_t)~mask) | (new_data & mask));
    }

    return bytes_pack_fmt(a, "<BBH", PCCC_RESP_CMD, (uint8_t)0, seq_id);
}
