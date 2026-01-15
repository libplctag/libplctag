# Coroutine Network API Refactoring Plan

## Overview

This document describes refactoring the `coro_net` protothread-based coroutine system to be cleaner and more explicit. The goal is to reduce macro complexity while keeping the explicit, low-overhead nature of protothreads.

## Current Problems

1. **Too many macros** - `socket_read_yield`, `socket_write_yield`, `socket_accept_yield`, etc. are all complex macros that hide control flow
2. **Macros can't return values** - Forces use of output parameters (`err`) rather than natural return values
3. **Frame checking embedded in macros** - The `socket_read_yield` macro has framing logic baked in

## New Design Principles

1. **Only 3 macros** - `CORO_START`, `coro_yield`, `CORO_END`
2. **Real functions return real values** - Socket operations return `util_err_t`
3. **Explicit while loops** - The yield-retry pattern is visible in user code
4. **Frame checking in a proper function** - `socket_recv_frame()` encapsulates the framing logic

## API Changes

### 1. Add Frame Check Callback Type to socket.h

```c
/**
 * @brief Frame check callback type
 *
 * Called after each recv() to check if a complete frame has been received.
 *
 * @param buf - Buffer containing received data so far
 * @param context - User-supplied context pointer
 * @return UTIL_OK if frame is complete, UTIL_EAGAIN if more data needed, error otherwise
 */
typedef util_err_t (*socket_frame_check_fn)(buf_t *buf, void *context);
```

### 2. Add socket_recv_frame() to socket.h

```c
/**
 * @brief Receive data until a complete frame is detected
 *
 * Reads data from socket into buffer, calling frame_check after each read
 * to determine if a complete frame has been received. If the buffer already
 * contains a complete frame (from previous reads), returns immediately.
 *
 * @param sock - Socket to receive data on
 * @param buf - Buffer to store received data (data is appended)
 * @param frame_check - Callback to check if frame is complete
 * @param context - User context passed to frame_check
 * @return UTIL_OK when frame complete, UTIL_EAGAIN if would block, error otherwise
 */
util_err_t socket_recv_frame(socket_t sock, buf_t *buf,
                             socket_frame_check_fn frame_check, void *context);
```

### 3. Implement socket_recv_frame() in socket.c

```c
util_err_t socket_recv_frame(socket_t sock, buf_t *buf,
                             socket_frame_check_fn frame_check, void *context) {
    util_err_t err;

    if (sock == INVALID_SOCKET || buf == NULL || frame_check == NULL) {
        return UTIL_EINVAL;
    }

    if (!buf_ok(buf)) {
        return UTIL_EINVAL;
    }

    /* First check if we already have a complete frame from previous data */
    err = frame_check(buf, context);
    if (err == UTIL_OK) {
        return UTIL_OK;  /* Frame already complete */
    }
    if (err != UTIL_EAGAIN) {
        return err;  /* Frame check found an error */
    }

    /* Try to receive more data */
    err = socket_recv_buf(sock, buf);
    if (err != UTIL_OK) {
        return err;  /* EAGAIN, ECLOSED, or error */
    }

    /* Check again after receiving new data */
    return frame_check(buf, context);
}
```

### 4. Simplify coro_net.h Macros

Remove all the `socket_*_yield` macros. Keep only:

```c
/**
 * Start a coroutine handler function.
 * Must be the first statement in the handler after variable declarations.
 */
#define CORO_START(task) switch(coro_get_line(task)) { case 0:

/**
 * Yield execution until the specified socket event occurs.
 * Use CORO_EVENT_READ for read/accept, CORO_EVENT_WRITE for write/connect.
 */
#define coro_yield(task, event) \
    do { \
        coro_set_line((task), __LINE__); \
        coro_set_task_event((task), (event)); \
        return; \
        case __LINE__:; \
    } while(0)

/**
 * End a coroutine handler function.
 * Automatically removes the task from the event loop.
 */
#define CORO_END(task) default: break; } coro_remove_task(task);
```

### 5. Keep Existing Event Constants

```c
#define CORO_NO_SOCKET      0
#define CORO_EVENT_ALWAYS   ((short)-1)
#define CORO_EVENT_NONE     0
#define CORO_EVENT_READ     POLLIN
#define CORO_EVENT_WRITE    POLLOUT
```

## Usage Pattern

All yielding operations follow the same explicit pattern:

