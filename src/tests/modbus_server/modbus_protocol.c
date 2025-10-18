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

#include "modbus_protocol.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

/* Helper functions for byte ordering (network byte order = big endian) */
static inline uint16_t read_uint16_be(const uint8_t *buffer) {
    return (uint16_t)((buffer[0] << 8) | buffer[1]);
}

static inline void write_uint16_be(uint8_t *buffer, uint16_t value) {
    buffer[0] = (uint8_t)(value >> 8);
    buffer[1] = (uint8_t)(value & 0xFF);
}

/* Helper function to decode Modbus function code to a string */
static const char* modbus_function_code_to_string(uint8_t function_code) {
    switch (function_code) {
        case MODBUS_FC_READ_COILS:
            return "Read Coils (FC 0x01)";
        case MODBUS_FC_READ_DISCRETE_INPUTS:
            return "Read Discrete Inputs (FC 0x02)";
        case MODBUS_FC_READ_HOLDING_REGISTERS:
            return "Read Holding Registers (FC 0x03)";
        case MODBUS_FC_READ_INPUT_REGISTERS:
            return "Read Input Registers (FC 0x04)";
        case MODBUS_FC_WRITE_SINGLE_COIL:
            return "Write Single Coil (FC 0x05)";
        case MODBUS_FC_WRITE_SINGLE_REGISTER:
            return "Write Single Register (FC 0x06)";
        case MODBUS_FC_WRITE_MULTIPLE_COILS:
            return "Write Multiple Coils (FC 0x0F)";
        case MODBUS_FC_WRITE_MULTIPLE_REGISTERS:
            return "Write Multiple Registers (FC 0x10)";
        default:
            return "Unknown Function Code";
    }
}

bool modbus_parse_mbap_header(const uint8_t *buffer, int buffer_length,
                               mbap_header_t *header) {
    if (buffer_length < MBAP_HEADER_SIZE) {
        log_debug("Buffer too small for MBAP header (%d < %d)", 
                 buffer_length, MBAP_HEADER_SIZE);
        return false;
    }
    
    header->transaction_id = read_uint16_be(&buffer[0]);
    header->protocol_id = read_uint16_be(&buffer[2]);
    header->length = read_uint16_be(&buffer[4]);
    header->unit_id = buffer[6];
    
    /* Validate protocol ID (must be 0 for Modbus TCP) */
    if (header->protocol_id != 0) {
        log_debug("Invalid protocol ID: %u (expected 0)", header->protocol_id);
        return false;
    }
    
    /* Validate length field */
    if (header->length < 2 || header->length > (MODBUS_MAX_PDU_SIZE + 1)) {
        log_debug("Invalid length field: %u", header->length);
        return false;
    }
    
    return true;
}

int modbus_build_mbap_header(uint8_t *buffer, const mbap_header_t *header) {
    write_uint16_be(&buffer[0], header->transaction_id);
    write_uint16_be(&buffer[2], header->protocol_id);
    write_uint16_be(&buffer[4], header->length);
    buffer[6] = header->unit_id;
    return MBAP_HEADER_SIZE;
}

bool modbus_parse_request(const uint8_t *buffer, int buffer_length,
                          modbus_message_t *request) {
    if (!modbus_parse_mbap_header(buffer, buffer_length, &request->mbap)) {
        return false;
    }
    
    /* Check if we have the complete PDU */
    int expected_length = MBAP_HEADER_SIZE + request->mbap.length - 1;
    if (buffer_length < expected_length) {
        log_debug("Incomplete PDU: got %d bytes, expected %d",
                 buffer_length, expected_length);
        return false;
    }
    
    /* Extract function code and data */
    request->function_code = buffer[MBAP_HEADER_SIZE];
    request->data_length = request->mbap.length - 2; /* Length includes unit_id + function_code */
    
    if (request->data_length > 0) {
        memcpy(request->data, &buffer[MBAP_HEADER_SIZE + 1], 
               (size_t)request->data_length);
    }
    
    log_debug("Parsed request: TID=%u, Unit=%u, %s, DataLen=%d",
             request->mbap.transaction_id, request->mbap.unit_id,
             modbus_function_code_to_string(request->function_code),
             request->data_length);
    
    return true;
}

