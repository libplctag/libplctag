# Design Addendum: Framing and Buffer API Refinement

**Date:** 2026-01-21
**Supplements:** design_recommendations.md

---

## Critical Issues Identified

### 1. Missing Framing Support

**Problem:** Industrial protocols need to determine if a complete packet has arrived before processing. The current design doesn't provide framing primitives.

**Examples of framing mechanisms:**
- **Length-prefixed:** Header contains total message length (Modbus TCP, EtherNet/IP)
- **Delimiter-based:** Message ends with specific byte sequence (ASCII protocols)
- **Fixed-length:** Known message size based on message type
- **Mixed:** Header of known size, then length-prefixed body

**Current gap:** Without framing helpers, every protocol must manually implement:
```c
// Check if we have enough data for header
if (buf_read_size(&buf) < HEADER_SIZE) return NEED_MORE_DATA;

// Parse header to get body length
uint16_t body_len;
buf_read_u16(&buf, "length", &body_len);

// Check if we have complete message
if (buf_read_size(&buf) < HEADER_SIZE + body_len) {
    // Roll back the header read!
    // But buf.h doesn't make this easy without checkpoint
    return NEED_MORE_DATA;
}
```

This is error-prone and repetitive.

### 2. Buffer API Split is Correct

**Current buf.h design:** Single `buf_t` type with both read and write cursors.

**Issue:** This conflates two distinct use cases:
- **Receive buffer:** Append incoming data, consume from front as we parse
- **Send buffer:** Build outgoing message, send from front

**Your design in smaller_code.md is better:** Separate `ev_read_buf_t` and `ev_write_buf_t`.

**Benefits of split:**
1. **Clearer API:** `decode_*` vs `encode_*` leaves no ambiguity
2. **Simpler implementation:** Each type only needs one cursor
3. **Type safety:** Can't accidentally mix operations
4. **Smaller memory:** Read buffer only needs one cursor for consumption

---

## Revised Buffer API Design

### Read Buffer (Receiving/Decoding)

```c
/**
 * @brief Read buffer for receiving and decoding data.
 *
 * Read buffers are used to receive data from sockets and decode protocol
 * messages. They maintain:
 * - A consume cursor: how much has been decoded/consumed
 * - A fill cursor: how much has been received from socket
 *
 * Data flows:
 *   Socket -> append to end -> decode from front -> compact
 */
typedef struct ev_read_buf_s ev_read_buf_t;

/* Lifecycle */
ev_read_buf_t *ev_read_buf_create(uint32_t capacity);
void ev_read_buf_destroy(ev_read_buf_t **buf);

/* Or stack-allocated with external storage */
ev_read_buf_t ev_read_buf_init(uint8_t *data, uint32_t capacity);

/* Buffer management */
ev_status_t ev_read_buf_reset(ev_read_buf_t *buf);
ev_status_t ev_read_buf_compact(ev_read_buf_t *buf);  /* memmove unread to start */

/* Query state */
uint32_t ev_read_buf_available(const ev_read_buf_t *buf);  /* bytes available to read */
uint32_t ev_read_buf_consumed(const ev_read_buf_t *buf);   /* bytes already consumed */
uint32_t ev_read_buf_capacity(const ev_read_buf_t *buf);   /* total capacity */
uint32_t ev_read_buf_space(const ev_read_buf_t *buf);      /* space for more recv */

/* Raw access (for socket I/O) */
uint8_t *ev_read_buf_append_ptr(ev_read_buf_t *buf);     /* where to recv into */
ev_status_t ev_read_buf_append_commit(ev_read_buf_t *buf, uint32_t n); /* after recv */

/* Decoding operations - consume from front */
bool ev_read_buf_decode_u8(ev_read_buf_t *buf, uint8_t *out);
bool ev_read_buf_decode_u16(ev_read_buf_t *buf, ev_endian_t endian, uint16_t *out);
bool ev_read_buf_decode_u32(ev_read_buf_t *buf, ev_endian_t endian, uint32_t *out);
bool ev_read_buf_decode_u64(ev_read_buf_t *buf, ev_endian_t endian, uint64_t *out);
bool ev_read_buf_decode_i8(ev_read_buf_t *buf, int8_t *out);
bool ev_read_buf_decode_i16(ev_read_buf_t *buf, ev_endian_t endian, int16_t *out);
bool ev_read_buf_decode_i32(ev_read_buf_t *buf, ev_endian_t endian, int32_t *out);
bool ev_read_buf_decode_i64(ev_read_buf_t *buf, ev_endian_t endian, int64_t *out);
bool ev_read_buf_decode_f32(ev_read_buf_t *buf, ev_endian_t endian, float *out);
bool ev_read_buf_decode_f64(ev_read_buf_t *buf, ev_endian_t endian, double *out);
bool ev_read_buf_decode_bytes(ev_read_buf_t *buf, uint8_t *dest, uint32_t len);

/* Peeking - read without consuming (for framing checks) */
bool ev_read_buf_peek_u8(const ev_read_buf_t *buf, uint32_t offset, uint8_t *out);
bool ev_read_buf_peek_u16(const ev_read_buf_t *buf, uint32_t offset,
                          ev_endian_t endian, uint16_t *out);
bool ev_read_buf_peek_u32(const ev_read_buf_t *buf, uint32_t offset,
                          ev_endian_t endian, uint32_t *out);
bool ev_read_buf_peek_bytes(const ev_read_buf_t *buf, uint32_t offset,
                            uint8_t *dest, uint32_t len);

/* Skip bytes without reading them */
bool ev_read_buf_skip(ev_read_buf_t *buf, uint32_t n);

/* Checkpointing for tentative reads */
typedef uint32_t ev_read_buf_mark_t;
ev_read_buf_mark_t ev_read_buf_mark(const ev_read_buf_t *buf);
void ev_read_buf_rewind(ev_read_buf_t *buf, ev_read_buf_mark_t mark);

/* Error state */
ev_status_t ev_read_buf_get_error(const ev_read_buf_t *buf);
void ev_read_buf_set_error(ev_read_buf_t *buf, ev_status_t err);
void ev_read_buf_clear_error(ev_read_buf_t *buf);
```

