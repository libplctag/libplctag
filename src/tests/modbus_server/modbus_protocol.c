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
#include "../utils/log.h"
#include <stdlib.h>
#include <string.h>

/* Map util_err_t to Modbus exception codes */
static inline uint8_t util_err_to_modbus_exception(util_err_t err) {
    switch (err) {
        case UTIL_OK:
            return 0; /* No error */
        case UTIL_EBOUNDS:
            return MODBUS_EXCEPTION_ILLEGAL_ADDRESS;
        case UTIL_EINVAL:
            return MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE;
        case UTIL_ERESOURCE:
            return MODBUS_EXCEPTION_DEVICE_FAILURE;
        default:
            return MODBUS_EXCEPTION_ILLEGAL_FUNCTION;
    }
}

/* Parse MBAP header from buffer using buf.h API */
util_err_t modbus_parse_mbap_header(buf_t *buf, mbap_header_t *header) {
    if (!buf || !header) {
        return UTIL_EINVAL;
    }

    /* Save checkpoint for potential rollback */
    buf_t checkpoint = buf_checkpoint(buf);

    /* Read MBAP header fields */
    if (!buf_read_u16_be(buf, "transaction_id", &header->transaction_id) ||
        !buf_read_u16_be(buf, "protocol_id", &header->protocol_id) ||
        !buf_read_u16_be(buf, "length", &header->length) ||
        !buf_read_u8(buf, "unit_id", &header->unit_id)) {
        buf_restore(buf, checkpoint);
        return UTIL_EBOUNDS;
    }

    /* Validate protocol ID (must be 0 for Modbus TCP) */
    if (header->protocol_id != 0) {
        buf_restore(buf, checkpoint);
        return UTIL_EINVAL;
    }

    /* Validate length field (PDU length: unit_id + function_code + data, min 2, max 260+1) */
    if (header->length < 2 || header->length > (MODBUS_MAX_PDU_SIZE + 1)) {
        buf_restore(buf, checkpoint);
        return UTIL_EINVAL;
    }

    return UTIL_OK;
}

/* Build MBAP response header in buffer */
util_err_t modbus_build_response_header(buf_t *response,
                                        const mbap_header_t *req_header,
                                        uint16_t pdu_length) {
    if (!response || !req_header) {
        return UTIL_EINVAL;
    }

    buf_reset(response);

    /* Write MBAP header */
    if (!buf_write_u16_be(response, "transaction_id", req_header->transaction_id) ||
        !buf_write_u16_be(response, "protocol_id", 0) || /* protocol_id = 0 */
        !buf_write_u16_be(response, "length", pdu_length + 1) || /* length = unit_id + PDU */
        !buf_write_u8(response, "unit_id", req_header->unit_id)) {
        return UTIL_EBOUNDS;
    }

    return UTIL_OK;
}

/* Build exception response */
void modbus_build_exception_response(buf_t *response,
                                     const mbap_header_t *req_header,
                                     uint8_t function_code,
                                     util_err_t error) {
    if (!response || !req_header) {
        return;
    }

    /* Build response header with 2-byte PDU (FC + exception code) */
    modbus_build_response_header(response, req_header, 2);

    /* Write exception function code (FC with high bit set) and exception code */
    buf_write_u8(response, "function_code", function_code | 0x80);
    buf_write_u8(response, "exception_code", util_err_to_modbus_exception(error));
}

/* Handle FC 0x01: Read Coils */
static util_err_t handle_read_coils(buf_t *request, buf_t *response,
                                    const mbap_header_t *req_header,
                                    register_storage_t *storage) {
    uint16_t start_address, count;

    /* Read request parameters */
    if (!buf_read_u16_be(request, "start_address", &start_address) ||
        !buf_read_u16_be(request, "count", &count)) {
        return UTIL_EINVAL;
    }

    /* Validate count (1-2000 coils) */
    if (count == 0 || count > MODBUS_MAX_READ_COILS) {
        return UTIL_EINVAL;
    }

    /* Allocate response buffer for coils */
    uint16_t tmp_byte_count = (uint16_t)(((uint16_t)count + (uint16_t)7) / (uint16_t)8);

    if (tmp_byte_count > MODBUS_MAX_READ_RESPONSE_BYTES) {
        pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_WARN, "Requested coil count %u results in byte count %u exceeding %d bytes", count, tmp_byte_count, MODBUS_MAX_READ_RESPONSE_BYTES);
        return UTIL_EINVAL;
    }

    uint8_t byte_count = (uint8_t)((count + 7) / 8);
    uint8_t *coil_data = malloc(byte_count);

    if (!coil_data) {
        return UTIL_ERESOURCE;
    }

    /* Read coils from storage */
    bool success = register_storage_read_coils(storage, start_address, count, coil_data);
    if (!success) {
        free(coil_data);
        return UTIL_EBOUNDS;
    }

    /* Build response header */
    modbus_build_response_header(response, req_header, 2 + byte_count); /* FC + byte_count + data */

    /* Write response: FC, byte_count, coil data */
    if (!buf_write_u8(response, "function_code", MODBUS_FC_READ_COILS) ||
        !buf_write_u8(response, "byte_count", byte_count) ||
        !buf_write_bytes(response, "coil_data", coil_data, byte_count)) {
        free(coil_data);
        return UTIL_EBOUNDS;
    }

    free(coil_data);
    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL, "Read %u coils from address %u", count, start_address);
    return UTIL_OK;
}

