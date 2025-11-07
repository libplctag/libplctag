# Modbus TCP Server Refactoring Plan

## Overview

This document outlines the complete plan to refactor the Modbus TCP server to use the centralized utility libraries in `src/tests/utils`. The refactoring transforms the server from an imperative, poll()-based event loop into a declarative, FSM-driven, reactor-based architecture.

---

## Architecture Transformation

### Current Architecture
- **Event Loop**: Manual `poll()` loop with explicit socket array management
- **State Management**: Imperative state machine using `client_state_t` enum
- **I/O**: Blocking/non-blocking socket calls with manual EAGAIN handling
- **Buffer Management**: Manual byte array indexing with offset tracking
- **Configuration**: Custom command-line parsing loop
- **Logging**: Custom logger with manual timestamp generation

### Target Architecture
- **Event Loop**: `reactor_run()` as main loop with event-driven callbacks
- **State Management**: Declarative FSM using `fsm.h` transition tables
- **I/O**: Reactor-managed async I/O with automatic edge-triggering
- **Buffer Management**: `buf_t` with automatic cursor management and error tracking
- **Configuration**: Declarative `args.h` flag definitions
- **Logging**: Centralized `log.h` with level-based filtering

---

## Phase 1: Remove Old Utilities

### Files to Delete
1. `config.c` - Replaced by `args.h`
2. `config.h` - Replaced by `args.h`
3. `logger.c` - Replaced by `log.h`
4. `logger.h` - Replaced by `log.h`
5. `socket_utils.c` - Replaced by `socket.h` and `reactor.h`
6. `socket_utils.h` - Replaced by `socket.h` and `reactor.h`

### Action Items
- Keep old files as reference during development (rename to `*.old`)
- Remove all `#include` statements referencing deleted files
- Update build system (Makefile/CMakeLists.txt) to exclude old files

---

## Phase 2: Command-Line Argument Handling

### Replace `config.c` with `args.h`

**File**: `modbus_server.c`

**Declarative Flag Definitions**:
```c
#include "args.h"

static args_flag_def_t g_flags[] = {
    {
        .name = "listen",
        .type = ARGS_TYPE_STRING,
        .required = ARGS_OPTIONAL,
        .repeat = ARGS_MULTIPLE,
        .debug_name = "server.listen_endpoints",
        .description = "Listen endpoint (IP:port, can repeat)",
        .default_value = { .has_default = false }
    },
    {
        .name = "coils",
        .type = ARGS_TYPE_INT,
        .required = ARGS_OPTIONAL,
        .repeat = ARGS_ONCE,
        .debug_name = "storage.coils",
        .description = "Number of coils",
        .default_value = { .has_default = true, .value.int_val = 100 }
    },
    {
        .name = "discrete_inputs",
        .type = ARGS_TYPE_INT,
        .required = ARGS_OPTIONAL,
        .repeat = ARGS_ONCE,
        .debug_name = "storage.discrete_inputs",
        .description = "Number of discrete inputs",
        .default_value = { .has_default = true, .value.int_val = 100 }
    },
    {
        .name = "holding_registers",
        .type = ARGS_TYPE_INT,
        .required = ARGS_OPTIONAL,
        .repeat = ARGS_ONCE,
        .debug_name = "storage.holding_registers",
        .description = "Number of holding registers",
        .default_value = { .has_default = true, .value.int_val = 100 }
    },
    {
        .name = "input_registers",
        .type = ARGS_TYPE_INT,
        .required = ARGS_OPTIONAL,
        .repeat = ARGS_ONCE,
        .debug_name = "storage.input_registers",
        .description = "Number of input registers",
        .default_value = { .has_default = true, .value.int_val = 100 }
    },
    {
        .name = "debug",
        .type = ARGS_TYPE_BOOL,
        .required = ARGS_OPTIONAL,
        .repeat = ARGS_ONCE,
        .debug_name = "logging.debug",
        .description = "Enable debug logging",
        .default_value = { .has_default = true, .value.bool_val = false }
    },
    {
        .name = "help",
        .type = ARGS_TYPE_BOOL,
        .required = ARGS_OPTIONAL,
        .repeat = ARGS_ONCE,
        .debug_name = "help",
        .description = "Show help message",
        .default_value = { .has_default = true, .value.bool_val = false }
    }
};

#define NUM_FLAGS (sizeof(g_flags) / sizeof(g_flags[0]))
```

**Parsing in main()**:
```c
int main(int argc, const char *argv[]) {
    args_result_t args;
    util_err_t err = args_parse(argc, argv, g_flags, NUM_FLAGS, &args);

    if (err != UTIL_OK) {
        log_error("Failed to parse arguments: %s", args_get_error_detail(&args));
        args_print_help(argv[0], g_flags, NUM_FLAGS);
        args_free(&args);
        return 1;
    }

    if (args_get_bool(&args, "help")) {
        args_print_help(argv[0], g_flags, NUM_FLAGS);
        args_free(&args);
        return 0;
    }

    // Set log level
    if (args_get_bool(&args, "debug")) {
        log_set_level(LOG_LEVEL_SPEW);
    } else {
        log_set_level(LOG_LEVEL_INFO);
    }

    // Get register counts
    int64_t num_coils = args_get_int(&args, "coils");
    int64_t num_discrete_inputs = args_get_int(&args, "discrete_inputs");
    int64_t num_holding_registers = args_get_int(&args, "holding_registers");
    int64_t num_input_registers = args_get_int(&args, "input_registers");

    // Get listen endpoints (repeated flag)
    size_t num_listeners = args_get_count(&args, "listen");
    if (num_listeners == 0) {
        log_error("At least one --listen endpoint required");
        args_free(&args);
        return 1;
    }

    // ... continue initialization ...
}
```

**Parsing Listen Endpoints**:
```c
// Helper to parse "IP:port" format
static util_err_t parse_listen_endpoint(const char *endpoint_str,
                                        socket_address_t *addr) {
    char ip_buf[256];
    uint16_t port = 0;

    // Find colon separator
    const char *colon = strchr(endpoint_str, ':');
    if (!colon) {
        log_error("Invalid endpoint format (expected IP:port): %s", endpoint_str);
        return UTIL_EINVAL;
    }

    // Extract IP
    size_t ip_len = colon - endpoint_str;
    if (ip_len >= sizeof(ip_buf)) {
        log_error("IP address too long: %s", endpoint_str);
        return UTIL_EINVAL;
    }
    memcpy(ip_buf, endpoint_str, ip_len);
    ip_buf[ip_len] = '\0';

    // Parse port
    port = (uint16_t)atoi(colon + 1);
    if (port == 0) {
        log_error("Invalid port number: %s", colon + 1);
        return UTIL_EINVAL;
    }

    return socket_address_init(addr, ip_buf, port);
}

// In main():
for (size_t i = 0; i < num_listeners; i++) {
    args_value_t val = args_get_at(&args, "listen", i);
    const char *endpoint_str = val.value.string_val;

    socket_address_t listen_addr;
    if (parse_listen_endpoint(endpoint_str, &listen_addr) != UTIL_OK) {
        continue;  // Error already logged
    }

    // Create listener socket
    socket_t listener = socket_create_tcp_server(&listen_addr, SOMAXCONN);
    if (listener == INVALID_SOCKET) {
        log_error("Failed to create listener on %s", endpoint_str);
        continue;
    }

    log_info("Listening on %s", endpoint_str);

    // Register with reactor (see Phase 6)
    // ...
}
```