### Write Buffer (Sending/Encoding)

```c
/**
 * @brief Write buffer for encoding and sending data.
 *
 * Write buffers are used to encode protocol messages and send to sockets.
 * They maintain:
 * - A write cursor: how much has been encoded
 * - A send cursor: how much has been sent
 *
 * Data flows:
 *   Encode -> build message -> send to socket -> reset
 */
typedef struct ev_write_buf_s ev_write_buf_t;

/* Lifecycle */
ev_write_buf_t *ev_write_buf_create(uint32_t capacity);
void ev_write_buf_destroy(ev_write_buf_t **buf);

/* Or stack-allocated */
ev_write_buf_t ev_write_buf_init(uint8_t *data, uint32_t capacity);

/* Buffer management */
ev_status_t ev_write_buf_reset(ev_write_buf_t *buf);

/* Query state */
uint32_t ev_write_buf_length(const ev_write_buf_t *buf);   /* bytes encoded */
uint32_t ev_write_buf_sent(const ev_write_buf_t *buf);     /* bytes already sent */
uint32_t ev_write_buf_remaining(const ev_write_buf_t *buf); /* bytes left to send */
uint32_t ev_write_buf_capacity(const ev_write_buf_t *buf);
uint32_t ev_write_buf_space(const ev_write_buf_t *buf);    /* space left for encoding */

/* Raw access (for socket I/O) */
const uint8_t *ev_write_buf_send_ptr(const ev_write_buf_t *buf); /* what to send */
ev_status_t ev_write_buf_send_commit(ev_write_buf_t *buf, uint32_t n); /* after send */

/* Encoding operations - append to end */
bool ev_write_buf_encode_u8(ev_write_buf_t *buf, uint8_t val);
bool ev_write_buf_encode_u16(ev_write_buf_t *buf, ev_endian_t endian, uint16_t val);
bool ev_write_buf_encode_u32(ev_write_buf_t *buf, ev_endian_t endian, uint32_t val);
bool ev_write_buf_encode_u64(ev_write_buf_t *buf, ev_endian_t endian, uint64_t val);
bool ev_write_buf_encode_i8(ev_write_buf_t *buf, int8_t val);
bool ev_write_buf_encode_i16(ev_write_buf_t *buf, ev_endian_t endian, int16_t val);
bool ev_write_buf_encode_i32(ev_write_buf_t *buf, ev_endian_t endian, int32_t val);
bool ev_write_buf_encode_i64(ev_write_buf_t *buf, ev_endian_t endian, int64_t val);
bool ev_write_buf_encode_f32(ev_write_buf_t *buf, ev_endian_t endian, float val);
bool ev_write_buf_encode_f64(ev_write_buf_t *buf, ev_endian_t endian, double val);
bool ev_write_buf_encode_bytes(ev_write_buf_t *buf, const uint8_t *src, uint32_t len);

/* Reserve space for header to be filled in later */
typedef struct {
    ev_write_buf_t *parent;
    uint32_t offset;
    uint32_t length;
} ev_write_buf_reservation_t;

ev_status_t ev_write_buf_reserve(ev_write_buf_t *buf, uint32_t length,
                                  ev_write_buf_reservation_t *reservation);

/* Encode into a reservation */
bool ev_write_buf_reservation_encode_u16(ev_write_buf_reservation_t *res,
                                         ev_endian_t endian, uint16_t val);
/* ... other reservation encode functions */

/* Error state */
ev_status_t ev_write_buf_get_error(const ev_write_buf_t *buf);
void ev_write_buf_set_error(ev_write_buf_t *buf, ev_status_t err);
void ev_write_buf_clear_error(ev_write_buf_t *buf);
```