int modbus_build_exception_response(const mbap_header_t *request_header,
                                    uint8_t function_code,
                                    uint8_t exception_code,
                                    uint8_t *buffer,
                                    int buffer_size) {
    if (buffer_size < (MBAP_HEADER_SIZE + 2)) {
        log_error("Buffer too small for exception response");
        return -1;
    }
    
    mbap_header_t response_header = *request_header;
    response_header.length = 3; /* unit_id + exception_function_code + exception_code */
    
    int offset = modbus_build_mbap_header(buffer, &response_header);
    buffer[offset++] = function_code | 0x80; /* Set high bit for exception */
    buffer[offset++] = exception_code;
    
    log_debug("Built exception response: FC=0x%02X, Exception=0x%02X",
             function_code, exception_code);
    
    return offset;
}

/* Function code handlers */
static int handle_read_coils(const modbus_message_t *request,
                            register_storage_t *storage,
                            uint8_t *response_buffer,
                            int response_buffer_size) {
    if (request->data_length < 4) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE,
                                               response_buffer, response_buffer_size);
    }
    
    uint16_t start_address = read_uint16_be(&request->data[0]);
    uint16_t count = read_uint16_be(&request->data[2]);
    
    if (count == 0 || count > 2000) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE,
                                               response_buffer, response_buffer_size);
    }
    
    int byte_count = (count + 7) / 8;
    uint8_t *coil_data = (uint8_t*)malloc((size_t)byte_count);
    if (!coil_data) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_SERVER_DEVICE_FAILURE,
                                               response_buffer, response_buffer_size);
    }
    
    if (!register_storage_read_coils(storage, (int)start_address, (int)count, coil_data)) {
        free(coil_data);
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS,
                                               response_buffer, response_buffer_size);
    }
    
    mbap_header_t response_header = request->mbap;
    response_header.length = (uint16_t)(3 + byte_count); /* unit_id + fc + byte_count + data */
    
    int offset = modbus_build_mbap_header(response_buffer, &response_header);
    response_buffer[offset++] = request->function_code;
    response_buffer[offset++] = (uint8_t)byte_count;
    memcpy(&response_buffer[offset], coil_data, (size_t)byte_count);
    offset += byte_count;
    
    free(coil_data);
    log_debug("Read %d coils from address %u", count, start_address);
    return offset;
}

static int handle_read_discrete_inputs(const modbus_message_t *request,
                                       register_storage_t *storage,
                                       uint8_t *response_buffer,
                                       int response_buffer_size) {
    if (request->data_length < 4) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE,
                                               response_buffer, response_buffer_size);
    }
    
    uint16_t start_address = read_uint16_be(&request->data[0]);
    uint16_t count = read_uint16_be(&request->data[2]);
    
    if (count == 0 || count > 2000) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE,
                                               response_buffer, response_buffer_size);
    }
    
    int byte_count = (count + 7) / 8;
    uint8_t *input_data = (uint8_t*)malloc((size_t)byte_count);
    if (!input_data) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_SERVER_DEVICE_FAILURE,
                                               response_buffer, response_buffer_size);
    }
    
    if (!register_storage_read_discrete_inputs(storage, (int)start_address, (int)count, input_data)) {
        free(input_data);
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS,
                                               response_buffer, response_buffer_size);
    }
    
    mbap_header_t response_header = request->mbap;
    response_header.length = (uint16_t)(3 + byte_count);
    
    int offset = modbus_build_mbap_header(response_buffer, &response_header);
    response_buffer[offset++] = request->function_code;
    response_buffer[offset++] = (uint8_t)byte_count;
    memcpy(&response_buffer[offset], input_data, (size_t)byte_count);
    offset += byte_count;
    
    free(input_data);
    log_debug("Read %d discrete inputs from address %u", count, start_address);
    return offset;
}

static int handle_read_holding_registers(const modbus_message_t *request,
                                         register_storage_t *storage,
                                         uint8_t *response_buffer,
                                         int response_buffer_size) {
    if (request->data_length < 4) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE,
                                               response_buffer, response_buffer_size);
    }
    
    uint16_t start_address = read_uint16_be(&request->data[0]);
    uint16_t count = read_uint16_be(&request->data[2]);
    
    if (count == 0 || count > 125) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE,
                                               response_buffer, response_buffer_size);
    }
    
    uint16_t *register_data = (uint16_t*)malloc((size_t)count * sizeof(uint16_t));
    if (!register_data) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_SERVER_DEVICE_FAILURE,
                                               response_buffer, response_buffer_size);
    }
    
    if (!register_storage_read_holding_registers(storage, (int)start_address, (int)count, register_data)) {
        free(register_data);
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS,
                                               response_buffer, response_buffer_size);
    }
    
    int byte_count = count * 2;
    mbap_header_t response_header = request->mbap;
    response_header.length = (uint16_t)(3 + byte_count);
    
    int offset = modbus_build_mbap_header(response_buffer, &response_header);
    response_buffer[offset++] = request->function_code;
    response_buffer[offset++] = (uint8_t)byte_count;
    
    for (int i = 0; i < count; i++) {
        write_uint16_be(&response_buffer[offset], register_data[i]);
        offset += 2;
    }
    
    free(register_data);
    log_debug("Read %d holding registers from address %u", count, start_address);
    return offset;
}