```c
while ((err = socket_operation(...)) == UTIL_EAGAIN) {
    coro_yield(task, EVENT_TYPE);
}
if (err != UTIL_OK) {
    /* handle error */
}
```

### Read a Framed Message

```c
while ((err = socket_recv_frame(fd, &buf, my_frame_check, ctx)) == UTIL_EAGAIN) {
    coro_yield(task, CORO_EVENT_READ);
}
if (err != UTIL_OK) {
    /* handle error or connection closed */
}
```

### Write Data

```c
while ((err = socket_send_buf(fd, &buf)) == UTIL_EAGAIN) {
    coro_yield(task, CORO_EVENT_WRITE);
}
if (err != UTIL_OK) {
    /* handle error */
}
```

### Accept Connection

```c
while ((err = socket_accept(listen_fd, &client_fd, &client_addr)) == UTIL_EAGAIN) {
    coro_yield(task, CORO_EVENT_READ);
}
if (err != UTIL_OK) {
    /* handle error */
}
```

### Connect to Server

```c
while ((err = socket_connect(fd, &addr)) == UTIL_EAGAIN) {
    coro_yield(task, CORO_EVENT_WRITE);
}
if (err != UTIL_OK) {
    /* handle error */
}
```

### UDP Receive

```c
while ((err = socket_recvfrom_buf(fd, &from_addr, &buf)) == UTIL_EAGAIN) {
    coro_yield(task, CORO_EVENT_READ);
}
if (err != UTIL_OK) {
    /* handle error */
}
```

### UDP Send

```c
while ((err = socket_sendto_buf(fd, &to_addr, &buf)) == UTIL_EAGAIN) {
    coro_yield(task, CORO_EVENT_WRITE);
}
if (err != UTIL_OK) {
    /* handle error */
}
```

## Complete Handler Example

```c
static util_err_t modbus_frame_check(buf_t *buf, void *context) {
    (void)context;

    /* Need at least MBAP header (7 bytes) */
    if (buf_read_size(buf) < 7) {
        return UTIL_EAGAIN;
    }

    /* Peek at length field at offset 4 (2 bytes, big-endian) */
    const uint8_t *data = buf_read_ptr(buf);
    uint16_t length = (uint16_t)((data[4] << 8) | data[5]);

    /* Total frame size is header (6 bytes) + length */
    size_t frame_size = 6 + length;

    if (buf_read_size(buf) < frame_size) {
        return UTIL_EAGAIN;
    }

    return UTIL_OK;
}

static void client_handler(coro_task_handle_t task, socket_t fd, void *context) {
    client_ctx_t *client = (client_ctx_t *)context;
    util_err_t err;

    CORO_START(task);

    while (1) {
        buf_compact(&client->recv_buf);

        /* Read until we have a complete frame */
        while ((err = socket_recv_frame(fd, &client->recv_buf,
                                        modbus_frame_check, NULL)) == UTIL_EAGAIN) {
            coro_yield(task, CORO_EVENT_READ);
        }
        if (err != UTIL_OK) {
            break;
        }

        /* Process request and prepare response */
        buf_reset(&client->send_buf);
        err = process_request(&client->recv_buf, &client->send_buf);
        if (err != UTIL_OK) {
            break;
        }

        /* Send response */
        while ((err = socket_send_buf(fd, &client->send_buf)) == UTIL_EAGAIN) {
            coro_yield(task, CORO_EVENT_WRITE);
        }
        if (err != UTIL_OK) {
            break;
        }
    }

    free(client);
    CORO_END(task);
}
```

## Files to Modify

1. **socket.h** - Add `socket_frame_check_fn` typedef and `socket_recv_frame()` declaration
2. **socket.c** - Add `socket_recv_frame()` implementation
3. **coro_net.h** - Remove all `socket_*_yield` macros, rename `coro_wait_for_event` to `coro_yield`
4. **modbus_server.c** - Update to use new pattern (example client code)

## Benefits

- **Fewer macros** - Only 3 instead of 7+
- **Real return values** - Functions return `util_err_t` naturally
- **Explicit control flow** - The while loop is visible, not hidden in a macro
- **Easier debugging** - Can step through real function calls
- **Consistent pattern** - All operations use the same `while/yield` pattern
- **Framing logic encapsulated** - `socket_recv_frame()` handles the recv+check loop