### Endianness Type

```c
typedef enum {
    EV_BIG_ENDIAN,
    EV_LITTLE_ENDIAN
} ev_endian_t;
```

---

## Framing API

### Core Framing Functions

```c
/**
 * @brief Frame check result.
 */
typedef enum {
    EV_FRAME_COMPLETE,      /* Complete frame is available */
    EV_FRAME_INCOMPLETE,    /* Need more data */
    EV_FRAME_INVALID,       /* Frame is malformed/invalid */
} ev_frame_status_t;

/**
 * @brief Frame checker callback.
 *
 * Examines a read buffer to determine if it contains a complete frame.
 * Must NOT consume data from the buffer (use peek operations only).
 *
 * @param buf Buffer to check (read-only examination)
 * @param frame_len Output: if COMPLETE, receives the frame length in bytes
 * @param ctx User context
 * @return Frame status
 */
typedef ev_frame_status_t (*ev_frame_checker_t)(const ev_read_buf_t *buf,
                                                 uint32_t *frame_len,
                                                 void *ctx);

/**
 * @brief Wait for a complete frame in the buffer.
 *
 * Receives data into the buffer until frame_checker reports a complete frame.
 * When complete, the buffer will contain at least one full frame starting at
 * the current position.
 *
 * Usage in coroutine:
 *   ev_recv_frame(task, sock, &buf, my_frame_checker, ctx);
 *   // buf now contains complete frame, decode it
 *   ev_read_buf_decode_...
 *
 * @param task Coroutine task handle
 * @param sock Socket to receive from
 * @param buf Read buffer to receive into
 * @param checker Function to check for complete frame
 * @param ctx Context passed to checker
 */
#define ev_recv_frame(task, sock, buf, checker, ctx) \
    do { \
        ev_frame_status_t __ev_frame_status; \
        uint32_t __ev_frame_len = 0; \
        util_err_t __ev_err = UTIL_OK; \
        \
        while (1) { \
            __ev_frame_status = (checker)(buf, &__ev_frame_len, ctx); \
            \
            if (__ev_frame_status == EV_FRAME_COMPLETE) { \
                break; /* Have complete frame */ \
            } \
            \
            if (__ev_frame_status == EV_FRAME_INVALID) { \
                ev_task_set_error(task, UTIL_EINVAL); \
                break; \
            } \
            \
            /* Need more data */ \
            if (ev_read_buf_space(buf) == 0) { \
                /* Buffer full but frame incomplete */ \
                ev_task_set_error(task, UTIL_EBOUNDS); \
                break; \
            } \
            \
            coro_yield(task, CORO_EVENT_READ); \
            __ev_err = ev_socket_recv_append(coro_get_fd(task), buf); \
            \
            if (__ev_err != UTIL_OK && __ev_err != UTIL_EAGAIN) { \
                ev_task_set_error(task, __ev_err); \
                break; \
            } \
        } \
    } while(0)
```

### Common Frame Checkers

