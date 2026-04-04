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
 * Modbus TCP PDU processing using Bytes/Arena instead of buf_t.
 *
 * Each handler receives the request body (bytes after the FC byte), returns
 * the complete response PDU (FC byte + data).  On exception the response is
 * (FC|0x80, exception_code).  On arena exhaustion the response is {NULL, 0}.
 *
 * The MBAP header is handled entirely by the caller (modbus_server3.c).
 */

#include "modbus_protocol3.h"
#include "log.h"

/* ============================================================================
 * Modbus exception codes (Modbus Application Protocol spec, Table 7)
 * ============================================================================ */

#define EX_ILLEGAL_FUNCTION    ((uint8_t)0x01)
#define EX_ILLEGAL_ADDRESS     ((uint8_t)0x02)
#define EX_ILLEGAL_DATA_VALUE  ((uint8_t)0x03)
#define EX_DEVICE_FAILURE      ((uint8_t)0x04)

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/* Build an exception PDU: (FC | 0x80), exception_code. */
static Bytes make_exception(Arena *a, uint8_t fc, uint8_t ex_code) {
    return bytes_pack(a, ">BB", (uint8_t)(fc | 0x80U), ex_code);
}

/* ============================================================================
 * FC handlers — each returns a complete response PDU (FC byte + data).
 * ============================================================================ */

/* FC 0x01: Read Coils */
static Bytes handle_read_coils(Arena *a, uint8_t fc, Bytes body,
                                register_storage_t *storage) {
    uint16_t addr = 0, count = 0;
    Bytes rest = bytes_unpack(body, ">HH", &addr, &count);
    if (bytes_is_null(rest)) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }
    if (count == 0 || count > MODBUS_MAX_READ_COILS) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }

    uint8_t byte_count = (uint8_t)((count + 7U) / 8U);
    Bytes coil_buf = bytes_alloc(a, byte_count);
    if (bytes_is_null(coil_buf)) {
        return (Bytes){NULL, 0};
    }
    bytes_zero(coil_buf);

    if (!register_storage_read_coils(storage, addr, count, coil_buf.data)) {
        return make_exception(a, fc, EX_ILLEGAL_ADDRESS);
    }

    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL,
          "Read %u coils from address %u", count, addr);

    return bytes_pack(a, ">BB*", fc, byte_count, &coil_buf);
}

/* FC 0x02: Read Discrete Inputs */
static Bytes handle_read_discrete_inputs(Arena *a, uint8_t fc, Bytes body,
                                          register_storage_t *storage) {
    uint16_t addr = 0, count = 0;
    Bytes rest = bytes_unpack(body, ">HH", &addr, &count);
    if (bytes_is_null(rest)) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }
    if (count == 0 || count > MODBUS_MAX_READ_DISCRETE_INPUTS) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }

    uint8_t byte_count = (uint8_t)((count + 7U) / 8U);
    Bytes di_buf = bytes_alloc(a, byte_count);
    if (bytes_is_null(di_buf)) {
        return (Bytes){NULL, 0};
    }
    bytes_zero(di_buf);

    if (!register_storage_read_discrete_inputs(storage, addr, count, di_buf.data)) {
        return make_exception(a, fc, EX_ILLEGAL_ADDRESS);
    }

    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL,
          "Read %u discrete inputs from address %u", count, addr);

    return bytes_pack(a, ">BB*", fc, byte_count, &di_buf);
}

