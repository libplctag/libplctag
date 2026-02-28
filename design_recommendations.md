# Design Recommendations for Minimal Industrial Protocol Stack Library

**Date:** 2026-01-21
**Project:** Small code library for industrial network protocols
**Based on:** Analysis of `smaller_code.md` and existing `src/tests/utils` code

---

## Executive Summary

The proposed library design solves two critical problems:

1. **File descriptor exhaustion** - Current approach uses 3 FDs per connection (1 data + 2 wake pipes). With macOS's 256 FD limit, you can only handle ~85 connections. The event loop design reduces this to 1 FD per connection + 2 FDs total for the shared wake mechanism.

2. **State machine boilerplate** - Callback-based async I/O forces every protocol implementation to write 100-200 lines of explicit state machine code. The coroutine-based blocking-style API eliminates this entirely.

**Recommendation:** Proceed with the event loop + coroutine + buffer API design with the refinements detailed below.

---

## Current State Analysis

### What Works Well

#### 1. Buffer API (`src/tests/utils/buf.h`)
**Status:** ✅ Excellent, use as-is with minor additions

The existing buffer implementation is exemplary:
- Embedded error state allows operation chaining
- Bounds checking is automatic, not pushed to caller
- Field names in error reporting aid debugging
- Checkpoint/restore enables transactional parsing
- Zero allocations (wraps existing memory)
- ~560 lines total for complete implementation

**No major changes needed.**

#### 2. Error Type System (`src/tests/utils/err.h`)
**Status:** ✅ Good foundation

Clear error taxonomy with platform-specific conversion functions.

**Minor additions needed** (see recommendations).

#### 3. Coroutine Event Loop (`src/tests/utils/coro_net.h`)
**Status:** ✅ Core design is sound, needs simplification

Key innovations:
- Shared wake socketpair (solves FD limit problem)
- Stackless coroutines via Duff's Device (minimal overhead)
- Struct-of-arrays for cache efficiency

**Needs refinement** (see recommendations).

### What Needs Work

#### 1. Thread Complexity
**Issue:** Current design requires event loop to run in its own thread with `coro_run()` blocking forever.

**Problem:** This is overkill for many use cases and makes integration harder.

#### 2. Missing I/O Helpers
**Issue:** Protocol implementers still need to handle partial reads/writes manually.

**Problem:** Common operations like "read exactly N bytes" require boilerplate in every protocol.

#### 3. API Surface Unclear
**Issue:** The sketch in `smaller_code.md` has unresolved questions about socket creation, buffer constraints, etc.

**Problem:** Implementers won't know which abstractions to use.

---

## Detailed Recommendations

### 1. Event Loop API Refinements

#### Change: Make Threading Optional

**Current (from `coro_net.h`):**
```c
util_err_t coro_run(coro_net_t **coro_net, uint32_t tick_interval_ms);
```

**Proposed:**
```c
// Single iteration - app controls the loop
util_err_t ev_loop_poll(ev_loop_t *loop, uint32_t timeout_ms);

// Optional: run until stopped (for apps that want it)
util_err_t ev_loop_run(ev_loop_t *loop, uint32_t tick_interval_ms);
```

**Rationale:**
- Simple apps can call `ev_loop_run()` and it behaves like current design
- Advanced apps can call `ev_loop_poll()` in their own main loop
- No behavioral change, just adds flexibility
- ~10 lines of code (`ev_loop_run` just loops calling `ev_loop_poll`)

**Example Usage:**
```c
// Simple usage
ev_loop_run(loop, 100);

// Advanced usage with custom main loop
while (running) {
    ev_loop_poll(loop, 100);
    check_application_events();
    update_statistics();
}
```

#### Change: Simplify Naming

**Current:**
```c
coro_net_t
coro_create()
coro_destroy()
coro_add_task()
```

**Proposed:**
```c
ev_loop_t
ev_loop_create()
ev_loop_destroy()
ev_loop_add_task()
```

**Rationale:**
- "ev_loop" is clearer than "coro" about what it does
- Consistent prefix helps with API documentation
- Users might not care it's implemented with coroutines

---

### 2. Add High-Level I/O Helpers

#### Addition: Blocking-Style Socket I/O

