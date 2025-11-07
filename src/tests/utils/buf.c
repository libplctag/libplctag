#include "buf.h"
#include <string.h>

/* Helper macro to check if buffer is in a valid state */
#define BUF_VALID(b) \
    ((b) != NULL && (b)->data != NULL && \
     (b)->read <= (b)->write && (b)->write <= (b)->capacity)

buf_t buf_init(uint8_t *data, size_t capacity) {
    buf_t b;

    if (data == NULL && capacity > 0) {
        b.data = NULL;
        b.capacity = 0;
        b.read = 0;
        b.write = 0;
        b.error = UTIL_EINVAL;
        b.failed_field = "data";
        return b;
    }

    b.data = data;
    b.capacity = capacity;
    b.read = 0;
    b.write = 0;
    b.error = UTIL_OK;
    b.failed_field = NULL;
    return b;
}

void buf_set_error(buf_t *b, util_err_t error, const char *field_name) {
    if (b == NULL) return;
    b->error = error;
    b->failed_field = field_name;
}

util_err_t buf_clear_error(buf_t *b) {
    if (b == NULL) return UTIL_EINVAL;
    util_err_t prev = b->error;
    b->error = UTIL_OK;
    b->failed_field = NULL;
    return prev;
}

util_err_t buf_get_error(const buf_t *b) {
    if (b == NULL) return UTIL_EINVAL;
    return b->error;
}

const char* buf_get_failed_field(const buf_t *b) {
    if (b == NULL) return NULL;
    return b->failed_field;
}

bool buf_ok(const buf_t *b) {
    if (b == NULL) return false;
    if (b->data == NULL && b->capacity > 0) return false;
    if (b->read > b->write) return false;
    if (b->write > b->capacity) return false;
    if (b->error != UTIL_OK) return false;
    return true;
}

size_t buf_capacity(const buf_t *b) {
    if (b == NULL) return 0;
    return b->capacity;
}

uint8_t* buf_data(buf_t *b) {
    if (b == NULL) return NULL;
    return b->data;
}

bool buf_cut(buf_t *b, size_t offset, size_t n) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;

    /* offset is relative to read cursor */
    size_t abs_offset = b->read + offset;

    /* Check bounds */
    if (abs_offset > b->write || (abs_offset + n) > b->write) {
        buf_set_error(b, UTIL_EBOUNDS, "cut");
        return false;
    }

    /* Shift data down */
    size_t remaining = b->write - (abs_offset + n);
    if (remaining > 0) {
        memmove(&b->data[abs_offset],
                &b->data[abs_offset + n],
                remaining);
    }

    b->write -= n;
    return true;
}

bool buf_splice_buf(buf_t *dest, size_t dest_offset,
                     const buf_t *src, size_t src_offset, size_t n) {
    if (!BUF_VALID(dest) || dest->error != UTIL_OK) return false;
    if (!BUF_VALID(src)) return false;

    /* Calculate absolute offsets (relative to read cursors) */
    size_t abs_dest_offset = dest->read + dest_offset;
    size_t abs_src_offset = src->read + src_offset;

    /* Check source bounds */
    if ((abs_src_offset + n) > src->write) {
        buf_set_error(dest, UTIL_EBOUNDS, "splice_buf_src");
        return false;
    }

    /* Check destination space after insertion */
    if ((dest->write + n) > dest->capacity) {
        buf_set_error(dest, UTIL_EBOUNDS, "splice_buf_dest");
        return false;
    }

    /* Check destination insertion point */
    if (abs_dest_offset > dest->write) {
        buf_set_error(dest, UTIL_EBOUNDS, "splice_buf_offset");
        return false;
    }

    /* Shift existing data up to make room */
    size_t shift_size = dest->write - abs_dest_offset;
    if (shift_size > 0) {
        memmove(&dest->data[abs_dest_offset + n],
                &dest->data[abs_dest_offset],
                shift_size);
    }

    /* Copy data in */
    memcpy(&dest->data[abs_dest_offset],
           &src->data[abs_src_offset],
           n);

    dest->write += n;
    return true;
}

