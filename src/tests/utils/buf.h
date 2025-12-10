#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "err.h"

/**
 * @brief Buffer structure and operations.
 * 
 * Buffers are used for managing byte arrays with read and write cursors.
 * They also carry error status for operations.  Most operations return
 * a boolean indicating success or failure, with details available in
 * the buffer's error fields.  This allows chaining multiple operations
 * while checking for errors at the end.
 * 
 * bool ok = true;
 * 
 * ok &= buf_splice_bytes(&buf, 0, data1, len1);
 * ok &= buf_write_u32_le(&buf, value);
 * ...
 * 
 * if(!ok) {
 *     // handle error 
 * }
 * 
 * Once a buffer has an error set, further operations are no-ops
 * until the error is cleared.
 */

typedef struct buf_s {
    uint8_t    *data;
    size_t     capacity;  // total buffer capacity
    size_t     read;      // read cursor
    size_t     write;     // write cursor
    util_err_t error;
    const char *failed_field;
} buf_t;

/* Creation/Initialization */

/**
 * @brief Initialize a new buffer.
 *
 * @param data Pointer to the buffer data.
 * @param capacity Total capacity of the buffer.
 * @return buf_t The initialized buffer, by value, not pointer.
 */
buf_t buf_init(uint8_t *data, size_t capacity);



/* Error handling */

/**
 * @brief Set the error status of the buffer.
 *
 * @param b Pointer to the buffer.
 * @param error Error status to set.
 * @param field_name Name of the field that caused the error.
 */
void buf_set_error(buf_t *b, util_err_t error, const char *field_name);

/**
 * @brief Clear the error status of the buffer.
 * 
 * Clear both the error code and failed field name.
 *
 * @param b Pointer to the buffer.
 * @return util_err_t Previous error status.
 */
util_err_t buf_clear_error(buf_t *b);


/**
 * @brief Get the current error status of the buffer.
 *
 * @param b Pointer to the buffer.
 * @return util_err_t Current error status.
 */
util_err_t buf_get_error(const buf_t *b);

/**
 * @brief Get the name of the field that caused the last error.
 *
 * @param b Pointer to the buffer.
 * @return const char* Name of the failed field or NULL if no error.
 */
const char* buf_get_failed_field(const buf_t *b);

/**
 * @brief Check if the buffer is in a good state (no errors).
 * 
 * Also checks for invalid state such as read/write cursors out of bounds.
 *
 * @param b Pointer to the buffer.
 * @return true if no errors, false otherwise.
 */
bool buf_ok(const buf_t *b);



/* Status/Access */

/**
 * @brief Get the total capacity of the buffer.
 *
 * @param b Pointer to the buffer.
 * @return size_t Total capacity of the buffer or zero if b is NULL.
 */
size_t buf_capacity(const buf_t *b);

/**
 * @brief Get a pointer to the buffer data.
 *
 * @param b Pointer to the buffer.
 * @return uint8_t* Pointer to the buffer data or NULL if b is NULL.
 */
uint8_t* buf_data(buf_t *b);

/* Buffer mgmt */

/**
 * @brief Cut a portion of the buffer.
 * 
 * This removes a portion of the buffer starting at the given offset
 * and spanning n bytes.  The remaining data is shifted down to fill
 * the gap.
 *
 * @param b Pointer to the buffer.
 * @param offset Offset from the beginning of the buffer.
 * @param n Number of bytes to cut.
 * @return true if the operation was successful, false otherwise.
 */
bool buf_cut(buf_t *b, size_t offset, size_t n);

/**
 * @brief Splice data into the buffer.
 *
 * This inserts data into the buffer at the given offset from the
 * read index, shifting existing data up to make room.
 *
 * @param dest Pointer to the buffer.
 * @param dest_offset Offset from the read index of the buffer.
 * @param src Pointer to the source buffer.
 * @param src_offset Offset from the read index of the source buffer.
 * @param n Number of bytes to splice.
 * @return true if the operation was successful, false otherwise.
 */
bool buf_splice_buf(buf_t *dest, size_t dest_offset, const buf_t *src, size_t src_offset, size_t n);

/**
 * @brief Splice bytes into the buffer.
 *
 * This inserts data into the buffer at the given offset from the
 * read index, shifting existing data up to make room.
 *
 * @param b Pointer to the buffer.
 * @param offset Offset from the read index of the buffer.
 * @param data Pointer to the data to splice in.
 * @param n Number of bytes to splice.
 * @return true if the operation was successful, false otherwise.
 */
bool buf_splice_bytes(buf_t *b, size_t offset, const uint8_t *data, size_t n);

/**
 * @brief Reserve space in the buffer.
 *
 * This reserves a portion of the buffer starting at the given offset from the read index
 * and spanning reservation_size bytes.  The passed dest buffer will
 * be set up to reference the reserved space.
 * 
 * If there is not enough data between the read and write cursors, the
 * write cursor is advanced to provide the requested space.
 * 
 * This is used to reserve space for headers that will be filled in later.
 *
 * @param src Pointer to the source buffer.
 * @param offset Offset from the read index of the source buffer.
 * @param reservation_size Size of the reservation.
 * @param dest Pointer to the destination buffer to receive the reservation.
 * @return true if the operation was successful, false otherwise.
 */
bool buf_reserve(buf_t *src, size_t offset, size_t reservation_size, buf_t *dest);

/**
 * @brief Compact space in the buffer.
 *
 * This moves the unread data to the beginning of the buffer,
 * freeing up space at the end for new data.  The read index is set to zero
 * and the write index is adjusted accordingly.
 *
 * @param b Pointer to the buffer.
 * @return true if the operation was successful, false otherwise.
 */