**Add to library:**
```c
/**
 * @brief Read exactly n bytes into buffer, yielding until complete.
 *
 * This handles partial reads, EAGAIN, and buffer management automatically.
 * On error or connection close, sets task error state.
 *
 * @param task Coroutine task handle
 * @param sock Socket to read from
 * @param buf Buffer to read into (must have space for n bytes)
 * @param n Number of bytes to read
 */
#define ev_recv_exact(task, sock, buf, n) \
    do { \
        size_t __ev_total_needed = (n); \
        util_err_t __ev_err = UTIL_OK; \
        while (buf_read_size(buf) < __ev_total_needed && __ev_err == UTIL_OK) { \
            coro_yield(task, CORO_EVENT_READ); \
            __ev_err = ev_socket_recv_append(coro_get_fd(task), buf); \
        } \
        if (__ev_err != UTIL_OK) { \
            ev_task_set_error(task, __ev_err); \
        } \
    } while(0)

/**
 * @brief Send all bytes from buffer, yielding until complete.
 *
 * Handles partial writes and EAGAIN automatically.
 *
 * @param task Coroutine task handle
 * @param sock Socket to write to
 * @param buf Buffer containing data to send
 */
#define ev_send_all(task, sock, buf) \
    do { \
        util_err_t __ev_err = UTIL_OK; \
        while (buf_read_size(buf) > 0 && __ev_err == UTIL_OK) { \
            coro_yield(task, CORO_EVENT_WRITE); \
            ssize_t __ev_sent = ev_socket_send_from_buf(coro_get_fd(task), buf); \
            if (__ev_sent < 0) { \
                __ev_err = util_err_from_errno(errno); \
                ev_task_set_error(task, __ev_err); \
            } else { \
                buf_read_advance(buf, __ev_sent); \
            } \
        } \
    } while(0)

/**
 * @brief Read until buffer has at least n bytes, yielding as needed.
 *
 * Unlike ev_recv_exact, this stops as soon as n bytes are available,
 * even if more data arrives. Useful for reading headers before knowing
 * full message length.
 *
 * @param task Coroutine task handle
 * @param sock Socket to read from
 * @param buf Buffer to read into
 * @param n Minimum bytes to read
 */
#define ev_recv_atleast(task, sock, buf, n) \
    do { \
        size_t __ev_min_needed = (n); \
        util_err_t __ev_err = UTIL_OK; \
        while (buf_read_size(buf) < __ev_min_needed && __ev_err == UTIL_OK) { \
            coro_yield(task, CORO_EVENT_READ); \
            __ev_err = ev_socket_recv_append(coro_get_fd(task), buf); \
        } \
        if (__ev_err != UTIL_OK) { \
            ev_task_set_error(task, __ev_err); \
        } \
    } while(0)
```

**Supporting Functions:**
```c
/**
 * @brief Attempt to receive data and append to buffer.
 *
 * Non-blocking receive that appends to the buffer's write position.
 * Updates buffer write cursor on success.
 *
 * @param sock Socket to read from
 * @param buf Buffer to append to
 * @return UTIL_OK on success, UTIL_EAGAIN if would block,
 *         UTIL_ECLOSED if connection closed, other error codes on failure
 */
util_err_t ev_socket_recv_append(socket_t sock, buf_t *buf);

/**
 * @brief Attempt to send data from buffer.
 *
 * Non-blocking send from the buffer's read position.
 * Does NOT update buffer read cursor (caller must do this).
 *
 * @param sock Socket to write to
 * @param buf Buffer to send from
 * @return Number of bytes sent on success, -1 on error
 */
ssize_t ev_socket_send_from_buf(socket_t sock, const buf_t *buf);

/**
 * @brief Set error state on a task.
 *
 * Tasks can check this later to handle errors.
 *
 * @param task Task handle
 * @param err Error code to set
 */
void ev_task_set_error(coro_task_handle_t task, util_err_t err);

/**
 * @brief Get error state from a task.
 *
 * @param task Task handle
 * @return Current error code
 */
util_err_t ev_task_get_error(coro_task_handle_t task);
```

**Before (protocol code with manual partial I/O):**
```c
void handle_modbus(coro_task_handle_t task, socket_t sock, void *ctx) {
    CORO_START(task);

    connection_t *conn = ctx;
    ssize_t n;

    while (1) {
        // Manually handle partial reads for header
        while (buf_read_size(&conn->recv_buf) < 7) {
            coro_yield(task, CORO_EVENT_READ);
            n = recv(sock, buf_write_ptr(&conn->recv_buf),
                     buf_write_size(&conn->recv_buf), MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EAGAIN) continue;
                // error handling...
            }
            if (n == 0) { /* connection closed */ }
            buf_write_advance(&conn->recv_buf, n);
        }

        uint16_t length;
        buf_read_u16_be(&conn->recv_buf, "length", &length);

        // Manually handle partial reads for body
        while (buf_read_size(&conn->recv_buf) < length) {
            coro_yield(task, CORO_EVENT_READ);
            n = recv(sock, buf_write_ptr(&conn->recv_buf),
                     buf_write_size(&conn->recv_buf), MSG_DONTWAIT);
            // ... same error handling ...
        }

        process_request(&conn->recv_buf, &conn->send_buf);

        // Manually handle partial writes
        while (buf_read_size(&conn->send_buf) > 0) {
            coro_yield(task, CORO_EVENT_WRITE);
            n = send(sock, buf_read_ptr(&conn->send_buf),
                     buf_read_size(&conn->send_buf), MSG_DONTWAIT);
            // ... error handling ...
            buf_read_advance(&conn->send_buf, n);
        }

        buf_reset(&conn->recv_buf);
        buf_reset(&conn->send_buf);
    }

    CORO_END(task);
}
```

**After (with helpers):**
```c
void handle_modbus(coro_task_handle_t task, socket_t sock, void *ctx) {
    CORO_START(task);

    connection_t *conn = ctx;

    while (1) {
        ev_recv_exact(task, sock, &conn->recv_buf, 7);

        uint16_t length;
        buf_read_u16_be(&conn->recv_buf, "length", &length);

        ev_recv_exact(task, sock, &conn->recv_buf, length);

        process_request(&conn->recv_buf, &conn->send_buf);

        ev_send_all(task, sock, &conn->send_buf);

        buf_reset(&conn->recv_buf);
        buf_reset(&conn->send_buf);
    }

    CORO_END(task);
}
```

**Savings:** ~30-40 lines of boilerplate eliminated per protocol handler.

**Code Size:** ~80 lines added to library (once), saves 30-40 lines per protocol.

---

### 3. Socket API Decisions