/* FC 0x03: Read Holding Registers */
static Bytes handle_read_holding_registers(Arena *a, uint8_t fc, Bytes body,
                                            register_storage_t *storage) {
    uint16_t addr = 0, count = 0;
    Bytes rest = bytes_unpack(body, ">HH", &addr, &count);
    if (bytes_is_null(rest)) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }
    if (count == 0 || count > MODBUS_MAX_READ_REGISTERS) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }

    uint16_t *reg_vals = arena_alloc(a, (size_t)count * sizeof(uint16_t));
    if (!reg_vals) {
        return (Bytes){NULL, 0};
    }

    if (!register_storage_read_holding_registers(storage, addr, count, reg_vals)) {
        return make_exception(a, fc, EX_ILLEGAL_ADDRESS);
    }

    /* Encode register values as big-endian byte stream. */
    uint8_t byte_count = (uint8_t)(count * 2U);
    Bytes reg_bytes = bytes_alloc(a, byte_count);
    if (bytes_is_null(reg_bytes)) {
        return (Bytes){NULL, 0};
    }
    for (uint16_t i = 0; i < count; i++) {
        reg_bytes.data[i * 2U]      = (uint8_t)(reg_vals[i] >> 8);
        reg_bytes.data[i * 2U + 1U] = (uint8_t)(reg_vals[i]);
    }

    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL,
          "Read %u holding registers from address %u", count, addr);

    return bytes_pack(a, ">BB*", fc, byte_count, &reg_bytes);
}

/* FC 0x04: Read Input Registers */
static Bytes handle_read_input_registers(Arena *a, uint8_t fc, Bytes body,
                                          register_storage_t *storage) {
    uint16_t addr = 0, count = 0;
    Bytes rest = bytes_unpack(body, ">HH", &addr, &count);
    if (bytes_is_null(rest)) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }
    if (count == 0 || count > MODBUS_MAX_READ_REGISTERS) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }

    uint16_t *reg_vals = arena_alloc(a, (size_t)count * sizeof(uint16_t));
    if (!reg_vals) {
        return (Bytes){NULL, 0};
    }

    if (!register_storage_read_input_registers(storage, addr, count, reg_vals)) {
        return make_exception(a, fc, EX_ILLEGAL_ADDRESS);
    }

    uint8_t byte_count = (uint8_t)(count * 2U);
    Bytes reg_bytes = bytes_alloc(a, byte_count);
    if (bytes_is_null(reg_bytes)) {
        return (Bytes){NULL, 0};
    }
    for (uint16_t i = 0; i < count; i++) {
        reg_bytes.data[i * 2U]      = (uint8_t)(reg_vals[i] >> 8);
        reg_bytes.data[i * 2U + 1U] = (uint8_t)(reg_vals[i]);
    }

    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL,
          "Read %u input registers from address %u", count, addr);

    return bytes_pack(a, ">BB*", fc, byte_count, &reg_bytes);
}

/* FC 0x05: Write Single Coil */
static Bytes handle_write_single_coil(Arena *a, uint8_t fc, Bytes body,
                                       register_storage_t *storage) {
    uint16_t addr = 0, value = 0;
    Bytes rest = bytes_unpack(body, ">HH", &addr, &value);
    if (bytes_is_null(rest)) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }
    /* Modbus spec: 0x0000 = OFF, 0xFF00 = ON, all other values are illegal. */
    if (value != 0x0000U && value != 0xFF00U) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }

    uint8_t coil_byte = (value == 0xFF00U) ? 0x01U : 0x00U;
    if (!register_storage_write_coils(storage, addr, 1, &coil_byte)) {
        return make_exception(a, fc, EX_ILLEGAL_ADDRESS);
    }

    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL,
          "Wrote single coil at address %u = %s",
          addr, (value == 0xFF00U) ? "ON" : "OFF");

    /* Echo the request as the response. */
    return bytes_pack(a, ">BHH", fc, addr, value);
}

/* FC 0x06: Write Single Register */
static Bytes handle_write_single_register(Arena *a, uint8_t fc, Bytes body,
                                           register_storage_t *storage) {
    uint16_t addr = 0, value = 0;
    Bytes rest = bytes_unpack(body, ">HH", &addr, &value);
    if (bytes_is_null(rest)) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }

    if (!register_storage_write_holding_registers(storage, addr, 1, &value)) {
        return make_exception(a, fc, EX_ILLEGAL_ADDRESS);
    }

    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL,
          "Wrote single register at address %u = 0x%04X", addr, value);

    return bytes_pack(a, ">BHH", fc, addr, value);
}