bool buf_compact(buf_t *b);

/**
 * @brief Reset the buffer.
 *
 * Sets the read and write cursors to zero and clear all error status.
 *
 * @param b Pointer to the buffer.
 * @return true if the operation was successful, false otherwise.
 */
bool buf_reset(buf_t *b);

/**
 * @brief Save a checkpoint of the buffer state.
 *
 * Creates a snapshot of the buffer's current state (read/write cursors and error status).
 * This can be used with buf_restore() to revert the buffer to a previous state.
 * Useful for transactional parsing where you want to attempt parsing and restore
 * the buffer if parsing fails (e.g., incomplete data).
 *
 * Example:
 *   buf_t checkpoint = buf_checkpoint(buf);
 *   if (!try_parse(buf, result)) {
 *       buf_restore(buf, checkpoint);  // Revert to checkpoint
 *       return INCOMPLETE;
 *   }
 *
 * @param b Pointer to the buffer.
 * @return buf_t A snapshot of the buffer state at the time of the call.
 */
static inline buf_t buf_checkpoint(buf_t *b) {
    return *b;
}

/**
 * @brief Restore a buffer to a previously saved checkpoint state.
 *
 * Reverts the buffer to the state captured by buf_checkpoint().
 * This restores the read/write cursors and error status.
 *
 * @param b Pointer to the buffer to restore.
 * @param checkpoint The checkpoint state to restore to (from buf_checkpoint()).
 */
static inline void buf_restore(buf_t *b, buf_t checkpoint) {
    *b = checkpoint;
}

/* Buffer read/write */

/**
 * @brief Advance the read cursor by a certain number of bytes.
 * 
 * This moves the read cursor forward by n bytes, if there is enough data
 * available to read.  If n exceeds the available data, an error is set.
 *
 * @param b Pointer to the buffer.
 * @param n Number of bytes to advance.
 * @return true if the operation was successful, false otherwise.
 */
bool buf_read_advance(buf_t *b, size_t n);

/**
 * @brief Get the current read position in the buffer.
 *
 * @param b Pointer to the buffer.
 * @return size_t Current read position in the buffer or zero if b is NULL.
 */
size_t buf_read_pos(const buf_t *b);

/**
 * @brief Get a pointer to the current read position in the buffer.
 *
 * @param b Pointer to the buffer.
 * @return uint8_t* Pointer to the current read position or NULL if b is NULL.
 */
const uint8_t* buf_read_ptr(const buf_t *b);

/**
 * @brief Get the number of bytes available to read in the buffer.
 *
 * @param b Pointer to the buffer.
 * @return size_t Number of bytes available to read or zero if b is NULL.
 */
size_t buf_read_size(const buf_t *b);

/**
 * @brief Advance the write cursor by a certain number of bytes.
 *
 * @param b Pointer to the buffer.
 * @param n Number of bytes to advance.
 * @return true if the operation was successful, false otherwise.
 */
bool buf_write_advance(buf_t *b, size_t n);

/**
 * @brief Get the current write position in the buffer.
 *
 * @param b Pointer to the buffer.
 * @return size_t Current write position in the buffer or zero if b is NULL.
 */
size_t buf_write_pos(const buf_t *b);

/**
 * @brief Get a pointer to the current write position in the buffer.
 *
 * @param b Pointer to the buffer.
 * @return uint8_t* Pointer to the current write position or NULL if b is NULL.
 */
uint8_t* buf_write_ptr(buf_t *b);

/**
 * @brief Get the number of bytes available to write in the buffer.
 * 
 * Note that there might be space available before the read cursor.
 * Compact the buffer to get the maximum available write space.
 *
 * @param b Pointer to the buffer.
 * @return size_t Number of bytes available to write or zero if b is NULL.
 */
size_t buf_write_size(const buf_t *b);


/**
 * @brief The following are convenience functions for reading and writing.
 * 
 * They return true on success, false on failure.  On failure, the buffer's
 * error status is set to indicate the type of error that occurred and the buffer is
 * left in the state it was in before the call, apart from the error state.
 * 
 * All will fail if the buffer is already in an error state.
 * 
 * The field_name parameter is used to indicate which field caused the error.
 */

/* Reads */
bool buf_read_u8(buf_t *b, const char *field_name, uint8_t *out);
bool buf_read_u16_be(buf_t *b, const char *field_name, uint16_t *out);
bool buf_read_u16_le(buf_t *b, const char *field_name, uint16_t *out);
bool buf_read_u32_be(buf_t *b, const char *field_name, uint32_t *out);
bool buf_read_u32_le(buf_t *b, const char *field_name, uint32_t *out);
bool buf_read_u64_be(buf_t *b, const char *field_name, uint64_t *out);
bool buf_read_u64_le(buf_t *b, const char *field_name, uint64_t *out);
bool buf_read_bytes(buf_t *b, const char *field_name, uint8_t *out, size_t len);

/* Writes */
bool buf_write_u8(buf_t *b, const char *field_name, uint8_t v);
bool buf_write_u16_be(buf_t *b, const char *field_name, uint16_t v);
bool buf_write_u16_le(buf_t *b, const char *field_name, uint16_t v);
bool buf_write_u32_be(buf_t *b, const char *field_name, uint32_t v);
bool buf_write_u32_le(buf_t *b, const char *field_name, uint32_t v);
bool buf_write_u64_be(buf_t *b, const char *field_name, uint64_t v);
bool buf_write_u64_le(buf_t *b, const char *field_name, uint64_t v);
bool buf_write_bytes(buf_t *b, const char *field_name, const uint8_t *data, size_t len);

/* Utils */
const char* buf_error_string(util_err_t e);

#ifdef __cplusplus
}
#endif