#### Recommendation: Minimal Socket Wrapper

**From `smaller_code.md`:**
> Q: Should this be higher level?
> Q: Where do we set the amount of space for receiving and sending buffers?
> Q: should we even have these? There is not a lot of gain over standard BSD socket operations.
> Q: if we use plain BSD socket functions, how do we ensure that the sockets are non-blocking, nodelay, reuseaddr etc.?

**Answer: Provide minimal configuration helper, not creation wrappers**

```c
/**
 * @brief Socket configuration options.
 */
typedef struct {
    bool non_blocking;   // Set O_NONBLOCK / FIONBIO
    bool no_delay;       // Set TCP_NODELAY (disable Nagle)
    bool reuse_addr;     // Set SO_REUSEADDR
    bool reuse_port;     // Set SO_REUSEPORT (if available)
    bool keep_alive;     // Set SO_KEEPALIVE
    int send_buffer_kb;  // SO_SNDBUF size in KB (0 = don't set)
    int recv_buffer_kb;  // SO_RCVBUF size in KB (0 = don't set)
} ev_socket_opts_t;

// Default options for protocol sockets
#define EV_SOCKET_OPTS_DEFAULT { \
    .non_blocking = true, \
    .no_delay = true, \
    .reuse_addr = true, \
    .reuse_port = false, \
    .keep_alive = false, \
    .send_buffer_kb = 0, \
    .recv_buffer_kb = 0 \
}

/**
 * @brief Configure a socket with common options.
 *
 * This handles platform differences (fcntl vs ioctlsocket, etc).
 *
 * @param sock Socket to configure
 * @param opts Configuration options
 * @return UTIL_OK on success, error code on failure
 */
util_err_t ev_socket_configure(socket_t sock, const ev_socket_opts_t *opts);

/**
 * @brief Create and configure a TCP client socket.
 *
 * Convenience function that creates socket, configures it, and initiates
 * non-blocking connect. Check connection status with ev_socket_check_connect.
 *
 * @param host Hostname or IP address
 * @param port Port number
 * @param opts Socket options (NULL for defaults)
 * @param out_sock Receives created socket on success
 * @return UTIL_OK if connect initiated, error code on failure
 */
util_err_t ev_socket_create_tcp_client(const char *host, uint16_t port,
                                        const ev_socket_opts_t *opts,
                                        socket_t *out_sock);

/**
 * @brief Create and configure a TCP server socket.
 *
 * Creates socket, configures, binds, and listens.
 *
 * @param bind_addr Address to bind to ("0.0.0.0" for all interfaces)
 * @param port Port to listen on
 * @param backlog Listen backlog
 * @param opts Socket options (NULL for defaults)
 * @param out_sock Receives created socket on success
 * @return UTIL_OK on success, error code on failure
 */
util_err_t ev_socket_create_tcp_server(const char *bind_addr, uint16_t port,
                                        int backlog,
                                        const ev_socket_opts_t *opts,
                                        socket_t *out_sock);

/**
 * @brief Create and configure a UDP socket.
 *
 * @param bind_addr Address to bind to (NULL for any)
 * @param port Port to bind to (0 for any)
 * @param enable_broadcast Enable SO_BROADCAST
 * @param opts Socket options (NULL for defaults)
 * @param out_sock Receives created socket on success
 * @return UTIL_OK on success, error code on failure
 */
util_err_t ev_socket_create_udp(const char *bind_addr, uint16_t port,
                                 bool enable_broadcast,
                                 const ev_socket_opts_t *opts,
                                 socket_t *out_sock);
```

**Rationale:**
- Apps can still use raw BSD sockets if they want control
- Configuration helper eliminates platform #ifdefs in app code
- Creation helpers are optional conveniences
- ~150 lines of platform-specific code (handles Windows/POSIX differences)

---

### 4. Buffer API Additions

#### Addition: Float Support

**Add to `buf.h`:**
```c
/* Float reads - converts from network byte order */
bool buf_read_f32_be(buf_t *b, const char *field_name, float *out);
bool buf_read_f32_le(buf_t *b, const char *field_name, float *out);
bool buf_read_f64_be(buf_t *b, const char *field_name, double *out);
bool buf_read_f64_le(buf_t *b, const char *field_name, double *out);

/* Float writes - converts to network byte order */
bool buf_write_f32_be(buf_t *b, const char *field_name, float v);
bool buf_write_f32_le(buf_t *b, const char *field_name, float v);
bool buf_write_f64_be(buf_t *b, const char *field_name, double v);
bool buf_write_f64_le(buf_t *b, const char *field_name, double v);
```

**Implementation Note:**
```c
bool buf_read_f32_be(buf_t *b, const char *field_name, float *out) {
    uint32_t bits;
    if (!buf_read_u32_be(b, field_name, &bits)) return false;
    memcpy(out, &bits, sizeof(float));  // Type-punning via memcpy
    return true;
}
```

**Rationale:**
- Industrial protocols commonly use floating point (sensor data, etc.)
- ~40 lines of code
- Type-punning via memcpy avoids strict aliasing violations

#### Addition: String Helpers

