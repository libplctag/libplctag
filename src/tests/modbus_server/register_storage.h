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

#ifndef REGISTER_STORAGE_H
#define REGISTER_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

/* Register storage structure */
typedef struct {
    /* Coils (read/write bits) */
    uint8_t *coils;
    int num_coils;
    
    /* Discrete Inputs (read-only bits) */
    uint8_t *discrete_inputs;
    int num_discrete_inputs;
    
    /* Holding Registers (read/write 16-bit) */
    uint16_t *holding_registers;
    int num_holding_registers;
    
    /* Input Registers (read-only 16-bit) */
    uint16_t *input_registers;
    int num_input_registers;
} register_storage_t;

/* Initialize register storage */
bool register_storage_init(register_storage_t *storage,
                           int num_coils,
                           int num_discrete_inputs,
                           int num_holding_registers,
                           int num_input_registers);

/* Cleanup register storage */
void register_storage_cleanup(register_storage_t *storage);

/* Coil operations */
bool register_storage_read_coil(register_storage_t *storage, int address, bool *value);
bool register_storage_write_coil(register_storage_t *storage, int address, bool value);
bool register_storage_read_coils(register_storage_t *storage, int start_address, 
                                 int count, uint8_t *values);
bool register_storage_write_coils(register_storage_t *storage, int start_address,
                                  int count, const uint8_t *values);

/* Discrete Input operations */
bool register_storage_read_discrete_input(register_storage_t *storage, int address, bool *value);
bool register_storage_read_discrete_inputs(register_storage_t *storage, int start_address,
                                          int count, uint8_t *values);

/* Holding Register operations */
bool register_storage_read_holding_register(register_storage_t *storage, int address, uint16_t *value);
bool register_storage_write_holding_register(register_storage_t *storage, int address, uint16_t value);
bool register_storage_read_holding_registers(register_storage_t *storage, int start_address,
                                            int count, uint16_t *values);
bool register_storage_write_holding_registers(register_storage_t *storage, int start_address,
                                             int count, const uint16_t *values);

/* Input Register operations */
bool register_storage_read_input_register(register_storage_t *storage, int address, uint16_t *value);
bool register_storage_read_input_registers(register_storage_t *storage, int start_address,
                                          int count, uint16_t *values);

#endif /* REGISTER_STORAGE_H */