/* Handle FC 0x02: Read Discrete Inputs */
static util_err_t handle_read_discrete_inputs(buf_t *request, buf_t *response,
                                              const mbap_header_t *req_header,
                                              register_storage_t *storage) {
    uint16_t start_address, count;

    /* Read request parameters */
    if (!buf_read_u16_be(request, "start_address", &start_address) ||
        !buf_read_u16_be(request, "count", &count)) {
        return UTIL_EINVAL;
    }

    /* Validate count (1-2000 inputs) */
    if (count == 0 || count > MODBUS_MAX_READ_DISCRETE_INPUTS) {
        return UTIL_EINVAL;
    }

    /* Allocate response buffer for inputs */
    uint16_t tmp_byte_count = (uint16_t)(((uint16_t)count + (uint16_t)7) / (uint16_t)8);

    if (tmp_byte_count > MODBUS_MAX_READ_RESPONSE_BYTES) {
        pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_WARN, "Requested discrete input count %u results in byte count %u exceeding %d bytes", count, tmp_byte_count, MODBUS_MAX_READ_RESPONSE_BYTES);
        return UTIL_EINVAL;
    }

    uint8_t byte_count = (uint8_t)((count + 7) / 8);
    uint8_t *input_data = malloc(byte_count);
    if (!input_data) {
        return UTIL_ERESOURCE;
    }

    /* Read discrete inputs from storage */
    bool success = register_storage_read_discrete_inputs(storage, start_address, count, input_data);
    if (!success) {
        free(input_data);
        return UTIL_EBOUNDS;
    }

    /* Build response header */
    modbus_build_response_header(response, req_header, 2 + byte_count); /* FC + byte_count + data */

    /* Write response: FC, byte_count, input data */
    if (!buf_write_u8(response, "function_code", MODBUS_FC_READ_DISCRETE_INPUTS) ||
        !buf_write_u8(response, "byte_count", byte_count) ||
        !buf_write_bytes(response, "input_data", input_data, byte_count)) {
        free(input_data);
        return UTIL_EBOUNDS;
    }

    free(input_data);
    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL, "Read %u discrete inputs from address %u", count, start_address);
    return UTIL_OK;
}

/* Handle FC 0x03: Read Holding Registers */
static util_err_t handle_read_holding_registers(buf_t *request, buf_t *response,
                                                const mbap_header_t *req_header,
                                                register_storage_t *storage) {
    uint16_t start_address, count;

    /* Read request parameters */
    if (!buf_read_u16_be(request, "start_address", &start_address) ||
        !buf_read_u16_be(request, "count", &count)) {
        return UTIL_EINVAL;
    }

    /* Validate count (1-125 registers) */
    if (count == 0 || count > MODBUS_MAX_READ_REGISTERS) {
        return UTIL_EINVAL;
    }

    /* Allocate response buffer for registers */
    uint16_t *register_data = malloc(count * sizeof(uint16_t));
    if (!register_data) {
        return UTIL_ERESOURCE;
    }

    /* Read holding registers from storage */
    bool success = register_storage_read_holding_registers(storage, start_address, count, register_data);
    if (!success) {
        free(register_data);
        return UTIL_EBOUNDS;
    }

    /* Build response header */
    uint8_t byte_count = (uint8_t)(count * 2);
    modbus_build_response_header(response, req_header, 2 + byte_count); /* FC + byte_count + data */

    /* Write response: FC, byte_count, register values (big-endian) */
    if (!buf_write_u8(response, "function_code", MODBUS_FC_READ_HOLDING_REGISTERS) ||
        !buf_write_u8(response, "byte_count", byte_count)) {
        free(register_data);
        return UTIL_EBOUNDS;
    }

    for (uint16_t i = 0; i < count; i++) {
        if (!buf_write_u16_be(response, "register", register_data[i])) {
            free(register_data);
            return UTIL_EBOUNDS;
        }
    }

    free(register_data);
    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL, "Read %u holding registers from address %u", count, start_address);
    return UTIL_OK;
}