**Add to `buf.h`:**
```c
/**
 * @brief Read a null-terminated string from buffer.
 *
 * Reads until null byte or max_len reached.
 *
 * @param b Buffer to read from
 * @param field_name Field name for error reporting
 * @param out Output buffer (receives null-terminated string)
 * @param max_len Maximum bytes to read (including null terminator)
 * @return true on success, false on error
 */
bool buf_read_cstring(buf_t *b, const char *field_name,
                      char *out, size_t max_len);

/**
 * @brief Read a fixed-length string (may not be null-terminated).
 *
 * Reads exactly len bytes and null-terminates output.
 *
 * @param b Buffer to read from
 * @param field_name Field name for error reporting
 * @param out Output buffer (receives null-terminated string)
 * @param len Number of bytes to read from buffer
 * @param out_size Size of output buffer (must be >= len + 1)
 * @return true on success, false on error
 */
bool buf_read_fixed_string(buf_t *b, const char *field_name,
                           char *out, size_t len, size_t out_size);

/**
 * @brief Write a null-terminated string to buffer.
 *
 * Writes string including null terminator.
 *
 * @param b Buffer to write to
 * @param field_name Field name for error reporting
 * @param str String to write
 * @return true on success, false on error
 */
bool buf_write_cstring(buf_t *b, const char *field_name, const char *str);

/**
 * @brief Write a fixed-length string, padding or truncating as needed.
 *
 * If str is shorter than len, pads with zeros.
 * If str is longer than len, truncates.
 *
 * @param b Buffer to write to
 * @param field_name Field name for error reporting
 * @param str String to write
 * @param len Fixed length to write
 * @return true on success, false on error
 */
bool buf_write_fixed_string(buf_t *b, const char *field_name,
                            const char *str, size_t len);
```

**Rationale:**
- Many industrial protocols use fixed-length string fields
- ~60 lines of code
- Handles padding/truncation safely

---

### 5. Error Handling Clarification

#### Change: Remove Thread-Local Status

**From `smaller_code.md`:**
```c
extern _Thread ev_status_t ev_status;
```

**Problem:** `_Thread_local` is C23, not widely supported yet in embedded toolchains.

**Recommendation:** Use error-in-object pattern consistently

**Already done correctly:**
- `buf_t` has embedded error
- `coro_task_handle_t` can have embedded error (add if missing)

**For one-shot operations that don't have a context object:**
```c
// Just return the error
util_err_t err = ev_socket_configure(sock, &opts);
if (err != UTIL_OK) {
    // handle error
}
```

**No thread-local needed.**

---

### 6. Logging/Debugging Support

#### Addition: Optional Debug Logging

**Add to library:**
```c
/**
 * @brief Log levels for event loop debugging.
 */
typedef enum {
    EV_LOG_NONE = 0,
    EV_LOG_ERROR,
    EV_LOG_WARN,
    EV_LOG_INFO,
    EV_LOG_DEBUG,
    EV_LOG_TRACE
} ev_log_level_t;

/**
 * @brief Log callback function type.
 *
 * @param level Log level
 * @param file Source file name
 * @param line Source line number
 * @param func Function name
 * @param fmt Printf-style format string
 * @param args Variable arguments
 */
typedef void (*ev_log_fn_t)(ev_log_level_t level,
                            const char *file, int line, const char *func,
                            const char *fmt, va_list args);

/**
 * @brief Set global logging function and level.
 *
 * @param log_fn Logging function (NULL to disable logging)
 * @param level Minimum level to log
 */
void ev_log_set(ev_log_fn_t log_fn, ev_log_level_t level);

/**
 * @brief Get current log level.
 */
ev_log_level_t ev_log_get_level(void);
```

**Internal Usage:**
```c
#define EV_LOG(level, ...) \
    do { \
        if (ev_log_get_level() >= level) { \
            ev_log_impl(level, __FILE__, __LINE__, __func__, __VA_ARGS__); \
        } \
    } while(0)
```

**Rationale:**
- Debug logging is essential for protocol development
- Callback approach lets app integrate with its own logging
- Zero overhead when disabled (compiler optimizes out if level check fails)
- ~50 lines of code

#### Addition: Buffer Dump Helper

**Add to `buf.h`:**
```c
/**
 * @brief Dump buffer contents in hex format.
 *
 * Useful for debugging protocol issues.
 *
 * @param b Buffer to dump
 * @param callback Called for each line of output
 * @param ctx User context passed to callback
 */
typedef void (*buf_dump_line_fn_t)(const char *line, void *ctx);

void buf_dump_hex(const buf_t *b, buf_dump_line_fn_t callback, void *ctx);

/**
 * @brief Dump buffer to file (convenience wrapper).
 */
void buf_dump_hex_to_file(const buf_t *b, FILE *fp);
```

**Example Output:**
```
0000: 00 01 00 00 00 06 01 03 00 00 00 0A  ............
0012: 00 00 00 00                          ....
```

**Rationale:**
- Essential for debugging binary protocols
- ~40 lines of code
- Already exists in `src/tests/utils/log.h`, just needs to be extracted

---

### 7. Documentation Structure

#### Recommendation: Separate Headers by Concern

**Proposed file structure:**
```
include/
  ev/
    loop.h         // Event loop core
    socket.h       // Socket helpers
    buf.h          // Buffer operations
    err.h          // Error codes
    log.h          // Logging (optional)

src/
  loop.c
  socket.c
  buf.c
  err.c
  log.c
```

**Master header for convenience:**
```c
// ev.h - includes everything
#include <ev/loop.h>
#include <ev/socket.h>
#include <ev/buf.h>
#include <ev/err.h>
```

**Rationale:**
- Apps can include just what they need
- Clearer separation of concerns
- Easier to document each module