bool buf_splice_bytes(buf_t *b, size_t offset,
                       const uint8_t *data, size_t n) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if (data == NULL && n > 0) {
        buf_set_error(b, UTIL_EINVAL, "splice_bytes_data");
        return false;
    }

    /* offset is relative to read cursor */
    size_t abs_offset = b->read + offset;

    /* Check destination space after insertion */
    if ((b->write + n) > b->capacity) {
        buf_set_error(b, UTIL_EBOUNDS, "splice_bytes_space");
        return false;
    }

    /* Check insertion point */
    if (abs_offset > b->write) {
        buf_set_error(b, UTIL_EBOUNDS, "splice_bytes_offset");
        return false;
    }

    /* Shift existing data up to make room */
    size_t shift_size = b->write - abs_offset;
    if (shift_size > 0) {
        memmove(&b->data[abs_offset + n],
                &b->data[abs_offset],
                shift_size);
    }

    /* Copy data in */
    if (n > 0) {
        memcpy(&b->data[abs_offset], data, n);
    }

    b->write += n;
    return true;
}

bool buf_reserve(buf_t *src, size_t offset, size_t reservation_size, buf_t *dest) {
    if (!BUF_VALID(src) || src->error != UTIL_OK) return false;
    if (dest == NULL) return false;

    /* offset is relative to read cursor */
    size_t abs_offset = src->read + offset;

    /* Check if offset is valid */
    if (abs_offset > src->write) {
        buf_set_error(src, UTIL_EBOUNDS, "reserve_offset");
        return false;
    }

    /* Ensure write cursor covers the reservation */
    size_t needed_write = abs_offset + reservation_size;
    if (needed_write > src->capacity) {
        buf_set_error(src, UTIL_EBOUNDS, "reserve_space");
        return false;
    }

    /* Advance write cursor if needed */
    if (needed_write > src->write) {
        src->write = needed_write;
    }

    /* Create dest buffer pointing to the reserved region */
    *dest = buf_init(&src->data[abs_offset], reservation_size);
    dest->capacity = reservation_size;
    dest->write = reservation_size;  /* Reserve is initially "full" */

    return true;
}

bool buf_compact(buf_t *b) {
    if (!BUF_VALID(b)) return false;

    size_t unread = b->write - b->read;

    if (b->read > 0 && unread > 0) {
        memmove(&b->data[0], &b->data[b->read], unread);
    }

    b->read = 0;
    b->write = unread;

    return true;
}

bool buf_reset(buf_t *b) {
    if (b == NULL) return false;
    b->read = 0;
    b->write = 0;
    b->error = UTIL_OK;
    b->failed_field = NULL;
    return true;
}

bool buf_read_advance(buf_t *b, size_t n) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if ((b->read + n) > b->write) {
        buf_set_error(b, UTIL_EBOUNDS, "read_advance");
        return false;
    }
    b->read += n;
    return true;
}

size_t buf_read_pos(const buf_t *b) {
    if (b == NULL) return 0;
    return b->read;
}

const uint8_t* buf_read_ptr(const buf_t *b) {
    if (b == NULL || b->data == NULL) return NULL;
    return &b->data[b->read];
}

size_t buf_read_size(const buf_t *b) {
    if (b == NULL) return 0;
    if (b->read > b->write) return 0;
    return b->write - b->read;
}

bool buf_write_advance(buf_t *b, size_t n) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if ((b->write + n) > b->capacity) {
        buf_set_error(b, UTIL_EBOUNDS, "write_advance");
        return false;
    }
    b->write += n;
    return true;
}

size_t buf_write_pos(const buf_t *b) {
    if (b == NULL) return 0;
    return b->write;
}

uint8_t* buf_write_ptr(buf_t *b) {
    if (b == NULL || b->data == NULL) return NULL;
    return &b->data[b->write];
}

size_t buf_write_size(const buf_t *b) {
    if (b == NULL) return 0;
    if (b->write > b->capacity) return 0;
    return b->capacity - b->write;
}

/* ================================================================
 * Read operations
 * ================================================================ */

bool buf_read_u8(buf_t *b, const char *field_name, uint8_t *out) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if (out == NULL) {
        buf_set_error(b, UTIL_EINVAL, field_name);
        return false;
    }
    if ((b->read + 1) > b->write) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    *out = b->data[b->read];
    b->read += 1;
    return true;
}

