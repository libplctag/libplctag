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

#include "register_storage.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

bool register_storage_init(register_storage_t *storage,
                           int num_coils,
                           int num_discrete_inputs,
                           int num_holding_registers,
                           int num_input_registers) {
    memset(storage, 0, sizeof(register_storage_t));
    
    /* Allocate coils (bits stored as bytes, 8 per byte) */
    if (num_coils > 0) {
        int coil_bytes = (num_coils + 7) / 8;
        storage->coils = (uint8_t*)calloc((size_t)coil_bytes, 1);
        if (!storage->coils) {
            log_error("Failed to allocate memory for %d coils", num_coils);
            return false;
        }
        storage->num_coils = num_coils;
        log_info("Allocated %d coils (%d bytes)", num_coils, coil_bytes);
    }
    
    /* Allocate discrete inputs */
    if (num_discrete_inputs > 0) {
        int di_bytes = (num_discrete_inputs + 7) / 8;
        storage->discrete_inputs = (uint8_t*)calloc((size_t)di_bytes, 1);
        if (!storage->discrete_inputs) {
            log_error("Failed to allocate memory for %d discrete inputs", num_discrete_inputs);
            register_storage_cleanup(storage);
            return false;
        }
        storage->num_discrete_inputs = num_discrete_inputs;
        log_info("Allocated %d discrete inputs (%d bytes)", num_discrete_inputs, di_bytes);
    }
    
    /* Allocate holding registers */
    if (num_holding_registers > 0) {
        storage->holding_registers = (uint16_t*)calloc((size_t)num_holding_registers, 
                                                       sizeof(uint16_t));
        if (!storage->holding_registers) {
            log_error("Failed to allocate memory for %d holding registers", 
                     num_holding_registers);
            register_storage_cleanup(storage);
            return false;
        }
        storage->num_holding_registers = num_holding_registers;
        log_info("Allocated %d holding registers (%zu bytes)", 
                num_holding_registers, (size_t)num_holding_registers * sizeof(uint16_t));
    }
    
    /* Allocate input registers */
    if (num_input_registers > 0) {
        storage->input_registers = (uint16_t*)calloc((size_t)num_input_registers,
                                                     sizeof(uint16_t));
        if (!storage->input_registers) {
            log_error("Failed to allocate memory for %d input registers", num_input_registers);
            register_storage_cleanup(storage);
            return false;
        }
        storage->num_input_registers = num_input_registers;
        log_info("Allocated %d input registers (%zu bytes)",
                num_input_registers, (size_t)num_input_registers * sizeof(uint16_t));
    }
    
    return true;
}

void register_storage_cleanup(register_storage_t *storage) {
    if (storage->coils) {
        free(storage->coils);
        storage->coils = NULL;
    }
    if (storage->discrete_inputs) {
        free(storage->discrete_inputs);
        storage->discrete_inputs = NULL;
    }
    if (storage->holding_registers) {
        free(storage->holding_registers);
        storage->holding_registers = NULL;
    }
    if (storage->input_registers) {
        free(storage->input_registers);
        storage->input_registers = NULL;
    }
    log_info("Register storage cleaned up");
}

/* Helper functions for bit operations */
static inline bool get_bit(const uint8_t *data, int bit_index) {
    int byte_index = bit_index / 8;
    int bit_offset = bit_index % 8;
    return (data[byte_index] & (1 << bit_offset)) != 0;
}

static inline void set_bit(uint8_t *data, int bit_index, bool value) {
    int byte_index = bit_index / 8;
    int bit_offset = bit_index % 8;
    if (value) {
        data[byte_index] |= (uint8_t)(1 << bit_offset);
    } else {
        data[byte_index] &= (uint8_t)~(1 << bit_offset);
    }
}

/* Coil operations */
bool register_storage_read_coil(register_storage_t *storage, int address, bool *value) {
    if (address < 0 || address >= storage->num_coils) {
        log_debug("Coil address %d out of range (0-%d)", address, storage->num_coils - 1);
        return false;
    }
    *value = get_bit(storage->coils, address);
    return true;
}

bool register_storage_write_coil(register_storage_t *storage, int address, bool value) {
    if (address < 0 || address >= storage->num_coils) {
        log_debug("Coil address %d out of range (0-%d)", address, storage->num_coils - 1);
        return false;
    }
    set_bit(storage->coils, address, value);
    log_debug("Wrote coil %d = %d", address, value ? 1 : 0);
    return true;
}

bool register_storage_read_coils(register_storage_t *storage, int start_address,
                                 int count, uint8_t *values) {
    if (start_address < 0 || count < 1 || 
        (start_address + count) > storage->num_coils) {
        log_debug("Coil range %d+%d out of bounds (max: %d)", 
                 start_address, count, storage->num_coils);
        return false;
    }
    
    /* Pack bits into bytes */
    int byte_count = (count + 7) / 8;
    memset(values, 0, (size_t)byte_count);
    
    for (int i = 0; i < count; i++) {
        bool bit = get_bit(storage->coils, start_address + i);
        if (bit) {
            values[i / 8] |= (uint8_t)(1 << (i % 8));
        }
    }
    
    return true;
}