---

## Proposed API Summary

### Core Event Loop (`ev/loop.h`)

```c
// Types
typedef struct ev_loop_s ev_loop_t;
typedef struct ev_task_handle_s {
    ev_loop_t *loop;
    int index;
} ev_task_handle_t;

// Event constants
#define EV_EVENT_NONE   0
#define EV_EVENT_READ   POLLIN
#define EV_EVENT_WRITE  POLLOUT
#define EV_EVENT_ALWAYS ((short)-1)

// Task handler callback
typedef void (*ev_task_fn_t)(ev_task_handle_t task, socket_t fd, void *ctx);

// Lifecycle
ev_loop_t *ev_loop_create(size_t max_tasks);
void ev_loop_destroy(ev_loop_t **loop);

// Running
util_err_t ev_loop_poll(ev_loop_t *loop, uint32_t timeout_ms);
util_err_t ev_loop_run(ev_loop_t *loop, uint32_t tick_interval_ms);
util_err_t ev_loop_stop(ev_loop_t *loop);

// Task management
util_err_t ev_loop_add_task(ev_task_handle_t *task, ev_loop_t *loop,
                            socket_t fd, ev_task_fn_t handler, void *ctx);
void ev_loop_remove_task(ev_task_handle_t task);

// Coroutine support (macros)
#define EV_TASK_START(task) switch(ev_task_get_line(task)) { case 0:
#define ev_yield(task, event) \
    do { \
        ev_task_set_line((task), __LINE__); \
        ev_task_set_event((task), (event)); \
        return; \
        case __LINE__:; \
    } while(0)
#define EV_TASK_END(task) default: break; } ev_loop_remove_task(task);

// I/O helpers (macros)
#define ev_recv_exact(task, sock, buf, n) /* ... */
#define ev_recv_atleast(task, sock, buf, n) /* ... */
#define ev_send_all(task, sock, buf) /* ... */

// Task accessors
int ev_task_get_line(ev_task_handle_t task);
util_err_t ev_task_set_line(ev_task_handle_t task, int line);
util_err_t ev_task_set_event(ev_task_handle_t task, short event);
socket_t ev_task_get_fd(ev_task_handle_t task);
void ev_task_set_error(ev_task_handle_t task, util_err_t err);
util_err_t ev_task_get_error(ev_task_handle_t task);
```

### Socket Helpers (`ev/socket.h`)

```c
// Platform-independent socket type
#ifdef _WIN32
    typedef SOCKET socket_t;
    #define INVALID_SOCKET_FD INVALID_SOCKET
#else
    typedef int socket_t;
    #define INVALID_SOCKET_FD (-1)
#endif

// Configuration
typedef struct {
    bool non_blocking;
    bool no_delay;
    bool reuse_addr;
    bool reuse_port;
    bool keep_alive;
    int send_buffer_kb;
    int recv_buffer_kb;
} ev_socket_opts_t;

#define EV_SOCKET_OPTS_DEFAULT { /* ... */ }

util_err_t ev_socket_configure(socket_t sock, const ev_socket_opts_t *opts);

// Creation helpers
util_err_t ev_socket_create_tcp_client(const char *host, uint16_t port,
                                        const ev_socket_opts_t *opts,
                                        socket_t *out_sock);
util_err_t ev_socket_create_tcp_server(const char *bind_addr, uint16_t port,
                                        int backlog,
                                        const ev_socket_opts_t *opts,
                                        socket_t *out_sock);
util_err_t ev_socket_create_udp(const char *bind_addr, uint16_t port,
                                 bool enable_broadcast,
                                 const ev_socket_opts_t *opts,
                                 socket_t *out_sock);

// Non-blocking I/O (used internally by macros, but available to apps)
util_err_t ev_socket_recv_append(socket_t sock, buf_t *buf);
ssize_t ev_socket_send_from_buf(socket_t sock, const buf_t *buf);

// Connection helpers
util_err_t ev_socket_check_connect(socket_t sock);
void ev_socket_close(socket_t sock);
```

### Buffer Operations (`ev/buf.h`)

```c
// Keep all of existing buf.h, add:

// Floats
bool buf_read_f32_be(buf_t *b, const char *field_name, float *out);
bool buf_read_f32_le(buf_t *b, const char *field_name, float *out);
bool buf_read_f64_be(buf_t *b, const char *field_name, double *out);
bool buf_read_f64_le(buf_t *b, const char *field_name, double *out);
bool buf_write_f32_be(buf_t *b, const char *field_name, float v);
bool buf_write_f32_le(buf_t *b, const char *field_name, float v);
bool buf_write_f64_be(buf_t *b, const char *field_name, double v);
bool buf_write_f64_le(buf_t *b, const char *field_name, double v);

// Strings
bool buf_read_cstring(buf_t *b, const char *field_name,
                      char *out, size_t max_len);
bool buf_read_fixed_string(buf_t *b, const char *field_name,
                           char *out, size_t len, size_t out_size);
bool buf_write_cstring(buf_t *b, const char *field_name, const char *str);
bool buf_write_fixed_string(buf_t *b, const char *field_name,
                            const char *str, size_t len);

// Debugging
typedef void (*buf_dump_line_fn_t)(const char *line, void *ctx);
void buf_dump_hex(const buf_t *b, buf_dump_line_fn_t callback, void *ctx);
void buf_dump_hex_to_file(const buf_t *b, FILE *fp);
```