---

## Phase 3: Logging Infrastructure

### Replace `logger.c` with `log.h`

**Global Changes**:
```c
// Remove all includes of "logger.h"
// Add:
#include "log.h"
```

**Log Level Mapping**:
| Old logger.c | New log.h |
|-------------|-----------|
| `logger_log(..., "INFO", ...)` | `log_info(...)` |
| `logger_log(..., "WARN", ...)` | `log_warn(...)` |
| `logger_log(..., "ERROR", ...)` | `log_error(...)` |
| `logger_log(..., "DEBUG", ...)` | `log_detail(...)` or `log_spew(...)` |
| `log_dump_bytes(data, len)` | `log_bytes_detail(buf)` or `log_bytes_spew(buf)` |

**Example Conversions**:
```c
// OLD
logger_log("INFO", "Server started on %s:%d", ip, port);

// NEW
log_info("Server started on %s:%d", ip, port);

// OLD (debug logging)
if (debug_enabled) {
    log_dump_bytes(buffer, length);
}

// NEW (automatically filtered by log level)
log_bytes_spew(&buf);  // Only logged if level >= LOG_LEVEL_SPEW
```

**Benefits**:
- Automatic timestamp and function/line tracking
- Level-based filtering (no manual `if (debug)` checks)
- Consistent format across all modules

---

## Phase 4: Error Handling

### Standardize on `util_err_t`

**Modbus Exception Mapping**:
```c
// modbus_protocol.h or modbus_protocol.c

// Modbus exception codes (from spec)
#define MODBUS_EXCEPTION_ILLEGAL_FUNCTION    0x01
#define MODBUS_EXCEPTION_ILLEGAL_ADDRESS     0x02
#define MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE  0x03
#define MODBUS_EXCEPTION_DEVICE_FAILURE      0x04

// Convert util_err_t to Modbus exception code
static uint8_t modbus_exception_from_util_err(util_err_t err) {
    switch (err) {
        case UTIL_EBOUNDS:       return MODBUS_EXCEPTION_ILLEGAL_ADDRESS;
        case UTIL_EINVAL:        return MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE;
        case UTIL_ERESOURCE:     return MODBUS_EXCEPTION_DEVICE_FAILURE;
        case UTIL_ENOTSUPPORTED: return MODBUS_EXCEPTION_ILLEGAL_FUNCTION;
        default:                 return MODBUS_EXCEPTION_DEVICE_FAILURE;
    }
}

// Convert Modbus exception to util_err_t (for symmetry)
static util_err_t modbus_exception_to_util_err(uint8_t exception) {
    switch (exception) {
        case MODBUS_EXCEPTION_ILLEGAL_FUNCTION:   return UTIL_ENOTSUPPORTED;
        case MODBUS_EXCEPTION_ILLEGAL_ADDRESS:    return UTIL_EBOUNDS;
        case MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE: return UTIL_EINVAL;
        case MODBUS_EXCEPTION_DEVICE_FAILURE:     return UTIL_EINTERNAL;
        default:                                  return UTIL_EINVAL;
    }
}
```

**Update Protocol Function Signatures**:
```c
// OLD (modbus_protocol.c)
int handle_read_coils(const uint8_t *request, int request_len,
                      uint8_t *response, int *response_len,
                      register_storage_t *storage);

// NEW
util_err_t handle_read_coils(buf_t *request, buf_t *response,
                             register_storage_t *storage);
```

**Error Handling in Protocol Functions**:
```c
util_err_t handle_read_coils(buf_t *request, buf_t *response,
                             register_storage_t *storage) {
    uint16_t start_address, quantity;

    // Parse request
    bool ok = true;
    ok &= buf_read_u16_be(request, "start_address", &start_address);
    ok &= buf_read_u16_be(request, "quantity", &quantity);

    if (!ok) {
        log_error("Failed to parse read coils request");
        return buf_get_error(request);
    }

    // Validate parameters
    if (quantity < 1 || quantity > 2000) {
        log_warn("Invalid quantity: %u (must be 1-2000)", quantity);
        return UTIL_EINVAL;  // Maps to ILLEGAL_DATA_VALUE
    }

    if (start_address + quantity > storage->num_coils) {
        log_warn("Address out of range: %u + %u > %u",
                 start_address, quantity, storage->num_coils);
        return UTIL_EBOUNDS;  // Maps to ILLEGAL_ADDRESS
    }

    // Build response
    uint8_t byte_count = (quantity + 7) / 8;
    ok = true;
    ok &= buf_write_u8(response, "byte_count", byte_count);

    // Read coils and pack into bytes
    for (uint16_t i = 0; i < quantity; i++) {
        bool coil_value = modbus_coil_array_get(storage->coils, start_address + i);
        // Pack into response buffer (LSB first per Modbus spec)
        // ...
    }

    if (!ok) {
        return buf_get_error(response);
    }

    return UTIL_OK;
}
```

**Exception Response Building**:
```c
// Build Modbus exception response
static void build_exception_response(buf_t *response, const mbap_header_t *req_header,
                                     uint8_t function_code, util_err_t error) {
    buf_reset(response);

    // MBAP header
    buf_write_u16_be(response, "transaction_id", req_header->transaction_id);
    buf_write_u16_be(response, "protocol_id", 0);
    buf_write_u16_be(response, "length", 3);  // unit_id + FC + exception
    buf_write_u8(response, "unit_id", req_header->unit_id);

    // Exception PDU
    buf_write_u8(response, "function_code", function_code | 0x80);  // High bit set
    buf_write_u8(response, "exception_code", modbus_exception_from_util_err(error));

    log_warn("Sending exception response: FC=0x%02X, Exception=0x%02X (%s)",
             function_code, modbus_exception_from_util_err(error), util_err_str(error));
}
```

---

## Phase 5: Buffer Management

### Replace Manual Byte Arrays with `buf_t`