bool buf_read_u16_be(buf_t *b, const char *field_name, uint16_t *out) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if (out == NULL) {
        buf_set_error(b, UTIL_EINVAL, field_name);
        return false;
    }
    if ((b->read + 2) > b->write) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    *out = ((uint16_t)b->data[b->read + 0] << 8) |
           ((uint16_t)b->data[b->read + 1] << 0);
    b->read += 2;
    return true;
}

bool buf_read_u16_le(buf_t *b, const char *field_name, uint16_t *out) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if (out == NULL) {
        buf_set_error(b, UTIL_EINVAL, field_name);
        return false;
    }
    if ((b->read + 2) > b->write) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    *out = ((uint16_t)b->data[b->read + 0] << 0) |
           ((uint16_t)b->data[b->read + 1] << 8);
    b->read += 2;
    return true;
}

bool buf_read_u32_be(buf_t *b, const char *field_name, uint32_t *out) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if (out == NULL) {
        buf_set_error(b, UTIL_EINVAL, field_name);
        return false;
    }
    if ((b->read + 4) > b->write) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    *out = ((uint32_t)b->data[b->read + 0] << 24) |
           ((uint32_t)b->data[b->read + 1] << 16) |
           ((uint32_t)b->data[b->read + 2] <<  8) |
           ((uint32_t)b->data[b->read + 3] <<  0);
    b->read += 4;
    return true;
}

bool buf_read_u32_le(buf_t *b, const char *field_name, uint32_t *out) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if (out == NULL) {
        buf_set_error(b, UTIL_EINVAL, field_name);
        return false;
    }
    if ((b->read + 4) > b->write) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    *out = ((uint32_t)b->data[b->read + 0] <<  0) |
           ((uint32_t)b->data[b->read + 1] <<  8) |
           ((uint32_t)b->data[b->read + 2] << 16) |
           ((uint32_t)b->data[b->read + 3] << 24);
    b->read += 4;
    return true;
}

bool buf_read_u64_be(buf_t *b, const char *field_name, uint64_t *out) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if (out == NULL) {
        buf_set_error(b, UTIL_EINVAL, field_name);
        return false;
    }
    if ((b->read + 8) > b->write) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    *out = ((uint64_t)b->data[b->read + 0] << 56) |
           ((uint64_t)b->data[b->read + 1] << 48) |
           ((uint64_t)b->data[b->read + 2] << 40) |
           ((uint64_t)b->data[b->read + 3] << 32) |
           ((uint64_t)b->data[b->read + 4] << 24) |
           ((uint64_t)b->data[b->read + 5] << 16) |
           ((uint64_t)b->data[b->read + 6] <<  8) |
           ((uint64_t)b->data[b->read + 7] <<  0);
    b->read += 8;
    return true;
}

bool buf_read_u64_le(buf_t *b, const char *field_name, uint64_t *out) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if (out == NULL) {
        buf_set_error(b, UTIL_EINVAL, field_name);
        return false;
    }
    if ((b->read + 8) > b->write) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    *out = ((uint64_t)b->data[b->read + 0] <<  0) |
           ((uint64_t)b->data[b->read + 1] <<  8) |
           ((uint64_t)b->data[b->read + 2] << 16) |
           ((uint64_t)b->data[b->read + 3] << 24) |
           ((uint64_t)b->data[b->read + 4] << 32) |
           ((uint64_t)b->data[b->read + 5] << 40) |
           ((uint64_t)b->data[b->read + 6] << 48) |
           ((uint64_t)b->data[b->read + 7] << 56);
    b->read += 8;
    return true;
}

bool buf_read_bytes(buf_t *b, const char *field_name, uint8_t *out, size_t len) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if (len > 0 && out == NULL) {
        buf_set_error(b, UTIL_EINVAL, field_name);
        return false;
    }
    if ((b->read + len) > b->write) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    if (len > 0) {
        memcpy(out, &b->data[b->read], len);
    }
    b->read += len;
    return true;
}

/* ================================================================
 * Write operations
 * ================================================================ */

bool buf_write_u8(buf_t *b, const char *field_name, uint8_t v) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if ((b->write + 1) > b->capacity) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    b->data[b->write] = v;
    b->write += 1;
    return true;
}