### Error Handling (`ev/err.h`)

```c
// Keep existing util_err_t enum and functions from err.h
// No changes needed
```

### Logging (`ev/log.h`)

```c
typedef enum {
    EV_LOG_NONE = 0,
    EV_LOG_ERROR,
    EV_LOG_WARN,
    EV_LOG_INFO,
    EV_LOG_DEBUG,
    EV_LOG_TRACE
} ev_log_level_t;

typedef void (*ev_log_fn_t)(ev_log_level_t level,
                            const char *file, int line, const char *func,
                            const char *fmt, va_list args);

void ev_log_set(ev_log_fn_t log_fn, ev_log_level_t level);
ev_log_level_t ev_log_get_level(void);

// Internal use
void ev_log_impl(ev_log_level_t level, const char *file, int line,
                 const char *func, const char *fmt, ...);

#define EV_LOG(level, ...) \
    do { if (ev_log_get_level() >= level) { \
        ev_log_impl(level, __FILE__, __LINE__, __func__, __VA_ARGS__); \
    } } while(0)
```

---

## Example: Complete Modbus TCP Server

This example shows how the proposed API would be used in practice:

```c
#include <ev.h>
#include <modbus_protocol.h>

typedef struct {
    uint8_t recv_data[512];
    uint8_t send_data[512];
    buf_t recv_buf;
    buf_t send_buf;
    register_storage_t *registers;
} modbus_conn_t;

void modbus_connection_handler(ev_task_handle_t task, socket_t sock, void *ctx) {
    EV_TASK_START(task);

    modbus_conn_t *conn = ctx;
    conn->recv_buf = buf_init(conn->recv_data, sizeof(conn->recv_data));
    conn->send_buf = buf_init(conn->send_data, sizeof(send_data));

    while (1) {
        // Read MBAP header (7 bytes)
        ev_recv_exact(task, sock, &conn->recv_buf, 7);

        if (ev_task_get_error(task) != UTIL_OK) {
            break;  // Connection closed or error
        }

        // Parse header
        uint16_t transaction_id, protocol_id, length;
        uint8_t unit_id;
        buf_read_u16_be(&conn->recv_buf, "transaction_id", &transaction_id);
        buf_read_u16_be(&conn->recv_buf, "protocol_id", &protocol_id);
        buf_read_u16_be(&conn->recv_buf, "length", &length);
        buf_read_u8(&conn->recv_buf, "unit_id", &unit_id);

        if (!buf_ok(&conn->recv_buf)) {
            EV_LOG(EV_LOG_ERROR, "Failed to parse MBAP header");
            break;
        }

        // Read PDU (length - 1 because unit_id already read)
        ev_recv_exact(task, sock, &conn->recv_buf, length - 1);

        if (ev_task_get_error(task) != UTIL_OK) {
            break;
        }

        // Process Modbus request
        process_modbus_pdu(&conn->recv_buf, &conn->send_buf, conn->registers);

        if (!buf_ok(&conn->send_buf)) {
            EV_LOG(EV_LOG_ERROR, "Failed to build response");
            break;
        }

        // Send response
        ev_send_all(task, sock, &conn->send_buf);

        if (ev_task_get_error(task) != UTIL_OK) {
            break;
        }

        // Reset for next request
        buf_reset(&conn->recv_buf);
        buf_reset(&conn->send_buf);
    }

    // Cleanup
    ev_socket_close(sock);
    free(conn);

    EV_TASK_END(task);
}

void accept_handler(ev_task_handle_t task, socket_t listen_sock, void *ctx) {
    EV_TASK_START(task);

    register_storage_t *registers = ctx;

    while (1) {
        ev_yield(task, EV_EVENT_READ);  // Wait for connection

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        socket_t client_sock = accept(listen_sock,
                                       (struct sockaddr *)&client_addr,
                                       &addr_len);

        if (client_sock == INVALID_SOCKET_FD) {
            continue;  // Accept failed, try again
        }

        // Configure client socket
        ev_socket_opts_t opts = EV_SOCKET_OPTS_DEFAULT;
        if (ev_socket_configure(client_sock, &opts) != UTIL_OK) {
            ev_socket_close(client_sock);
            continue;
        }

        // Create connection context
        modbus_conn_t *conn = calloc(1, sizeof(modbus_conn_t));
        if (!conn) {
            ev_socket_close(client_sock);
            continue;
        }
        conn->registers = registers;

        // Add task for this connection
        ev_task_handle_t conn_task;
        if (ev_loop_add_task(&conn_task, task.loop, client_sock,
                             modbus_connection_handler, conn) != UTIL_OK) {
            free(conn);
            ev_socket_close(client_sock);
            continue;
        }

        EV_LOG(EV_LOG_INFO, "Accepted connection from %s:%d",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    }

    EV_TASK_END(task);
}

int main(int argc, char *argv[]) {
    // Setup logging
    ev_log_set(my_log_function, EV_LOG_INFO);

    // Create register storage
    register_storage_t *registers = register_storage_create(1000);

    // Create event loop
    ev_loop_t *loop = ev_loop_create(100);  // Max 100 connections

    // Create listening socket
    socket_t listen_sock;
    ev_socket_opts_t listen_opts = EV_SOCKET_OPTS_DEFAULT;
    listen_opts.reuse_addr = true;

    if (ev_socket_create_tcp_server("0.0.0.0", 502, 10,
                                     &listen_opts, &listen_sock) != UTIL_OK) {
        fprintf(stderr, "Failed to create listening socket\n");
        return 1;
    }

    // Add accept task
    ev_task_handle_t accept_task;
    ev_loop_add_task(&accept_task, loop, listen_sock,
                     accept_handler, registers);

    // Run event loop
    ev_loop_run(loop, 100);  // 100ms tick

    // Cleanup
    ev_socket_close(listen_sock);
    ev_loop_destroy(&loop);
    register_storage_destroy(registers);

    return 0;
}
```

