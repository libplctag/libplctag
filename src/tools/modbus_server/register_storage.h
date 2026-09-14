#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "modbus_bitarray.h"

/**
 * @brief Storage for Modbus registers and coils
 */
typedef struct {
    modbus_bitarray_t *coils;               // Read/write bits
    modbus_bitarray_t *discrete_inputs;     // Read-only bits
    uint16_t *holding_registers;            // Read/write 16-bit values
    size_t num_holding_registers;
    uint16_t *input_registers;              // Read-only 16-bit values
    size_t num_input_registers;
} register_storage_t;

/**
 * @brief Create register storage
 */
register_storage_t* register_storage_create(size_t num_coils,
                                            size_t num_discrete_inputs,
                                            size_t num_holding_registers,
                                            size_t num_input_registers);

/**
 * @brief Destroy register storage
 */
void register_storage_destroy(register_storage_t *storage);

/**
 * @brief Read coils into a byte array (LSB-first per Modbus spec)
 */
bool register_storage_read_coils(register_storage_t *storage,
                                 uint16_t address, uint16_t count,
                                 uint8_t *out_bytes);

/**
 * @brief Read discrete inputs into a byte array
 */
bool register_storage_read_discrete_inputs(register_storage_t *storage,
                                           uint16_t address, uint16_t count,
                                           uint8_t *out_bytes);

/**
 * @brief Write coils from a byte array
 */
bool register_storage_write_coils(register_storage_t *storage,
                                  uint16_t address, uint16_t count,
                                  const uint8_t *in_bytes);

/**
 * @brief Read holding registers
 */
bool register_storage_read_holding_registers(register_storage_t *storage,
                                             uint16_t address, uint16_t count,
                                             uint16_t *out_values);

/**
 * @brief Write holding registers
 */
bool register_storage_write_holding_registers(register_storage_t *storage,
                                              uint16_t address, uint16_t count,
                                              const uint16_t *in_values);

/**
 * @brief Read input registers
 */
bool register_storage_read_input_registers(register_storage_t *storage,
                                           uint16_t address, uint16_t count,
                                           uint16_t *out_values);
