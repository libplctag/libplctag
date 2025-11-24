#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Variable-length bit array for Modbus coils and discrete inputs.
 *
 * Bits are packed into bytes, LSB first (per Modbus specification).
 * Bit 0 is the LSB of byte 0, bit 7 is the MSB of byte 0, etc.
 */
typedef struct {
    uint8_t *bytes;       // Byte array (heap allocated)
    size_t byte_count;    // Number of bytes
    size_t bit_capacity;  // Total bits (may be > byte_count * 8 due to rounding)
} modbus_bitarray_t;

/**
 * @brief Create a new bit array.
 *
 * @param num_bits Number of bits to allocate
 * @return modbus_bitarray_t* Pointer to new bit array, or NULL on failure
 */
static inline modbus_bitarray_t* modbus_bitarray_create(size_t num_bits) {
    if (num_bits == 0) {
        return NULL;
    }

    modbus_bitarray_t *arr = calloc(1, sizeof(*arr));
    if (!arr) {
        return NULL;
    }

    arr->byte_count = (num_bits + 7) / 8;
    arr->bit_capacity = num_bits;
    arr->bytes = calloc(arr->byte_count, 1);

    if (!arr->bytes) {
        free(arr);
        return NULL;
    }

    return arr;
}

/**
 * @brief Destroy a bit array.
 */
static inline void modbus_bitarray_destroy(modbus_bitarray_t *arr) {
    if (arr) {
        free(arr->bytes);
        free(arr);
    }
}

/**
 * @brief Get a single bit value.
 *
 * @param arr Pointer to bit array
 * @param bit_index Bit index (0-based)
 * @return true if bit is set, false otherwise
 */
static inline bool modbus_bitarray_get(const modbus_bitarray_t *arr, size_t bit_index) {
    if (!arr || bit_index >= arr->bit_capacity) {
        return false;
    }

    size_t byte_idx = bit_index / 8;
    unsigned int bit_offset = bit_index % 8;

    return (arr->bytes[byte_idx] & (1 << bit_offset)) != 0;
}

/**
 * @brief Set a single bit value.
 *
 * @param arr Pointer to bit array
 * @param bit_index Bit index (0-based)
 * @param value true to set, false to clear
 */
static inline void modbus_bitarray_set(modbus_bitarray_t *arr, size_t bit_index, bool value) {
    if (!arr || bit_index >= arr->bit_capacity) {
        return;
    }

    size_t byte_idx = bit_index / 8;
    unsigned int bit_offset = bit_index % 8;

    if (value) {
        arr->bytes[byte_idx] |= (uint8_t)(1 << bit_offset);
    } else {
        arr->bytes[byte_idx] &= (uint8_t)~(1 << bit_offset);
    }
}

/**
 * @brief Read multiple bits into a byte array.
 *
 * Packs bits LSB-first per Modbus spec.
 *
 * @param arr Source bit array
 * @param start_bit Starting bit index
 * @param num_bits Number of bits to read
 * @param out_bytes Output byte array (must have (num_bits+7)/8 bytes)
 * @return true on success, false if out of bounds
 */
static inline bool modbus_bitarray_read_bits(const modbus_bitarray_t *arr,
                                             size_t start_bit, size_t num_bits,
                                             uint8_t *out_bytes) {
    if (!arr || !out_bytes || start_bit + num_bits > arr->bit_capacity) {
        return false;
    }

    size_t out_byte_count = (num_bits + 7) / 8;
    memset(out_bytes, 0, out_byte_count);

    for (size_t i = 0; i < num_bits; i++) {
        if (modbus_bitarray_get(arr, start_bit + i)) {
            size_t out_byte_idx = i / 8;
            unsigned int out_bit_offset = i % 8;
            out_bytes[out_byte_idx] |= (1 << out_bit_offset);
        }
    }

    return true;
}

/**
 * @brief Write multiple bits from a byte array.
 *
 * Unpacks bits LSB-first per Modbus spec.
 *
 * @param arr Destination bit array
 * @param start_bit Starting bit index
 * @param num_bits Number of bits to write
 * @param in_bytes Input byte array
 * @return true on success, false if out of bounds
 */
static inline bool modbus_bitarray_write_bits(modbus_bitarray_t *arr,
                                              size_t start_bit, size_t num_bits,
                                              const uint8_t *in_bytes) {
    if (!arr || !in_bytes || start_bit + num_bits > arr->bit_capacity) {
        return false;
    }

    for (size_t i = 0; i < num_bits; i++) {
        size_t in_byte_idx = i / 8;
        unsigned int in_bit_offset = i % 8;
        bool bit_value = (in_bytes[in_byte_idx] & (1 << in_bit_offset)) != 0;
        modbus_bitarray_set(arr, start_bit + i, bit_value);
    }

    return true;
}