**Line Count:**
- Protocol handler: ~60 lines
- Accept handler: ~40 lines
- Main: ~40 lines
- **Total: ~140 lines for complete Modbus TCP server**

**Without the library, same functionality would be:**
- Manual poll() setup: ~50 lines
- Per-connection wake pipes: ~30 lines
- Partial I/O handling: ~100 lines
- State machine management: ~80 lines
- Buffer management: ~40 lines
- **Total: ~440 lines**

**Savings: ~300 lines (~68% reduction)**

---

## Code Size Estimates

### Library Components

| Component | Lines of Code | Notes |
|-----------|--------------|-------|
| Buffer API (existing) | 560 | No changes |
| Buffer additions (floats, strings, dump) | 140 | New functions |
| Error types (existing) | 100 | No changes |
| Event loop core | 350 | Based on coro_net.c |
| I/O helper macros | 80 | New |
| Socket helpers | 200 | Platform-specific config |
| Socket creation helpers | 150 | Convenience wrappers |
| Logging system | 50 | Optional |
| **Total Library** | **1,630** | **One-time cost** |

### Per-Protocol Savings

| Without Library | With Library | Savings |
|----------------|--------------|---------|
| State machines | 100-150 lines | 0 lines | 100-150 |
| Partial I/O handling | 80-100 lines | 0 lines | 80-100 |
| Wake pipe management | 30-40 lines | 0 lines | 30-40 |
| Buffer operations | 40-60 lines | 0 lines | 40-60 |
| **Total per protocol** | **250-350** | **60-80** | **~200 lines** |

**Break-even point:** After implementing 8 protocols, total code is smaller with library.

**For 20 protocols:** ~4,000 lines saved overall.

---

## Platform Support

### Confirmed Working
- Linux (tested in `src/tests/utils`)
- macOS (tested in `src/tests/utils`)
- Windows (see `src/platform/windows`)

### Should Work With Minor Additions
- FreeBSD / NetBSD / OpenBSD (poll() available)
- Android (Linux-based)

### Future Work Needed
- **FreeRTOS:** Need to implement event loop using FreeRTOS event groups
- **Zephyr:** Can use native Zephyr thread/socket APIs
- **NuttX:** Similar to FreeRTOS

**Strategy for RTOS:**
- Core buffer API works unchanged (no OS dependencies)
- Event loop needs RTOS-specific backend
- ~200 lines per RTOS port

---

## Migration Path

For existing libplctag codebase:

### Phase 1: Extract Core Components
1. Copy `src/tests/utils/buf.[ch]` to new library
2. Copy `src/tests/utils/err.[ch]` to new library
3. Copy `src/tests/utils/coro_net.[ch]` as basis for event loop
4. Add proposed enhancements

### Phase 2: Create Examples
1. Port `src/tests/modbus_server` to use new API
2. Create minimal EtherNet/IP example
3. Document patterns

### Phase 3: Optional Integration
1. Evaluate using new library in libplctag itself
2. Could replace per-socket wake pipes with shared event loop
3. Reduce FD usage significantly

**No disruption to existing libplctag code required.**

---

## Security Considerations

### Built-in Safety Features

1. **Bounds checking:** All buffer operations check bounds automatically
2. **Integer overflow protection:** Buffer size calculations use size_t
3. **Type safety:** Field names in buf_read/write catch type mismatches
4. **Error propagation:** Errors can't be silently ignored (embedded in objects)

### Application Responsibilities

1. **Input validation:** App must validate protocol-specific constraints
2. **Resource limits:** App must limit max connections, buffer sizes
3. **Timeout handling:** App must implement protocol timeouts
4. **Authentication:** Library provides transport only

### Recommended Practices

```c
// Always validate lengths before allocating/reading
uint16_t msg_len;
buf_read_u16_be(&buf, "length", &msg_len);

if (msg_len > MAX_MESSAGE_SIZE) {
    EV_LOG(EV_LOG_WARN, "Message too large: %u", msg_len);
    return;  // Reject
}

// Use buffer checkpointing for tentative parsing
buf_t checkpoint = buf_checkpoint(&buf);
if (!parse_message(&buf, &msg)) {
    buf_restore(&buf, checkpoint);  // Revert on parse failure
    return;  // Need more data
}
```

---

## Testing Strategy

### Unit Tests

Each module should have comprehensive tests:

```c
// test_buf.c
void test_buf_read_u16_be(void) {
    uint8_t data[] = {0x12, 0x34};
    buf_t buf = buf_init(data, sizeof(data));
    uint16_t value;

    assert(buf_read_u16_be(&buf, "test", &value));
    assert(value == 0x1234);
    assert(buf_read_pos(&buf) == 2);
}

// test_buf_bounds.c
void test_buf_read_past_end(void) {
    uint8_t data[1] = {0x00};
    buf_t buf = buf_init(data, sizeof(data));
    uint16_t value;

    assert(!buf_read_u16_be(&buf, "test", &value));
    assert(buf_get_error(&buf) == UTIL_EBOUNDS);
    assert(strcmp(buf_get_failed_field(&buf), "test") == 0);
}
```