/* Handle FC 0x04: Read Input Registers */
static util_err_t handle_read_input_registers(buf_t *request, buf_t *response,
                                              const mbap_header_t *req_header,
                                              register_storage_t *storage) {
    uint16_t start_address, count;

    /* Read request parameters */
    if (!buf_read_u16_be(request, "start_address", &start_address) ||
        !buf_read_u16_be(request, "count", &count)) {
        return UTIL_EINVAL;
    }

    /* Validate count (1-125 registers) */
    if (count == 0 || count > MODBUS_MAX_READ_REGISTERS) {
        return UTIL_EINVAL;
    }

    /* Allocate response buffer for registers */
    uint16_t *register_data = malloc(count * sizeof(uint16_t));
    if (!register_data) {
        return UTIL_ERESOURCE;
    }

    /* Read input registers from storage */
    bool success = register_storage_read_input_registers(storage, start_address, count, register_data);
    if (!success) {
        free(register_data);
        return UTIL_EBOUNDS;
    }

    /* Build response header */
    uint8_t byte_count = (uint8_t)(count * 2);
    modbus_build_response_header(response, req_header, 2 + byte_count); /* FC + byte_count + data */

    /* Write response: FC, byte_count, register values (big-endian) */
    if (!buf_write_u8(response, "function_code", MODBUS_FC_READ_INPUT_REGISTERS) ||
        !buf_write_u8(response, "byte_count", byte_count)) {
        free(register_data);
        return UTIL_EBOUNDS;
    }

    for (uint16_t i = 0; i < count; i++) {
        if (!buf_write_u16_be(response, "register", register_data[i])) {
            free(register_data);
            return UTIL_EBOUNDS;
        }
    }

    free(register_data);
    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL, "Read %u input registers from address %u", count, start_address);
    return UTIL_OK;
}

/* Handle FC 0x05: Write Single Coil */
static util_err_t handle_write_single_coil(buf_t *request, buf_t *response,
                                           const mbap_header_t *req_header,
                                           register_storage_t *storage) {
    uint16_t address, value;

    /* Read request parameters */
    if (!buf_read_u16_be(request, "address", &address) ||
        !buf_read_u16_be(request, "value", &value)) {
        return UTIL_EINVAL;
    }

    /* Validate coil value (0x0000 = OFF, 0xFF00 = ON) */
    if (value != 0x0000 && value != 0xFF00) {
        return UTIL_EINVAL;
    }

    /* Write coil to storage */
    bool coil_value = (value == 0xFF00);
    uint8_t coil_bytes = coil_value ? 0x01 : 0x00;
    if (!register_storage_write_coils(storage, address, 1, &coil_bytes)) {
        return UTIL_EBOUNDS;
    }

    /* Build response header (echo request as response) */
    modbus_build_response_header(response, req_header, 5); /* FC + address + value */

    /* Write response: FC, address, value */
    if (!buf_write_u8(response, "function_code", MODBUS_FC_WRITE_SINGLE_COIL) ||
        !buf_write_u16_be(response, "address", address) ||
        !buf_write_u16_be(response, "value", value)) {
        return UTIL_EBOUNDS;
    }

    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL, "Wrote single coil at address %u = %s", address, coil_value ? "ON" : "OFF");
    return UTIL_OK;
}

/* Handle FC 0x06: Write Single Register */
static util_err_t handle_write_single_register(buf_t *request, buf_t *response,
                                               const mbap_header_t *req_header,
                                               register_storage_t *storage) {
    uint16_t address, value;

    /* Read request parameters */
    if (!buf_read_u16_be(request, "address", &address) ||
        !buf_read_u16_be(request, "value", &value)) {
        return UTIL_EINVAL;
    }

    /* Write register to storage */
    if (!register_storage_write_holding_registers(storage, address, 1, &value)) {
        return UTIL_EBOUNDS;
    }

    /* Build response header (echo request as response) */
    modbus_build_response_header(response, req_header, 5); /* FC + address + value */

    /* Write response: FC, address, value */
    if (!buf_write_u8(response, "function_code", MODBUS_FC_WRITE_SINGLE_REGISTER) ||
        !buf_write_u16_be(response, "address", address) ||
        !buf_write_u16_be(response, "value", value)) {
        return UTIL_EBOUNDS;
    }

    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL, "Wrote single register at address %u = 0x%04X", address, value);
    return UTIL_OK;
}