```c
/**
 * @brief Frame checker for fixed-length frames.
 *
 * @param expected_len Length of the frame
 */
ev_frame_status_t ev_frame_check_fixed_length(const ev_read_buf_t *buf,
                                               uint32_t *frame_len,
                                               void *ctx);
/* ctx should be pointer to uint32_t containing expected_len */

/**
 * @brief Frame checker for length-prefixed frames.
 *
 * Common pattern: N-byte length field at fixed offset, followed by payload.
 *
 * @param config Configuration (see below)
 */
typedef struct {
    uint32_t length_offset;      /* Offset to length field */
    uint32_t length_size;        /* Size of length field (1, 2, 4) */
    ev_endian_t length_endian;   /* Endianness of length field */
    uint32_t header_size;        /* Total header size (before payload) */
    bool length_includes_header; /* Does length field include header? */
    uint32_t max_frame_size;     /* Maximum allowed frame size (safety) */
} ev_frame_length_prefixed_cfg_t;

ev_frame_status_t ev_frame_check_length_prefixed(const ev_read_buf_t *buf,
                                                  uint32_t *frame_len,
                                                  void *ctx);
/* ctx should be pointer to ev_frame_length_prefixed_cfg_t */

/**
 * @brief Frame checker for delimiter-terminated frames.
 *
 * @param config Configuration (see below)
 */
typedef struct {
    uint8_t delimiter[8];   /* Delimiter sequence */
    uint32_t delim_len;     /* Length of delimiter */
    uint32_t max_frame_size;
} ev_frame_delimiter_cfg_t;

ev_frame_status_t ev_frame_check_delimiter(const ev_read_buf_t *buf,
                                           uint32_t *frame_len,
                                           void *ctx);
/* ctx should be pointer to ev_frame_delimiter_cfg_t */
```

---

## Usage Examples

### Example 1: Modbus TCP (Length-Prefixed)

Modbus TCP uses a 7-byte MBAP header where bytes 4-5 contain the length of the remaining message.

```c
void modbus_tcp_handler(coro_task_handle_t task, socket_t sock, void *ctx) {
    CORO_START(task);

    uint8_t recv_data[512];
    uint8_t send_data[512];
    ev_read_buf_t recv_buf = ev_read_buf_init(recv_data, sizeof(recv_data));
    ev_write_buf_t send_buf = ev_write_buf_init(send_data, sizeof(send_data));

    /* Configure Modbus TCP framing */
    ev_frame_length_prefixed_cfg_t frame_cfg = {
        .length_offset = 4,             /* Length field at byte 4 */
        .length_size = 2,               /* 16-bit length */
        .length_endian = EV_BIG_ENDIAN,
        .header_size = 6,               /* 6 bytes before payload */
        .length_includes_header = false,/* Length is payload only */
        .max_frame_size = 512
    };

    while (1) {
        /* Receive complete frame */
        ev_recv_frame(task, sock, &recv_buf,
                      ev_frame_check_length_prefixed, &frame_cfg);

        if (ev_task_get_error(task) != UTIL_OK) {
            break;  /* Error or connection closed */
        }

        /* Decode MBAP header */
        uint16_t transaction_id, protocol_id, length;
        uint8_t unit_id;

        ev_read_buf_decode_u16(&recv_buf, EV_BIG_ENDIAN, &transaction_id);
        ev_read_buf_decode_u16(&recv_buf, EV_BIG_ENDIAN, &protocol_id);
        ev_read_buf_decode_u16(&recv_buf, EV_BIG_ENDIAN, &length);
        ev_read_buf_decode_u8(&recv_buf, &unit_id);

        /* Decode and process PDU */
        uint8_t function_code;
        ev_read_buf_decode_u8(&recv_buf, &function_code);

        /* Process request based on function_code */
        process_modbus_request(function_code, &recv_buf, &send_buf);

        /* Send response */
        ev_send_all(task, sock, &send_buf);

        /* Reset for next request */
        ev_read_buf_reset(&recv_buf);
        ev_write_buf_reset(&send_buf);
    }

    CORO_END(task);
}
```

### Example 2: Custom Frame Checker

For protocols with complex framing logic:

```c
/* EtherNet/IP uses encapsulation with 24-byte header + variable payload */
typedef struct {
    uint32_t max_size;
} enip_frame_ctx_t;

ev_frame_status_t enip_frame_checker(const ev_read_buf_t *buf,
                                     uint32_t *frame_len,
                                     void *ctx) {
    enip_frame_ctx_t *cfg = ctx;

    /* Need at least header */
    if (ev_read_buf_available(buf) < 24) {
        return EV_FRAME_INCOMPLETE;
    }

    /* Peek at command (bytes 0-1) to validate */
    uint16_t command;
    if (!ev_read_buf_peek_u16(buf, 0, EV_LITTLE_ENDIAN, &command)) {
        return EV_FRAME_INVALID;
    }

    /* Validate command is known */
    if (command > 0x006F && command != 0x0070) {
        return EV_FRAME_INVALID;  /* Unknown command */
    }

    /* Get length from bytes 2-3 */
    uint16_t length;
    if (!ev_read_buf_peek_u16(buf, 2, EV_LITTLE_ENDIAN, &length)) {
        return EV_FRAME_INVALID;
    }

    /* Validate length is reasonable */
    if (length > cfg->max_size) {
        return EV_FRAME_INVALID;
    }

    /* Total frame is header + length */
    uint32_t total = 24 + length;

    if (ev_read_buf_available(buf) < total) {
        return EV_FRAME_INCOMPLETE;
    }

    *frame_len = total;
    return EV_FRAME_COMPLETE;
}

void enip_handler(coro_task_handle_t task, socket_t sock, void *ctx) {
    CORO_START(task);

    uint8_t recv_data[2048];
    ev_read_buf_t recv_buf = ev_read_buf_init(recv_data, sizeof(recv_data));

    enip_frame_ctx_t frame_ctx = { .max_size = 2000 };

    while (1) {
        /* Receive complete EtherNet/IP encapsulation */
        ev_recv_frame(task, sock, &recv_buf, enip_frame_checker, &frame_ctx);

        if (ev_task_get_error(task) != UTIL_OK) {
            break;
        }

        /* Process encapsulation... */
        process_enip_encapsulation(&recv_buf);

        ev_read_buf_reset(&recv_buf);
    }

    CORO_END(task);
}
```

### Example 3: Building Messages with Reserved Headers

Many protocols require you to build the body before you know the header length field:

```c
void build_modbus_response(ev_write_buf_t *buf, uint16_t transaction_id) {
    /* Reserve space for MBAP header (will fill in later) */
    ev_write_buf_reservation_t header;
    ev_write_buf_reserve(buf, 7, &header);

    /* Mark where PDU starts */
    uint32_t pdu_start = ev_write_buf_length(buf);

    /* Encode PDU */
    ev_write_buf_encode_u8(buf, MODBUS_FUNC_READ_HOLDING);
    ev_write_buf_encode_u8(buf, 10);  /* byte count */
    for (int i = 0; i < 5; i++) {
        ev_write_buf_encode_u16(buf, EV_BIG_ENDIAN, register_values[i]);
    }

    /* Calculate PDU length */
    uint32_t pdu_length = ev_write_buf_length(buf) - pdu_start;

    /* Now fill in the header we reserved */
    ev_write_buf_t hdr_buf = ev_write_buf_init(
        ev_write_buf_data(buf), 7  /* just the header region */
    );

    ev_write_buf_encode_u16(&hdr_buf, EV_BIG_ENDIAN, transaction_id);
    ev_write_buf_encode_u16(&hdr_buf, EV_BIG_ENDIAN, 0);  /* protocol ID */
    ev_write_buf_encode_u16(&hdr_buf, EV_BIG_ENDIAN, pdu_length + 1); /* +1 for unit_id */
    ev_write_buf_encode_u8(&hdr_buf, 1);  /* unit ID */
}
```

Actually, better API for reservations:

```c
void build_modbus_response(ev_write_buf_t *buf, uint16_t transaction_id) {
    /* Reserve space for length field specifically */
    ev_write_buf_encode_u16(buf, EV_BIG_ENDIAN, transaction_id);
    ev_write_buf_encode_u16(buf, EV_BIG_ENDIAN, 0);  /* protocol ID */

    /* Reserve the length field, get a handle to update it later */
    uint32_t length_pos = ev_write_buf_length(buf);
    ev_write_buf_encode_u16(buf, EV_BIG_ENDIAN, 0);  /* placeholder */

    ev_write_buf_encode_u8(buf, 1);  /* unit ID */

    uint32_t pdu_start = ev_write_buf_length(buf);

    /* Encode PDU */
    ev_write_buf_encode_u8(buf, MODBUS_FUNC_READ_HOLDING);
    ev_write_buf_encode_u8(buf, 10);
    for (int i = 0; i < 5; i++) {
        ev_write_buf_encode_u16(buf, EV_BIG_ENDIAN, register_values[i]);
    }

    /* Calculate and update length */
    uint32_t pdu_length = ev_write_buf_length(buf) - pdu_start;
    ev_write_buf_poke_u16(buf, length_pos, EV_BIG_ENDIAN, pdu_length + 1);
}
```