### Integration Tests

Test protocol implementations:

```c
// test_modbus_server.c
void test_modbus_read_holding_registers(void) {
    // Create client socket, connect to test server
    // Send Modbus request
    // Verify response
}
```

### Stress Tests

```c
// test_many_connections.c
void test_1000_concurrent_connections(void) {
    ev_loop_t *loop = ev_loop_create(1000);
    // Create 1000 client connections
    // Verify all work correctly
    // Check FD usage (should be ~1002, not ~3000)
}
```

---

## Documentation Deliverables

### 1. API Reference
- Doxygen comments in headers
- Generate HTML/PDF reference

### 2. User Guide
- Quick start tutorial
- Common patterns
- Platform-specific notes

### 3. Protocol Implementation Guide
- Step-by-step guide to implementing a protocol
- Example: minimal MODBUS implementation
- Example: minimal EtherNet/IP implementation

### 4. Migration Guide
- For users of libplctag platform layer
- For users of traditional BSD sockets

---

## Open Questions

### 1. Naming
Current proposal uses `ev_` prefix (event loop).

**Alternatives:**
- `minp_` (minimal protocol)
- `iprot_` (industrial protocol)
- `net_` (network)
- `nio_` (network I/O)

**Recommendation:** Stick with `ev_` - it's short and clear.

### 2. Build System
Should this be:
- **Option A:** Header-only library (single header with implementations)
- **Option B:** Traditional library (.a / .so)
- **Option C:** Both (header-only for simple use, library for complex)

**Recommendation:** Option B (traditional library) for:
- Better debugging
- Faster compile times
- Easier version management

### 3. C Standard
Target C99 or C11?

**Current codebase uses:** C99-compatible

**Recommendation:** Require C99 minimum, use C11 features when available:
- `_Static_assert` (C11) - fall back to runtime assert in C99
- `_Alignas` (C11) - not critical, skip if not available

### 4. Floating Point
Should float support be mandatory or optional?

**Consideration:** Some embedded systems don't have FPU.

**Recommendation:** Make it optional via `#ifdef EV_ENABLE_FLOAT`

---

## Implementation Priority

### Phase 1: Core (2-3 weeks)
1. Refactor `coro_net.h` to `ev/loop.h` with simplified API
2. Add `ev_loop_poll()` and `ev_loop_run()`
3. Add I/O helper macros (`ev_recv_exact`, `ev_send_all`)
4. Extract and clean up buffer API
5. Unit tests for all components

### Phase 2: Socket Helpers (1 week)
1. Implement `ev_socket_configure()`
2. Implement creation helpers
3. Unit tests

### Phase 3: Buffer Additions (1 week)
1. Add float support (optional)
2. Add string support
3. Add hex dump
4. Unit tests

### Phase 4: Documentation (1 week)
1. API reference (Doxygen)
2. Quick start guide
3. Example protocol implementation

### Phase 5: Examples (1-2 weeks)
1. Port Modbus TCP server
2. Create minimal EtherNet/IP example
3. Performance comparison with old approach

**Total estimated effort: 6-8 weeks**

---

## Success Metrics

### Code Size
- **Target:** 70% reduction in protocol implementation code
- **Measure:** Compare line count before/after for 3 protocols

### File Descriptors
- **Target:** 3x reduction in FD usage
- **Measure:** Current: 3 FDs per connection; New: 1 FD per connection + 2 total

### Performance
- **Target:** No regression vs. current libplctag
- **Measure:** Requests/sec for Modbus TCP server

### Developer Experience
- **Target:** Protocol implementation in <100 lines
- **Measure:** Time to implement new protocol from scratch

### Portability
- **Target:** Works on Linux, macOS, Windows, 1 RTOS
- **Measure:** CI tests passing on all platforms

---

## Conclusion

The proposed minimal protocol stack library solves real problems:

1. **File descriptor exhaustion** via shared wake mechanism
2. **State machine boilerplate** via stackless coroutines
3. **Bounds checking errors** via safe buffer API
4. **Platform differences** via unified socket configuration

The design is **minimal** (~1,600 lines total) but provides **maximum leverage** (saves ~200 lines per protocol).

**Recommendation: Proceed with implementation using the refinements outlined in this document.**

---

## Appendix: Alternative Approaches Considered

### Alternative 1: Pure Callbacks (No Coroutines)
**Rejected because:** Forces explicit state machines in every protocol (100+ lines of boilerplate each).

### Alternative 2: Stackful Coroutines
**Rejected because:** Stack overhead (4-8KB per connection), platform-specific, not available on all RTOSes.

### Alternative 3: Thread-Per-Connection
**Rejected because:** Even more stack overhead, synchronization complexity, doesn't scale.

### Alternative 4: libev/libuv Style API
**Rejected because:** Too complex for embedded use, large code size, learning curve.

### Why Stackless Coroutines Win
- **Minimal overhead:** Just an int per task for line number
- **Portable:** Pure C macros, works everywhere
- **Familiar syntax:** Looks like blocking code
- **Small code size:** ~350 lines for entire event loop

The Duff's Device approach is unconventional but it's the optimal solution for the constraints:
- Minimal memory per connection
- Blocking-style code
- Maximum portability
- Small code size