/* Handle FC 0x0F: Write Multiple Coils */
static util_err_t handle_write_multiple_coils(buf_t *request, buf_t *response,
                                              const mbap_header_t *req_header,
                                              register_storage_t *storage) {
    uint16_t start_address, count;
    uint8_t byte_count;

    /* Read request parameters */
    if (!buf_read_u16_be(request, "start_address", &start_address) ||
        !buf_read_u16_be(request, "count", &count) ||
        !buf_read_u8(request, "byte_count", &byte_count)) {
        return UTIL_EINVAL;
    }

    /* Validate count and byte count */
    if (count == 0 || count > MODBUS_MAX_WRITE_COILS || byte_count != ((count + 7) / 8)) {
        return UTIL_EINVAL;
    }

    /* Read coil data from request */
    const uint8_t *coil_data = buf_read_ptr(request);
    if (!coil_data || buf_read_size(request) < byte_count) {
        return UTIL_EINVAL;
    }

    /* Write coils to storage */
    if (!register_storage_write_coils(storage, start_address, count, coil_data)) {
        return UTIL_EBOUNDS;
    }

    /* Advance read cursor */
    buf_read_advance(request, byte_count);

    /* Build response header */
    modbus_build_response_header(response, req_header, 5); /* FC + start_address + count */

    /* Write response: FC, start_address, count */
    if (!buf_write_u8(response, "function_code", MODBUS_FC_WRITE_MULTIPLE_COILS) ||
        !buf_write_u16_be(response, "start_address", start_address) ||
        !buf_write_u16_be(response, "count", count)) {
        return UTIL_EBOUNDS;
    }

    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL, "Wrote %u coils starting at address %u", count, start_address);
    return UTIL_OK;
}

/* Handle FC 0x10: Write Multiple Registers */
static util_err_t handle_write_multiple_registers(buf_t *request, buf_t *response,
                                                  const mbap_header_t *req_header,
                                                  register_storage_t *storage) {
    uint16_t start_address, count;
    uint8_t byte_count;

    /* Read request parameters */
    if (!buf_read_u16_be(request, "start_address", &start_address) ||
        !buf_read_u16_be(request, "count", &count) ||
        !buf_read_u8(request, "byte_count", &byte_count)) {
        return UTIL_EINVAL;
    }

    /* Validate count and byte count */
    if (count == 0 || count > MODBUS_MAX_WRITE_REGISTERS || byte_count != (count * 2)) {
        return UTIL_EINVAL;
    }

    /* Allocate temporary buffer for register values */
    uint16_t *register_data = malloc(count * sizeof(uint16_t));
    if (!register_data) {
        return UTIL_ERESOURCE;
    }

    /* Read register values from request (big-endian) */
    for (uint16_t i = 0; i < count; i++) {
        if (!buf_read_u16_be(request, "register", &register_data[i])) {
            free(register_data);
            return UTIL_EINVAL;
        }
    }

    /* Write registers to storage */
    bool success = register_storage_write_holding_registers(storage, start_address, count, register_data);
    free(register_data);

    if (!success) {
        return UTIL_EBOUNDS;
    }

    /* Build response header */
    modbus_build_response_header(response, req_header, 5); /* FC + start_address + count */

    /* Write response: FC, start_address, count */
    if (!buf_write_u8(response, "function_code", MODBUS_FC_WRITE_MULTIPLE_REGISTERS) ||
        !buf_write_u16_be(response, "start_address", start_address) ||
        !buf_write_u16_be(response, "count", count)) {
        return UTIL_EBOUNDS;
    }

    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL, "Wrote %u registers starting at address %u", count, start_address);
    return UTIL_OK;
}

/* Main request dispatcher */
util_err_t modbus_process_request(uint8_t function_code,
                                  buf_t *request,
                                  buf_t *response,
                                  const mbap_header_t *req_header,
                                  register_storage_t *storage) {
    if (!request || !response || !req_header || !storage) {
        return UTIL_EINVAL;
    }

    util_err_t err = UTIL_OK;

    switch (function_code) {
        case MODBUS_FC_READ_COILS:
            err = handle_read_coils(request, response, req_header, storage);
            break;

        case MODBUS_FC_READ_DISCRETE_INPUTS:
            err = handle_read_discrete_inputs(request, response, req_header, storage);
            break;

        case MODBUS_FC_READ_HOLDING_REGISTERS:
            err = handle_read_holding_registers(request, response, req_header, storage);
            break;

        case MODBUS_FC_READ_INPUT_REGISTERS:
            err = handle_read_input_registers(request, response, req_header, storage);
            break;

        case MODBUS_FC_WRITE_SINGLE_COIL:
            err = handle_write_single_coil(request, response, req_header, storage);
            break;

        case MODBUS_FC_WRITE_SINGLE_REGISTER:
            err = handle_write_single_register(request, response, req_header, storage);
            break;

        case MODBUS_FC_WRITE_MULTIPLE_COILS:
            err = handle_write_multiple_coils(request, response, req_header, storage);
            break;

        case MODBUS_FC_WRITE_MULTIPLE_REGISTERS:
            err = handle_write_multiple_registers(request, response, req_header, storage);
            break;

        default:
            pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL, "Unsupported function code: 0x%02X", function_code);
            return UTIL_ENOTSUPPORTED;
    }

    /* Build exception response if handler failed */
    if (err != UTIL_OK) {
        modbus_build_exception_response(response, req_header, function_code, err);
    }

    return err;
}