static int handle_read_input_registers(const modbus_message_t *request,
                                       register_storage_t *storage,
                                       uint8_t *response_buffer,
                                       int response_buffer_size) {
    if (request->data_length < 4) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE,
                                               response_buffer, response_buffer_size);
    }
    
    uint16_t start_address = read_uint16_be(&request->data[0]);
    uint16_t count = read_uint16_be(&request->data[2]);
    
    if (count == 0 || count > 125) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE,
                                               response_buffer, response_buffer_size);
    }
    
    uint16_t *register_data = (uint16_t*)malloc((size_t)count * sizeof(uint16_t));
    if (!register_data) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_SERVER_DEVICE_FAILURE,
                                               response_buffer, response_buffer_size);
    }
    
    if (!register_storage_read_input_registers(storage, (int)start_address, (int)count, register_data)) {
        free(register_data);
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS,
                                               response_buffer, response_buffer_size);
    }
    
    int byte_count = count * 2;
    mbap_header_t response_header = request->mbap;
    response_header.length = (uint16_t)(3 + byte_count);
    
    int offset = modbus_build_mbap_header(response_buffer, &response_header);
    response_buffer[offset++] = request->function_code;
    response_buffer[offset++] = (uint8_t)byte_count;
    
    for (int i = 0; i < count; i++) {
        write_uint16_be(&response_buffer[offset], register_data[i]);
        offset += 2;
    }
    
    free(register_data);
    log_debug("Read %d input registers from address %u", count, start_address);
    return offset;
}

static int handle_write_single_coil(const modbus_message_t *request,
                                    register_storage_t *storage,
                                    uint8_t *response_buffer,
                                    int response_buffer_size) {
    if (request->data_length < 4) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE,
                                               response_buffer, response_buffer_size);
    }
    
    uint16_t address = read_uint16_be(&request->data[0]);
    uint16_t value = read_uint16_be(&request->data[2]);
    
    /* Valid coil values are 0x0000 (OFF) or 0xFF00 (ON) */
    if (value != 0x0000 && value != 0xFF00) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE,
                                               response_buffer, response_buffer_size);
    }
    
    bool coil_value = (value == 0xFF00);
    if (!register_storage_write_coil(storage, (int)address, coil_value)) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS,
                                               response_buffer, response_buffer_size);
    }
    
    /* Echo request as response */
    mbap_header_t response_header = request->mbap;
    response_header.length = 6; /* unit_id + fc + address + value */
    
    int offset = modbus_build_mbap_header(response_buffer, &response_header);
    response_buffer[offset++] = request->function_code;
    write_uint16_be(&response_buffer[offset], address);
    offset += 2;
    write_uint16_be(&response_buffer[offset], value);
    offset += 2;
    
    log_debug("Wrote single coil at address %u = %s", address, coil_value ? "ON" : "OFF");
    return offset;
}

static int handle_write_single_register(const modbus_message_t *request,
                                        register_storage_t *storage,
                                        uint8_t *response_buffer,
                                        int response_buffer_size) {
    if (request->data_length < 4) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE,
                                               response_buffer, response_buffer_size);
    }
    
    uint16_t address = read_uint16_be(&request->data[0]);
    uint16_t value = read_uint16_be(&request->data[2]);
    
    if (!register_storage_write_holding_register(storage, (int)address, value)) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS,
                                               response_buffer, response_buffer_size);
    }
    
    /* Echo request as response */
    mbap_header_t response_header = request->mbap;
    response_header.length = 6;
    
    int offset = modbus_build_mbap_header(response_buffer, &response_header);
    response_buffer[offset++] = request->function_code;
    write_uint16_be(&response_buffer[offset], address);
    offset += 2;
    write_uint16_be(&response_buffer[offset], value);
    offset += 2;
    
    log_debug("Wrote single register at address %u = 0x%04X", address, value);
    return offset;
}