**Data Structure Changes**:
```c
// OLD (modbus_server.c)
typedef struct {
    socket_t socket;
    client_state_t state;
    uint8_t recv_buffer[267];     // MODBUS_MAX_ADU_SIZE
    int recv_offset;
    int expected_length;
    uint8_t send_buffer[267];
    int send_length;
    int send_offset;
    char client_info[64];
} client_connection_t;

// NEW
typedef struct {
    socket_t socket;
    fsm_t *fsm;                   // FSM handles state
    uint8_t recv_data[267];
    buf_t recv_buf;               // Buffer with cursors
    uint8_t send_data[267];
    buf_t send_buf;
    socket_address_t peer_addr;
    mbap_header_t mbap_header;    // Parsed MBAP header
    modbus_message_t request;     // Parsed request
    size_t expected_pdu_length;   // Expected PDU bytes (from MBAP length field)
    register_storage_t *storage;  // Shared storage reference
} client_ctx_t;
```

**Buffer Initialization**:
```c
// In client creation
client_ctx_t *client = calloc(1, sizeof(*client));
client->socket = client_socket;
client->recv_buf = buf_init(client->recv_data, sizeof(client->recv_data));
client->send_buf = buf_init(client->send_data, sizeof(client->send_data));
client->peer_addr = peer_addr;
client->storage = storage;
// ... FSM creation in Phase 7 ...
```

**MBAP Header Parsing** (modbus_protocol.c):
```c
util_err_t modbus_parse_mbap_header(buf_t *buf, mbap_header_t *header) {
    // Use checkpoint for transactional parsing
    buf_t checkpoint = buf_checkpoint(buf);

    bool ok = true;
    ok &= buf_read_u16_be(buf, "transaction_id", &header->transaction_id);
    ok &= buf_read_u16_be(buf, "protocol_id", &header->protocol_id);
    ok &= buf_read_u16_be(buf, "length", &header->length);
    ok &= buf_read_u8(buf, "unit_id", &header->unit_id);

    if (!ok) {
        buf_restore(buf, checkpoint);
        return buf_get_error(buf);
    }

    // Validate protocol ID
    if (header->protocol_id != 0) {
        log_warn("Invalid protocol ID: %u (expected 0)", header->protocol_id);
        buf_restore(buf, checkpoint);
        return UTIL_EINVAL;
    }

    // Validate length field (1 byte unit_id + 1-260 bytes PDU)
    if (header->length < 2 || header->length > 261) {
        log_warn("Invalid length field: %u (expected 2-261)", header->length);
        buf_restore(buf, checkpoint);
        return UTIL_EINVAL;
    }

    log_detail("MBAP: TID=%u, PID=%u, Len=%u, UID=%u",
               header->transaction_id, header->protocol_id,
               header->length, header->unit_id);

    return UTIL_OK;
}
```

**Response Building**:
```c
// Build successful response with MBAP header
static util_err_t build_response_header(buf_t *response,
                                        const mbap_header_t *req_header,
                                        uint16_t pdu_length) {
    buf_reset(response);

    bool ok = true;
    ok &= buf_write_u16_be(response, "transaction_id", req_header->transaction_id);
    ok &= buf_write_u16_be(response, "protocol_id", 0);
    ok &= buf_write_u16_be(response, "length", pdu_length + 1);  // +1 for unit_id
    ok &= buf_write_u8(response, "unit_id", req_header->unit_id);

    if (!ok) {
        return buf_get_error(response);
    }

    return UTIL_OK;
}
```

---

## Phase 6: Bit Array Management

### Custom Bit Array for Modbus Coils

**Note**: The existing `bitarray.h` is fixed at 64 bits. Modbus requires up to 65536 coils/discrete inputs, so we need a custom implementation.

**New File**: `modbus_bitarray.h`
```c
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
        arr->bytes[byte_idx] |= (1 << bit_offset);
    } else {
        arr->bytes[byte_idx] &= ~(1 << bit_offset);
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
```

**Update Register Storage** (register_storage.h):
```c
#include "modbus_bitarray.h"

typedef struct {
    modbus_bitarray_t *coils;               // Read/write bits
    modbus_bitarray_t *discrete_inputs;     // Read-only bits
    uint16_t *holding_registers;            // Read/write 16-bit values
    size_t num_holding_registers;
    uint16_t *input_registers;              // Read-only 16-bit values
    size_t num_input_registers;
} register_storage_t;
```

**Update Register Storage Functions** (register_storage.c):
```c
register_storage_t* register_storage_create(size_t num_coils,
                                            size_t num_discrete_inputs,
                                            size_t num_holding_registers,
                                            size_t num_input_registers) {
    register_storage_t *storage = calloc(1, sizeof(*storage));
    if (!storage) {
        return NULL;
    }

    storage->coils = modbus_bitarray_create(num_coils);
    storage->discrete_inputs = modbus_bitarray_create(num_discrete_inputs);
    storage->holding_registers = calloc(num_holding_registers, sizeof(uint16_t));
    storage->input_registers = calloc(num_input_registers, sizeof(uint16_t));

    storage->num_holding_registers = num_holding_registers;
    storage->num_input_registers = num_input_registers;

    if (!storage->coils || !storage->discrete_inputs ||
        !storage->holding_registers || !storage->input_registers) {
        register_storage_destroy(storage);
        return NULL;
    }

    return storage;
}

void register_storage_destroy(register_storage_t *storage) {
    if (storage) {
        modbus_bitarray_destroy(storage->coils);
        modbus_bitarray_destroy(storage->discrete_inputs);
        free(storage->holding_registers);
        free(storage->input_registers);
        free(storage);
    }
}

// Update all read/write functions to use modbus_bitarray_t API
bool register_storage_read_coils(register_storage_t *storage,
                                 uint16_t address, uint16_t count,
                                 uint8_t *out_bytes) {
    return modbus_bitarray_read_bits(storage->coils, address, count, out_bytes);
}

bool register_storage_write_coils(register_storage_t *storage,
                                  uint16_t address, uint16_t count,
                                  const uint8_t *in_bytes) {
    return modbus_bitarray_write_bits(storage->coils, address, count, in_bytes);
}

// Similar for discrete inputs, holding registers, input registers...
```

---

## Phase 7: Reactor Integration

### Replace poll() Loop with Reactor

**Key Design Point: Reactor-FSM Shim**

The reactor-to-FSM integration uses a clean separation of concerns:

1. **Client Context** (`client_ctx_t`): Allocated when client is accepted, contains socket, FSM, buffers, and peer address
2. **Reactor Callback**: Receives client context as `socket_ctx_data` parameter
3. **Direct Context Flow**: The same `client_ctx_t` pointer passed to `reactor_add_socket()` is passed back to the callback and then to FSM actions as event context
4. **Event Processing**: Reactor callback queues the reactor event to the FSM, then processes all queued events

This eliminates the need for separate event context structures—the client context itself serves as the event context.

**Global Reactor Instance** (modbus_server.c):
```c
#include "reactor.h"
#include "socket.h"

// Global reactor (accessible from signal handlers)
static reactor_t *g_reactor = NULL;

// Global storage (shared by all clients)
static register_storage_t *g_storage = NULL;

// Signal handler for graceful shutdown
static void signal_handler(int sig) {
    (void)sig;
    log_info("Received signal, shutting down...");
    if (g_reactor) {
        reactor_stop(g_reactor);
    }
}
```

