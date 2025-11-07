#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * @file bitarray.h
 * @brief Type-safe wrapper for 64-bit bitarray operations.
 *
 * Provides a thin abstraction around a single uint64_t for bit manipulation.
 * All operations are inline for zero-overhead abstraction. This is used
 * internally by the reactor to manage event masks and pending events.
 */

/* Type-safe wrapper around a single 64-bit word */
typedef struct {
    uint64_t bits;
} bitarray_t;

/* Initialization macros - no runtime cost */
#define BITARRAY_ZERO() ((bitarray_t){0})
#define BITARRAY_FULL() ((bitarray_t){UINT64_MAX})

/**
 * @brief Set a single bit by index.
 * @param ba Pointer to bitarray
 * @param index Bit index (0-63)
 */
static inline void bitarray_set(bitarray_t *ba, unsigned int index) {
    if (index < 64) {
        ba->bits |= (1ULL << index);
    }
}

/**
 * @brief Clear a single bit by index.
 * @param ba Pointer to bitarray
 * @param index Bit index (0-63)
 */
static inline void bitarray_clear(bitarray_t *ba, unsigned int index) {
    if (index < 64) {
        ba->bits &= ~(1ULL << index);
    }
}

/**
 * @brief Test if a single bit is set by index.
 * @param ba Pointer to bitarray
 * @param index Bit index (0-63)
 * @return true if bit is set, false otherwise
 */
static inline bool bitarray_test(const bitarray_t *ba, unsigned int index) {
    if (index < 64) {
        return (ba->bits & (1ULL << index)) != 0;
    }
    return false;
}

/**
 * @brief Clear all bits (set to zero).
 * @param ba Pointer to bitarray
 */
static inline void bitarray_clear_all(bitarray_t *ba) {
    ba->bits = 0;
}

/**
 * @brief Set all bits (set to UINT64_MAX).
 * @param ba Pointer to bitarray
 */
static inline void bitarray_set_all(bitarray_t *ba) {
    ba->bits = UINT64_MAX;
}

/**
 * @brief Check if any bit is set.
 * @param ba Pointer to bitarray
 * @return true if any bit is set, false if all bits are zero
 */
static inline bool bitarray_has_any(const bitarray_t *ba) {
    return ba->bits != 0;
}

/**
 * @brief Check if any bit is set in both this and mask.
 * @param ba Pointer to bitarray
 * @param mask Pointer to mask bitarray
 * @return true if (ba->bits & mask->bits) != 0
 */
static inline bool bitarray_has_any_masked(const bitarray_t *ba, const bitarray_t *mask) {
    return (ba->bits & mask->bits) != 0;
}

/**
 * @brief Clear bits that are set in the mask.
 * @param ba Pointer to bitarray to modify
 * @param mask Pointer to mask bitarray
 */
static inline void bitarray_clear_masked(bitarray_t *ba, const bitarray_t *mask) {
    ba->bits &= ~mask->bits;
}

/**
 * @brief Assign one bitarray to another.
 * @param dst Destination bitarray
 * @param src Source bitarray
 */
static inline void bitarray_assign(bitarray_t *dst, const bitarray_t *src) {
    dst->bits = src->bits;
}

/**
 * @brief Compute bitwise AND of two bitarrays.
 * @param a First bitarray
 * @param b Second bitarray
 * @return New bitarray with result of a AND b
 */
static inline bitarray_t bitarray_and(const bitarray_t *a, const bitarray_t *b) {
    return (bitarray_t){a->bits & b->bits};
}