So we need poke functions:

```c
/* Write buffer poke operations - update at specific offset without moving cursor */
bool ev_write_buf_poke_u8(ev_write_buf_t *buf, uint32_t offset, uint8_t val);
bool ev_write_buf_poke_u16(ev_write_buf_t *buf, uint32_t offset,
                           ev_endian_t endian, uint16_t val);
bool ev_write_buf_poke_u32(ev_write_buf_t *buf, uint32_t offset,
                           ev_endian_t endian, uint32_t val);
```

---

## Implementation Notes

### Read Buffer Internal Structure

```c
struct ev_read_buf_s {
    uint8_t *data;
    uint32_t capacity;
    uint32_t consumed;   /* how much has been decoded/consumed */
    uint32_t filled;     /* how much has been received */
    ev_status_t error;
};
```

Operations:
- `ev_read_buf_available()` → `filled - consumed`
- `ev_read_buf_space()` → `capacity - filled`
- `decode_*()` → read from `data[consumed]`, increment `consumed`
- `append_ptr()` → return `&data[filled]`
- `append_commit(n)` → `filled += n`
- `compact()` → `memmove(data, &data[consumed], filled - consumed); filled -= consumed; consumed = 0;`

### Write Buffer Internal Structure

```c
struct ev_write_buf_s {
    uint8_t *data;
    uint32_t capacity;
    uint32_t length;     /* how much has been encoded */
    uint32_t sent;       /* how much has been sent to socket */
    ev_status_t error;
};
```

Operations:
- `ev_write_buf_length()` → `length`
- `ev_write_buf_remaining()` → `length - sent`
- `encode_*()` → write to `data[length]`, increment `length`
- `send_ptr()` → return `&data[sent]`
- `send_commit(n)` → `sent += n`
- `reset()` → `length = 0; sent = 0;`

---

## Revised Code Size Estimates

| Component | Lines of Code | Notes |
|-----------|--------------|-------|
| Read buffer | 350 | Decode operations, peek, mark/rewind |
| Write buffer | 300 | Encode operations, poke |
| Framing helpers | 200 | Frame checkers, recv_frame macro |
| **Total buffer API** | **850** | vs 700 in original estimate |

Still very reasonable for the functionality provided.

---

## Migration from buf.h

The existing `buf.h` in `src/tests/utils` can be deprecated in favor of the split design.

**Advantages of new design over buf.h:**
1. **Clearer semantics** - decode vs encode, no confusion
2. **Simpler internals** - each buffer type has fewer responsibilities
3. **Framing support** - built-in helpers for packet detection
4. **Peek operations** - essential for framing without consuming data
5. **Poke operations** - update length fields after encoding body

**Migration path:**
- Keep `buf.h` for backward compatibility in libplctag
- New protocol code uses `ev_read_buf.h` and `ev_write_buf.h`
- No disruption to existing code

---

## Summary of Changes

### Key Additions

1. **Split buffer types**
   - `ev_read_buf_t` for receiving/decoding
   - `ev_write_buf_t` for encoding/sending

2. **Peek operations** (read without consuming)
   - Essential for frame checking
   - `ev_read_buf_peek_u16()`, etc.

3. **Poke operations** (write without advancing cursor)
   - Essential for updating length fields
   - `ev_write_buf_poke_u16()`, etc.

4. **Mark/rewind** (better than checkpoint)
   - Lightweight (just saves consume position)
   - `ev_read_buf_mark()`, `ev_read_buf_rewind()`

5. **Framing API**
   - `ev_frame_checker_t` callback type
   - `ev_recv_frame()` macro
   - Common frame checkers (fixed-length, length-prefixed, delimiter)

### Impact on Code Size

Protocol implementation with framing:

**Before (no framing support):**
```c
// Manual frame checking and partial read handling: ~80 lines
```

**After (with framing):**
```c
// Configure frame checker: ~5 lines
// ev_recv_frame() call: 1 line
// Decode: ~10 lines
// Total: ~16 lines
```

**Savings: ~64 lines per protocol handler**

---

## Recommendation

**Replace buf.h with split buffer design** (`ev_read_buf_t` / `ev_write_buf_t`) and **add framing API**.

This provides:
- Clearer semantics (decode vs encode)
- Essential framing support for packet protocols
- Simpler, more focused implementation
- ~64 additional lines saved per protocol

The split design is objectively better for the use case and the framing API eliminates a major source of boilerplate.