**Listener Socket Callback**:
```c
// Context for listener sockets
typedef struct {
    char endpoint_str[64];  // For logging (e.g., "127.0.0.1:502")
} listener_ctx_t;

// Callback for listener socket events
static void listener_socket_callback(reactor_t *r, socket_t sock,
                                    event_type_t event, util_err_t status,
                                    void *socket_ctx_data) {
    listener_ctx_t *ctx = (listener_ctx_t *)socket_ctx_data;

    if (event == REACTOR_EVENT_CAN_ACCEPT) {
        // Accept new client in a loop (drain accept queue)
        while (true) {
            socket_t client_sock = INVALID_SOCKET;
            socket_address_t client_addr;

            util_err_t err = socket_accept(sock, &client_sock, &client_addr);

            if (err == UTIL_EAGAIN) {
                // No more pending connections
                break;
            }

            if (err != UTIL_OK) {
                log_error("Accept failed on %s: %s",
                         ctx ? ctx->endpoint_str : "unknown",
                         util_err_str(err));
                break;
            }

            // Format client address for logging
            char client_addr_str[64];
            socket_address_get_addr_str(&client_addr, client_addr_str, sizeof(client_addr_str));
            uint16_t client_port = socket_address_get_port(&client_addr);

            log_info("Accepted connection from %s:%u", client_addr_str, client_port);

            // Create client context
            // The client_ctx_t will be passed to reactor_add_socket() as socket_ctx_data
            // and will be returned to client_socket_callback() and then to FSM actions
            client_ctx_t *client = client_create(client_sock, client_addr, g_storage);
            if (!client) {
                log_error("Failed to create client context");
                socket_close(client_sock);
                continue;
            }

            // Register client socket with reactor, passing client context
            // This context will be passed back to client_socket_callback()
            err = reactor_add_socket(r, client_sock, client_socket_callback, (void *)client);
            if (err != UTIL_OK) {
                log_error("Failed to register client socket: %s", util_err_str(err));
                client_destroy(client);
                continue;
            }
        }

        // Re-enable accept event (one-shot semantics)
        reactor_set_event_enable_mask(r, sock, REACTOR_EVENT_CAN_ACCEPT, true);

    } else if (event == REACTOR_EVENT_ERROR) {
        log_error("Listener socket error on %s: %s",
                 ctx ? ctx->endpoint_str : "unknown",
                 util_err_str(status));

    } else if (event == REACTOR_EVENT_CLOSED) {
        log_warn("Listener socket closed on %s",
                ctx ? ctx->endpoint_str : "unknown");

    } else if (event == REACTOR_EVENT_SHUTDOWN) {
        log_info("Listener shutting down on %s",
                ctx ? ctx->endpoint_str : "unknown");
        socket_close(sock);
        if (ctx) {
            free(ctx);
        }
    }
}
```

**Client Socket Callback** (shim between reactor and FSM):
```c
// Callback for client socket events - acts as shim between reactor and FSM
//
// IMPORTANT: The socket_ctx_data parameter IS the client_ctx_t pointer
// that was passed to reactor_add_socket() when this socket was registered.
// Do NOT allocate a new context here - just use the one passed in.
//
static void client_socket_callback(reactor_t *r, socket_t sock,
                                   event_type_t event, util_err_t status,
                                   void *socket_ctx_data) {
    // socket_ctx_data IS the client_ctx_t that was passed to reactor_add_socket()
    // This is the same context object created in listener_socket_callback()
    client_ctx_t *client = (client_ctx_t *)socket_ctx_data;

    log_spew("Client socket event: reactor_event=%u (status: %s)", event, util_err_str(status));

    // Queue the reactor event to the FSM
    // Pass the same client context as the event context
    // The FSM will receive this pointer in action functions
    util_err_t err = fsm_queue_event(client->fsm, event, status, (void *)client);
    if (err != UTIL_OK) {
        log_error("Failed to queue FSM event: %s", util_err_str(err));
        return;
    }

    // Process all queued events (may generate internal events that queue more events)
    err = fsm_process_events(client->fsm);
    if (err != UTIL_OK) {
        log_error("FSM event processing failed: %s", util_err_str(err));
    }
}
```

**Context Flow Diagram**:

```
1. NEW CLIENT CONNECTION ARRIVES
   |
   v
listener_socket_callback() [reactor calls this]
   |
   +---> client_ctx_t *client = client_create(...)
   |     [Allocates new client context]
   |
   +---> client->socket = client_sock
   +---> client->recv_buf = buf_init(...)
   +---> client->send_buf = buf_init(...)
   +---> client->fsm = fsm_create(..., (void *)client)  <-- FSM user_data = client
   |
   +---> reactor_add_socket(r, client_sock, client_socket_callback, (void *)client)
         [Register socket with reactor, pass client context]
   |
   v
REACTOR LOOP DETECTS SOCKET EVENT
   |
   v
reactor calls: client_socket_callback(r, sock, event, status, socket_ctx_data)
   |
   socket_ctx_data = (void *)client  <-- Same context passed to reactor_add_socket()
   |
   v
client_socket_callback()
   |
   +---> client_ctx_t *client = (client_ctx_t *)socket_ctx_data
   |     [Extract context - no allocation, just casting]
   |
   +---> fsm_queue_event(client->fsm, event, status, (void *)client)
         [Queue event with same client context as event context]
   |
   +---> fsm_process_events(client->fsm)
         [Process queued events]
         |
         v
FSM DISPATCHER CALLS ACTION FUNCTION
   |
   v
client_action_read_data(fsm, current_state, event, status, next_state, user_data)
   |
   +---> user_data = (void *)client  <-- From fsm_create(..., client)
   +---> fsm_get_event_ctx(fsm) = (void *)client  <-- From fsm_queue_event(..., client)
   |
   +---> client_ctx_t *client = (client_ctx_t *)user_data
   |     [Use context to access socket, buffers, FSM, storage, etc.]
   |
   +---> socket_recv_buf(client->socket, &client->recv_buf)
   +---> buf_read_size(&client->recv_buf)
   |
   +---> fsm_queue_event(client->fsm, event, status, (void *)client)
         [Queue internal events with same context]
   |
   v
All actions use the same client_ctx_t pointer for all state data
```

**Listener Socket FSM**:

Note: The listener socket also has an FSM, created in main:

```c
// In main(), before registering listeners:

// Listener FSM transition table (for reference)
static fsm_transition_t g_listener_transitions[] = {
    {
        .current_state = LISTENER_STATE_LISTENING,
        .event = REACTOR_EVENT_CAN_ACCEPT,
        .action = listener_action_accept,
        .next_state = LISTENER_STATE_LISTENING
    },
    // ... error handling ...
};

// Create listener FSM (optional, but consistent)
listener_fsm = fsm_create(g_listener_transitions, NUM_LISTENER_TRANSITIONS,
                          LISTENER_STATE_LISTENING, 8, NULL);
// Note: FSM context is NULL for listener (no state to track)

// Then when registering listener socket:
listener_ctx_t *listener_ctx = malloc(sizeof(*listener_ctx));
snprintf(listener_ctx->endpoint_str, sizeof(listener_ctx->endpoint_str), ...);

reactor_add_socket(g_reactor, listener, listener_socket_callback, (void *)listener_ctx);
// socket_ctx_data for listener socket = (void *)listener_ctx
```

**Main Loop with Reactor**:
```c
int main(int argc, const char *argv[]) {
    // ... argument parsing (Phase 2) ...

    // Initialize socket subsystem
    util_err_t err = socket_init();
    if (err != UTIL_OK) {
        log_error("Failed to initialize socket subsystem: %s", util_err_str(err));
        return 1;
    }

    // Create register storage
    g_storage = register_storage_create(num_coils, num_discrete_inputs,
                                       num_holding_registers, num_input_registers);
    if (!g_storage) {
        log_error("Failed to create register storage");
        socket_cleanup();
        return 1;
    }

    // Create reactor
    g_reactor = reactor_create(MAX_SOCKETS);
    if (!g_reactor) {
        log_error("Failed to create reactor");
        register_storage_destroy(g_storage);
        socket_cleanup();
        return 1;
    }

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create and register listener sockets
    for (size_t i = 0; i < num_listeners; i++) {
        args_value_t val = args_get_at(&args, "listen", i);
        const char *endpoint_str = val.value.string_val;

        socket_address_t listen_addr;
        if (parse_listen_endpoint(endpoint_str, &listen_addr) != UTIL_OK) {
            continue;
        }

        socket_t listener = socket_create_tcp_server(&listen_addr, SOMAXCONN);
        if (listener == INVALID_SOCKET) {
            log_error("Failed to create listener on %s", endpoint_str);
            continue;
        }

        // Create listener context for logging
        listener_ctx_t *listener_ctx = malloc(sizeof(*listener_ctx));
        snprintf(listener_ctx->endpoint_str, sizeof(listener_ctx->endpoint_str),
                 "%s", endpoint_str);

        // Register with reactor
        err = reactor_add_socket(g_reactor, listener, listener_socket_callback, listener_ctx);
        if (err != UTIL_OK) {
            log_error("Failed to register listener %s: %s", endpoint_str, util_err_str(err));
            socket_close(listener);
            free(listener_ctx);
            continue;
        }

        log_info("Listening on %s", endpoint_str);
    }

    // Run reactor (MAIN LOOP)
    log_info("Server starting, entering main loop...");
    err = reactor_run(g_reactor, 1000);  // 1 second poll timeout for TICK events

    if (err != UTIL_OK) {
        log_error("Reactor exited with error: %s", util_err_str(err));
    } else {
        log_info("Reactor exited cleanly");
    }

    // Cleanup
    reactor_destroy(g_reactor);
    register_storage_destroy(g_storage);
    socket_cleanup();
    args_free(&args);

    log_info("Server shutdown complete");
    return 0;
}
```

---

## Phase 8: FSM State Machine Design

### Client FSM States and Events

**State Definitions** (modbus_server.c or modbus_server.h):
```c
// FSM state IDs
typedef enum {
    CLIENT_STATE_READING_HEADER = 1,    // Reading MBAP header (7 bytes)
    CLIENT_STATE_READING_BODY = 2,      // Reading PDU body
    CLIENT_STATE_PROCESSING = 3,        // Processing complete request
    CLIENT_STATE_SENDING = 4,           // Sending response
    CLIENT_STATE_CLOSING = 5,           // Closing connection
} client_state_id_t;

// Application events (in addition to REACTOR_EVENT_* from reactor.h)
enum {
    CLIENT_EVENT_HEADER_COMPLETE = 100,
    CLIENT_EVENT_BODY_COMPLETE = 101,
    CLIENT_EVENT_PROCESSING_COMPLETE = 102,
    CLIENT_EVENT_SEND_COMPLETE = 103,
};
```

**Pointer Context Flow Summary Table**:

| Component | Code | Context Pointer | Retrieval | Notes |
|-----------|------|-----------------|-----------|-------|
| **Creation** | `listener_socket_callback()` | `client = client_create(...)` | Direct allocation | Allocates new `client_ctx_t` |
| **FSM Setup** | `client_create()` | Pass to `fsm_create(..., (void *)client)` | `fsm_get_ctx(fsm)` returns `client` | FSM stores as user_data |
| **Reactor Registration** | `listener_socket_callback()` | Pass to `reactor_add_socket(..., (void *)client)` | Reactor stores internally | Same pointer passed back in callback |
| **Reactor Event** | `client_socket_callback()` received | `socket_ctx_data = (void *)client` | Function parameter | From `reactor_add_socket()` |
| **FSM Queue** | `client_socket_callback()` queues | `fsm_queue_event(..., (void *)client)` | Passed as event context | Same pointer as socket_ctx_data |
| **FSM Action** | `client_action_read_data()` | `user_data = (void *)client` | Function parameter | From `fsm_create()` or `fsm_queue_event()` |
| **Cleanup** | `client_action_cleanup()` | Cast `user_data` to access | Direct access | Destroys single allocation |

**Event Context Design**:

Events are queued with the `client_ctx_t` pointer as the event context. When FSM action functions are invoked, they receive the `client_ctx_t` from:
- `user_data` parameter (from `fsm_create(..., (void *)client)`)
- Event context parameter (from `fsm_queue_event(..., (void *)client)`)

Both are the same pointer, providing direct access to the client state. No separate event context structures needed.