bool buf_write_u16_be(buf_t *b, const char *field_name, uint16_t v) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if ((b->write + 2) > b->capacity) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    b->data[b->write + 0] = (uint8_t)((v >> 8) & 0xFF);
    b->data[b->write + 1] = (uint8_t)((v >> 0) & 0xFF);
    b->write += 2;
    return true;
}

bool buf_write_u16_le(buf_t *b, const char *field_name, uint16_t v) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if ((b->write + 2) > b->capacity) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    b->data[b->write + 0] = (uint8_t)((v >> 0) & 0xFF);
    b->data[b->write + 1] = (uint8_t)((v >> 8) & 0xFF);
    b->write += 2;
    return true;
}

bool buf_write_u32_be(buf_t *b, const char *field_name, uint32_t v) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if ((b->write + 4) > b->capacity) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    b->data[b->write + 0] = (uint8_t)((v >> 24) & 0xFF);
    b->data[b->write + 1] = (uint8_t)((v >> 16) & 0xFF);
    b->data[b->write + 2] = (uint8_t)((v >>  8) & 0xFF);
    b->data[b->write + 3] = (uint8_t)((v >>  0) & 0xFF);
    b->write += 4;
    return true;
}

bool buf_write_u32_le(buf_t *b, const char *field_name, uint32_t v) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if ((b->write + 4) > b->capacity) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    b->data[b->write + 0] = (uint8_t)((v >>  0) & 0xFF);
    b->data[b->write + 1] = (uint8_t)((v >>  8) & 0xFF);
    b->data[b->write + 2] = (uint8_t)((v >> 16) & 0xFF);
    b->data[b->write + 3] = (uint8_t)((v >> 24) & 0xFF);
    b->write += 4;
    return true;
}

bool buf_write_u64_be(buf_t *b, const char *field_name, uint64_t v) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if ((b->write + 8) > b->capacity) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    b->data[b->write + 0] = (uint8_t)((v >> 56) & 0xFF);
    b->data[b->write + 1] = (uint8_t)((v >> 48) & 0xFF);
    b->data[b->write + 2] = (uint8_t)((v >> 40) & 0xFF);
    b->data[b->write + 3] = (uint8_t)((v >> 32) & 0xFF);
    b->data[b->write + 4] = (uint8_t)((v >> 24) & 0xFF);
    b->data[b->write + 5] = (uint8_t)((v >> 16) & 0xFF);
    b->data[b->write + 6] = (uint8_t)((v >>  8) & 0xFF);
    b->data[b->write + 7] = (uint8_t)((v >>  0) & 0xFF);
    b->write += 8;
    return true;
}

bool buf_write_u64_le(buf_t *b, const char *field_name, uint64_t v) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if ((b->write + 8) > b->capacity) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    b->data[b->write + 0] = (uint8_t)((v >>  0) & 0xFF);
    b->data[b->write + 1] = (uint8_t)((v >>  8) & 0xFF);
    b->data[b->write + 2] = (uint8_t)((v >> 16) & 0xFF);
    b->data[b->write + 3] = (uint8_t)((v >> 24) & 0xFF);
    b->data[b->write + 4] = (uint8_t)((v >> 32) & 0xFF);
    b->data[b->write + 5] = (uint8_t)((v >> 40) & 0xFF);
    b->data[b->write + 6] = (uint8_t)((v >> 48) & 0xFF);
    b->data[b->write + 7] = (uint8_t)((v >> 56) & 0xFF);
    b->write += 8;
    return true;
}

bool buf_write_bytes(buf_t *b, const char *field_name, const uint8_t *data, size_t len) {
    if (!BUF_VALID(b) || b->error != UTIL_OK) return false;
    if (len > 0 && data == NULL) {
        buf_set_error(b, UTIL_EINVAL, field_name);
        return false;
    }
    if ((b->write + len) > b->capacity) {
        buf_set_error(b, UTIL_EBOUNDS, field_name);
        return false;
    }
    if (len > 0) {
        memcpy(&b->data[b->write], data, len);
    }
    b->write += len;
    return true;
}

const char* buf_error_string(util_err_t e) {
    return util_err_str(e);
}