static int handle_write_multiple_coils(const modbus_message_t *request,
                                       register_storage_t *storage,
                                       uint8_t *response_buffer,
                                       int response_buffer_size) {
    if (request->data_length < 5) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE,
                                               response_buffer, response_buffer_size);
    }
    
    uint16_t start_address = read_uint16_be(&request->data[0]);
    uint16_t count = read_uint16_be(&request->data[2]);
    uint8_t byte_count = request->data[4];
    
    if (count == 0 || count > 1968 || byte_count != ((count + 7) / 8) ||
        request->data_length < (5 + byte_count)) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE,
                                               response_buffer, response_buffer_size);
    }
    
    if (!register_storage_write_coils(storage, (int)start_address, (int)count, &request->data[5])) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS,
                                               response_buffer, response_buffer_size);
    }
    
    mbap_header_t response_header = request->mbap;
    response_header.length = 6;
    
    int offset = modbus_build_mbap_header(response_buffer, &response_header);
    response_buffer[offset++] = request->function_code;
    write_uint16_be(&response_buffer[offset], start_address);
    offset += 2;
    write_uint16_be(&response_buffer[offset], count);
    offset += 2;
    
    log_debug("Wrote %d coils starting at address %u", count, start_address);
    return offset;
}

static int handle_write_multiple_registers(const modbus_message_t *request,
                                           register_storage_t *storage,
                                           uint8_t *response_buffer,
                                           int response_buffer_size) {
    if (request->data_length < 5) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE,
                                               response_buffer, response_buffer_size);
    }
    
    uint16_t start_address = read_uint16_be(&request->data[0]);
    uint16_t count = read_uint16_be(&request->data[2]);
    uint8_t byte_count = request->data[4];
    
    if (count == 0 || count > 123 || byte_count != (count * 2) ||
        request->data_length < (5 + byte_count)) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE,
                                               response_buffer, response_buffer_size);
    }
    
    uint16_t *register_data = (uint16_t*)malloc((size_t)count * sizeof(uint16_t));
    if (!register_data) {
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_SERVER_DEVICE_FAILURE,
                                               response_buffer, response_buffer_size);
    }
    
    for (int i = 0; i < count; i++) {
        register_data[i] = read_uint16_be(&request->data[5 + i * 2]);
    }
    
    if (!register_storage_write_holding_registers(storage, (int)start_address, (int)count, register_data)) {
        free(register_data);
        return modbus_build_exception_response(&request->mbap, request->function_code,
                                               MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS,
                                               response_buffer, response_buffer_size);
    }
    
    free(register_data);
    
    mbap_header_t response_header = request->mbap;
    response_header.length = 6;
    
    int offset = modbus_build_mbap_header(response_buffer, &response_header);
    response_buffer[offset++] = request->function_code;
    write_uint16_be(&response_buffer[offset], start_address);
    offset += 2;
    write_uint16_be(&response_buffer[offset], count);
    offset += 2;
    
    log_debug("Wrote %d registers starting at address %u", count, start_address);
    return offset;
}

int modbus_process_request(const modbus_message_t *request,
                           register_storage_t *storage,
                           uint8_t *response_buffer,
                           int response_buffer_size) {
    log_debug("Processing: %s", modbus_function_code_to_string(request->function_code));
    
    switch (request->function_code) {
        case MODBUS_FC_READ_COILS:
            return handle_read_coils(request, storage, response_buffer, response_buffer_size);
            
        case MODBUS_FC_READ_DISCRETE_INPUTS:
            return handle_read_discrete_inputs(request, storage, response_buffer, response_buffer_size);
            
        case MODBUS_FC_READ_HOLDING_REGISTERS:
            return handle_read_holding_registers(request, storage, response_buffer, response_buffer_size);
            
        case MODBUS_FC_READ_INPUT_REGISTERS:
            return handle_read_input_registers(request, storage, response_buffer, response_buffer_size);
            
        case MODBUS_FC_WRITE_SINGLE_COIL:
            return handle_write_single_coil(request, storage, response_buffer, response_buffer_size);
            
        case MODBUS_FC_WRITE_SINGLE_REGISTER:
            return handle_write_single_register(request, storage, response_buffer, response_buffer_size);
            
        case MODBUS_FC_WRITE_MULTIPLE_COILS:
            return handle_write_multiple_coils(request, storage, response_buffer, response_buffer_size);
            
        case MODBUS_FC_WRITE_MULTIPLE_REGISTERS:
            return handle_write_multiple_registers(request, storage, response_buffer, response_buffer_size);
            
        default:
            log_debug("Unsupported function code: 0x%02X", request->function_code);
            return modbus_build_exception_response(&request->mbap, request->function_code,
                                                   MODBUS_EXCEPTION_ILLEGAL_FUNCTION,
                                                   response_buffer, response_buffer_size);
    }
}