**FSM Transition Table**:
```c
static fsm_transition_t g_client_transitions[] = {
    // Initial state: waiting for data
    {
        .current_state = CLIENT_STATE_READING_HEADER,
        .event = REACTOR_EVENT_CAN_READ,
        .action = client_action_read_data,
        .next_state = CLIENT_STATE_READING_HEADER  // May transition in action
    },
    {
        .current_state = CLIENT_STATE_READING_HEADER,
        .event = CLIENT_EVENT_HEADER_COMPLETE,
        .action = client_action_header_complete,
        .next_state = CLIENT_STATE_READING_BODY
    },

    // Reading body
    {
        .current_state = CLIENT_STATE_READING_BODY,
        .event = REACTOR_EVENT_CAN_READ,
        .action = client_action_read_data,
        .next_state = CLIENT_STATE_READING_BODY  // May transition in action
    },
    {
        .current_state = CLIENT_STATE_READING_BODY,
        .event = CLIENT_EVENT_BODY_COMPLETE,
        .action = client_action_body_complete,
        .next_state = CLIENT_STATE_PROCESSING
    },

    // Processing request
    {
        .current_state = CLIENT_STATE_PROCESSING,
        .event = CLIENT_EVENT_PROCESSING_COMPLETE,
        .action = client_action_processing_complete,
        .next_state = CLIENT_STATE_SENDING
    },

    // Sending response
    {
        .current_state = CLIENT_STATE_SENDING,
        .event = REACTOR_EVENT_CAN_WRITE,
        .action = client_action_send_data,
        .next_state = CLIENT_STATE_SENDING  // May transition in action
    },
    {
        .current_state = CLIENT_STATE_SENDING,
        .event = CLIENT_EVENT_SEND_COMPLETE,
        .action = client_action_send_complete,
        .next_state = CLIENT_STATE_READING_HEADER  // Ready for next request
    },

    // Error handling (any state)
    {
        .current_state = FSM_STATE_ID_ANY,
        .event = REACTOR_EVENT_ERROR,
        .action = client_action_error,
        .next_state = CLIENT_STATE_CLOSING
    },
    {
        .current_state = FSM_STATE_ID_ANY,
        .event = REACTOR_EVENT_CLOSED,
        .action = client_action_closed,
        .next_state = CLIENT_STATE_CLOSING
    },
    {
        .current_state = FSM_STATE_ID_ANY,
        .event = REACTOR_EVENT_SHUTDOWN,
        .action = client_action_shutdown,
        .next_state = CLIENT_STATE_CLOSING
    },

    // Cleanup
    {
        .current_state = CLIENT_STATE_CLOSING,
        .event = FSM_STATE_ID_ANY,  // Any event in closing state
        .action = client_action_cleanup,
        .next_state = CLIENT_STATE_CLOSING  // Stay in closing
    },
};

#define NUM_CLIENT_TRANSITIONS (sizeof(g_client_transitions) / sizeof(g_client_transitions[0]))
```

**Client Context Creation**:
```c
static client_ctx_t* client_create(socket_t sock, socket_address_t peer_addr,
                                   register_storage_t *storage) {
    client_ctx_t *client = calloc(1, sizeof(*client));
    if (!client) {
        return NULL;
    }

    client->socket = sock;
    client->recv_buf = buf_init(client->recv_data, sizeof(client->recv_data));
    client->send_buf = buf_init(client->send_data, sizeof(client->send_data));
    client->peer_addr = peer_addr;
    client->storage = storage;

    // Create FSM
    client->fsm = fsm_create(g_client_transitions, NUM_CLIENT_TRANSITIONS,
                            CLIENT_STATE_READING_HEADER,  // Initial state
                            16,  // Event queue size
                            client);  // FSM context = client

    if (!client->fsm) {
        free(client);
        return NULL;
    }

    return client;
}

static void client_destroy(client_ctx_t *client) {
    if (client) {
        if (client->fsm) {
            fsm_destroy(client->fsm);
        }
        free(client);
    }
}
```

**FSM Action Functions**:

```c
// Action: Read data from socket
static void client_action_read_data(fsm_t *fsm, fsm_state_id_t current_state,
                                   event_type_t event, util_err_t status,
                                   fsm_state_id_t next_state, void *user_data) {
    client_ctx_t *client = (client_ctx_t *)user_data;

    // Read available data
    util_err_t err = socket_recv_buf(client->socket, &client->recv_buf);

    if (err == UTIL_ECLOSED) {
        log_info("Client closed connection");
        fsm_queue_event(fsm, REACTOR_EVENT_CLOSED, UTIL_ECLOSED, (void *)client);
        return;
    }

    if (err != UTIL_OK && err != UTIL_EAGAIN) {
        log_error("Socket read error: %s", util_err_str(err));
        fsm_queue_event(fsm, REACTOR_EVENT_ERROR, err, (void *)client);
        return;
    }

    log_spew("Read data, total buffered: %zu bytes", buf_read_size(&client->recv_buf));

    // Check if we have complete data
    if (current_state == CLIENT_STATE_READING_HEADER) {
        // Need at least 7 bytes for MBAP header
        if (buf_read_size(&client->recv_buf) >= MBAP_HEADER_SIZE) {
            // Try to parse header
            buf_t checkpoint = buf_checkpoint(&client->recv_buf);

            err = modbus_parse_mbap_header(&client->recv_buf, &client->mbap_header);

            if (err == UTIL_OK) {
                log_detail("MBAP header complete, length=%u", client->mbap_header.length);
                // Calculate expected PDU bytes (length field includes unit_id)
                client->expected_pdu_length = client->mbap_header.length - 1;

                fsm_queue_event(fsm, CLIENT_EVENT_HEADER_COMPLETE, UTIL_OK, (void *)client);
                return;
            } else {
                // Parse error, send exception
                log_warn("MBAP header parse error: %s", util_err_str(err));
                buf_restore(&client->recv_buf, checkpoint);
                fsm_queue_event(fsm, REACTOR_EVENT_ERROR, err, (void *)client);
                return;
            }
        }
    } else if (current_state == CLIENT_STATE_READING_BODY) {
        // Check if we have complete PDU (MBAP already consumed, now reading PDU body)
        if (buf_read_size(&client->recv_buf) >= client->expected_pdu_length) {
            log_detail("PDU complete, %zu bytes total", buf_read_size(&client->recv_buf));
            fsm_queue_event(fsm, CLIENT_EVENT_BODY_COMPLETE, UTIL_OK, (void *)client);
            return;
        }
    }

    // Need more data, re-enable read event in reactor
    reactor_set_event_enable_mask(g_reactor, client->socket,
                                  REACTOR_EVENT_CAN_READ, true);
}

// Action: Header complete, prepare for body
static void client_action_header_complete(fsm_t *fsm, fsm_state_id_t current_state,
                                         event_type_t event, util_err_t status,
                                         fsm_state_id_t next_state, void *user_data) {
    client_ctx_t *client = (client_ctx_t *)user_data;

    log_detail("Header complete, expecting %zu PDU bytes", client->expected_pdu_length);

    // Check if body is already complete (arrived in same read)
    if (buf_read_size(&client->recv_buf) >= client->expected_pdu_length) {
        log_detail("Body already complete in buffer");
        fsm_queue_event(fsm, CLIENT_EVENT_BODY_COMPLETE, UTIL_OK, (void *)client);
    } else {
        // Need to read more, re-enable read event
        reactor_set_event_enable_mask(g_reactor, client->socket,
                                      REACTOR_EVENT_CAN_READ, true);
    }
}

// Action: Body complete, process request
static void client_action_body_complete(fsm_t *fsm, fsm_state_id_t current_state,
                                       event_type_t event, util_err_t status,
                                       fsm_state_id_t next_state, void *user_data) {
    client_ctx_t *client = (client_ctx_t *)user_data;

    log_detail("Processing request...");
    log_bytes_spew(&client->recv_buf);

    // Parse function code from PDU
    uint8_t function_code;
    if (!buf_read_u8(&client->recv_buf, "function_code", &function_code)) {
        log_error("Failed to read function code");
        build_exception_response(&client->send_buf, &client->mbap_header,
                                0, UTIL_EINVAL);
        buf_reset(&client->recv_buf);
        fsm_queue_event(fsm, CLIENT_EVENT_PROCESSING_COMPLETE, UTIL_OK, (void *)client);
        return;
    }

    // Process request based on function code
    util_err_t err = modbus_process_request(function_code, &client->recv_buf,
                                           &client->send_buf, &client->mbap_header,
                                           client->storage);

    if (err != UTIL_OK) {
        log_warn("Request processing failed: %s", util_err_str(err));
        build_exception_response(&client->send_buf, &client->mbap_header,
                                function_code, err);
    }

    // Clear receive buffer for next request
    buf_reset(&client->recv_buf);

    log_bytes_spew(&client->send_buf);

    // Queue processing complete event
    fsm_queue_event(fsm, CLIENT_EVENT_PROCESSING_COMPLETE, UTIL_OK, (void *)client);
}

// Action: Processing complete, prepare to send
static void client_action_processing_complete(fsm_t *fsm, fsm_state_id_t current_state,
                                              event_type_t event, util_err_t status,
                                              fsm_state_id_t next_state, void *user_data) {
    client_ctx_t *client = (client_ctx_t *)user_data;

    log_detail("Ready to send response (%zu bytes)", buf_read_size(&client->send_buf));

    // Enable write event in reactor
    reactor_set_event_enable_mask(g_reactor, client->socket,
                                  REACTOR_EVENT_CAN_WRITE, true);
}

// Action: Send data to socket
static void client_action_send_data(fsm_t *fsm, fsm_state_id_t current_state,
                                   event_type_t event, util_err_t status,
                                   fsm_state_id_t next_state, void *user_data) {
    client_ctx_t *client = (client_ctx_t *)user_data;

    util_err_t err = socket_send_buf(client->socket, &client->send_buf);

    if (err == UTIL_EAGAIN) {
        // Partial write, re-enable write event for next cycle
        reactor_set_event_enable_mask(g_reactor, client->socket,
                                      REACTOR_EVENT_CAN_WRITE, true);
        return;
    }

    if (err != UTIL_OK) {
        log_error("Socket write error: %s", util_err_str(err));
        fsm_queue_event(fsm, REACTOR_EVENT_ERROR, err, (void *)client);
        return;
    }

    // Check if send complete
    if (buf_read_size(&client->send_buf) == 0) {
        log_detail("Response sent completely");
        fsm_queue_event(fsm, CLIENT_EVENT_SEND_COMPLETE, UTIL_OK, (void *)client);
    } else {
        // More data to send, re-enable write event
        reactor_set_event_enable_mask(g_reactor, client->socket,
                                      REACTOR_EVENT_CAN_WRITE, true);
    }
}

// Action: Send complete, prepare for next request
static void client_action_send_complete(fsm_t *fsm, fsm_state_id_t current_state,
                                       event_type_t event, util_err_t status,
                                       fsm_state_id_t next_state, void *user_data) {
    client_ctx_t *client = (client_ctx_t *)user_data;

    log_detail("Response sent, ready for next request");

    // Reset send buffer
    buf_reset(&client->send_buf);

    // Re-enable read for next request
    reactor_set_event_enable_mask(g_reactor, client->socket,
                                  REACTOR_EVENT_CAN_READ, true);
}

// Action: Error occurred
static void client_action_error(fsm_t *fsm, fsm_state_id_t current_state,
                               event_type_t event, util_err_t status,
                               fsm_state_id_t next_state, void *user_data) {
    client_ctx_t *client = (client_ctx_t *)user_data;

    log_error("Client error in state %u: %s", current_state, util_err_str(status));

    // Transition to cleanup will be handled by next state transition
}

// Action: Connection closed
static void client_action_closed(fsm_t *fsm, fsm_state_id_t current_state,
                                event_type_t event, util_err_t status,
                                fsm_state_id_t next_state, void *user_data) {
    client_ctx_t *client = (client_ctx_t *)user_data;

    char addr_str[64];
    socket_address_get_addr_str(&client->peer_addr, addr_str, sizeof(addr_str));
    log_info("Client %s:%u closed connection",
             addr_str, socket_address_get_port(&client->peer_addr));

    // Cleanup will happen in CLIENT_STATE_CLOSING via client_action_cleanup
}

// Action: Shutdown requested
static void client_action_shutdown(fsm_t *fsm, fsm_state_id_t current_state,
                                  event_type_t event, util_err_t status,
                                  fsm_state_id_t next_state, void *user_data) {
    client_ctx_t *client = (client_ctx_t *)user_data;

    log_info("Client shutdown requested");

    // Cleanup will happen in CLIENT_STATE_CLOSING via client_action_cleanup
}

// Action: Cleanup resources
static void client_action_cleanup(fsm_t *fsm, fsm_state_id_t current_state,
                                 event_type_t event, util_err_t status,
                                 fsm_state_id_t next_state, void *user_data) {
    client_ctx_t *client = (client_ctx_t *)user_data;

    log_detail("Cleaning up client resources");

    // Remove from reactor
    reactor_remove_socket(g_reactor, client->socket);

    // Close socket
    socket_close(client->socket);

    // Destroy client context (including FSM)
    client_destroy(client);
}
```

**Event Context and Pointer Flow Summary**:

The same `client_ctx_t` pointer flows through the entire system without re-allocation:

```
CREATION (listener_socket_callback):
   client_ctx_t *client = client_create(...)
   client->fsm = fsm_create(..., (void *)client)  // FSM stores client as user_data
   reactor_add_socket(..., client_socket_callback, (void *)client)  // Reactor stores client

REACTOR EVENT (reactor loop):
   client_socket_callback(..., socket_ctx_data)
   // socket_ctx_data == (void *)client (same pointer from reactor_add_socket)

FSMEVENT QUEUE:
   fsm_queue_event(client->fsm, event, status, (void *)client)
   // Pass same client pointer as event context

FSM ACTION EXECUTION:
   client_action_read_data(..., user_data)
   // user_data == (void *)client (from fsm_create)
   // fsm_get_event_ctx() == (void *)client (from fsm_queue_event)
   // Both are the same object

INTERNAL EVENT QUEUE:
   fsm_queue_event(client->fsm, event, status, (void *)client)
   // Queue more events with same client context
```

**Key Points**:

1. **Single Allocation**: `client_ctx_t` is allocated once in `listener_socket_callback()` when client connects
2. **No Re-allocation**: The pointer is passed to reactor and FSM, never duplicated
3. **Pointer Consistency**: The same `(void *)client` is used:
   - As `fsm_create()` user_data
   - As `reactor_add_socket()` socket context
   - As `fsm_queue_event()` event context