bool register_storage_write_coils(register_storage_t *storage, int start_address,
                                  int count, const uint8_t *values) {
    if (start_address < 0 || count < 1 || 
        (start_address + count) > storage->num_coils) {
        log_debug("Coil range %d+%d out of bounds (max: %d)",
                 start_address, count, storage->num_coils);
        return false;
    }
    
    for (int i = 0; i < count; i++) {
        bool bit = (values[i / 8] & (1 << (i % 8))) != 0;
        set_bit(storage->coils, start_address + i, bit);
    }
    
    log_debug("Wrote %d coils starting at %d", count, start_address);
    return true;
}

/* Discrete Input operations */
bool register_storage_read_discrete_input(register_storage_t *storage, int address, bool *value) {
    if (address < 0 || address >= storage->num_discrete_inputs) {
        log_debug("Discrete input address %d out of range (0-%d)",
                 address, storage->num_discrete_inputs - 1);
        return false;
    }
    *value = get_bit(storage->discrete_inputs, address);
    return true;
}

bool register_storage_read_discrete_inputs(register_storage_t *storage, int start_address,
                                          int count, uint8_t *values) {
    if (start_address < 0 || count < 1 ||
        (start_address + count) > storage->num_discrete_inputs) {
        log_debug("Discrete input range %d+%d out of bounds (max: %d)",
                 start_address, count, storage->num_discrete_inputs);
        return false;
    }
    
    int byte_count = (count + 7) / 8;
    memset(values, 0, (size_t)byte_count);
    
    for (int i = 0; i < count; i++) {
        bool bit = get_bit(storage->discrete_inputs, start_address + i);
        if (bit) {
            values[i / 8] |= (uint8_t)(1 << (i % 8));
        }
    }
    
    return true;
}

/* Holding Register operations */
bool register_storage_read_holding_register(register_storage_t *storage, int address, 
                                           uint16_t *value) {
    if (address < 0 || address >= storage->num_holding_registers) {
        log_debug("Holding register address %d out of range (0-%d)",
                 address, storage->num_holding_registers - 1);
        return false;
    }
    *value = storage->holding_registers[address];
    return true;
}

bool register_storage_write_holding_register(register_storage_t *storage, int address,
                                            uint16_t value) {
    if (address < 0 || address >= storage->num_holding_registers) {
        log_debug("Holding register address %d out of range (0-%d)",
                 address, storage->num_holding_registers - 1);
        return false;
    }
    storage->holding_registers[address] = value;
    log_debug("Wrote holding register %d = 0x%04X", address, value);
    return true;
}

bool register_storage_read_holding_registers(register_storage_t *storage, int start_address,
                                            int count, uint16_t *values) {
    if (start_address < 0 || count < 1 ||
        (start_address + count) > storage->num_holding_registers) {
        log_debug("Holding register range %d+%d out of bounds (max: %d)",
                 start_address, count, storage->num_holding_registers);
        return false;
    }
    
    memcpy(values, &storage->holding_registers[start_address], 
           (size_t)count * sizeof(uint16_t));
    return true;
}

bool register_storage_write_holding_registers(register_storage_t *storage, int start_address,
                                             int count, const uint16_t *values) {
    if (start_address < 0 || count < 1 ||
        (start_address + count) > storage->num_holding_registers) {
        log_debug("Holding register range %d+%d out of bounds (max: %d)",
                 start_address, count, storage->num_holding_registers);
        return false;
    }
    
    memcpy(&storage->holding_registers[start_address], values,
           (size_t)count * sizeof(uint16_t));
    log_debug("Wrote %d holding registers starting at %d", count, start_address);
    return true;
}

/* Input Register operations */
bool register_storage_read_input_register(register_storage_t *storage, int address,
                                         uint16_t *value) {
    if (address < 0 || address >= storage->num_input_registers) {
        log_debug("Input register address %d out of range (0-%d)",
                 address, storage->num_input_registers - 1);
        return false;
    }
    *value = storage->input_registers[address];
    return true;
}

bool register_storage_read_input_registers(register_storage_t *storage, int start_address,
                                          int count, uint16_t *values) {
    if (start_address < 0 || count < 1 ||
        (start_address + count) > storage->num_input_registers) {
        log_debug("Input register range %d+%d out of bounds (max: %d)",
                 start_address, count, storage->num_input_registers);
        return false;
    }
    
    memcpy(values, &storage->input_registers[start_address],
           (size_t)count * sizeof(uint16_t));
    return true;
}