/* FC 0x0F: Write Multiple Coils */
static Bytes handle_write_multiple_coils(Arena *a, uint8_t fc, Bytes body,
                                          register_storage_t *storage) {
    uint16_t addr = 0, count = 0;
    uint8_t byte_count = 0;
    Bytes rest = bytes_unpack(body, ">HHB", &addr, &count, &byte_count);
    if (bytes_is_null(rest)) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }
    if (count == 0 || count > MODBUS_MAX_WRITE_COILS) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }
    if (byte_count != (uint8_t)((count + 7U) / 8U)) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }
    if (rest.len < (size_t)byte_count) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }

    if (!register_storage_write_coils(storage, addr, count, rest.data)) {
        return make_exception(a, fc, EX_ILLEGAL_ADDRESS);
    }

    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL,
          "Wrote %u coils starting at address %u", count, addr);

    /* Response: FC + start_address + quantity written. */
    return bytes_pack(a, ">BHH", fc, addr, count);
}

/* FC 0x10: Write Multiple Registers */
static Bytes handle_write_multiple_registers(Arena *a, uint8_t fc, Bytes body,
                                              register_storage_t *storage) {
    uint16_t addr = 0, count = 0;
    uint8_t byte_count = 0;
    Bytes rest = bytes_unpack(body, ">HHB", &addr, &count, &byte_count);
    if (bytes_is_null(rest)) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }
    if (count == 0 || count > MODBUS_MAX_WRITE_REGISTERS) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }
    if (byte_count != (uint8_t)(count * 2U)) {
        return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
    }

    uint16_t *reg_vals = arena_alloc(a, (size_t)count * sizeof(uint16_t));
    if (!reg_vals) {
        return (Bytes){NULL, 0};
    }

    /* Unpack each register value (big-endian). */
    Bytes cur = rest;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t val = 0;
        cur = bytes_unpack(cur, ">H", &val);
        if (bytes_is_null(cur)) {
            return make_exception(a, fc, EX_ILLEGAL_DATA_VALUE);
        }
        reg_vals[i] = val;
    }

    if (!register_storage_write_holding_registers(storage, addr, count, reg_vals)) {
        return make_exception(a, fc, EX_ILLEGAL_ADDRESS);
    }

    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL,
          "Wrote %u registers starting at address %u", count, addr);

    return bytes_pack(a, ">BHH", fc, addr, count);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

Bytes modbus_process_request_bytes(Arena *a, uint8_t fc, Bytes body,
                                    register_storage_t *storage) {
    pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_DETAIL,
          "Processing Modbus request FC=0x%02X body_len=%zu", fc, body.len);

    switch (fc) {
        case MODBUS_FC_READ_COILS:
            return handle_read_coils(a, fc, body, storage);

        case MODBUS_FC_READ_DISCRETE_INPUTS:
            return handle_read_discrete_inputs(a, fc, body, storage);

        case MODBUS_FC_READ_HOLDING_REGISTERS:
            return handle_read_holding_registers(a, fc, body, storage);

        case MODBUS_FC_READ_INPUT_REGISTERS:
            return handle_read_input_registers(a, fc, body, storage);

        case MODBUS_FC_WRITE_SINGLE_COIL:
            return handle_write_single_coil(a, fc, body, storage);

        case MODBUS_FC_WRITE_SINGLE_REGISTER:
            return handle_write_single_register(a, fc, body, storage);

        case MODBUS_FC_WRITE_MULTIPLE_COILS:
            return handle_write_multiple_coils(a, fc, body, storage);

        case MODBUS_FC_WRITE_MULTIPLE_REGISTERS:
            return handle_write_multiple_registers(a, fc, body, storage);

        default:
            pdlog(LOG_MODULE_MODBUS_PROTOCOL, LOG_LEVEL_WARN,
                  "Unsupported function code: 0x%02X", fc);
            return make_exception(a, fc, EX_ILLEGAL_FUNCTION);
    }
}