4. **No Memory Leaks**: Single allocation means single cleanup in `client_action_cleanup()`
5. **Efficient Access**: Action functions can access all state through one pointer:
   - `client->socket` for I/O
   - `client->recv_buf`, `client->send_buf` for buffers
   - `client->fsm` to queue events
   - `client->storage` to access registers
   - `client->peer_addr` for client info

---

## Phase 9: Protocol Processing Updates

### Update `modbus_protocol.c` Function Signatures

**Old Signatures**:
```c
int modbus_process_request(const uint8_t *request, int request_len,
                           uint8_t *response, int *response_len,
                           register_storage_t *storage);
```

**New Signatures**:
```c
util_err_t modbus_process_request(uint8_t function_code,
                                  buf_t *request, buf_t *response,
                                  const mbap_header_t *req_header,
                                  register_storage_t *storage);
```

**Dispatcher Function**:
```c
util_err_t modbus_process_request(uint8_t function_code,
                                  buf_t *request, buf_t *response,
                                  const mbap_header_t *req_header,
                                  register_storage_t *storage) {
    util_err_t err;

    // Dispatch to handler based on function code
    switch (function_code) {
        case 0x01:  // Read Coils
            err = handle_read_coils(request, response, req_header, storage);
            break;
        case 0x02:  // Read Discrete Inputs
            err = handle_read_discrete_inputs(request, response, req_header, storage);
            break;
        case 0x03:  // Read Holding Registers
            err = handle_read_holding_registers(request, response, req_header, storage);
            break;
        case 0x04:  // Read Input Registers
            err = handle_read_input_registers(request, response, req_header, storage);
            break;
        case 0x05:  // Write Single Coil
            err = handle_write_single_coil(request, response, req_header, storage);
            break;
        case 0x06:  // Write Single Register
            err = handle_write_single_register(request, response, req_header, storage);
            break;
        case 0x0F:  // Write Multiple Coils
            err = handle_write_multiple_coils(request, response, req_header, storage);
            break;
        case 0x10:  // Write Multiple Registers
            err = handle_write_multiple_registers(request, response, req_header, storage);
            break;
        default:
            log_warn("Unsupported function code: 0x%02X", function_code);
            err = UTIL_ENOTSUPPORTED;
    }

    // Build response or exception
    if (err != UTIL_OK) {
        build_exception_response(response, req_header, function_code, err);
        return UTIL_OK;  // Exception response is still a valid response
    }

    return UTIL_OK;
}
```

**Example Handler** (Read Holding Registers):
```c
static util_err_t handle_read_holding_registers(buf_t *request, buf_t *response,
                                                const mbap_header_t *req_header,
                                                register_storage_t *storage) {
    uint16_t start_address, quantity;

    // Parse request
    bool ok = true;
    ok &= buf_read_u16_be(request, "start_address", &start_address);
    ok &= buf_read_u16_be(request, "quantity", &quantity);

    if (!ok) {
        return buf_get_error(request);
    }

    log_detail("Read Holding Registers: addr=%u, qty=%u", start_address, quantity);

    // Validate
    if (quantity < 1 || quantity > 125) {
        return UTIL_EINVAL;
    }

    if (start_address + quantity > storage->num_holding_registers) {
        return UTIL_EBOUNDS;
    }

    // Build response header
    util_err_t err = build_response_header(response, req_header,
                                          2 + quantity * 2);  // FC + byte_count + data
    if (err != UTIL_OK) {
        return err;
    }

    // Write function code
    ok &= buf_write_u8(response, "function_code", 0x03);

    // Write byte count
    uint8_t byte_count = quantity * 2;
    ok &= buf_write_u8(response, "byte_count", byte_count);

    // Write register values (big-endian)
    for (uint16_t i = 0; i < quantity; i++) {
        uint16_t reg_value = storage->holding_registers[start_address + i];
        ok &= buf_write_u16_be(response, "register_value", reg_value);
    }

    if (!ok) {
        return buf_get_error(response);
    }

    return UTIL_OK;
}
```

---

## Phase 10: Build System Updates

### Update Makefile or CMakeLists.txt

**Remove Old Object Files**:
```makefile
# Remove these:
# config.o
# logger.o
# socket_utils.o
```

**Add New Dependencies**:
```makefile
# Utility library path
UTILS_DIR = ../utils

# Include paths
INCLUDES = -I$(UTILS_DIR)

# Link against utility libraries
LIBS = -L$(UTILS_DIR) -lutils

# Object files
OBJS = modbus_server.o \
       modbus_protocol.o \
       register_storage.o

# Build
modbus_server: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

modbus_server.o: modbus_server.c modbus_bitarray.h
	$(CC) $(CFLAGS) $(INCLUDES) -c modbus_server.c

modbus_protocol.o: modbus_protocol.c modbus_protocol.h
	$(CC) $(CFLAGS) $(INCLUDES) -c modbus_protocol.c

register_storage.o: register_storage.c register_storage.h modbus_bitarray.h
	$(CC) $(CFLAGS) $(INCLUDES) -c register_storage.c
```

---

## Phase 11: Testing Strategy

### Validation Steps

1. **Compilation**
   - Ensure all old includes are removed
   - Verify all new utility headers are included
   - Check for type mismatches (util_err_t, buf_t, etc.)
   - Confirm no warnings

2. **Basic Connectivity**
   - Start server with `--listen 127.0.0.1:5502 --debug`
   - Connect with telnet or netcat
   - Verify reactor accepts connection
   - Check FSM transitions in debug logs

3. **Protocol Testing**
   - Use existing test client or pymodbus
   - Test each function code (0x01-0x06, 0x0F, 0x10)
   - Verify correct responses
   - Test exception handling (invalid addresses, quantities, etc.)

4. **Load Testing**
   - Multiple concurrent clients
   - Rapid connect/disconnect cycles
   - Large register reads/writes
   - Monitor for memory leaks (valgrind)

5. **Edge Cases**
   - Partial reads (small recv buffer, slow network)
   - Partial writes (small send buffer)
   - Client disconnects mid-request
   - Invalid protocol ID, length fields
   - Malformed PDUs

---

## Summary

This refactoring plan transforms the Modbus TCP server into a modern, declarative, event-driven architecture:

- **Args**: Declarative flag definitions replace custom parsing
- **Buffers**: Cursor-based `buf_t` with automatic bounds checking
- **Bit Arrays**: Custom `modbus_bitarray_t` for variable-length coil storage
- **Errors**: Unified `util_err_t` with Modbus exception mapping
- **FSM**: Declarative state machine with transition tables
- **Logging**: Level-based filtering with automatic timestamps
- **Reactor**: `reactor_run()` as main loop, event-driven callbacks
- **Sockets**: Cross-platform `socket_t` with async I/O

The architecture is cleaner, more maintainable, and more testable. The protocol logic is explicit in the FSM transition table, making it easy to audit and extend.
