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

#ifndef MODBUS_PROTOCOL_H
#define MODBUS_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include "register_storage.h"

/* Modbus TCP Application Protocol (MBAP) Header */
#define MBAP_HEADER_SIZE 7

/* Modbus Function Codes */
#define MODBUS_FC_READ_COILS                0x01
#define MODBUS_FC_READ_DISCRETE_INPUTS      0x02
#define MODBUS_FC_READ_HOLDING_REGISTERS    0x03
#define MODBUS_FC_READ_INPUT_REGISTERS      0x04
#define MODBUS_FC_WRITE_SINGLE_COIL         0x05
#define MODBUS_FC_WRITE_SINGLE_REGISTER     0x06
#define MODBUS_FC_WRITE_MULTIPLE_COILS      0x0F
#define MODBUS_FC_WRITE_MULTIPLE_REGISTERS  0x10

/* Modbus Exception Codes */
#define MODBUS_EXCEPTION_ILLEGAL_FUNCTION        0x01
#define MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS    0x02
#define MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE      0x03
#define MODBUS_EXCEPTION_SERVER_DEVICE_FAILURE   0x04

/* Maximum Modbus PDU size (260 bytes) */
#define MODBUS_MAX_PDU_SIZE 260

/* Maximum ADU (MBAP + PDU) size */
#define MODBUS_MAX_ADU_SIZE (MBAP_HEADER_SIZE + MODBUS_MAX_PDU_SIZE)

/* MBAP Header structure */
typedef struct {
    uint16_t transaction_id;
    uint16_t protocol_id;
    uint16_t length;
    uint8_t unit_id;
} mbap_header_t;

/* Modbus Request/Response structure */
typedef struct {
    mbap_header_t mbap;
    uint8_t function_code;
    uint8_t data[MODBUS_MAX_PDU_SIZE];
    int data_length;
} modbus_message_t;

/**
 * Parse MBAP header from buffer
 * Returns true on success, false on error
 */
bool modbus_parse_mbap_header(const uint8_t *buffer, int buffer_length, 
                               mbap_header_t *header);

/**
 * Build MBAP header into buffer
 * Returns number of bytes written (always MBAP_HEADER_SIZE)
 */
int modbus_build_mbap_header(uint8_t *buffer, const mbap_header_t *header);

/**
 * Parse a complete Modbus request from buffer
 * Returns true on success, false on error
 */
bool modbus_parse_request(const uint8_t *buffer, int buffer_length,
                          modbus_message_t *request);

/**
 * Process a Modbus request and generate a response
 * Returns number of bytes in response buffer, or -1 on error
 */
int modbus_process_request(const modbus_message_t *request,
                           register_storage_t *storage,
                           uint8_t *response_buffer,
                           int response_buffer_size);

/**
 * Build a Modbus exception response
 * Returns number of bytes written
 */
int modbus_build_exception_response(const mbap_header_t *request_header,
                                    uint8_t function_code,
                                    uint8_t exception_code,
                                    uint8_t *buffer,
                                    int buffer_size);

#endif /* MODBUS_PROTOCOL_H */
